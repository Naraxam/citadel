#include "CardFilter.h"
#include "CardStats.h"
#include "GameState.h"
#include "KeywordAbility.h"
#include <algorithm>
#include <charconv>
#include <string>

namespace mtg {

bool cardMatchesFilter(const Card& c, std::string_view filter,
                       uint8_t activeController,
                       ObjectId selfId,
                       const Card* sourceCard,
                       const GameState* game,
                       const std::vector<ObjectId>* remembered) noexcept {
    if (filter.empty()) return true;

    auto dot  = filter.find('.');
    std::string_view type    = (dot == std::string_view::npos) ? filter
                                                               : filter.substr(0, dot);
    std::string_view qualStr = (dot == std::string_view::npos) ? std::string_view{"All"}
                                                               : filter.substr(dot + 1);

    // ── Type check ─────────────────────────────────────────────────────────
    bool typeOk = false;
    if      (type == "Creature")     typeOk = c.isCreature();
    else if (type == "Artifact")     typeOk = c.rules->type.isArtifact();
    else if (type == "Enchantment")  typeOk = c.rules->type.isEnchantment();
    else if (type == "Land")         typeOk = c.isLand();
    else if (type == "Planeswalker") typeOk = c.rules->type.isPlaneswalker();
    else if (type == "Permanent")    typeOk = c.isPermanent();
    else if (type == "Instant")      typeOk = c.rules->type.isInstant();
    else if (type == "Sorcery")      typeOk = c.rules->type.isSorcery();
    // "Spell" = any card currently on the stack (active spell/ability)
    else if (type == "Spell")        typeOk = (c.zone == ZoneType::Stack);
    else if (type == "Card")         typeOk = true;
    else if (type == "Any")          typeOk = true;
    else if (type == "Basic")        typeOk = c.isLand() && c.rules->type.isBasic();
    else {
        typeOk = c.rules->type.hasSubtype(type); // creature/land subtypes: Elf, Goblin, Forest…
        // Also check dynamically-added subtypes from static abilities (Xenograft, etc.)
        if (!typeOk) {
            for (const auto& sub : c.continuousSubtypes)
                if (sub == type) { typeOk = true; break; }
        }
        // Changeling: has all creature types
        if (!typeOk && c.rules->hasKeyword("Changeling") && c.isCreature())
            typeOk = true;
    }

    if (!typeOk) return false;

    // ── Color identity helper ─────────────────────────────────────────────
    uint8_t colorId = (c.colorIdOverride != 0xFF)
                      ? c.colorIdOverride
                      : c.rules->hasKeyword("Devoid")
                          ? uint8_t(0)  // Devoid: colorless regardless of mana cost
                          : c.rules->manaCost.colorIdentity();

    // ── Qualifier chain (split on '+', ALL must match) ─────────────────────
    while (!qualStr.empty()) {
        auto plus = qualStr.find('+');
        std::string_view q = (plus == std::string_view::npos) ? qualStr
                                                              : qualStr.substr(0, plus);
        qualStr = (plus == std::string_view::npos) ? std::string_view{}
                                                   : qualStr.substr(plus + 1);

        if (q.empty() || q == "All") continue;

        // ── Controller qualifiers ────────────────────────────────────────
        if (q == "YouCtrl")               { if (c.controllerId != activeController) return false; }
        else if (q == "OppCtrl"
              || q == "NotYouCtrl")       { if (c.controllerId == activeController) return false; }
        else if (q == "Other")            { if (selfId != kInvalidId && c.id == selfId) return false; }
        else if (q == "Self")             { if (selfId == kInvalidId || c.id != selfId) return false; }

        // ── Ownership qualifiers ─────────────────────────────────────────
        else if (q == "YouOwn")           { if (c.ownerId != activeController) return false; }
        else if (q == "OppOwn")           { if (c.ownerId == activeController) return false; }

        // ── State qualifiers ─────────────────────────────────────────────
        else if (q == "Tapped")           { if (!c.tapped)    return false; }
        else if (q == "Untapped")         { if (c.tapped)     return false; }
        else if (q == "Token")            { if (!c.isToken)   return false; }
        else if (q == "nonToken")         { if (c.isToken)    return false; }
        else if (q == "hasCounters")      { if (c.counters.empty()) return false; }
        else if (q == "escaped")          { if (!c.escaped)   return false; }
        else if (q == "IsSuspected")      { if (!c.hasAttribute("Suspected"))  return false; }
        else if (q == "IsSaddled")        { if (!c.hasAttribute("Saddled"))    return false; }
        else if (q == "IsPrepared")       { if (!c.hasAttribute("Prepared"))   return false; }
        else if (q == "IsPlotted")        { if (!c.hasAttribute("Plotted"))    return false; }
        else if (q == "IsSolved")         { if (!c.hasAttribute("Solved"))     return false; }
        // NamedCard: c must match the name designated by the source card's namedCard field
        else if (q == "NamedCard") {
            if (!sourceCard || sourceCard->namedCard.empty()) return false;
            if (c.name() != sourceCard->namedCard) return false;
        }

        // ── Remembered / imprinted qualifiers ────────────────────────────
        // IsRemembered: card is in the current effect's "remembered" list.
        // When the remembered list is unavailable (nullptr), accept conservatively.
        else if (q == "IsRemembered" || q == "IsTriggerRemembered") {
            if (!remembered) { /* conservative: accept */ }
            else {
                bool found = std::find(remembered->begin(), remembered->end(), c.id)
                             != remembered->end();
                if (!found) return false;
            }
        }
        // IsImprinted: card was exiled by the source (imprint-style effects).
        // Equivalent to ExiledWithSource in our engine.
        else if (q == "IsImprinted" || q == "ExiledWithSource") {
            if (!sourceCard) return false;
            if (c.zone != ZoneType::Exile || c.exiledBy != sourceCard->id) return false;
        }
        // EffectSource: the card IS the source of the current effect.
        else if (q == "EffectSource") {
            if (!sourceCard || c.id != sourceCard->id) return false;
        }
        // RememberedPlayerCtrl: controlled by the player stored in game->chosenPlayerHint.
        else if (q == "RememberedPlayerCtrl") {
            if (!game || game->chosenPlayerHint == 255) { /* conservative */ }
            else if (c.controllerId != game->chosenPlayerHint) return false;
        }

        // ── Game-state qualifiers ─────────────────────────────────────────
        // TopLibrary: card is the top card of a player's library.
        else if (q == "TopLibrary") {
            if (c.zone != ZoneType::Library) return false;
            if (game) {
                const Card* top = game->player(c.ownerId).library().front();
                if (top != &c) return false;
            }
            // no game pointer: accept any library card conservatively
        }
        // ChosenType: card's type line includes the chosen type. Prefer the
        // ability source's per-card choice (Herald's Horn et al.), falling back
        // to the global chosenTypeName for spells that set it transiently.
        else if (q == "ChosenType") {
            std::string ctStore = (sourceCard && !sourceCard->chosenType.empty())
                                  ? sourceCard->chosenType
                                  : (game ? game->chosenTypeName : std::string());
            if (ctStore.empty()) { /* conservative: no choice yet — accept */ }
            else {
                const std::string& ct = ctStore;
                bool typeMatch = false;
                if      (ct == "Creature")     typeMatch = c.isCreature();
                else if (ct == "Land")         typeMatch = c.isLand();
                else if (ct == "Artifact")     typeMatch = c.rules->type.isArtifact();
                else if (ct == "Enchantment")  typeMatch = c.rules->type.isEnchantment();
                else if (ct == "Planeswalker") typeMatch = c.rules->type.isPlaneswalker();
                else if (ct == "Instant")      typeMatch = c.rules->type.isInstant();
                else if (ct == "Sorcery")      typeMatch = c.rules->type.isSorcery();
                else                           typeMatch = c.rules->type.hasSubtype(ct);
                if (!typeMatch) return false;
            }
        }
        // ChosenColor: card's color identity includes the color stored in game->chosenColorName.
        else if (q == "ChosenColor") {
            if (!game || game->chosenColorName.empty()) { /* conservative */ }
            else {
                const std::string& cc = game->chosenColorName;
                bool colorMatch = false;
                if      (cc == "White")     colorMatch = (colorId & 0x01) != 0;
                else if (cc == "Blue")      colorMatch = (colorId & 0x02) != 0;
                else if (cc == "Black")     colorMatch = (colorId & 0x04) != 0;
                else if (cc == "Red")       colorMatch = (colorId & 0x08) != 0;
                else if (cc == "Green")     colorMatch = (colorId & 0x10) != 0;
                else if (cc == "Colorless") colorMatch = (colorId == 0);
                if (!colorMatch) return false;
            }
        }

        // ── Type qualifiers ──────────────────────────────────────────────
        else if (q == "nonCreature")      { if (c.isCreature())            return false; }
        else if (q == "nonLand")          { if (c.isLand())                return false; }
        else if (q == "nonArtifact")      { if (c.rules->type.isArtifact())     return false; }
        else if (q == "nonEnchantment")   { if (c.rules->type.isEnchantment())  return false; }
        else if (q == "nonPlaneswalker")  { if (c.rules->type.isPlaneswalker()) return false; }
        else if (q == "Legendary")        { if (!c.rules->type.isLegendary())   return false; }
        else if (q == "nonLegendary")     { if (c.rules->type.isLegendary())    return false; }
        else if (q == "Basic")            { if (!c.rules->type.isBasic())       return false; }
        else if (q == "nonBasic")         { if (c.rules->type.isBasic())        return false; }
        else if (q == "Equipment") {
            bool isEquip = false;
            for (const auto& sub : c.rules->type.subtypes)
                if (sub == "Equipment") { isEquip = true; break; }
            if (!isEquip) return false;
        }

        // ── Color qualifiers ─────────────────────────────────────────────
        // colorId uses ManaAtom bits: W=0x01 U=0x02 B=0x04 R=0x08 G=0x10
        else if (q == "White")            { if (!(colorId & 0x01)) return false; }
        else if (q == "Blue")             { if (!(colorId & 0x02)) return false; }
        else if (q == "Black")            { if (!(colorId & 0x04)) return false; }
        else if (q == "Red")              { if (!(colorId & 0x08)) return false; }
        else if (q == "Green")            { if (!(colorId & 0x10)) return false; }
        else if (q == "Colorless")        { if (colorId != 0)       return false; }
        else if (q == "nonWhite")         { if ( (colorId & 0x01)) return false; }
        else if (q == "nonBlue")          { if ( (colorId & 0x02)) return false; }
        else if (q == "nonBlack")         { if ( (colorId & 0x04)) return false; }
        else if (q == "nonRed")           { if ( (colorId & 0x08)) return false; }
        else if (q == "nonGreen")         { if ( (colorId & 0x10)) return false; }
        else if (q == "nonColorless")     { if (colorId == 0)       return false; }
        // MultiColor: card has 2+ colors. (colorId & (colorId-1)) != 0 iff 2+ bits set.
        else if (q == "MultiColor"
              || q == "multicolored")    { if ((colorId & (colorId - 1)) == 0) return false; }
        else if (q == "nonMultiColor"
              || q == "Monocolored")     { if ((colorId & (colorId - 1)) != 0) return false; }

        // ── Historic ─────────────────────────────────────────────────────
        // A card is historic if it is Legendary, an Artifact, or a Saga.
        else if (q == "Historic") {
            bool isHistoric = c.rules->type.isLegendary()
                           || c.rules->type.isArtifact()
                           || c.rules->type.hasSubtype("Saga");
            if (!isHistoric) return false;
        }
        else if (q == "nonHistoric") {
            bool isHistoric = c.rules->type.isLegendary()
                           || c.rules->type.isArtifact()
                           || c.rules->type.hasSubtype("Saga");
            if (isHistoric) return false;
        }

        // ── Combat qualifiers ────────────────────────────────────────────────
        else if (q == "Attacking")        { if (!c.attacking)  return false; }
        else if (q == "Blocking")         { if (!c.blocking)   return false; }
        else if (q == "isBlocked")        { if (!c.isBlocked)  return false; }
        else if (q == "unblocked")        {
            // unblocked = attacking and not blocked
            if (!c.attacking || c.isBlocked) return false;
        }

        // ── Permanent-state qualifiers ────────────────────────────────────
        else if (q == "Monstrous")        { if (!c.monstrous)  return false; }
        else if (q == "nonMonstrous")     { if ( c.monstrous)  return false; }
        else if (q == "Renowned")         { if (!c.renowned)   return false; }
        else if (q == "nonRenowned")      { if ( c.renowned)   return false; }
        else if (q == "toughnessGTpower") { if (!(effectiveToughness(c) > effectivePower(c))) return false; }
        else if (q == "powerGTtoughness") { if (!(effectivePower(c) > effectiveToughness(c))) return false; }

        // ── Source-relative qualifiers (require sourceCard) ──────────────────
        // sharesNameWith: c has the same name as sourceCard.
        else if (q == "sharesNameWith") {
            if (!sourceCard) { /* conservative: accept */ }
            else if (c.name() != sourceCard->name()) return false;
        }
        // sharesCreatureType: c shares at least one creature subtype with sourceCard.
        else if (q == "sharesCreatureType") {
            if (!sourceCard) { /* conservative: accept */ }
            else {
                bool shared = false;
                for (const auto& sub : c.rules->type.subtypes)
                    if (sourceCard->rules->type.hasSubtype(sub)) { shared = true; break; }
                // Also check continuousSubtypes added by static effects
                if (!shared) {
                    for (const auto& sub : c.continuousSubtypes)
                        if (sourceCard->rules->type.hasSubtype(sub) ||
                            std::find(sourceCard->continuousSubtypes.begin(),
                                      sourceCard->continuousSubtypes.end(), sub)
                                != sourceCard->continuousSubtypes.end())
                        { shared = true; break; }
                }
                if (!shared) return false;
            }
        }
        // sharesColorWith: c shares at least one color with sourceCard.
        else if (q == "sharesColorWith") {
            if (!sourceCard) { /* conservative: accept */ }
            else {
                uint8_t srcColor = (sourceCard->colorIdOverride != 0xFF)
                                   ? sourceCard->colorIdOverride
                                   : sourceCard->rules->hasKeyword("Devoid")
                                       ? uint8_t(0)
                                       : sourceCard->rules->manaCost.colorIdentity();
                if ((colorId & srcColor) == 0) return false;
            }
        }

        // ── Stat comparisons — PowerGE2, ToughnessLE3, CMC.EQ4 … ──────────
        else if (q.size() >= 8 && q.substr(0, 7) == "PowerGE") {
            int n = 0; std::from_chars(q.data()+7, q.data()+q.size(), n);
            if (effectivePower(c) < n) return false;
        }
        else if (q.size() >= 8 && q.substr(0, 7) == "PowerLE") {
            int n = 0; std::from_chars(q.data()+7, q.data()+q.size(), n);
            if (effectivePower(c) > n) return false;
        }
        else if (q.size() >= 8 && q.substr(0, 7) == "PowerEQ") {
            int n = 0; std::from_chars(q.data()+7, q.data()+q.size(), n);
            if (effectivePower(c) != n) return false;
        }
        else if (q.size() >= 8 && q.substr(0, 7) == "PowerGT") {
            int n = 0; std::from_chars(q.data()+7, q.data()+q.size(), n);
            if (effectivePower(c) <= n) return false;
        }
        else if (q.size() >= 8 && q.substr(0, 7) == "PowerLT") {
            int n = 0; std::from_chars(q.data()+7, q.data()+q.size(), n);
            if (effectivePower(c) >= n) return false;
        }
        else if (q.size() >= 12 && q.substr(0, 11) == "ToughnessGE") {
            int n = 0; std::from_chars(q.data()+11, q.data()+q.size(), n);
            if (effectiveToughness(c) < n) return false;
        }
        else if (q.size() >= 12 && q.substr(0, 11) == "ToughnessLE") {
            int n = 0; std::from_chars(q.data()+11, q.data()+q.size(), n);
            if (effectiveToughness(c) > n) return false;
        }
        else if (q.size() >= 12 && q.substr(0, 11) == "ToughnessEQ") {
            int n = 0; std::from_chars(q.data()+11, q.data()+q.size(), n);
            if (effectiveToughness(c) != n) return false;
        }
        else if (q.size() >= 12 && q.substr(0, 11) == "ToughnessGT") {
            int n = 0; std::from_chars(q.data()+11, q.data()+q.size(), n);
            if (effectiveToughness(c) <= n) return false;
        }
        else if (q.size() >= 12 && q.substr(0, 11) == "ToughnessLT") {
            int n = 0; std::from_chars(q.data()+11, q.data()+q.size(), n);
            if (effectiveToughness(c) >= n) return false;
        }
        // CMC.EQ4, CMC.GE2, CMC.LE5, CMC.GT3, CMC.LT6
        else if (q.size() >= 7 && q.substr(0, 4) == "CMC.") {
            auto op = q.substr(4, 2);
            int n = 0; std::from_chars(q.data()+6, q.data()+q.size(), n);
            int cmc = c.rules->cmc();
            if      (op == "GE" && cmc < n)  return false;
            else if (op == "GT" && cmc <= n) return false;
            else if (op == "LE" && cmc > n)  return false;
            else if (op == "LT" && cmc >= n) return false;
            else if (op == "EQ" && cmc != n) return false;
        }

        // ── Counter-type qualifier — hasCounter.P1P1, hasCounter.CHARGE … ─
        else if (q.size() > 11 && q.substr(0, 11) == "hasCounter.") {
            auto ctype = q.substr(11);
            std::string key;
            if      (ctype == "P1P1")    key = "+1/+1";
            else if (ctype == "M1M1")    key = "-1/-1";
            else if (ctype == "CHARGE")  key = "charge";
            else if (ctype == "LOYALTY") key = "loyalty";
            else                         key = std::string(ctype);
            if (c.counterCount(key) <= 0) return false;
        }

        // ── Counter range qualifiers — counters_GE<N>_<TYPE>, counters_LE<N>_<TYPE>, EQ, GT, LT ──
        // Used by Level Up conditions and Mode$ Always: counters_EQ0_P1P1, counters_GE1_LEVEL, etc.
        else if (q.size() > 9 && q.substr(0, 9) == "counters_") {
            auto rest = q.substr(9);  // e.g. "GE1_LEVEL" or "EQ0_P1P1"
            bool isGE = rest.size() >= 3 && rest[0] == 'G' && rest[1] == 'E';
            bool isLE = rest.size() >= 3 && rest[0] == 'L' && rest[1] == 'E';
            bool isEQ = rest.size() >= 3 && rest[0] == 'E' && rest[1] == 'Q';
            bool isGT = rest.size() >= 3 && rest[0] == 'G' && rest[1] == 'T';
            bool isLT = rest.size() >= 3 && rest[0] == 'L' && rest[1] == 'T';
            if (isGE || isLE || isEQ || isGT || isLT) {
                auto numAndType = rest.substr(2);
                auto sep = numAndType.find('_');
                if (sep != std::string_view::npos) {
                    int n = 0;
                    std::from_chars(numAndType.data(), numAndType.data() + sep, n);
                    auto typePart = numAndType.substr(sep + 1);
                    std::string key;
                    if      (typePart == "P1P1")   key = "+1/+1";
                    else if (typePart == "M1M1")   key = "-1/-1";
                    else if (typePart == "CHARGE")  key = "charge";
                    else if (typePart == "LOYALTY") key = "loyalty";
                    else                            key = std::string(typePart);
                    int cnt = c.counterCount(key);
                    if (isGE && cnt <  n) return false;
                    if (isLE && cnt >  n) return false;
                    if (isEQ && cnt != n) return false;
                    if (isGT && cnt <= n) return false;
                    if (isLT && cnt >= n) return false;
                }
            }
        }

        // ── Owner/controller mismatch — OwnerDoesntControl ──────────────────
        else if (q == "OwnerDoesntControl") { if (c.ownerId == c.controllerId) return false; }

        // ── Card-type qualifiers used as secondary filters (Card.Self+Enchantment) ──
        else if (q == "Enchantment")  { if (!c.rules->type.isEnchantment())  return false; }
        else if (q == "Artifact")     { if (!c.rules->type.isArtifact())     return false; }
        else if (q == "Planeswalker") { if (!c.rules->type.isPlaneswalker()) return false; }
        // Creature/Land are already handled at the type-check level, but also accept as qualifiers
        else if (q == "CreatureType") { if (!c.isCreature())                 return false; }

        // ── with<Keyword> / without<Keyword> qualifier — withFlying, withoutHaste … ──
        // "Card.YouCtrl+withFlying" means the card currently has the named keyword.
        // "Creature.OppCtrl+withoutFlying" means the card does NOT have the keyword.
        else if (q.size() > 7 && q.substr(0, 7) == "without") {
            auto kwName = std::string(q.substr(7));
            auto kw = parseKeyword(kwName);
            if (kw != KeywordAbility::None) {
                if (c.hasKeyword(kw)) return false;
            }
        }
        else if (q.size() > 4 && q.substr(0, 4) == "with") {
            auto kwName = std::string(q.substr(4));
            auto kw = parseKeyword(kwName);
            if (kw != KeywordAbility::None) {
                if (!c.hasKeyword(kw)) return false;
            }
        }

        // ── Keyword ability qualifier — keyword:Flying, keyword:Haste … ────
        else if (q.size() > 8 && q.substr(0, 8) == "keyword:") {
            auto kwName = q.substr(8);
            auto kw = parseKeyword(std::string(kwName));
            if (kw != KeywordAbility::None) {
                if (!c.hasKeyword(kw)) return false;
            }
            // If parseKeyword returns None (unknown keyword), accept conservatively.
        }

        // ── Face-down: morph (isFaceDown) or manifest (manifested) ─────────
        else if (q == "FaceDown")    { if (!c.isFaceDown && !c.manifested)  return false; }
        else if (q == "nonFaceDown") { if ( c.isFaceDown ||  c.manifested)  return false; }

        // ── Attachment qualifiers ────────────────────────────────────────────
        // IsEquipped: permanent has at least one Equipment attached to it.
        else if (q == "IsEquipped") {
            bool equipped = false;
            for (ObjectId aid : c.attachments) {
                if (!game) { equipped = true; break; }
                const Card* att = game->findCard(aid);
                if (att && att->rules->type.hasSubtype("Equipment")) { equipped = true; break; }
            }
            if (!equipped) return false;
        }
        // IsEnchanted: permanent has at least one Aura attached to it.
        else if (q == "IsEnchanted") {
            bool enchanted = false;
            for (ObjectId aid : c.attachments) {
                if (!game) { enchanted = true; break; }
                const Card* att = game->findCard(aid);
                if (att && att->rules->type.hasSubtype("Aura")) { enchanted = true; break; }
            }
            if (!enchanted) return false;
        }
        // IsAttached: permanent has any attachment (Equipment or Aura).
        else if (q == "IsAttached") { if (c.attachments.empty()) return false; }

        // ── Subtype check (Elf, Human, Goblin, Dragon, Forest, Island …) ──
        else {
            // If it's a known subtype → reject if not matched.
            // If it starts with lowercase it's probably an unknown behavioral qualifier
            // → accept conservatively.
            if (!q.empty() && std::isupper(static_cast<unsigned char>(q[0]))) {
                bool hasSub = c.rules->type.hasSubtype(q);
                if (!hasSub) {
                    for (const auto& csub : c.continuousSubtypes)
                        if (csub == q) { hasSub = true; break; }
                }
                // Changeling: has all creature types
                if (!hasSub && c.isCreature() && c.rules->hasKeyword("Changeling"))
                    hasSub = true;
                if (!hasSub) return false;
            }
            // lowercase or empty → accept
        }
    }
    return true;
}

bool cardMatchesAnyFilter(const Card& c, std::string_view filter,
                          uint8_t activeController,
                          ObjectId selfId,
                          const Card* sourceCard,
                          const GameState* game,
                          const std::vector<ObjectId>* remembered) noexcept {
    while (!filter.empty()) {
        auto comma = filter.find(',');
        std::string_view part = (comma == std::string_view::npos)
                                ? filter : filter.substr(0, comma);
        filter = (comma == std::string_view::npos)
                 ? std::string_view{} : filter.substr(comma + 1);
        if (cardMatchesFilter(c, part, activeController, selfId, sourceCard, game, remembered))
            return true;
    }
    return false;
}

bool manaRestrictionAllows(std::string_view spec, const Card& payee,
                           bool isSpell, bool isActivated,
                           uint8_t activeController,
                           const Card* producer,
                           const GameState* game) noexcept {
    if (spec.empty()) return true;

    // CardFilter requires a primary TYPE token, then qualifiers. RestrictValid
    // tokens after the context gate are a flat '+'-joined predicate set that may
    // lead with a type (Creature, Instant…) OR a bare property (Legendary,
    // ChosenType, MultiColor…). Pick a real type if one is present, else "Card",
    // and move the remaining tokens to qualifiers so they're matched correctly.
    auto isTypeTok = [](std::string_view t) {
        return t == "Creature" || t == "Artifact" || t == "Enchantment" ||
               t == "Land"     || t == "Planeswalker" || t == "Permanent" ||
               t == "Instant"  || t == "Sorcery" || t == "Battle" ||
               t == "Card"     || t == "Any";
    };

    size_t pos = 0;
    while (pos <= spec.size()) {
        size_t comma = spec.find(',', pos);
        std::string_view sub = spec.substr(pos, comma == std::string_view::npos
                                                ? std::string_view::npos : comma - pos);
        pos = (comma == std::string_view::npos) ? spec.size() + 1 : comma + 1;
        if (sub.empty()) continue;

        size_t dot = sub.find('.');
        std::string_view lead = sub.substr(0, dot == std::string_view::npos ? sub.size() : dot);
        std::string_view rest = (dot == std::string_view::npos)
                                ? std::string_view{} : sub.substr(dot + 1);

        // Context gate. If the lead token isn't a gate, the whole sub is the filter.
        if      (lead == "Spell")     { if (!isSpell)     continue; }
        else if (lead == "Activated") { if (!isActivated) continue; }
        else if (lead == "Triggered" || lead == "Static") { continue; }  // not a mana spend
        else { rest = sub; }   // un-gated: filter is the entire sub

        // Build "Type" or "Type.q1+q2" from the '+'-joined tokens in `rest`.
        std::string typeTok = "Card";
        std::string quals;
        bool tookType = false;
        size_t tp = 0;
        while (tp <= rest.size()) {
            size_t plus = rest.find('+', tp);
            std::string_view tok = rest.substr(tp, plus == std::string_view::npos
                                                   ? std::string_view::npos : plus - tp);
            tp = (plus == std::string_view::npos) ? rest.size() + 1 : plus + 1;
            if (tok.empty()) continue;
            if (!tookType && isTypeTok(tok)) { typeTok = std::string(tok); tookType = true; }
            else { if (!quals.empty()) quals += '+'; quals += std::string(tok); }
        }
        std::string filter = quals.empty() ? typeTok : (typeTok + '.' + quals);

        if (cardMatchesFilter(payee, filter, activeController, kInvalidId, producer, game))
            return true;
    }
    return false;
}

} // namespace mtg
