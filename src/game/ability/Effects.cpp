#include "Effects.h"
#include "../GameState.h"
#include "../CardStats.h"
#include "../KeywordAbility.h"
#include "../CardFilter.h"
#include "../EquipSystem.h"
#include "../TriggerSystem.h"
#include "ScriptLine.h"
#include <algorithm>
#include <cctype>
#include <charconv>
#include <charconv>
#include <climits>
#include <string_view>
#include <vector>

namespace {
// Parse Forge chapter designators ("I", "II", "III", "1", "2" …) to int
static int parseChapterNum(std::string_view s) noexcept {
    if (s == "I"   || s == "i")   return 1;
    if (s == "II"  || s == "ii")  return 2;
    if (s == "III" || s == "iii") return 3;
    if (s == "IV"  || s == "iv")  return 4;
    if (s == "V"   || s == "v")   return 5;
    int v = 0; std::from_chars(s.data(), s.data() + s.size(), v); return v;
}
} // anon

namespace mtg {

namespace {

int toInt(std::string_view sv, int defaultVal = 0) {
    if (sv.empty()) return defaultVal;
    int v = defaultVal;
    std::from_chars(sv.data(), sv.data() + sv.size(), v);
    return v;
}

void addProduced(std::string_view produced, int amount, ManaPool& pool) {
    // Colourless {C} is its OWN mana type — it pays generic costs but NOT
    // coloured pips. Store it as a colourless atom, never as "generic".
    if (produced == "C") {
        pool.add(ManaCostShard::COLORLESS, amount);
        return;
    }
    // "Any colour" → a flexible atom carrying all five colour bits, so it can
    // pay any single coloured pip (and counts toward generic). NOT generic mana.
    if (produced == "Any" || produced == "AnyColor") {
        pool.add(ManaCostShard{ManaAtom::COLORS_MASK, "Any"}, amount);
        return;
    }
    // "Combo W U G" — flexible atom over just the listed colours (the human
    // normally gets a choice in effectMana; this is the direct-call fallback).
    if (produced.size() > 6 && produced.substr(0, 6) == "Combo ") {
        uint32_t mask = 0;
        for (char c : produced.substr(6)) {
            if      (c == 'W') mask |= ManaAtom::WHITE;
            else if (c == 'U') mask |= ManaAtom::BLUE;
            else if (c == 'B') mask |= ManaAtom::BLACK;
            else if (c == 'R') mask |= ManaAtom::RED;
            else if (c == 'G') mask |= ManaAtom::GREEN;
        }
        if (mask == 0) mask = ManaAtom::COLORS_MASK;
        pool.add(ManaCostShard{mask, "Combo"}, amount);
        return;
    }
    for (char c : produced) {
        switch (c) {
            case 'W': pool.add(ManaCostShard::WHITE, amount); break;
            case 'U': pool.add(ManaCostShard::BLUE,  amount); break;
            case 'B': pool.add(ManaCostShard::BLACK, amount); break;
            case 'R': pool.add(ManaCostShard::RED,   amount); break;
            case 'G': pool.add(ManaCostShard::GREEN, amount); break;
            default:  break; // ignore spaces and other formatting chars
        }
    }
}

// Check R:Event$ DamageDone replacement effects that might prevent/redirect damage.
// Returns the modified damage amount (0 = fully prevented).
static int applyDamageReplacements(int amount, const Card* sourceCard,
                                    const Card* targetCard, const GameState& game) {
    if (amount <= 0) return 0;
    // Check target card's own R: lines for damage prevention
    const auto checkLines = [&](const std::vector<std::string>& rlines,
                                  bool /*isSelf*/) -> int {
        for (const auto& rl : rlines) {
            auto s = parseScriptLine(rl);
            if (s.effectType != "DamageDone") continue;
            // ValidSource$ filter
            auto vs = s.get("ValidSource", "");
            if (!vs.empty() && sourceCard &&
                !cardMatchesAnyFilter(*sourceCard, vs, sourceCard->controllerId,
                                      kInvalidId, nullptr, &game)) continue;
            // ValidTarget$ filter
            auto vt = s.get("ValidTarget", "");
            if (!vt.empty() && targetCard &&
                !cardMatchesAnyFilter(*targetCard, vt, targetCard->controllerId,
                                      kInvalidId, nullptr, &game)) continue;
            auto rw = s.get("ReplaceWith", "");
            // "PreventDamage" = prevent all damage
            if (rw == "PreventDamage" || rw == "Prevent") return 0;
            // "PreventDamage<N>" = prevent up to N
            if (rw.size() > 14 && rw.substr(0, 14) == "PreventDamage<") {
                int n = 0;
                std::from_chars(rw.data() + 14, rw.data() + rw.size() - 1, n);
                return std::max(0, amount - n);
            }
        }
        return amount;
    };
    // Target card's own replacement lines
    if (targetCard) amount = checkLines(targetCard->rules->replacementLines, true);
    // Global watchers on battlefield
    if (amount > 0) {
        for (const Card* watcher : game.battlefield().cards()) {
            if (targetCard && watcher->id == targetCard->id) continue;
            amount = checkLines(watcher->rules->replacementLines, false);
            if (amount <= 0) break;
        }
    }
    return amount;
}

// Deal damage from a source to one target (handles deathtouch / lifelink / shields).
// Also fires DamageDone triggers so "when ~ deals damage" abilities work.
void dealDamageTo(int amount, const Target& t, EffectContext& ctx) {
    bool hasDeathtouch = ctx.source && ctx.source->hasKeyword(KeywordAbility::Deathtouch);
    bool hasLifelink   = ctx.source && ctx.source->hasKeyword(KeywordAbility::Lifelink);
    bool hasInfect     = ctx.source && ctx.source->hasKeyword(KeywordAbility::Infect);
    bool hasWither     = ctx.source && ctx.source->hasKeyword(KeywordAbility::Wither);
    uint8_t sourceColor = ctx.source ? ctx.source->rules->manaCost.colorIdentity() : 0;

    // Apply R:Event$ DamageDone replacement effects before dealing damage
    {
        const Card* tgtCard = t.isCard() ? ctx.game.findCard(t.cardId) : nullptr;
        amount = applyDamageReplacements(amount, ctx.source, tgtCard, ctx.game);
        if (amount <= 0) return;
    }

    if (t.isPlayer()) {
        // Per-player damage prevention shield
        Player& p = ctx.game.player(t.playerId);
        if (p.damageShield() > 0) {
            int prevented = std::min(amount, p.damageShield());
            p.addDamageShield(-prevented);
            amount -= prevented;
        }
        if (amount <= 0) return;
        if (hasInfect) {
            p.addPoison(amount);
        } else {
            ctx.game.loseLife(t.playerId, amount);
            ctx.game.playerDamagedThisTurn[t.playerId] = true;
            { std::vector<PendingTrigger> trigs; TriggerSystem::onLoseLife(t.playerId, amount, ctx.game, trigs); ctx.game.queueTriggers(std::move(trigs)); }
        }
        if (hasLifelink)
            ctx.game.gainLife(ctx.controller, amount);
    } else if (t.isCard()) {
        Card* c = ctx.game.findCard(t.cardId);
        if (!c || !c->isOnBattlefield()) return;
        // Protection prevents damage
        if (hasProtectionFrom(c->keywordMask, sourceColor)) return;
        // Per-card damage prevention shield
        if (c->damageShield > 0) {
            int prevented = std::min(amount, c->damageShield);
            c->damageShield -= prevented;
            amount -= prevented;
        }
        if (amount <= 0) return;
        if (hasInfect || hasWither) {
            c->addCounter("-1/-1", amount);
        } else {
            c->markedDamage += amount;
            if (hasDeathtouch && amount > 0) c->deathtouchDamage = true;
        }
        if (hasLifelink) ctx.game.gainLife(ctx.controller, amount);
    }

    // Fire DamageDone triggers (spell / ability damage — not combat)
    if (ctx.source) {
        std::vector<PendingTrigger> triggered;
        TriggerSystem::onDamageDone(
            *ctx.source, amount, /*isCombat=*/false,
            t.isPlayer(),
            t.isPlayer() ? kInvalidId : t.cardId,
            t.isPlayer() ? t.playerId : static_cast<uint8_t>(0),
            ctx.game, triggered);
        if (!triggered.empty())
            ctx.game.queueTriggers(std::move(triggered));
    }
}

// Resolve Defined$/ValidPlayers$ to a player id.
// triggerPlayer (255 = none) is used for "TriggeredTarget" when the trigger target was a player.
static uint8_t resolveDefinedPlayer(std::string_view def, uint8_t controller,
                                     uint8_t triggerPlayer = 255,
                                     uint8_t chosenPlayer  = 255) noexcept {
    if (def == "TriggeredTarget" && triggerPlayer != 255) return triggerPlayer;
    if (def == "Opponent" || def == "OppCtrl" || def == "Each") return controller ^ 1;
    if (def == "ChosenPlayer" && chosenPlayer != 255) return chosenPlayer;
    return controller;
}


// Resolve Defined$ to a Card* (for single-card effects).
// Returns nullptr if the Defined$ points to a player or isn't resolvable.
static Card* resolveDefinedCard(const ScriptLine& s, EffectContext& ctx) {
    auto def = s.get("Defined", "");
    if (def == "Self"  || def == "This")
        return ctx.source;
    if (def == "TriggeredCard" || def == "TriggeredCardLKI" ||
        def == "TriggeredAttacker" || def == "TriggeredBlocker" ||
        def == "TriggeredTarget") { // BecomesTarget: the card that was targeted
        if (ctx.triggeredCardId != kInvalidId)
            return ctx.game.findCard(ctx.triggeredCardId);
    }
    if (def == "Equipped" && ctx.source) {
        if (ctx.source->attachedTo != kInvalidId)
            return ctx.game.findCard(ctx.source->attachedTo);
    }
    if (def == "Enchanted" && ctx.source) {
        if (ctx.source->attachedTo != kInvalidId)
            return ctx.game.findCard(ctx.source->attachedTo);
    }
    // ExiledWith — the card exiled by this source (Hideaway lands, Oblivion
    // Ring-style "play the exiled card", etc.).
    if (def == "ExiledWith" && ctx.source) {
        for (Card* ec : ctx.game.exile().cards())
            if (ec && ec->exiledBy == ctx.source->id) return ec;
    }
    return nullptr;
}

} // namespace

// ── executeEffectChain ────────────────────────────────────────────────────────

void executeEffectChain(const ScriptLine& script, EffectContext& ctx) {
    // Recursion guard: effectCopySpell re-enters this function to execute the
    // copied spell's effect. Ral+Galvanic Iteration combos can chain many copies,
    // causing stack overflow without this limit.
    thread_local int chainDepth = 0;
    if (chainDepth >= 10) return;
    ++chainDepth;
    struct ChainGuard { ~ChainGuard() { --chainDepth; } } cg;

    // Cache the source ID before calling executeEffect. The ctx.source pointer may
    // become a dangling pointer if an effect moves the source card (zone change
    // destroys the old Card object). After each effect, use findCard(srcId) for
    // safe re-lookup instead of dereferencing the potentially-stale ctx.source.
    // Note: if ctx.source was already null (e.g. trigger from a dead permanent),
    // srcId = kInvalidId and freshRules() returns nullptr immediately.
    const ObjectId srcId = ctx.source ? ctx.source->id : kInvalidId;

    executeEffect(script, ctx);

    // Safe rule lookup by stable ObjectId — never dereferences ctx.source directly.
    auto freshRules = [&]() -> const CardRules* {
        if (srcId == kInvalidId) return nullptr;
        const Card* c = ctx.game.findCard(srcId);
        return (c && c->rules) ? c->rules : nullptr;
    };

    const CardRules* rules = freshRules();

    // IfKickedSVar$ — bonus sub-chain that fires only when the spell was kicked
    auto ikName = std::string(script.get("IfKickedSVar", ""));
    if (!ikName.empty() && ctx.kicked && rules) {
        auto it = rules->svars.find(ikName);
        if (it != rules->svars.end()) {
            auto kickedScript = parseScriptLine(it->second);
            if (!kickedScript.empty())
                executeEffectChain(kickedScript, ctx); // recurse: handles SubAbility$ etc.
        }
    }

    auto subName = std::string(script.get("SubAbility", ""));
    while (!subName.empty()) {
        rules = freshRules();
        if (!rules) break;
        auto it = rules->svars.find(subName);
        if (it == rules->svars.end()) break;
        auto sub = parseScriptLine(it->second);
        if (sub.empty()) break;
        executeEffect(sub, ctx);
        subName = std::string(sub.get("SubAbility", ""));
    }
}

// ── Dispatch ──────────────────────────────────────────────────────────────────

bool executeEffect(const ScriptLine& script, EffectContext& ctx) {
    // General pre-condition: ConditionPresent$ <filter> + ConditionCompare$ <op><N>
    // The effect is skipped if the condition is not met.
    auto condPresent = script.get("ConditionPresent", "");
    if (!condPresent.empty()) {
        auto condCompare = script.get("ConditionCompare", "GE1");
        int n = 0;
        ObjectId selfId = ctx.source ? ctx.source->id : kInvalidId;
        for (const Card* c : ctx.game.battlefield().cards())
            if (cardMatchesAnyFilter(*c, condPresent, ctx.controller, selfId, ctx.source, &ctx.game)) n++;
        std::string_view op = std::string_view(condCompare).substr(0, 2);
        int thresh = 0;
        std::from_chars(condCompare.data() + 2, condCompare.data() + condCompare.size(), thresh);
        bool pass = (op == "GE") ? (n >= thresh) :
                    (op == "GT") ? (n >  thresh) :
                    (op == "LE") ? (n <= thresh) :
                    (op == "LT") ? (n <  thresh) :
                    (op == "EQ") ? (n == thresh) : (n != thresh);
        auto condInverted = script.get("ConditionInverted", "");
        if (condInverted == "True") pass = !pass;
        if (!pass) return true;
    }

    // ConditionCheckSVar$ <name> | ConditionSVarCompare$ <op><N|SVar> — skip if condition fails.
    // Compares the evaluated SVar against a number or another SVar.
    auto condCheckSvar = script.get("ConditionCheckSVar", "");
    if (!condCheckSvar.empty()) {
        auto cmp = script.get("ConditionSVarCompare", "EQ1");
        const CardRules* rules = ctx.source ? ctx.source->rules : nullptr;
        ObjectId selfId = ctx.source ? ctx.source->id : kInvalidId;
        int lhs = ctx.game.evaluateSVar(std::string(condCheckSvar), ctx.controller,
                                         rules, selfId);
        if (cmp.size() >= 3) {
            auto op     = cmp.substr(0, 2);
            auto rhsStr = cmp.substr(2);
            int rhs = 0;
            // If rhs starts with a digit it's a literal; otherwise it's another SVar name
            if (!rhsStr.empty() && std::isdigit(static_cast<unsigned char>(rhsStr[0])))
                std::from_chars(rhsStr.data(), rhsStr.data() + rhsStr.size(), rhs);
            else
                rhs = ctx.game.evaluateSVar(std::string(rhsStr), ctx.controller, rules, selfId);
            bool pass = (op == "GE") ? (lhs >= rhs) :
                        (op == "GT") ? (lhs >  rhs) :
                        (op == "LE") ? (lhs <= rhs) :
                        (op == "LT") ? (lhs <  rhs) :
                        (op == "EQ") ? (lhs == rhs) : (lhs != rhs);
            if (!pass) return true; // condition not met — skip effect
        }
    }

    // ConditionNotPresent$ <filter> — skip this effect if ANY matching card IS found.
    // Used by Uro/Kroxa: "Sacrifice unless it escaped" = skip sacrifice when self+escaped present.
    auto condNotPresent = script.get("ConditionNotPresent", "");
    if (!condNotPresent.empty()) {
        ObjectId selfId = ctx.source ? ctx.source->id : kInvalidId;
        bool found = false;
        for (const Card* c : ctx.game.battlefield().cards()) {
            if (cardMatchesAnyFilter(*c, condNotPresent, ctx.controller, selfId, ctx.source, &ctx.game)) {
                found = true; break;
            }
        }
        if (found) return true; // condition not met — skip effect
    }

    const auto& type = script.effectType;
    if      (type == "DealDamage"    || type == "Damage")
                                        { effectDealDamage(script, ctx);    return true; }
    else if (type == "Mana")            { effectMana(script, ctx);           return true; }
    else if (type == "Draw")            { effectDraw(script, ctx);           return true; }
    else if (type == "GainLife")        { effectGainLife(script, ctx);       return true; }
    else if (type == "LoseLife") {
        int n        = script.getIntOrX("LifeAmount", ctx.xValue, 1);
        auto defined = script.get("Defined", "You");
        auto pid     = resolveDefinedPlayer(defined, ctx.controller, ctx.triggerPlayer);
        if (defined == "Each" || defined == "All") {
            ctx.game.loseLife(0, n);
            ctx.game.loseLife(1, n);
            { std::vector<PendingTrigger> t; TriggerSystem::onLoseLife(0, n, ctx.game, t); ctx.game.queueTriggers(std::move(t)); }
            { std::vector<PendingTrigger> t; TriggerSystem::onLoseLife(1, n, ctx.game, t); ctx.game.queueTriggers(std::move(t)); }
        } else {
            ctx.game.loseLife(pid, n);
            { std::vector<PendingTrigger> t; TriggerSystem::onLoseLife(pid, n, ctx.game, t); ctx.game.queueTriggers(std::move(t)); }
        }
        return true;
    }
    else if (type == "Destroy")         { effectDestroy(script, ctx);        return true; }
    else if (type == "Counter")         { effectCounter(script, ctx);        return true; }
    else if (type == "Pump")            { effectPump(script, ctx);           return true; }
    else if (type == "ChangeZone")      { effectChangeZone(script, ctx);     return true; }
    else if (type == "Token")           { effectToken(script, ctx);          return true; }
    else if (type == "PutCounter")      { effectPutCounter(script, ctx);     return true; }
    else if (type == "PumpAll")         { effectPumpAll(script, ctx);        return true; }
    else if (type == "ChangeZoneAll")   { effectChangeZoneAll(script, ctx);  return true; }
    else if (type == "DamageAll")       { effectDamageAll(script, ctx);      return true; }
    else if (type == "Discard"
          || type == "DiscardCard")     { effectDiscard(script, ctx);        return true; }
    else if (type == "Tap"
          || type == "Untap")           { effectTap(script, ctx);            return true; }
    else if (type == "Charm"
          || type == "GenericChoice")   { effectCharm(script, ctx);          return true; }
    else if (type == "Dig")             { effectDig(script, ctx);            return true; }
    else if (type == "Scry")            { effectScry(script, ctx);           return true; }
    else if (type == "Regenerate")      { effectRegenerate(script, ctx);     return true; }
    else if (type == "Attach")          { effectAttach(script, ctx);         return true; }
    else if (type == "Effect")          { effectEffect(script, ctx);         return true; }
    else if (type == "Mill")            { effectMill(script, ctx);           return true; }
    else if (type == "GainAbility"
          || type == "AddAbility")      { effectGainAbility(script, ctx);    return true; }
    else if (type == "GainControl")     { effectGainControl(script, ctx);    return true; }
    else if (type == "Shuffle")         { effectShuffle(script, ctx);          return true; }
    else if (type == "PreventDamage"
          || type == "Fog")             { effectPreventDamage(script, ctx);    return true; }
    else if (type == "Fight")           { effectFight(script, ctx);            return true; }
    else if (type == "SetState"
          || type == "TapAll"
          || type == "UntapAll")        { effectSetState(script, ctx);         return true; }
    else if (type == "RemoveCounter")   { effectRemoveCounter(script, ctx);    return true; }
    else if (type == "Explore")         { effectExplore(script, ctx);          return true; }
    else if (type == "Surveil")         { effectSurveil(script, ctx);          return true; }
    else if (type == "Proliferate")     { effectProliferate(script, ctx);      return true; }
    else if (type == "PutCounterAll")          { effectPutCounterAll(script, ctx);           return true; }
    else if (type == "RearrangeTopOfLibrary") { effectRearrangeTopOfLibrary(script, ctx);    return true; }
    else if (type == "Branch")               { effectBranch(script, ctx);                  return true; }
    else if (type == "Animate")              { effectAnimate(script, ctx);                  return true; }
    else if (type == "Connive")              { effectConnive(script, ctx);                  return true; }
    // ── Phase 14+ effects ────────────────────────────────────────────────────
    else if (type == "Investigate")          { effectInvestigate(script, ctx);              return true; }
    else if (type == "CopyPermanent"
          || type == "Populate")             { effectCopyPermanent(script, ctx);            return true; }
    else if (type == "Play"
          || type == "CastFromExile")        { effectPlay(script, ctx);                     return true; }
    else if (type == "DigUntil"
          || type == "DigMultiple")          { effectDigUntil(script, ctx);                 return true; }
    else if (type == "Reveal"
          || type == "RevealHand")           { effectReveal(script, ctx);                   return true; }
    else if (type == "PeekAndReveal")        { effectPeekAndReveal(script, ctx);            return true; }
    else if (type == "LookAt")               { effectLookAt(script, ctx);                   return true; }
    else if (type == "MakeCard")             { effectMakeCard(script, ctx);                 return true; }
    else if (type == "FlipCoin")             { effectFlipCoin(script, ctx);                 return true; }
    else if (type == "RepeatEach")           { effectRepeatEach(script, ctx);               return true; }
    else if (type == "StoreSVar")            { effectStoreSVar(script, ctx);                return true; }
    else if (type == "Goad")                 { effectGoad(script, ctx);                     return true; }
    else if (type == "RemoveFromCombat")     { effectRemoveFromCombat(script, ctx);         return true; }
    else if (type == "AddTurn")              { effectAddTurn(script, ctx);                  return true; }
    else if (type == "SkipTurn"
          || type == "SkipPhase")            { effectSkipTurn(script, ctx);                 return true; }
    else if (type == "LifeSet"
          || type == "LifeSetEffect")        { effectLifeSet(script, ctx);                  return true; }
    else if (type == "ControlExchange")      { effectControlExchange(script, ctx);          return true; }
    else if (type == "ZoneExchange")         { effectZoneExchange(script, ctx);             return true; }
    else if (type == "Amass")               { effectAmass(script, ctx);                    return true; }
    else if (type == "Discover")             { effectDiscover(script, ctx);                 return true; }
    else if (type == "Manifest"
          || type == "ManifestDread"
          || type == "Cloak")                { effectManifest(script, ctx);                 return true; }
    else if (type == "Incubate")             { effectIncubate(script, ctx);                 return true; }
    else if (type == "Learn")               { effectLearn(script, ctx);                    return true; }
    else if (type == "MultiplyCounter"
          || type == "CountersMultiply")     { effectMultiplyCounter(script, ctx);          return true; }
    else if (type == "MoveCounter"
          || type == "CountersMove")         { effectMoveCounter(script, ctx);              return true; }
    else if (type == "Unattach")             { effectUnattach(script, ctx);                 return true; }
    else if (type == "Protect"
          || type == "ProtectAll")           { effectProtect(script, ctx);                  return true; }
    else if (type == "AnimateAll")           { effectAnimateAll(script, ctx);               return true; }
    else if (type == "Balance")              { effectBalance(script, ctx);                  return true; }
    else if (type == "Phase"
          || type == "Phases"
          || type == "PhaseOut")             { effectPhasing(script, ctx);                  return true; }
    else if (type == "Vote")                 { effectVote(script, ctx);                     return true; }
    else if (type == "ChooseColor")          { effectChooseColor(script, ctx);              return true; }
    else if (type == "ChooseCardName"
          || type == "ChooseName")           { effectChooseName(script, ctx);               return true; }
    else if (type == "NameCard")             { effectNameCard(script, ctx);                 return true; }
    else if (type == "TwoPiles")             { effectTwoPiles(script, ctx);                 return true; }
    else if (type == "LifeExchange")         { effectLifeExchange(script, ctx);             return true; }
    else if (type == "Detain")               { effectDetain(script, ctx);                   return true; }
    else if (type == "ImmediateTrigger")     { effectImmediateTrigger(script, ctx);         return true; }
    else if (type == "DelayedTrigger")       { effectDelayedTrigger(script, ctx);           return true; }
    else if (type == "SacrificeAll")         { effectSacrificeAll(script, ctx);             return true; }
    else if (type == "DestroyAll")           { effectDestroyAll(script, ctx);               return true; }
    else if (type == "CopySpell"
          || type == "CopySpellAbility")     { effectCopySpell(script, ctx);                return true; }
    else if (type == "AddPhase")             { effectAddPhase(script, ctx);                 return true; }
    else if (type == "MustBlock")            { effectMustBlock(script, ctx);                return true; }
    else if (type == "Seek")                 { effectSeek(script, ctx);                     return true; }
    else if (type == "Clone")                { effectClone(script, ctx);                    return true; }
    else if (type == "Sacrifice")            { effectSacrifice(script, ctx);                return true; }
    else if (type == "Bolster")              { effectBolster(script, ctx);                  return true; }
    else if (type == "Monstrosity")          { effectMonstrosity(script, ctx);              return true; }
    else if (type == "RegenerateAll")        { effectRegenerateAll(script, ctx);            return true; }
    else if (type == "Exert")               { effectExert(script, ctx);                    return true; }
    else if (type == "Transform")           { effectTransform(script, ctx);                return true; }
    else if (type == "CleanUp" || type == "Cleanup") { effectCleanup(script, ctx);          return true; }
    else if (type == "ChooseNumber") {
        // AI heuristic: choose the maximum available number.
        auto maxStr = script.get("Max", "");
        int maxVal = 0;
        const CardRules* rules = ctx.source ? ctx.source->rules : nullptr;
        ObjectId selfId = ctx.source ? ctx.source->id : kInvalidId;
        if (!maxStr.empty()) {
            bool isNum = !maxStr.empty() && std::isdigit(static_cast<unsigned char>(maxStr[0]));
            if (isNum) std::from_chars(maxStr.data(), maxStr.data() + maxStr.size(), maxVal);
            else if (maxStr == "Max") maxVal = INT_MAX; // ChooseAnyNumber
            else maxVal = ctx.game.evaluateSVar(std::string(maxStr), ctx.controller, rules, selfId);
        }
        int minVal = script.getInt("Min", 0);
        ctx.game.chosenNumberHint = std::max(minVal, std::min(maxVal, maxVal));
        return true;
    }
    else if (type == "ChoosePlayer")        { effectChoosePlayer(script, ctx);             return true; }
    else if (type == "ChooseType")          { effectChooseType(script, ctx);               return true; }
    else if (type == "RollDice")            { effectRollDice(script, ctx);                 return true; }
    else if (type == "Repeat")              { effectRepeat(script, ctx);                   return true; }
    // ── Ignored / always-safe no-ops ────────────────────────────────────────
    else if (type == "ChooseCard")            { effectChooseCard(script, ctx);               return true; }
    else if (type == "SetColor"
          || type == "ChangeColor")           { effectSetColor(script, ctx);                 return true; }
    else if (type == "BecomeMonarch")         { effectBecomeMonarch(script, ctx);            return true; }
    else if (type == "Foretell")              { effectForetell(script, ctx);                 return true; }
    else if (type == "TakeInitiative")           { effectTakeInitiative(script, ctx);           return true; }
    else if (type == "Venture")                  { effectVenture(script, ctx);                  return true; }
    else if (type == "Meld")                     { effectMeld(script, ctx);                     return true; }
    else if (type == "Mutate")                   { effectMutate(script, ctx);                   return true; }
    else if (type == "ClassLevelUp")             { effectClassLevelUp(script, ctx);             return true; }
    else if (type == "DayTime")                  { effectDayTime(script, ctx);                  return true; }
    else if (type == "UnlockDoor") {
        // Room mechanic: activate a Door (A or B) and fire its effect.
        // The door index is stored in the "Door" param: "A" or "B".
        // The effect chain after UnlockDoor fires the door's static bonus.
        if (ctx.source) {
            auto door = script.get("Door", "A");
            if (door == "A") ctx.source->doorAUnlocked = true;
            else             ctx.source->doorBUnlocked = true;
            ctx.game.recomputeStaticBonuses();  // room bonus may now apply
        }
        return true;
    }
    else if (type == "GainExperience"
          || type == "Experience") {
        // Experience counters: add N to the controller's persistent experience total.
        int n   = script.getIntOrX("Num", ctx.xValue, 1);
        auto def = script.get("Defined", "You");
        uint8_t pid = resolveDefinedPlayer(def, ctx.controller, ctx.triggerPlayer);
        ctx.game.addExperience(pid, n);
        return true;
    }
    else if (type == "ChooseGeneric"
          || type == "ChooseDirection"
          || type == "ChooseEvenOdd"
          || type == "ChooseSource"
          || type == "AssignGroup"
          || type == "RingTemptsYou"
          || type == "VillainousChoice"
          || type == "RollPlanarDice"
          // Meld handled below
          //|| type == "Meld"
          || type == "Encode"
          || type == "Haunt"
          || type == "Subgame"
          || type == "GameDraw"
          || type == "RestartGame"
          || type == "ReverseTurnOrder"
          || type == "Ascend"
          || type == "Endure"
          || type == "InternalRadiation"
          || type == "Radiation"
          || type == "AdvanceCrank"
          || type == "AssembleContraption"
          || type == "HeistEffect"
          || type == "OpenAttraction"
          || type == "TimeTravelEffect"
          || type == "Airbend"
          || type == "Earthbend"
          || type == "BondEffect"
          || type == "BlightEffect"
          || type == "PlanewalkEffect"
          || type == "ChaosEnsues"
          || type == "RunChaos"
          || type == "ChangeSector"
          || type == "ChangeSpeed"
          || type == "SetInMotion"
          || type == "ActivateAbility"
          || type == "EndTurn"
          || type == "EndCombatPhase"
          || type == "BlankLine"
          || type == "Abandon"
          // Emblem handled below — no longer a stub
          //|| type == "Emblem"
          || type == "ForetellCost"
          || type == "DamageResolve"
          || type == "ReplaceEffect")          { return true; /* intentional no-op */ }
    else if (type == "Emblem") {
        // Create a persistent emblem for the controller in the command zone.
        // The emblem's effects come from SVars referenced in the script.
        GameState::Emblem emblem;
        emblem.controllerId = ctx.controller;
        emblem.sourceName   = ctx.source ? ctx.source->rules->name : "Unknown";
        // Collect T:/S: lines from the source card's SVars that define the emblem's effect
        if (ctx.source && ctx.source->rules) {
            auto svIt = ctx.source->rules->svars.find("EmblemAbility");
            if (svIt == ctx.source->rules->svars.end())
                svIt = ctx.source->rules->svars.find("Emblem");
            if (svIt != ctx.source->rules->svars.end()) {
                // Store the SVar body as a trigger line for TriggerSystem to evaluate
                emblem.triggerLines.push_back(svIt->second);
            }
        }
        ctx.game.addEmblem(std::move(emblem));
        return true;
    }
    else if (type == "LosesGame") {
        auto defined = script.get("Defined", "You");
        auto pid = resolveDefinedPlayer(defined, ctx.controller, ctx.triggerPlayer);
        ctx.game.player(pid).lose();
        return true;
    }
    else if (type == "RevealHand") {
        // In AI mode: no UI action needed; the game state is already known
        return true;
    }
    else if (type == "AlterAttribute") {
        effectAlterAttribute(script, ctx);
        return true;
    }
    else if (type == "WinsGame") {
        auto defined = script.get("Defined", "You");
        uint8_t winner = (defined == "You") ? ctx.controller : (ctx.controller ^ 1);
        ctx.game.player(winner ^ 1).lose();
        return true;
    }
    else if (type == "Poison") {
        int n = script.getInt("Num", 1);
        for (const auto& t : ctx.targets)
            if (t.isPlayer()) ctx.game.player(t.playerId).addPoison(n);
        if (ctx.targets.empty())
            ctx.game.player(ctx.controller ^ 1).addPoison(n);
        return true;
    }
    return false;
}

// ── Effect implementations ────────────────────────────────────────────────────

void effectDealDamage(const ScriptLine& s, EffectContext& ctx) {
    int amount = s.getIntOrX("NumDmg", ctx.xValue, 0);
    // Kicker may override the damage amount
    if (ctx.kicked) {
        int kickerAmt = s.getIntOrX("KickerNumDmg", ctx.xValue, 0);
        if (kickerAmt > 0) amount = kickerAmt;
    }
    if (amount <= 0) return;

    if (!ctx.targets.empty()) {
        for (const auto& t : ctx.targets)
            dealDamageTo(amount, t, ctx);
    } else {
        auto defined   = s.get("Defined",   "");
        auto validTgts = s.get("ValidTgts", "");
        if (!defined.empty() && defined != "Targeted") {
            uint8_t opp = ctx.controller ^ 1;
            if (defined == "Player.Opponent" || defined == "Player.OppCtrl" ||
                defined == "Opponent"        || defined == "OppCtrl") {
                dealDamageTo(amount, Target::forPlayer(opp), ctx);
            } else if (defined == "Player" || defined == "You" || defined == "Controller") {
                dealDamageTo(amount, Target::forPlayer(ctx.controller), ctx);
            } else if (defined == "Each" || defined == "All" ||
                       defined == "EachPlayer" || defined == "Player.Each") {
                dealDamageTo(amount, Target::forPlayer(0), ctx);
                dealDamageTo(amount, Target::forPlayer(1), ctx);
            }
            // Other Defined$ (e.g. TriggeredCard) ignored for damage
        } else if (validTgts.find("Player") != std::string_view::npos ||
                   validTgts == "Any") {
            // No chosen target but targets every player (e.g. "each player")
            Target tp0 = Target::forPlayer(0);
            Target tp1 = Target::forPlayer(1);
            dealDamageTo(amount, tp0, ctx);
            dealDamageTo(amount, tp1, ctx);
        }
        // Otherwise nothing to do — targets should have been chosen before resolution
    }
}

void effectMana(const ScriptLine& s, EffectContext& ctx) {
    int amount   = s.getIntOrX("Amount", ctx.xValue, 1);
    auto produced = s.get("Produced", "C");
    ManaPool& pool = ctx.game.player(ctx.controller).manaPool();

    // "Any colour" is just a choice over all five colours — route it through the
    // same colour-choice path as Combo so the human is prompted (and the AI picks
    // a needed colour) instead of silently making generic mana.
    bool isAnyColor = (produced == "Any" || produced == "AnyColor");

    // "Combo X Y [Z]" — controller picks one colour from the listed letters.
    // Human → defer to UI overlay; AI → pick a colour that most helps an
    // unaffordable hand cost, falling back to the first listed colour.
    if (isAnyColor || (produced.size() > 6 && produced.substr(0, 6) == "Combo ")) {
        std::string tail = isAnyColor ? std::string("WUBRG")
                                      : std::string(produced.substr(6));
        std::string colors;

        // "Combo ColorIdentity" (Command Tower, Path of Ancestry…) expands to
        // the colours of the controller's commander(s). Default to WUBRG when
        // there's no commander on record (sealed games, dead commander, etc.).
        if (tail.find("ColorIdentity") != std::string::npos) {
            uint8_t mask = 0;
            for (const Card* cmd : ctx.game.command().cards())
                if (cmd && cmd->isCommander && cmd->ownerId == ctx.controller)
                    mask |= cmd->rules->manaCost.colorIdentity();
            // Also fold in any commander currently on the battlefield (post-cast).
            for (const Card* bf : ctx.game.battlefield().cards())
                if (bf && bf->isCommander && bf->ownerId == ctx.controller)
                    mask |= bf->rules->manaCost.colorIdentity();
            if (mask == 0) mask = ManaAtom::WHITE | ManaAtom::BLUE  |
                                  ManaAtom::BLACK | ManaAtom::RED  |
                                  ManaAtom::GREEN;
            if (mask & ManaAtom::WHITE) colors += 'W';
            if (mask & ManaAtom::BLUE)  colors += 'U';
            if (mask & ManaAtom::BLACK) colors += 'B';
            if (mask & ManaAtom::RED)   colors += 'R';
            if (mask & ManaAtom::GREEN) colors += 'G';
        } else {
            for (char c : tail)
                if (c == 'W' || c == 'U' || c == 'B' || c == 'R' || c == 'G')
                    colors += c;
        }
        if (colors.empty()) { pool.addGeneric(amount); return; }
        // Single-colour Combo is no choice at all — fast-path: produce it.
        if (colors.size() == 1) {
            addProduced(std::string(1, colors[0]), amount, pool);
            return;
        }

        if (ctx.controller == 0 && ctx.game.isHumanInteractive()) {
            ctx.game.setPendingManaChoice(0, colors, amount);
            return;
        }

        // AI: tally colour shards needed by hand spells and pick the most-needed
        // colour that this land can produce. If no hand spell needs a colour
        // we can offer, take the first listed colour.
        int demand[5] = {0,0,0,0,0};  // W U B R G
        const Player& p = ctx.game.player(ctx.controller);
        for (const Card* h : p.hand().cards()) {
            for (const auto& shard : h->rules->manaCost.shards()) {
                uint32_t a = shard.atoms;
                if (a & ManaAtom::WHITE) ++demand[0];
                if (a & ManaAtom::BLUE)  ++demand[1];
                if (a & ManaAtom::BLACK) ++demand[2];
                if (a & ManaAtom::RED)   ++demand[3];
                if (a & ManaAtom::GREEN) ++demand[4];
            }
        }
        auto idxOf = [](char c) -> int {
            switch (c) { case 'W': return 0; case 'U': return 1; case 'B': return 2;
                         case 'R': return 3; case 'G': return 4; }
            return -1;
        };
        char pick = colors[0];
        int best = -1;
        for (char c : colors) {
            int i = idxOf(c);
            if (i >= 0 && demand[i] > best) { best = demand[i]; pick = c; }
        }
        addProduced(std::string(1, pick), amount, pool);
        return;
    }

    addProduced(produced, amount, pool);
}

// Attempt to use Dredge instead of drawing one card. Returns true if Dredge was used.
// AI heuristic: always Dredge if possible (fills GY for synergies).
static bool tryDredgeInstead(uint8_t pid, EffectContext& ctx) {
    Player& p = ctx.game.player(pid);
    // Find the first Dredge card in GY
    Card* dredgeCard = nullptr;
    int   dredgeN    = 0;
    for (Card* c : p.graveyard().cards()) {
        if (c->rules->hasDredge && c->rules->dredgeAmount > 0) {
            dredgeCard = c;
            dredgeN    = c->rules->dredgeAmount;
            break;
        }
    }
    if (!dredgeCard) return false;
    // Need enough cards in library to mill
    if (static_cast<int>(p.library().size()) < dredgeN) return false;

    // Mill dredgeN cards
    ObjectId dredgeId = dredgeCard->id;
    for (int i = 0; i < dredgeN; ++i) {
        Card* top = p.library().front();
        if (!top) break;
        ctx.game.moveToZone(top->id, ZoneType::Graveyard, pid);
    }
    // Return the dredge card from GY to hand (id may have changed if GY was rebuilt
    // but Dredge cards just mill → the dredgeCard id itself stays valid)
    // Re-find it because mill may have triggered recomputeStaticBonuses
    if (Card* dc = ctx.game.findCard(dredgeId)) {
        ctx.game.moveToZone(dc->id, ZoneType::Hand, pid);
    }
    return true;
}

void effectDraw(const ScriptLine& s, EffectContext& ctx) {
    int numCards = s.getIntOrX("NumCards", ctx.xValue, 1);
    auto defined = s.get("Defined", "You");
    bool noDredge = (s.get("Dredge", "True") == "False"); // rarely disabled

    auto doDrawFor = [&](uint8_t pid) {
        Player& p = ctx.game.player(pid);
        for (int i = 0; i < numCards; ++i) {
            if (!noDredge && tryDredgeInstead(pid, ctx)) continue;
            if (p.library().empty()) { p.lose(); return; }
            Card* top = p.library().front();
            bool isFirstDraw = (ctx.game.cardsDrawnThisTurn[pid] == 0);
            Card* drawn = ctx.game.moveToZone(top->id, ZoneType::Hand, pid);
            ++ctx.game.cardsDrawnThisTurn[pid];
            if (isFirstDraw && drawn && drawn->rules->hasMiracle) {
                drawn->miracleEligible = true;
                // Queue a pending miracle choice so the player can cast at miracle cost
                if (ctx.game.isHumanInteractive() && pid == 0)
                    ctx.game.setPendingMiracle(drawn->id, pid);
            }
            // Fire Drawn triggers
            std::vector<PendingTrigger> drawnTrigs;
            TriggerSystem::onDraw(pid, ctx.game.cardsDrawnThisTurn[pid],
                                  drawn ? drawn->id : kInvalidId, ctx.game, drawnTrigs);
            ctx.game.queueTriggers(std::move(drawnTrigs));
        }
    };

    if (defined == "Each" || defined == "All") {
        doDrawFor(0); doDrawFor(1);
    } else {
        doDrawFor(resolveDefinedPlayer(defined, ctx.controller, ctx.triggerPlayer));
    }
}

void effectGainLife(const ScriptLine& s, EffectContext& ctx) {
    int amount   = s.getIntOrX("LifeAmount", ctx.xValue, 0);
    auto defined = s.get("Defined", "You");
    auto pid     = resolveDefinedPlayer(defined, ctx.controller, ctx.triggerPlayer);
    if (amount <= 0) return;
    ctx.game.gainLife(pid, amount);
    std::vector<PendingTrigger> trigs;
    TriggerSystem::onGainLife(pid, amount, ctx.game, trigs);
    ctx.game.queueTriggers(std::move(trigs));
}

void effectDestroy(const ScriptLine& s, EffectContext& ctx) {
    auto destroy = [&](Card* c) {
        if (!c || !c->isOnBattlefield()) return;
        if (c->hasKeyword(KeywordAbility::Indestructible)) return;
        // Shield counter: absorbs one destruction effect (rule 702.164)
        if (c->counterCount("shield") > 0) {
            c->removeCounter("shield", 1);
            return;
        }
        ctx.game.moveToZone(c->id, ZoneType::Graveyard, c->ownerId);
    };

    if (!ctx.targets.empty()) {
        for (const auto& t : ctx.targets) {
            if (!t.isCard()) continue;
            destroy(ctx.game.findCard(t.cardId));
        }
    } else if (Card* def = resolveDefinedCard(s, ctx)) {
        destroy(def);
    }
}

void effectCounter(const ScriptLine& s, EffectContext& ctx) {
    bool rememberCmc = (s.get("RememberCounteredCMC", "") == "True");
    for (const auto& t : ctx.targets) {
        if (!t.isCard()) continue;
        Card* c = ctx.game.findCard(t.cardId);
        if (!c || c->zone != ZoneType::Stack) continue;
        // "Can't be countered" replacement effect
        if (c->rules->cantBeCountered) continue;
        if (rememberCmc) {
            ctx.game.rememberedNumberHint = c->rules->cmc();
            ctx.remembered.push_back(c->id);
        }
        ctx.game.moveToZone(t.cardId, ZoneType::Graveyard, c->ownerId);
    }
}

void effectPump(const ScriptLine& s, EffectContext& ctx) {
    int  p   = s.getInt("NumAtt", 0);
    int  t   = s.getInt("NumDef", 0);
    auto dur = s.get("Duration", "EndOfTurn");

    // Build candidate list: explicit targets, then Defined$ card if no targets
    std::vector<Card*> pumpTargets;
    for (const auto& tgt : ctx.targets) {
        if (!tgt.isCard()) continue;
        Card* c = ctx.game.findCard(tgt.cardId);
        if (c && c->isOnBattlefield()) pumpTargets.push_back(c);
    }
    if (pumpTargets.empty()) {
        if (Card* def = resolveDefinedCard(s, ctx))
            if (def->isOnBattlefield()) pumpTargets.push_back(def);
    }

    for (Card* c : pumpTargets) {

        if (dur == "Permanent") {
            if (p > 0) c->addCounter("+1/+1", p);
            if (t > 0 && t != p) c->addCounter("+1/+1", t);
        } else {
            c->tempPower     += p;
            c->tempToughness += t;
        }

        auto kwStr = std::string(s.get("KW", ""));
        if (!kwStr.empty()) {
            std::string token;
            kwStr += '&';
            for (char ch : kwStr) {
                if (ch == '&') {
                    if (!token.empty()) {
                        auto kw = parseKeyword(token);
                        if (kw != KeywordAbility::None) {
                            if (dur == "Permanent") {
                                c->grantKeyword(kw);
                            } else {
                                c->tempKeywords |= static_cast<uint32_t>(kw);
                                c->keywordMask  |= static_cast<uint32_t>(kw);
                            }
                        }
                        token.clear();
                    }
                } else if (ch != ' ') {
                    token += ch;
                } else if (!token.empty()) {
                    token += ch;
                }
            }
        }
    }
}

void effectChangeZone(const ScriptLine& s, EffectContext& ctx) {
    auto origin      = s.get("Origin",      "Library");
    auto destination = s.get("Destination", "Hand");
    auto changeType  = s.get("ChangeType",  "");
    int  numCards    = s.getInt("ChangeNum", 1);

    ZoneType dest = ZoneType::Hand;
    if      (destination == "Battlefield") dest = ZoneType::Battlefield;
    else if (destination == "Graveyard")   dest = ZoneType::Graveyard;
    else if (destination == "Exile")       dest = ZoneType::Exile;
    else if (destination == "Library")     dest = ZoneType::Library;
    else if (destination == "Hand")        dest = ZoneType::Hand;

    bool rememberChanged  = (s.get("RememberChanged",  "") == "True");
    bool rememberTargets  = (s.get("RememberTargets",  "") == "True");
    bool hostLeavesPlay   = (s.get("Duration", "") == "UntilHostLeavesPlay");
    // Imprint$ True links the moved card to the source (exiledBy) so filters like
    // Card.IsImprinted+ExiledWithSource can find it later (Isochron Scepter).
    bool imprint          = (s.get("Imprint", "") == "True");

    // Defined$ — move a specific card (or all "remembered" cards) determined by context
    auto defined = s.get("Defined", "");
    if (!defined.empty() && defined != "Targeted") {
        // ExiledWith — return cards in exile that were banished by this source
        if (defined == "ExiledWith") {
            if (!ctx.source) return;
            std::vector<ObjectId> toReturn;
            for (const Card* ec : ctx.game.exile().cards())
                if (ec->exiledBy == ctx.source->id) toReturn.push_back(ec->id);
            for (ObjectId rid : toReturn) {
                Card* rc = ctx.game.findCard(rid);
                if (rc) ctx.game.moveToZone(rid, dest, rc->ownerId);
            }
            return;
        }
        if (defined == "Remembered" || defined == "RememberedLKI" ||
            defined == "DelayTriggerRememberedLKI") {
            // Move every card in the remembered list
            std::vector<ObjectId> toMove = ctx.remembered;
            ctx.remembered.clear();
            for (ObjectId remId : toMove) {
                Card* c = ctx.game.findCard(remId);
                if (!c) continue;
                uint8_t owner = (dest == ZoneType::Hand || dest == ZoneType::Library)
                                ? c->ownerId : ctx.controller;
                Card* moved = ctx.game.moveToZone(c->id, dest, owner);
                if (moved && rememberChanged)
                    ctx.remembered.push_back(moved->id);
            }
            return;
        }
        Card* defCard = resolveDefinedCard(s, ctx);
        if (!defCard) {
            // "TriggeredCard" stored in triggeredCardId
            if ((defined == "TriggeredCard" || defined == "TriggeredCardLKI") &&
                ctx.triggeredCardId != kInvalidId)
                defCard = ctx.game.findCard(ctx.triggeredCardId);
        }
        if (defCard) {
            uint8_t owner = (dest == ZoneType::Hand || dest == ZoneType::Library)
                            ? defCard->ownerId : ctx.controller;
            Card* moved = ctx.game.moveToZone(defCard->id, dest, owner);
            if (moved) {
                if (rememberChanged)
                    ctx.remembered.push_back(moved->id);
                if ((hostLeavesPlay || imprint) && dest == ZoneType::Exile && ctx.source)
                    moved->exiledBy = ctx.source->id;
            }
        }
        return;
    }

    if (!ctx.targets.empty()) {
        bool anyMoved = false;
        for (const auto& t : ctx.targets) {
            if (!t.isCard()) continue;
            Card* c = ctx.game.findCard(t.cardId);
            if (!c) continue; // stale id (card already moved by a prior effect)
            uint8_t destCtrl = (dest == ZoneType::Battlefield) ? ctx.controller
                                                                : c->ownerId;
            Card* moved = ctx.game.moveToZone(t.cardId, dest, destCtrl);
            if (moved) {
                if (rememberTargets)
                    ctx.remembered.push_back(moved->id);
                if ((hostLeavesPlay || imprint) && dest == ZoneType::Exile && ctx.source)
                    moved->exiledBy = ctx.source->id;
                // Aura entering BF directly (not via spell): attach to best own creature.
                if (dest == ZoneType::Battlefield &&
                    moved->rules->type.isEnchantment() &&
                    moved->rules->type.hasSubtype("Aura") &&
                    moved->attachedTo == kInvalidId) {
                    Card* best = nullptr;
                    int bestScore = -1;
                    for (Card* bf : ctx.game.battlefield().cards()) {
                        if (!bf->isCreature() || bf->id == moved->id) continue;
                        if (bf->controllerId != ctx.controller) continue;
                        int score = effectivePower(*bf) + effectiveToughness(*bf);
                        if (score > bestScore) { bestScore = score; best = bf; }
                    }
                    if (best) {
                        moved->attachedTo = best->id;
                        best->attachments.push_back(moved->id);
                        ctx.game.recomputeStaticBonuses();
                    }
                }
            }
            anyMoved = true;
        }
        // Only return if we actually moved something or if no search criteria is given.
        // If all targets are stale (e.g. they were exiled by a prior effect in the chain)
        // fall through to the search logic (e.g. Path to Exile's library search SubAbility).
        if (anyMoved || changeType.empty()) return;
    }

    // ── Library search (tutor) ──────────────────────────────────────────────
    if (!changeType.empty() && origin == "Library") {
        // Controller$ TargetController: search the targeted player's library
        auto controllerParam = s.get("Controller", "");
        uint8_t libPlayer = ctx.controller;
        if (controllerParam == "TargetController") {
            if (ctx.firstTargetController != 255)
                libPlayer = ctx.firstTargetController;          // snapshotted before exile
            else if (!ctx.targets.empty()) {
                const Card* tc = ctx.game.findCard(ctx.targets[0].cardId);
                if (tc) libPlayer = tc->controllerId;
            }
        } else if (controllerParam == "Opponent") {
            libPlayer = ctx.controller ^ 1;
        }

        uint8_t destCtrl = (dest == ZoneType::Battlefield) ? ctx.controller : libPlayer;

        // Human player (id 0) searches interactively in UI mode — defer to GameWindow
        if (libPlayer == 0 && ctx.game.isHumanInteractive()) {
            ctx.game.setPendingSearch(libPlayer, dest, destCtrl, std::string(changeType));
            // Shuffle happens after the human completes their selection (in GameWindow)
            return;
        }

        // AI / opponent — auto-pick first matching card
        Player& p = ctx.game.player(libPlayer);
        for (Card* c : p.library().cards()) {
            if (cardMatchesAnyFilter(*c, changeType, libPlayer, kInvalidId, ctx.source, &ctx.game)) {
                ctx.game.moveToZone(c->id, dest, destCtrl);
                break;
            }
        }
        // Always shuffle after searching
        p.library().shuffle(ctx.game.rng());
        return;
    }

    // ── Graveyard → Battlefield (reanimation) ──────────────────────────────
    if (!changeType.empty() && origin == "Graveyard") {
        Player& p = ctx.game.player(ctx.controller);
        for (Card* c : p.graveyard().cards()) {
            if (cardMatchesAnyFilter(*c, changeType, ctx.controller, kInvalidId, ctx.source, &ctx.game)) {
                Card* entered = ctx.game.moveToZone(c->id, dest, ctx.controller);
                // Aura entering directly from GY needs to attach to a target.
                // AI: pick the own creature with the highest P/T.
                if (entered && dest == ZoneType::Battlefield &&
                    entered->rules->type.isEnchantment() &&
                    entered->rules->type.hasSubtype("Aura") &&
                    entered->attachedTo == kInvalidId) {
                    Card* best = nullptr;
                    int bestScore = -1;
                    for (Card* bf : ctx.game.battlefield().cards()) {
                        if (!bf->isCreature() || bf->id == entered->id) continue;
                        if (bf->controllerId != ctx.controller) continue;
                        int score = effectivePower(*bf) + effectiveToughness(*bf);
                        if (score > bestScore) { bestScore = score; best = bf; }
                    }
                    if (best) {
                        entered->attachedTo = best->id;
                        best->attachments.push_back(entered->id);
                        ctx.game.recomputeStaticBonuses();
                    }
                }
                break;
            }
        }
        return;
    }

    // ── Hand → Exile/other, choosing cards that match ChangeType (Imprint:
    //    Isochron Scepter exiling an instant; "may exile a card" effects). ───
    if (!changeType.empty() && origin == "Hand" && dest != ZoneType::Library) {
        uint8_t handPlayer = ctx.controller;
        Player& p = ctx.game.player(handPlayer);
        // Pick the highest-mana-value matching card(s) — the most valuable to
        // imprint/exile. (A human selection prompt could refine this later.)
        std::vector<Card*> matching;
        for (Card* c : p.hand().cards())
            if (cardMatchesAnyFilter(*c, changeType, handPlayer, kInvalidId, ctx.source, &ctx.game))
                matching.push_back(c);
        std::sort(matching.begin(), matching.end(), [](const Card* a, const Card* b) {
            return a->rules->manaCost.cmc() > b->rules->manaCost.cmc();
        });
        int taken = 0;
        for (Card* c : matching) {
            if (taken >= numCards) break;
            uint8_t destCtrl = (dest == ZoneType::Battlefield) ? ctx.controller : c->ownerId;
            Card* moved = ctx.game.moveToZone(c->id, dest, destCtrl);
            if (moved) {
                if (imprint && ctx.source) moved->exiledBy = ctx.source->id;
                if (rememberChanged) ctx.remembered.push_back(moved->id);
            }
            ++taken;
        }
        return;
    }

    // ── Hand → Library (Brainstorm / Looting effects) ─────────────────────
    if (origin == "Hand" && dest == ZoneType::Library) {
        Player& p = ctx.game.player(ctx.controller);
        // Collect up to numCards cards from hand (AI: take from front of hand)
        std::vector<ObjectId> toReturn;
        for (Card* c : p.hand().cards()) {
            if ((int)toReturn.size() >= numCards) break;
            toReturn.push_back(c->id);
        }
        // Move each to library (lands at bottom — acceptable AI simplification)
        for (ObjectId id : toReturn)
            ctx.game.moveToZone(id, ZoneType::Library, ctx.controller);
        return;
    }

    // ── Default: take N cards from top/front of origin zone ────────────────
    if (dest == ZoneType::Hand || dest == ZoneType::Battlefield) {
        Player& p = ctx.game.player(ctx.controller);
        Zone* srcZone = nullptr;
        if      (origin == "Library")   srcZone = &p.library();
        else if (origin == "Graveyard") srcZone = &p.graveyard();
        if (!srcZone) return;

        for (int i = 0; i < numCards; ++i) {
            if (srcZone->empty()) { if (dest == ZoneType::Hand) p.lose(); break; }
            Card* top = srcZone->front();
            if (!top) break;
            ctx.game.moveToZone(top->id, dest, ctx.controller);
        }
    }
}

// ── Phase 8 effects ───────────────────────────────────────────────────────────

// Clone — a permanent (Defined$ Self/Enchanted) becomes a copy of another
// permanent (a card target, or CloneTarget$ Remembered). Supports the common
// riders: KeepName$, AddTypes$, SetPower$/SetToughness$, Keywords$. Note: the
// copy is applied permanently here; "until end of turn / until this leaves"
// reversion is not yet modelled (a continuous-effect-duration feature).
void effectClone(const ScriptLine& s, EffectContext& ctx) {
    Card* target = resolveDefinedCard(s, ctx);   // the permanent that becomes a copy
    if (!target || !target->isOnBattlefield() || !target->rules) return;

    // The permanent whose characteristics are copied.
    const Card* source = nullptr;
    if (s.get("CloneTarget", "") == "Remembered") {
        if (!ctx.remembered.empty()) source = ctx.game.findCard(ctx.remembered.front());
    } else {
        for (const auto& t : ctx.targets) {
            if (!t.isCard() || t.cardId == target->id) continue;
            source = ctx.game.findCard(t.cardId);
            if (source) break;
        }
    }
    if (!source || !source->rules) return;

    const std::string origName = target->rules->name;
    target->ownedRules = std::make_shared<CardRules>(*source->rules);
    target->rules      = target->ownedRules.get();

    if (s.get("KeepName", "") == "True") target->ownedRules->name = origName;

    // AddTypes$ A,B,C — layer extra types on top of the copied ones.
    auto addTypes = s.get("AddTypes", "");
    if (!addTypes.empty()) {
        std::string tstr(addTypes);
        for (char& ch : tstr) if (ch == ',') ch = ' ';
        CardType extra = CardType::parse(tstr);
        auto& tt = target->ownedRules->type;
        for (auto st : extra.supertypes) tt.supertypes.push_back(st);
        for (auto mt : extra.types)      tt.types.push_back(mt);
        for (auto& sub : extra.subtypes) tt.subtypes.push_back(sub);
    }

    auto sp = s.get("SetPower", "");     if (!sp.empty()) target->ownedRules->power     = std::string(sp);
    auto st = s.get("SetToughness", ""); if (!st.empty()) target->ownedRules->toughness = std::string(st);

    auto kw = s.get("Keywords", s.get("Keyword", ""));
    if (!kw.empty()) {
        std::string cur;
        std::string kwStr(kw); kwStr += '&';
        for (char ch : kwStr) {
            if (ch == '&' || ch == ',') {
                size_t b = cur.find_first_not_of(" \t"), e = cur.find_last_not_of(" \t");
                if (b != std::string::npos) target->ownedRules->keywords.push_back(cur.substr(b, e - b + 1));
                cur.clear();
            } else cur += ch;
        }
    }

    target->keywordMask = buildKeywordMask(*target->rules);
    target->summoningSickness = target->rules->isCreature()
        && !maskHas(target->keywordMask, KeywordAbility::Haste);
}

void effectToken(const ScriptLine& s, EffectContext& ctx) {
    int amount = s.getInt("TokenAmount", 1);

    // AmountPipsOfTrigger$ <colors> — set the token count to the number of mana
    // symbols of those colors in the spell that triggered this effect (e.g.
    // Namor: one Merfolk per blue pip in the noncreature spell you cast).
    auto pipColors = s.get("AmountPipsOfTrigger", "");
    if (!pipColors.empty() && ctx.triggeredCardId != 0) {
        if (const Card* sp = ctx.game.findCard(ctx.triggeredCardId); sp && sp->rules) {
            uint8_t mask = 0;
            using namespace ManaAtom;
            for (char ch : pipColors) switch (ch) {
                case 'W': mask |= static_cast<uint8_t>(WHITE); break;
                case 'U': mask |= static_cast<uint8_t>(BLUE);  break;
                case 'B': mask |= static_cast<uint8_t>(BLACK); break;
                case 'R': mask |= static_cast<uint8_t>(RED);   break;
                case 'G': mask |= static_cast<uint8_t>(GREEN); break;
            }
            int pips = 0;
            for (const auto& sh : sp->rules->manaCost.shards())
                if (sh.colorMask() & mask) ++pips;
            amount = pips;
        }
    }

    if (amount <= 0) return;

    std::string types = "Creature";
    {
        std::string raw = std::string(s.get("TokenTypes", "Creature"));
        std::string acc;
        for (char c : raw) { if (c == ',') acc += ' '; else acc += c; }
        if (!acc.empty()) types = acc;
    }

    uint8_t colorMask = 0;
    {
        auto colStr = s.get("TokenColors", "");
        using namespace ManaAtom;
        for (char c : colStr) {
            switch (c) {
                case 'W': colorMask |= static_cast<uint8_t>(WHITE); break;
                case 'U': colorMask |= static_cast<uint8_t>(BLUE);  break;
                case 'B': colorMask |= static_cast<uint8_t>(BLACK); break;
                case 'R': colorMask |= static_cast<uint8_t>(RED);   break;
                case 'G': colorMask |= static_cast<uint8_t>(GREEN); break;
            }
        }
    }

    std::string power     = std::string(s.get("TokenPower",     "1"));
    std::string toughness = std::string(s.get("TokenToughness", "1"));

    std::vector<std::string> keywords;
    {
        std::string kwStr = std::string(s.get("TokenKeywords", ""));
        if (!kwStr.empty()) {
            // Trim surrounding whitespace so "Flying & Indestructible" yields
            // "Flying"/"Indestructible" (not " Indestructible"), which would
            // otherwise fail keyword matching. Internal spaces (e.g. "First
            // Strike") are preserved.
            auto trim = [](std::string s) {
                size_t b = s.find_first_not_of(" \t");
                size_t e = s.find_last_not_of(" \t");
                return (b == std::string::npos) ? std::string{} : s.substr(b, e - b + 1);
            };
            std::string kw;
            for (char c : kwStr) {
                if (c == '&' || c == ',') {
                    auto t = trim(kw);
                    if (!t.empty()) keywords.push_back(t);
                    kw.clear();
                } else { kw += c; }
            }
            auto t = trim(kw);
            if (!t.empty()) keywords.push_back(t);
        }
    }

    std::string name = std::string(s.get("TokenName", "Token"));

    // ── TokenScript$ — resolve a Forge token-script name into the token's full
    //    definition. The name encodes everything: "<colors>_<P>_<T>_<subtypes>_
    //    <keywords>" for creatures (e.g. w_2_2_samurai_double_strike → white 2/2
    //    Samurai with double strike) or "c_a_<name>" for artifact tokens (e.g.
    //    c_a_treasure_sac → a Treasure; createToken injects its sac ability).
    //    Without this, every token-script card made a default 1/1 creature.
    {
        auto tokenScript = std::string(s.get("TokenScript", ""));
        if (!tokenScript.empty()) {
            std::vector<std::string> parts;
            { std::string cur;
              for (char ch : tokenScript) {
                  if (ch == '_') { if (!cur.empty()) { parts.push_back(cur); cur.clear(); } }
                  else cur += ch;
              }
              if (!cur.empty()) parts.push_back(cur); }

            auto cap = [](std::string w) {
                if (!w.empty() && w[0] >= 'a' && w[0] <= 'z') w[0] = static_cast<char>(w[0] - 32);
                return w;
            };
            auto isNum = [](const std::string& t) {
                return !t.empty() && ((t[0] >= '0' && t[0] <= '9') || t == "x" || t == "X");
            };
            // underscore-form → keyword display name (single + two-word).
            auto kwName = [](const std::string& k) -> std::string {
                if (k == "double") return "Double Strike";   // consumed with "strike"
                if (k == "first")  return "First Strike";
                if (k == "flying")        return "Flying";
                if (k == "trample")       return "Trample";
                if (k == "vigilance")     return "Vigilance";
                if (k == "haste")         return "Haste";
                if (k == "lifelink")      return "Lifelink";
                if (k == "deathtouch")    return "Deathtouch";
                if (k == "menace")        return "Menace";
                if (k == "reach")         return "Reach";
                if (k == "defender")      return "Defender";
                if (k == "hexproof")      return "Hexproof";
                if (k == "indestructible")return "Indestructible";
                if (k == "flash")         return "Flash";
                if (k == "fear")          return "Fear";
                if (k == "intimidate")    return "Intimidate";
                if (k == "shroud")        return "Shroud";
                return "";   // not a keyword (it's a subtype)
            };
            // Trailing functional suffixes on Forge token scripts encode an
            // ability variant, NOT a creature subtype (e.g. eldrazi_spawn_SAC,
            // zombie_DECAYED, devil_BURN). Treating them as subtypes corrupts the
            // token's name — "Eldrazi Spawn" became "Eldrazi Spawn Sac" — which
            // breaks the Scryfall token-art lookup (keyed on the exact name).
            auto isAbilityHint = [](const std::string& t) {
                return t == "sac"     || t == "decayed" || t == "tappump" ||
                       t == "lifegain"|| t == "noblock" || t == "burn"    ||
                       t == "search"  || t == "draw"    || t == "tapped";
            };

            if (!parts.empty()) {
                using namespace ManaAtom;
                uint8_t cm = 0;
                for (char ch : parts[0]) switch (ch) {
                    case 'w': cm |= static_cast<uint8_t>(WHITE); break;
                    case 'u': cm |= static_cast<uint8_t>(BLUE);  break;
                    case 'b': cm |= static_cast<uint8_t>(BLACK); break;
                    case 'r': cm |= static_cast<uint8_t>(RED);   break;
                    case 'g': cm |= static_cast<uint8_t>(GREEN); break;
                    default: break;  // 'c' = colourless
                }
                colorMask = cm;
                size_t idx = 1;
                bool isArtifact = (idx < parts.size() && parts[idx] == "a");
                if (isArtifact) ++idx;

                if (!isArtifact && idx + 1 < parts.size() &&
                    isNum(parts[idx]) && isNum(parts[idx + 1])) {
                    // Creature token: power, toughness, subtypes…, keywords…
                    power     = (parts[idx]     == "x" || parts[idx]     == "X") ? "0" : parts[idx];
                    toughness = (parts[idx + 1] == "x" || parts[idx + 1] == "X") ? "0" : parts[idx + 1];
                    idx += 2;
                    std::vector<std::string> subs;
                    keywords.clear();
                    for (size_t k = idx; k < parts.size(); ++k) {
                        std::string disp = kwName(parts[k]);
                        if (!disp.empty()) {
                            // "double"/"first" consume the following "strike".
                            if ((parts[k] == "double" || parts[k] == "first") &&
                                k + 1 < parts.size() && parts[k + 1] == "strike")
                                ++k;
                            keywords.push_back(disp);
                        } else if (isAbilityHint(parts[k])) {
                            // Functional suffix, not a subtype — drop it from the
                            // name/types so art lookup matches. (createToken injects
                            // the matching ability by token name, e.g. Eldrazi Spawn.)
                            continue;
                        } else {
                            subs.push_back(cap(parts[k]));
                        }
                    }
                    std::string subJoined;
                    for (auto& su : subs) { if (!subJoined.empty()) subJoined += ' '; subJoined += su; }
                    types = subJoined.empty() ? "Creature" : ("Creature " + subJoined);
                    name  = subJoined.empty() ? "Token" : subJoined;
                } else {
                    // Artifact / non-creature named token (Treasure, Clue, Food…).
                    // The first word after the artifact marker is the token's
                    // name/subtype; trailing words (sac, tapped, …) are ability
                    // hints, not part of the name — drop them so createToken's
                    // named-token ability injection (Treasure → sac for mana) hits.
                    std::string nm = (idx < parts.size()) ? cap(parts[idx]) : std::string("Token");
                    name      = nm;
                    types     = (isArtifact ? "Artifact " : "") + nm;   // e.g. "Artifact Treasure"
                    power     = "0";
                    toughness = "0";
                    keywords.clear();
                }
            }
        }
    }

    // TokenOwner$ — who the token enters under. Default: the source's controller.
    // "Opponent"/"Targeted" → the opponent (e.g. The Sentry: "target opponent
    // creates The Void"). Two-player: opponent is controller ^ 1.
    uint8_t owner = ctx.controller;
    auto tokenOwner = s.get("TokenOwner", "");
    if (tokenOwner == "Opponent" || tokenOwner == "Targeted") {
        owner = ctx.controller ^ 1;
        // If an actual player was targeted, prefer that.
        for (const auto& t : ctx.targets)
            if (t.isPlayer()) { owner = t.playerId; break; }
    }

    // TokenMustAttack$ True — the token attacks each combat if able (e.g. The
    // Void from The Sentry). General per-token static abilities aren't modelled;
    // this covers the common "attacks each combat" rider.
    bool tokMustAttack = (s.get("TokenMustAttack", "") == "True");

    for (int i = 0; i < amount; ++i) {
        Card* tok = ctx.game.createToken(name, types, colorMask, power, toughness,
                                         owner, keywords);
        if (tok && tokMustAttack) { tok->mustAttack = true; tok->mustAttackTarget = 255; }
    }
}

// Doc Samson, Super Psychiatrist: "if you would put one or more counters on a
// permanent you control, put that many plus one of each kind instead." Modeled
// as a static "S:Mode$ CounterBonus | Amount$ N" on a permanent the counter's
// recipient controls. Returns the total bonus that player's permanents grant.
static int counterBonusForController(EffectContext& ctx, uint8_t pid) {
    int bonus = 0;
    for (const Card* c : ctx.game.battlefield().cards()) {
        if (c->controllerId != pid || !c->rules) continue;
        for (const auto& line : c->rules->staticAbilityLines) {
            auto st = parseScriptLine(line);
            if (st.effectType == "CounterBonus") bonus += st.getInt("Amount", 1);
        }
    }
    return bonus;
}

void effectPutCounter(const ScriptLine& s, EffectContext& ctx) {
    // Adapt$ N: like PutCounter but only if the creature has no +1/+1 counters.
    int adaptN = s.getInt("Adapt", -1);
    bool isAdapt = (adaptN >= 0);

    std::string ctype = std::string(s.get("CounterType", "P1P1"));
    int amount        = isAdapt ? adaptN : s.getInt("CounterNum", 1);

    std::string key;
    if      (ctype == "P1P1")    key = "+1/+1";
    else if (ctype == "M1M1")    key = "-1/-1";
    else if (ctype == "CHARGE")  key = "charge";
    else if (ctype == "LOYALTY") key = "loyalty";
    else                         key = ctype;

    auto applyTo = [&](Card* c) {
        if (!c || !c->isOnBattlefield()) return;
        // Adapt: skip if the creature already has +1/+1 counters.
        if (isAdapt && c->counterCount("+1/+1") > 0) return;
        // Doc Samson-style counter bonus: +N of the same kind for the controller.
        int addAmt = amount + (amount > 0 ? counterBonusForController(ctx, c->controllerId) : 0);
        c->addCounter(key, addAmt);

        // Fire CounterAdded triggers
        {
            std::vector<PendingTrigger> t;
            TriggerSystem::onCounterAdded(*c, key, addAmt, ctx.game, t);
            ctx.game.queueTriggers(std::move(t));
        }

        // Saga: when a LORE counter is added, fire the matching chapter ability
        if (key == "LORE" &&
            c->rules->type.isEnchantment() &&
            c->rules->type.hasSubtype("Saga")) {
            int newLore = c->counterCount("LORE");
            for (const auto& raw : c->rules->abilityLines) {
                auto chScript = parseScriptLine(raw);
                auto chStr    = chScript.get("Chapter", "");
                if (chStr.empty()) continue;
                if (parseChapterNum(chStr) != newLore) continue;
                // Execute this chapter's effect
                EffectContext sagaCtx{ ctx.game, c, ctx.controller, {}, 0 };
                executeEffectChain(chScript, sagaCtx);
                break;
            }
        }
    };

    // Defined$ You / Player — apply to the player (energy, experience counters).
    auto defined = s.get("Defined", "");
    bool targetsPlayer = (defined == "You" || defined == "Player"
                       || defined == "Opponent" || defined == "TriggeredPlayer");

    if (targetsPlayer) {
        uint8_t pid = ctx.controller;
        if (defined == "Opponent")        pid = ctx.controller ^ 1;
        else if (defined == "TriggeredPlayer") pid = ctx.triggerPlayer;
        ctx.game.player(pid).addCounter(key, amount);
    } else if (!ctx.targets.empty()) {
        for (const auto& t : ctx.targets)
            if (t.isCard()) applyTo(ctx.game.findCard(t.cardId));
    } else if (Card* def = resolveDefinedCard(s, ctx)) {
        applyTo(def);
    } else {
        if (ctx.source) applyTo(ctx.source);
    }
}

void effectPumpAll(const ScriptLine& s, EffectContext& ctx) {
    auto validCards = s.get("ValidCards", "Creature.YouCtrl");
    int  pBoost     = s.getInt("NumAtt", 0);
    int  tBoost     = s.getInt("NumDef", 0);
    auto dur        = s.get("Duration", "EndOfTurn");
    auto kwStr      = std::string(s.get("KW", ""));

    // Parse '&'-separated keyword list
    uint32_t kwMask = 0;
    if (!kwStr.empty()) {
        std::string tok;
        kwStr += '&';
        for (char ch : kwStr) {
            if (ch == '&') {
                while (!tok.empty() && tok.back()  == ' ') tok.pop_back();
                auto f = tok.find_first_not_of(' ');
                if (f != std::string::npos) tok = tok.substr(f);
                auto kw = parseKeyword(tok);
                if (kw != KeywordAbility::None) kwMask |= static_cast<uint32_t>(kw);
                tok.clear();
            } else {
                tok += ch;
            }
        }
    }

    ObjectId pumpSelfId = ctx.source ? ctx.source->id : kInvalidId;
    for (Card* c : ctx.game.battlefield().cards()) {
        if (!cardMatchesAnyFilter(*c, validCards, ctx.controller, pumpSelfId, ctx.source, &ctx.game)) continue;

        if (dur == "Permanent") {
            if (c->isCreature() && pBoost != 0)
                c->addCounter("+1/+1", pBoost);
            if (kwMask) c->keywordMask |= kwMask;
        } else {
            c->tempPower     += pBoost;
            c->tempToughness += tBoost;
            if (kwMask) {
                c->tempKeywords |= kwMask;
                c->keywordMask  |= kwMask;
            }
        }
    }
}

void effectChangeZoneAll(const ScriptLine& s, EffectContext& ctx) {
    auto validCards  = s.get("ValidCards", "Creature.All");
    auto destination = s.get("Destination", "Graveyard");

    ZoneType dest = ZoneType::Graveyard;
    if      (destination == "Hand")        dest = ZoneType::Hand;
    else if (destination == "Exile")       dest = ZoneType::Exile;
    else if (destination == "Library")     dest = ZoneType::Library;
    else if (destination == "Battlefield") dest = ZoneType::Battlefield;

    std::vector<ObjectId> toMove;
    for (const Card* c : ctx.game.battlefield().cards()) {
        if (cardMatchesAnyFilter(*c, validCards, ctx.controller, kInvalidId, ctx.source, &ctx.game))
            toMove.push_back(c->id);
    }
    for (ObjectId id : toMove) {
        const Card* c = ctx.game.findCard(id);
        if (!c) continue;
        if (dest == ZoneType::Graveyard && c->hasKeyword(KeywordAbility::Indestructible))
            continue;
        ctx.game.moveToZone(id, dest, c->ownerId);
    }
}

void effectDamageAll(const ScriptLine& s, EffectContext& ctx) {
    auto validCards   = s.get("ValidCards", "Creature.All");
    int  amount       = s.getIntOrX("NumDmg", ctx.xValue, 0);
    bool rememberDmg  = (s.get("RememberDamaged", "") == "True");
    if  (amount <= 0) return;

    bool hasDT = ctx.source && ctx.source->hasKeyword(KeywordAbility::Deathtouch);
    bool hasLL = ctx.source && ctx.source->hasKeyword(KeywordAbility::Lifelink);
    int lifelinkTotal = 0;

    for (Card* c : ctx.game.battlefield().cards()) {
        if (!cardMatchesAnyFilter(*c, validCards, ctx.controller, kInvalidId, ctx.source, &ctx.game)) continue;
        c->markedDamage += amount;
        if (hasDT && amount > 0) c->deathtouchDamage = true;
        lifelinkTotal += amount;
        if (rememberDmg) ctx.remembered.push_back(c->id);
    }
    if (rememberDmg)
        ctx.game.rememberedSizeHint = static_cast<int>(ctx.remembered.size());

    if (validCards.find("Player") != std::string_view::npos || validCards == "Any") {
        ctx.game.loseLife(0, amount);
        ctx.game.loseLife(1, amount);
        lifelinkTotal += amount * 2;
        { std::vector<PendingTrigger> t; TriggerSystem::onLoseLife(0, amount, ctx.game, t); ctx.game.queueTriggers(std::move(t)); }
        { std::vector<PendingTrigger> t; TriggerSystem::onLoseLife(1, amount, ctx.game, t); ctx.game.queueTriggers(std::move(t)); }
    }

    if (hasLL && lifelinkTotal > 0)
        ctx.game.gainLife(ctx.controller, lifelinkTotal);
}

void effectDiscard(const ScriptLine& s, EffectContext& ctx) {
    bool discardHand = (s.get("Mode", "") == "Hand");
    int num  = discardHand ? 999 : s.getInt("NumCards", 1);
    auto who = s.get("ValidPlayers", "");
    if (who.empty()) who = s.get("Defined", "Opponent");
    auto validCards = s.get("ValidCards", "");

    auto discardFrom = [&](uint8_t pid) {
        Player& p = ctx.game.player(pid);
        // For the human player in interactive mode, defer card selection to GameWindow.
        if (pid == 0 && ctx.game.isHumanInteractive()) {
            int actualNum = std::min(num, static_cast<int>(p.hand().size()));
            if (actualNum > 0)
                ctx.game.setPendingDiscard(actualNum);
            return;
        }
        for (int i = 0; i < num && !p.hand().empty(); ++i) {
            // If ValidCards$ is specified, find the first matching card; otherwise take from back
            Card* c = nullptr;
            if (!validCards.empty()) {
                for (Card* hc : p.hand().cards()) {
                    if (cardMatchesAnyFilter(*hc, validCards, pid, kInvalidId, ctx.source, &ctx.game)) {
                        c = hc;
                        break;
                    }
                }
                if (!c) break; // no matching card → discard loop ends
            } else {
                c = p.hand().back();
            }
            if (!c) break;
            // Madness: discard to exile and optionally cast for Madness cost
            if (c->rules->hasMadness) {
                // Check if we can afford the Madness cost
                int madnessCmc = c->rules->madnessCost.cmc();
                if (ctx.game.player(pid).manaPool().total() >= madnessCmc) {
                    // Move to exile temporarily (represents the Madness trigger zone)
                    // Then immediately cast from exile-like state by moving to stack
                    // Simplified: directly move to GY if we can't cast, or cast it
                    // For AI and simplified human: always cast Madness if affordable
                    ObjectId cardId = c->id;
                    ctx.game.moveToZone(cardId, ZoneType::Exile, pid);
                    // Re-find in exile
                    Card* inExile = ctx.game.exile().back();
                    if (inExile && inExile->rules == c->rules) {
                        // Cast from exile at Madness cost
                        ctx.game.player(pid).manaPool().addGeneric(-madnessCmc);
                        Card* onStack = ctx.game.moveToZone(inExile->id, ZoneType::Stack, pid);
                        if (onStack) {
                            // The card will be resolved — put it on the stack as a pending effect
                            // For simplicity, just execute the effect immediately
                            EffectContext madnessCtx{ ctx.game, onStack, pid, {}, 0 };
                            for (const auto& raw : onStack->rules->abilityLines) {
                                auto s2 = parseScriptLine(raw);
                                if (s2.abilityType == "SP" || s2.abilityType == "DB") {
                                    executeEffect(s2, madnessCtx);
                                    break;
                                }
                            }
                            ctx.game.moveToZone(onStack->id, ZoneType::Graveyard, pid);
                        }
                    }
                    continue;
                }
                // Can't afford Madness → just discard normally
            }
            {
                ObjectId cid = c->id;
                Card* inGY = ctx.game.moveToZone(cid, ZoneType::Graveyard, pid);
                if (inGY) {
                    std::vector<PendingTrigger> trigs;
                    TriggerSystem::onDiscard(*inGY, pid, ctx.game, trigs);
                    ctx.game.queueTriggers(std::move(trigs));
                }
            }
        }
    };

    if      (who == "You")               discardFrom(ctx.controller);
    else if (who == "Each" || who == "All") { discardFrom(0); discardFrom(1); }
    else                                 discardFrom(ctx.controller ^ 1);
}

void effectTap(const ScriptLine& s, EffectContext& ctx) {
    bool doUntap = (s.effectType == "Untap");

    // "UnlessCost$ PayLife<N>" (shock lands): the permanent enters tapped UNLESS
    // the payer pays N life. Used on the ETB Tap with Defined$ Self.
    if (!doUntap && ctx.source) {
        std::string unless(s.get("UnlessCost", ""));
        if (unless.rfind("PayLife<", 0) == 0) {
            int amt = 0;
            auto lt = unless.find('<'), gt = unless.find('>');
            if (lt != std::string::npos && gt != std::string::npos && gt > lt + 1)
                for (size_t i = lt + 1; i < gt; ++i)
                    if (unless[i] >= '0' && unless[i] <= '9')
                        amt = amt * 10 + (unless[i] - '0');
            uint8_t payer = ctx.source->controllerId;   // UnlessPayer$ You
            if (amt > 0) {
                if (payer == 0 && ctx.game.isHumanInteractive()) {
                    // Human: leave it untapped for now and ask via the pending UI;
                    // GameWindow resolves (pay → stays untapped, decline → tap).
                    ctx.game.setPendingPayLife(ctx.source->id, amt, payer);
                    return;
                }
                // AI / simulation: pay only when life is comfortably above the
                // cost — an untapped land is almost always worth N life.
                if (ctx.game.player(payer).life() > amt + 4) {
                    ctx.game.loseLife(payer, amt);
                    return;                       // paid → stays untapped
                }
                // else: not worth it at low life → fall through and tap it.
            }
        }
        // "UnlessCost$ Reveal<N/filterA;filterB/desc>" (slow/check lands like
        // Foreboding Ruins): enters tapped UNLESS you reveal a matching card
        // (e.g. a Swamp or Mountain) from hand. Revealing has no real cost, so
        // we reveal automatically whenever the payer holds a qualifying card —
        // the land then enters untapped.
        else if (unless.rfind("Reveal<", 0) == 0) {
            std::string inner = unless.substr(7);
            if (!inner.empty() && inner.back() == '>') inner.pop_back();
            // inner = "N/filterA;filterB/desc" — the middle segment is the filter.
            std::vector<std::string> segs;
            { std::string cur;
              for (char ch : inner) { if (ch == '/') { segs.push_back(cur); cur.clear(); } else cur += ch; }
              segs.push_back(cur); }
            std::string filterPart = (segs.size() > 1) ? segs[1] : "";
            std::vector<std::string> filters;
            { std::string cur;
              for (char ch : filterPart) { if (ch == ';') { if (!cur.empty()) filters.push_back(cur); cur.clear(); } else cur += ch; }
              if (!cur.empty()) filters.push_back(cur); }

            uint8_t payer = ctx.source->controllerId;   // UnlessPayer$ You
            bool canReveal = false;
            for (const Card* h : ctx.game.player(payer).hand().cards()) {
                if (!h || !h->rules) continue;
                for (const auto& f : filters) {
                    if (h->rules->type.hasSubtype(f) ||
                        cardMatchesAnyFilter(*h, f, payer, kInvalidId, ctx.source, &ctx.game)) {
                        canReveal = true; break;
                    }
                }
                if (canReveal) break;
            }
            if (canReveal) return;   // revealed a qualifying card → stays untapped
            // else: nothing to reveal → fall through and tap it.
        }
    }

    if (!ctx.targets.empty()) {
        for (const auto& t : ctx.targets) {
            if (!t.isCard()) continue;
            Card* c = ctx.game.findCard(t.cardId);
            if (!c || !c->isOnBattlefield()) continue;
            bool wasTapped = c->tapped;
            c->tapped = !doUntap;
            // Fire Inspired triggers when a permanent becomes tapped (not already tapped)
            if (!doUntap && !wasTapped) {
                std::vector<PendingTrigger> tapTrigs;
                TriggerSystem::onTap(*c, ctx.game, tapTrigs);
                ctx.game.queueTriggers(std::move(tapTrigs));
            }
        }
        return;
    }
    // Defined$ Self — tap/untap the source card (used by ETB replacement effects)
    auto defined = s.get("Defined", "");
    if ((defined == "Self" || defined == "This") && ctx.source && ctx.source->isOnBattlefield()) {
        bool wasTapped = ctx.source->tapped;
        ctx.source->tapped = !doUntap;
        if (!doUntap && !wasTapped) {
            std::vector<PendingTrigger> tapTrigs;
            TriggerSystem::onTap(*ctx.source, ctx.game, tapTrigs);
            ctx.game.queueTriggers(std::move(tapTrigs));
        }
    }
}

// Lightweight board evaluation from a player's perspective, used to compare the
// outcomes of modal choices (effectCharm). Self-contained (no AI-layer coupling):
// material (creature P/T) + life + card advantage, signed for the given player.
static int charmBoardScore(const GameState& g, uint8_t pid) {
    uint8_t opp = pid ^ 1;
    int s = (g.player(pid).life() - g.player(opp).life()) * 2;
    for (const Card* c : g.battlefield().cards()) {
        if (!c->isCreature()) continue;
        int v = std::max(0, effectivePower(*c)) * 3 + std::max(0, effectiveToughness(*c));
        s += (c->controllerId == pid) ? v : -v;
    }
    s += (static_cast<int>(g.player(pid).hand().size())
          - static_cast<int>(g.player(opp).hand().size())) * 2;
    return s;
}

void effectCharm(const ScriptLine& s, EffectContext& ctx) {
    auto choicesStr = s.get("Choices", "");
    if (choicesStr.empty()) return;

    std::vector<std::string> choices;
    std::string cur;
    for (char c : std::string(choicesStr)) {
        if (c == ',') { if (!cur.empty()) { choices.push_back(cur); cur.clear(); } }
        else cur += c;
    }
    if (!cur.empty()) choices.push_back(cur);
    if (choices.empty()) return;

    // Human player in interactive mode: defer mode selection to UI overlay.
    // Store the SVar bodies now (source card moves to GY after resolution).
    if (ctx.controller == 0 && ctx.game.isHumanInteractive()) {
        PendingCharmChoice pending;
        pending.active     = true;
        pending.controller = ctx.controller;
        pending.targets    = ctx.targets;
        pending.xValue     = ctx.xValue;
        pending.sourceId   = ctx.source ? ctx.source->id : kInvalidId;
        for (const auto& name : choices) {
            pending.labels.push_back(name);
            pending.bodies.push_back(std::string(ctx.svar(name)));
        }
        ctx.game.setPendingCharm(std::move(pending));
        return;
    }

    // AI: score each mode by SIMULATING it on a clone and measuring the board
    // swing (material + life + card advantage), then execute the best mode(s).
    // Honors CharmNum$/MinCharmNum$ ("choose one or both" / "choose two").
    struct ModeScore { int delta = 0; std::string body; };
    std::vector<ModeScore> modes;
    int baseline = charmBoardScore(ctx.game, ctx.controller);
    for (const auto& choiceName : choices) {
        std::string body = std::string(ctx.svar(choiceName));
        if (body.empty()) continue;
        auto sub = parseScriptLine(body);
        if (sub.empty()) continue;

        // Simulate this mode on a clone and measure the resulting board swing.
        GameState sim = ctx.game.clone();
        Card* simSrc = ctx.source ? sim.findCard(ctx.source->id) : nullptr;
        EffectContext simCtx{ sim, simSrc, ctx.controller, ctx.targets, ctx.xValue };
        simCtx.triggeredCardId = ctx.triggeredCardId;
        executeEffect(sub, simCtx);
        modes.push_back({ charmBoardScore(sim, ctx.controller) - baseline, std::move(body) });
    }
    if (modes.empty()) return;
    std::sort(modes.begin(), modes.end(),
              [](const ModeScore& a, const ModeScore& b) { return a.delta > b.delta; });

    // How many modes to take. Default 1; "choose one or both" sets CharmNum$ 2.
    int maxModes = std::max(1, s.getInt("CharmNum", 1));
    int minModes = std::max(1, s.getInt("MinCharmNum", 1));
    maxModes = std::min<int>(maxModes, static_cast<int>(modes.size()));

    int taken = 0;
    for (const auto& m : modes) {
        if (taken >= maxModes) break;
        // Always take the minimum required; beyond that, only beneficial modes.
        if (taken >= minModes && m.delta <= 0) break;
        auto line = parseScriptLine(m.body);
        if (!line.empty()) executeEffect(line, ctx);
        ++taken;
    }
}

void effectDig(const ScriptLine& s, EffectContext& ctx) {
    int digNum  = s.getInt("DigNum",    3);
    int keepNum = s.getInt("ChangeNum", 1);

    Player& p = ctx.game.player(ctx.controller);
    for (int i = 0; i < keepNum && i < digNum; ++i) {
        if (p.library().empty()) { p.lose(); return; }
        Card* top = p.library().front();
        ctx.game.moveToZone(top->id, ZoneType::Hand, ctx.controller);
    }
}

void effectScry(const ScriptLine& s, EffectContext& ctx) {
    int scryNum = s.getInt("ScryNum", 1);
    Player& p   = ctx.game.player(ctx.controller);

    std::vector<Card*> looked;
    for (int i = 0; i < scryNum && !p.library().empty(); ++i) {
        Card* top = p.library().front();
        if (!top) break;
        p.library().remove(top->id);
        looked.push_back(top);
    }

    // Human player: hand the looked-at cards to the UI overlay. They stay
    // tracked by id only (still alive in m_objects, just not in any zone)
    // until the player resolves each one via completePendingScry().
    if (ctx.controller == 0 && ctx.game.isHumanInteractive()) {
        std::vector<ObjectId> ids;
        ids.reserve(looked.size());
        for (Card* c : looked) ids.push_back(c->id);
        ctx.game.setPendingScry(0, std::move(ids));
        return;
    }

    // AI scry heuristic: keep a card if it would be useful right now.
    // "Useful" = castable within 2 turns (cmc <= lands+2) OR is a land if we need one.
    int myLands = 0, mySpells = 0;
    for (const Card* c : ctx.game.battlefield().cards()) {
        if (c->controllerId != ctx.controller) continue;
        if (c->isLand()) ++myLands;
    }
    for (const Card* c : p.hand().cards()) {
        if (!c->rules->type.isLand()) ++mySpells;
    }
    bool needLand = (myLands < 3) || (p.hand().size() >= 3 && mySpells > 0 && myLands < 4);

    std::vector<Card*> keep, bottom;
    for (Card* c : looked) {
        bool isLand = c->rules->type.isLand();
        bool castSoon = c->rules->cmc() <= myLands + 2;
        bool useful = (isLand && needLand) || (!isLand && castSoon);
        if (useful) keep.push_back(c); else bottom.push_back(c);
    }
    // Put kept cards back on top in original order, bottom cards go under.
    // Kept-on-top cards are now known to their owner (shown face-up in the
    // library browser until drawn or shuffled away).
    for (Card* c : keep) {
        c->revealedToOwner = true;
        p.library().addToFront(c);
    }
    for (Card* c : bottom)
        p.library().addToBack(c);
}

void effectRegenerate(const ScriptLine& s, EffectContext& ctx) {
    (void)s;
    for (const auto& t : ctx.targets) {
        if (!t.isCard()) continue;
        Card* c = ctx.game.findCard(t.cardId);
        if (!c || !c->isOnBattlefield()) continue;
        c->addCounter("regen", 1);
    }
}

void effectAttach(const ScriptLine& s, EffectContext& ctx) {
    (void)s;
    if (!ctx.source || !ctx.source->isOnBattlefield()) return;
    for (const auto& t : ctx.targets) {
        if (!t.isCard()) continue;
        Card* target = ctx.game.findCard(t.cardId);
        if (!target || !target->isCreature() || !target->isOnBattlefield()) continue;
        if (target->controllerId != ctx.controller) continue;
        attachEquipment(*ctx.source, *target, ctx.game);
        break;
    }
}

void effectEffect(const ScriptLine& s, EffectContext& ctx) {
    // RememberObjects$ Targeted — populate ctx.remembered from the ability's targets.
    auto remObj = s.get("RememberObjects", "");
    if (remObj == "Targeted") {
        for (const auto& t : ctx.targets)
            if (t.isCard()) ctx.remembered.push_back(t.cardId);
    } else if (remObj == "Remembered") {
        // Keep ctx.remembered as-is (already populated by a prior sub-effect).
    }

    // Execute$ SomeVar — run a sub-ability SVar.
    auto execName = std::string(s.get("Execute", ""));
    if (!execName.empty() && ctx.source) {
        auto it = ctx.source->rules->svars.find(execName);
        if (it != ctx.source->rules->svars.end()) {
            auto sub = parseScriptLine(it->second);
            if (!sub.empty()) { executeEffect(sub, ctx); return; }
        }
    }

    // StaticAbilities$ SomeVar — apply a runtime static ability (e.g. MustAttack).
    auto staticAbName = std::string(s.get("StaticAbilities", ""));
    if (!staticAbName.empty() && ctx.source) {
        auto it = ctx.source->rules->svars.find(staticAbName);
        if (it == ctx.source->rules->svars.end()) return;
        auto stLine = parseScriptLine(it->second);
        auto mode = stLine.get("Mode", "");

        if (mode == "MustAttack") {
            // ValidCreature$ — which creatures must attack.
            auto validCr = std::string(stLine.get("ValidCreature", ""));
            // MustAttack$ You — the player the creature must attack (You = source controller).
            auto mustAtkStr = stLine.get("MustAttack", "");
            uint8_t targetPid = 255; // 255 = any player
            if (mustAtkStr == "You") targetPid = ctx.controller;

            bool useRemembered = (validCr.find("IsRemembered") != std::string::npos);
            if (useRemembered) {
                // Apply to all remembered card IDs.
                for (ObjectId oid : ctx.remembered) {
                    Card* c = ctx.game.findCard(oid);
                    if (!c || !c->isCreature() || !c->isOnBattlefield()) continue;
                    c->mustAttack       = true;
                    c->mustAttackTarget = targetPid;
                }
                // Also apply to current targets if remembered is empty.
                if (ctx.remembered.empty()) {
                    for (const auto& t : ctx.targets) {
                        if (!t.isCard()) continue;
                        Card* c = ctx.game.findCard(t.cardId);
                        if (!c || !c->isCreature() || !c->isOnBattlefield()) continue;
                        c->mustAttack       = true;
                        c->mustAttackTarget = targetPid;
                    }
                }
            } else if (!validCr.empty()) {
                for (Card* c : ctx.game.battlefield().cards()) {
                    if (!c->isCreature()) continue;
                    if (!cardMatchesAnyFilter(*c, validCr, ctx.controller,
                                              ctx.source ? ctx.source->id : kInvalidId,
                                              ctx.source, &ctx.game))
                        continue;
                    c->mustAttack       = true;
                    c->mustAttackTarget = targetPid;
                }
            }
        }
    }
}

// ── Phase 10 effects ──────────────────────────────────────────────────────────

// Mill — put N cards from target player's library into their graveyard
void effectMill(const ScriptLine& s, EffectContext& ctx) {
    int numCards       = s.getIntOrX("NumCards", ctx.xValue, 1);
    auto who           = s.get("ValidPlayers", "Opponent");
    bool rememberMilled = (s.get("RememberMilled", "") == "True");

    auto millPlayer = [&](uint8_t pid) {
        Player& p = ctx.game.player(pid);
        for (int i = 0; i < numCards && !p.library().empty(); ++i) {
            Card* top = p.library().front();
            if (!top) break;
            Card* milled = ctx.game.moveToZone(top->id, ZoneType::Graveyard, pid);
            if (milled && rememberMilled)
                ctx.remembered.push_back(milled->id);
        }
    };

    if      (who == "You")                  millPlayer(ctx.controller);
    else if (who == "Each" || who == "All") { millPlayer(0); millPlayer(1); }
    else                                    millPlayer(ctx.controller ^ 1);
}

// GainAbility / AddAbility — grant keyword(s) to targets or self
// Key fields: KW$ (keyword name), Duration$ (EndOfTurn / Permanent), Defined$ (Self)
void effectGainAbility(const ScriptLine& s, EffectContext& ctx) {
    auto kwStr   = std::string(s.get("KW", ""));
    auto dur     = s.get("Duration", "EndOfTurn");
    auto defined = s.get("Defined",  "");

    if (kwStr.empty()) return;

    // Parse '&'-separated keyword list (e.g. "Flying & Lifelink")
    std::vector<KeywordAbility> kws;
    {
        std::string tok;
        kwStr += '&';
        for (char ch : kwStr) {
            if (ch == '&') {
                // trim whitespace from token
                while (!tok.empty() && tok.back() == ' ') tok.pop_back();
                auto first = tok.find_first_not_of(' ');
                if (first != std::string::npos) tok = tok.substr(first);
                auto kw = parseKeyword(tok);
                if (kw != KeywordAbility::None) kws.push_back(kw);
                tok.clear();
            } else {
                tok += ch;
            }
        }
    }
    if (kws.empty()) return;

    auto applyTo = [&](Card* c) {
        if (!c || !c->isOnBattlefield()) return;
        for (auto kw : kws) {
            if (dur == "Permanent") {
                c->grantKeyword(kw);
            } else {
                c->tempKeywords |= static_cast<uint32_t>(kw);
                c->keywordMask  |= static_cast<uint32_t>(kw);
            }
        }
    };

    auto validCards = s.get("ValidCards", "");

    if (!defined.empty() && defined != "Targeted") {
        if (defined == "All" || defined == "Each" || !validCards.empty()) {
            auto filter = validCards.empty() ? std::string_view("Creature.All") : validCards;
            for (Card* c : ctx.game.battlefield().cards()) {
                if (cardMatchesAnyFilter(*c, filter, ctx.controller,
                                        ctx.source ? ctx.source->id : kInvalidId,
                                        ctx.source, &ctx.game))
                    applyTo(c);
            }
        } else if (Card* def = resolveDefinedCard(s, ctx)) {
            applyTo(def);
        } else if ((defined == "Self" || defined == "You") && ctx.source) {
            applyTo(ctx.source);
        }
    } else if (!validCards.empty()) {
        for (Card* c : ctx.game.battlefield().cards()) {
            if (cardMatchesAnyFilter(*c, validCards, ctx.controller,
                                    ctx.source ? ctx.source->id : kInvalidId,
                                    ctx.source, &ctx.game))
                applyTo(c);
        }
    } else if (!ctx.targets.empty()) {
        for (const auto& t : ctx.targets)
            if (t.isCard()) applyTo(ctx.game.findCard(t.cardId));
    } else if (ctx.source) {
        applyTo(ctx.source);
    }
}

// GainControl — change controller of target permanent.
// Duration$ Permanent (default) keeps the new controller indefinitely.
// Duration$ EndOfTurn reverts at the next Cleanup step.
void effectGainControl(const ScriptLine& s, EffectContext& ctx) {
    bool eot = (s.get("Duration", "Permanent") == "EndOfTurn");

    auto steal = [&](Card* c) {
        if (!c || !c->isOnBattlefield()) return;
        if (c->controllerId == ctx.controller) return; // already ours
        if (eot && c->originalControllerId == 0xFF)
            c->originalControllerId = c->controllerId;
        c->controllerId = ctx.controller;
        c->summoningSickness = true; // stolen creature has summoning sickness
    };

    if (!ctx.targets.empty()) {
        for (const auto& t : ctx.targets)
            if (t.isCard()) steal(ctx.game.findCard(t.cardId));
    } else if (Card* def = resolveDefinedCard(s, ctx)) {
        steal(def);
    }
}

// PreventDamage / Fog — prevent N damage to a target or all combat damage this turn.
// Forge uses:
//   SP$ PreventDamage | NumDmg$ X | ValidTgts$ Creature,Player   — targeted prevention
//   SP$ PreventDamage | Defined$ All | IsCombat$ True             — Fog
//   SP$ Fog                                                       — shorthand for Fog
void effectPreventDamage(const ScriptLine& s, EffectContext& ctx) {
    auto defined  = s.get("Defined", "");
    auto isCombat = s.get("IsCombat", "");

    // "Fog" variant: prevent all combat damage this turn
    if (s.effectType == "Fog" ||
        defined == "All" || defined == "AllCreatures" ||
        isCombat == "True") {
        ctx.game.preventAllCombatDamage = true;
        return;
    }

    int amount = s.getIntOrX("NumDmg", ctx.xValue, 0);
    if (amount <= 0) return;

    if (!ctx.targets.empty()) {
        // Set a damage shield on each chosen target
        for (const auto& t : ctx.targets) {
            if (t.isPlayer()) {
                ctx.game.player(t.playerId).addDamageShield(amount);
            } else if (t.isCard()) {
                Card* c = ctx.game.findCard(t.cardId);
                if (c && c->isOnBattlefield()) c->damageShield += amount;
            }
        }
    } else if (defined == "You" || defined.empty()) {
        ctx.game.player(ctx.controller).addDamageShield(amount);
    } else if (defined == "Opponent") {
        ctx.game.player(ctx.controller ^ 1).addDamageShield(amount);
    }
}

// SetState / TapAll / UntapAll — tap, untap, or transform matching permanents.
// Key fields: Mode$ Tap|Untap|Transform (or inferred from effectType), ValidCards$
void effectSetState(const ScriptLine& s, EffectContext& ctx) {
    if (s.get("Mode", "") == "Transform") {
        effectTransform(s, ctx);
        return;
    }

    bool doUntap = (s.effectType == "UntapAll" ||
                    s.effectType == "Untap" ||
                    s.get("Mode", "") == "Untap");

    auto validCards = s.get("ValidCards", "Permanent.All");

    if (!ctx.targets.empty()) {
        // Targeted tap/untap
        for (const auto& t : ctx.targets) {
            if (!t.isCard()) continue;
            Card* c = ctx.game.findCard(t.cardId);
            if (c && c->isOnBattlefield()) c->tapped = !doUntap;
        }
        return;
    }

    // Mass tap/untap all matching cards on the battlefield
    std::vector<ObjectId> toProcess;
    for (const Card* c : ctx.game.battlefield().cards())
        if (cardMatchesAnyFilter(*c, validCards, ctx.controller, kInvalidId, ctx.source, &ctx.game))
            toProcess.push_back(c->id);
    for (ObjectId id : toProcess) {
        Card* c = ctx.game.findCard(id);
        if (!c || !c->isOnBattlefield()) continue;
        bool wasTapped = c->tapped;
        c->tapped = !doUntap;
        if (!doUntap && !wasTapped) {
            std::vector<PendingTrigger> tapTrigs;
            TriggerSystem::onTap(*c, ctx.game, tapTrigs);
            ctx.game.queueTriggers(std::move(tapTrigs));
        }
    }
}

// RemoveCounter — remove N counters of a type from target(s) or source.
void effectRemoveCounter(const ScriptLine& s, EffectContext& ctx) {
    std::string ctype = std::string(s.get("CounterType", "P1P1"));
    bool removeAll    = (s.get("CounterNum", "") == "All");
    int  amount       = removeAll ? INT_MAX : s.getInt("CounterNum", 1);
    bool rememberRem  = (s.get("RememberRemoved", "") == "True");

    std::string key;
    if      (ctype == "P1P1")    key = "+1/+1";
    else if (ctype == "M1M1")    key = "-1/-1";
    else if (ctype == "CHARGE")  key = "charge";
    else if (ctype == "LOYALTY") key = "loyalty";
    else                         key = ctype;

    int totalRemoved = 0;
    auto applyTo = [&](Card* c) {
        if (!c || !c->isOnBattlefield()) return;
        int had = c->counterCount(key);
        int rem = removeAll ? had : std::min(amount, had);
        if (rem > 0) { c->removeCounter(key, rem); totalRemoved += rem; }
    };

    if (!ctx.targets.empty()) {
        for (const auto& t : ctx.targets)
            if (t.isCard()) applyTo(ctx.game.findCard(t.cardId));
    } else if (Card* def = resolveDefinedCard(s, ctx)) {
        applyTo(def);
    } else if (ctx.source) {
        applyTo(ctx.source);
    }

    if (rememberRem && totalRemoved > 0) {
        ctx.game.rememberedSizeHint   = totalRemoved;
        ctx.game.rememberedNumberHint = totalRemoved;
    }
}

// Fight — two creatures deal damage to each other simultaneously.
// One combatant is typically Defined$ Self; the other is the target.
void effectFight(const ScriptLine& s, EffectContext& ctx) {
    auto defined = s.get("Defined", "");
    Card* c1 = nullptr;
    Card* c2 = nullptr;

    if (!defined.empty() && defined != "Targeted") {
        c1 = resolveDefinedCard(s, ctx);
        if (!ctx.targets.empty() && ctx.targets[0].isCard())
            c2 = ctx.game.findCard(ctx.targets[0].cardId);
    } else if (ctx.targets.size() >= 2) {
        c1 = ctx.game.findCard(ctx.targets[0].cardId);
        c2 = ctx.game.findCard(ctx.targets[1].cardId);
    } else if (!ctx.targets.empty() && ctx.source) {
        c1 = ctx.source;
        c2 = ctx.game.findCard(ctx.targets[0].cardId);
    }

    if (!c1 || !c2 || !c1->isOnBattlefield() || !c2->isOnBattlefield()) return;

    int pow1 = effectivePower(*c1);
    int pow2 = effectivePower(*c2);

    // c1 deals damage to c2
    if (pow1 > 0 && !hasProtectionFrom(c2->keywordMask, c1->rules->manaCost.colorIdentity())) {
        if (c1->hasKeyword(KeywordAbility::Infect) || c1->hasKeyword(KeywordAbility::Wither))
            c2->addCounter("-1/-1", pow1);
        else {
            c2->markedDamage += pow1;
            if (c1->hasKeyword(KeywordAbility::Deathtouch)) c2->deathtouchDamage = true;
        }
        if (c1->hasKeyword(KeywordAbility::Lifelink))
            ctx.game.gainLife(c1->controllerId, pow1);
    }

    // c2 deals damage to c1
    if (pow2 > 0 && !hasProtectionFrom(c1->keywordMask, c2->rules->manaCost.colorIdentity())) {
        if (c2->hasKeyword(KeywordAbility::Infect) || c2->hasKeyword(KeywordAbility::Wither))
            c1->addCounter("-1/-1", pow2);
        else {
            c1->markedDamage += pow2;
            if (c2->hasKeyword(KeywordAbility::Deathtouch)) c1->deathtouchDamage = true;
        }
        if (c2->hasKeyword(KeywordAbility::Lifelink))
            ctx.game.gainLife(c2->controllerId, pow2);
    }
}

// Explore — look at the top card of your library.
// If it's a land, put it in your hand.
// Otherwise, you may put a +1/+1 counter on the exploring creature
// and optionally put the card in your GY (AI always does this).
void effectExplore(const ScriptLine& s, EffectContext& ctx) {
    Card* explorer = resolveDefinedCard(s, ctx);
    if (!explorer && !ctx.targets.empty() && ctx.targets[0].isCard())
        explorer = ctx.game.findCard(ctx.targets[0].cardId);
    if (!explorer) explorer = ctx.source;

    Player& p = ctx.game.player(ctx.controller);
    if (p.library().empty()) return;

    Card* top = p.library().front();
    if (top->isLand()) {
        ctx.game.moveToZone(top->id, ZoneType::Hand, ctx.controller);
    } else {
        if (explorer && explorer->isOnBattlefield())
            explorer->addCounter("+1/+1", 1);
        // AI heuristic: always put non-land in GY (mills for value)
        ctx.game.moveToZone(top->id, ZoneType::Graveyard, ctx.controller);
    }
}

// Shuffle — shuffle target player's library
void effectShuffle(const ScriptLine& s, EffectContext& ctx) {
    auto who = s.get("ValidPlayers", "You");
    auto shuffle = [&](uint8_t pid) {
        ctx.game.player(pid).library().shuffle(ctx.game.rng());
    };
    if      (who == "Opponent") shuffle(ctx.controller ^ 1);
    else if (who == "Each")   { shuffle(0); shuffle(1); }
    else                        shuffle(ctx.controller);
}

void effectSurveil(const ScriptLine& s, EffectContext& ctx) {
    int n = s.getInt("Amount", 1);
    Player& p = ctx.game.player(ctx.controller);

    std::vector<Card*> looked;
    for (int i = 0; i < n && !p.library().empty(); ++i) {
        Card* top = p.library().front();
        p.library().remove(top->id);
        looked.push_back(top);
    }
    // AI: keep high-value cards on top, mill low-value ones (surveil allows mill)
    int myLands = 0;
    for (const Card* c : ctx.game.battlefield().cards())
        if (c->controllerId == ctx.controller && c->isLand()) ++myLands;
    bool needLand = myLands < 3;

    // Put kept cards on top (reverse order), mill bad cards to graveyard
    for (auto it = looked.rbegin(); it != looked.rend(); ++it) {
        Card* c = *it;
        bool isLand    = c->rules->type.isLand();
        bool castSoon  = c->rules->cmc() <= myLands + 2;
        bool keep = (isLand && needLand) || (!isLand && castSoon);
        if (keep) { c->revealedToOwner = true; p.library().addToFront(c); }
        else      ctx.game.moveToZone(c->id, ZoneType::Graveyard, ctx.controller);
    }
}

void effectProliferate(const ScriptLine& s, EffectContext& ctx) {
    int times = s.getInt("Amount", 1);

    // Human player in interactive mode: defer choice to UI overlay.
    // AI: add one counter to every eligible permanent and player automatically.
    if (ctx.controller == 0 && ctx.game.isHumanInteractive()) {
        ctx.game.setPendingProliferate(times);
        return;
    }

    // AI / non-interactive: proliferate everything
    for (int t = 0; t < times; ++t) {
        for (Card* c : ctx.game.battlefield().cards()) {
            if (c->counters.empty()) continue;
            auto snapshot = c->counters;
            for (const auto& [type, count] : snapshot)
                if (count > 0) c->counters[type]++;
        }
        for (uint8_t i = 0; i < ctx.game.numPlayers(); ++i) {
            Player& pl = ctx.game.player(i);
            if (pl.poisonCounters() > 0) pl.addPoison(1);
        }
    }
}

void effectPutCounterAll(const ScriptLine& s, EffectContext& ctx) {
    std::string ctype = std::string(s.get("CounterType", "P1P1"));
    int  num          = s.getInt("CounterNum", 1);
    auto validCards   = std::string(s.get("ValidCards", "Creature"));

    std::string key;
    if      (ctype == "P1P1")    key = "+1/+1";
    else if (ctype == "M1M1")    key = "-1/-1";
    else if (ctype == "CHARGE")  key = "charge";
    else if (ctype == "LOYALTY") key = "loyalty";
    else                         key = ctype;

    // If a player is targeted, only affect that player's permanents
    std::vector<Card*> affected;
    if (!ctx.targets.empty() && ctx.targets[0].isPlayer()) {
        uint8_t pid = ctx.targets[0].playerId;
        for (Card* c : ctx.game.battlefield().cards()) {
            if (c->controllerId != pid) continue;
            if (!cardMatchesAnyFilter(*c, validCards, ctx.controller, kInvalidId, ctx.source, &ctx.game)) continue;
            affected.push_back(c);
        }
    } else {
        for (Card* c : ctx.game.battlefield().cards()) {
            if (!cardMatchesAnyFilter(*c, validCards, ctx.controller, kInvalidId, ctx.source, &ctx.game)) continue;
            affected.push_back(c);
        }
    }
    for (Card* c : affected) {
        c->addCounter(key, num);
        std::vector<PendingTrigger> t;
        TriggerSystem::onCounterAdded(*c, key, num, ctx.game, t);
        ctx.game.queueTriggers(std::move(t));
    }
}

// ── Branch (conditional SubAbility fork) ─────────────────────────────────────
// DB$ Branch | BranchConditionSVar$ X | BranchConditionSVarCompare$ GE6
//            | TrueSubAbility$ DBCopy | FalseSubAbility$ DBToken
void effectBranch(const ScriptLine& s, EffectContext& ctx) {
    auto svarName = s.get("BranchConditionSVar", "X");
    auto compareStr = s.get("BranchConditionSVarCompare", "GE1");

    // Evaluate the SVar
    int val = 0;
    if (ctx.source) {
        auto it = ctx.source->rules->svars.find(std::string(svarName));
        if (it != ctx.source->rules->svars.end()) {
            ObjectId sid = ctx.source->id;
            val = ctx.game.evaluateCountExpr(it->second, ctx.controller, sid);
        }
    }

    // Parse comparison: GE, GT, LE, LT, EQ, NEQ followed by a number
    bool condition = false;
    std::string_view op  = compareStr.substr(0, 2);
    std::string_view num = compareStr.substr(2);
    int threshold = 0;
    std::from_chars(num.data(), num.data() + num.size(), threshold);
    if      (op == "GE") condition = (val >= threshold);
    else if (op == "GT") condition = (val >  threshold);
    else if (op == "LE") condition = (val <= threshold);
    else if (op == "LT") condition = (val <  threshold);
    else if (op == "EQ") condition = (val == threshold);
    else                 condition = (val != threshold); // NEQ

    // Follow the chosen branch via SubAbility chains
    auto branchSVar = condition ? s.get("TrueSubAbility", "") : s.get("FalseSubAbility", "");
    if (branchSVar.empty() || !ctx.source) return;
    auto it = ctx.source->rules->svars.find(std::string(branchSVar));
    if (it == ctx.source->rules->svars.end()) return;
    auto branchEffect = parseScriptLine(it->second);
    if (!branchEffect.empty())
        executeEffectChain(branchEffect, ctx);
}

// ── Animate (turn a permanent into a creature until EOT) ─────────────────────
void effectAnimate(const ScriptLine& s, EffectContext& ctx) {
    int  pow = s.getInt("Power",     0);
    int  tgh = s.getInt("Toughness", 0);
    // Keywords to grant (e.g. "Haste")
    auto kwStr = std::string(s.get("Keywords", ""));
    if (kwStr.empty()) kwStr = std::string(s.get("KW", ""));

    uint32_t kwMask = 0;
    if (!kwStr.empty()) {
        std::string tok;
        kwStr += '&';
        for (char ch : kwStr) {
            if (ch == '&') {
                while (!tok.empty() && tok.back()  == ' ') tok.pop_back();
                auto f = tok.find_first_not_of(' ');
                if (f != std::string::npos) tok = tok.substr(f);
                auto kw = parseKeyword(tok);
                if (kw != KeywordAbility::None) kwMask |= static_cast<uint32_t>(kw);
                tok.clear();
            } else {
                tok += ch;
            }
        }
    }

    auto apply = [&](Card* c) {
        if (!c || !c->isOnBattlefield()) return;
        c->tempPower       += pow;
        c->tempToughness   += tgh;
        c->tempIsCreature   = true;
        if (kwMask) {
            c->tempKeywords |= kwMask;
            c->keywordMask  |= kwMask;
        }
    };

    // Apply to each target or Defined$ card
    if (!ctx.targets.empty()) {
        for (const auto& t : ctx.targets) {
            if (!t.isCard()) continue;
            apply(ctx.game.findCard(t.cardId));
        }
    } else if (Card* def = resolveDefinedCard(s, ctx)) {
        apply(def);
    }
}

// ── Connive (draw + discard, counter if nonland discarded) ────────────────────
void effectConnive(const ScriptLine& s, EffectContext& ctx) {
    int times = s.getInt("Amount", 1);
    for (int i = 0; i < times; ++i) {
        // Find the conniving creature
        Card* creature = nullptr;
        if (!ctx.targets.empty() && ctx.targets[0].isCard())
            creature = ctx.game.findCard(ctx.targets[0].cardId);
        if (!creature) creature = ctx.source;
        if (!creature) continue;

        // Draw a card
        Player& p = ctx.game.player(ctx.controller);
        if (!p.library().empty()) {
            Card* top = p.library().front();
            if (top) ctx.game.moveToZone(top->id, ZoneType::Hand, ctx.controller);
        }

        // Discard a card (AI: discard from front of hand, prefer land to keep)
        if (p.hand().empty()) continue;
        // Find a non-land to discard if possible
        Card* toDiscard = nullptr;
        for (Card* c : p.hand().cards()) {
            if (!c->isLand()) { toDiscard = c; break; }
        }
        if (!toDiscard) toDiscard = p.hand().front();
        if (!toDiscard) continue;

        bool wasNonLand = !toDiscard->isLand();
        ctx.game.moveToZone(toDiscard->id, ZoneType::Graveyard, ctx.controller);

        // If discarded a nonland → put +1/+1 counter on the conniving creature
        if (wasNonLand) {
            Card* fresh = ctx.game.findCard(creature->id);
            if (fresh) fresh->addCounter("+1/+1", 1);
        }
    }
}

// ── RearrangeTopOfLibrary (Ponder, Opt) ──────────────────────────────────────
// Looks at top N cards; AI keeps them in the same order.
// MayShuffle$ True → AI never shuffles (keeping best order).
void effectRearrangeTopOfLibrary(const ScriptLine& s, EffectContext& ctx) {
    int n = s.getInt("NumCards", 1);
    // mayShuffle ignored for AI — AI always prefers known order
    (void)s.get("MayShuffle", "False");

    // Resolve whose library (Defined$ You → controller)
    auto defined = s.get("Defined", "You");
    uint8_t pid = (defined == "Opponent") ? (ctx.controller ^ 1) : ctx.controller;
    (void)pid; // Already set — the player to look at

    // AI: no reordering — top N stay exactly as they are
    (void)n;
}

// ── Phase 14+ effects ─────────────────────────────────────────────────────────

// Investigate — create N colorless Artifact Clue tokens.
// Each Clue has "{2}, Sacrifice CARDNAME: Draw a card." modelled via an AB$ line.
// Num$ (default 1), Defined$ (player, default You)
void effectInvestigate(const ScriptLine& s, EffectContext& ctx) {
    int num = s.getInt("Num", 1);
    auto defined = s.get("Defined", "You");
    uint8_t pid = resolveDefinedPlayer(defined, ctx.controller, ctx.triggerPlayer);

    for (int i = 0; i < num; ++i) {
        // The Clue token has a mana ability encoded as an ability line.
        // GameState::createToken accepts a keyword vector; we use that to store the
        // "sacrifice: draw" note so AI can find and activate it.
        ctx.game.createToken("Clue", "Artifact Clue", 0 /*colorless*/, "0", "0", pid,
                             {"Clue", "SacrificeToDrawOne"});
    }
}

// CopyPermanent / Populate — create token copies of permanents.
// Populate: copy a creature token you control.
// Defined$ / ValidCards$ : choose what to copy.
// NumCopies$ (default 1).
void effectCopyPermanent(const ScriptLine& s, EffectContext& ctx) {
    bool populate = (s.get("Populate", "False") == "True" ||
                     s.effectType == "Populate");
    int copies = s.getInt("NumCopies", 1);

    const Card* toCopy = nullptr;

    if (populate) {
        // Find any creature token we control — pick highest P/T
        int best = -1;
        for (const Card* c : ctx.game.battlefield().cards()) {
            if (c->controllerId != ctx.controller) continue;
            if (!c->isToken || !c->isCreature()) continue;
            int score = effectivePower(*c) + effectiveToughness(*c);
            if (score > best) { best = score; toCopy = c; }
        }
    } else {
        // Pick from targets or defined filter
        if (!ctx.targets.empty() && ctx.targets[0].isCard()) {
            toCopy = ctx.game.findCard(ctx.targets[0].cardId);
        } else {
            auto validCards = s.get("ValidCards", "Creature.YouCtrl");
            int best = -1;
            for (const Card* c : ctx.game.battlefield().cards()) {
                if (!cardMatchesAnyFilter(*c, validCards, ctx.controller, kInvalidId, ctx.source, &ctx.game)) continue;
                int score = effectivePower(*c) + effectiveToughness(*c);
                if (score > best) { best = score; toCopy = c; }
            }
        }
    }

    if (!toCopy) return;

    // Build keyword list from the source card's keywords
    std::vector<std::string> kws;
    for (const auto& kw : toCopy->rules->keywords)
        kws.push_back(kw);

    for (int i = 0; i < copies; ++i) {
        Card* tok = ctx.game.createToken(
            toCopy->name(),
            toCopy->rules->type.toString(),
            toCopy->rules->manaCost.colorIdentity(),
            toCopy->rules->power,
            toCopy->rules->toughness,
            ctx.controller,
            kws);
        if (tok) {
            // Token is a full copy — share the original rules
            tok->ownedRules = std::make_shared<CardRules>(*toCopy->rules);
            tok->rules      = tok->ownedRules.get();
            tok->keywordMask     = buildKeywordMask(*tok->rules);
        }
    }
}

// Play — cast a card for free (without paying mana cost).
// Typically: DB$ Play | Defined$ ExiledCards | WithoutManaCost$ True
// AI simplification: move the card directly to the battlefield (if permanent)
// or execute its effect immediately (if spell).
void effectPlay(const ScriptLine& s, EffectContext& ctx) {
    auto defined = s.get("Defined", "");
    auto valid   = s.get("Valid", "");

    Card* c = nullptr;
    if (!defined.empty() && defined != "Targeted") {
        c = resolveDefinedCard(s, ctx);
    } else if (!ctx.targets.empty() && ctx.targets[0].isCard()) {
        c = ctx.game.findCard(ctx.targets[0].cardId);
    } else if (!valid.empty()) {
        // Valid$ + ValidZone$ form (Isochron Scepter): find a matching card in
        // the named zone (e.g. the imprinted card in exile).
        auto validZone = s.get("ValidZone", "Exile");
        ObjectId srcId = ctx.source ? ctx.source->id : kInvalidId;
        auto scan = [&](const std::vector<Card*>& cards) {
            for (Card* zc : cards)
                if (cardMatchesAnyFilter(*zc, valid, ctx.controller, srcId, ctx.source, &ctx.game))
                    { c = zc; return; }
        };
        if      (validZone == "Exile")     scan(ctx.game.exile().cards());
        else if (validZone == "Graveyard") scan(ctx.game.player(ctx.controller).graveyard().cards());
        else if (validZone == "Hand")      scan(ctx.game.player(ctx.controller).hand().cards());
    }
    if (!c) return;

    // CopyCard$ True: cast a COPY for free — execute the spell's effect but leave
    // the original where it is (Isochron keeps its imprinted card in exile).
    bool copyCard = (s.get("CopyCard", "") == "True");

    if (c->isPermanent() && !copyCard) {
        ctx.game.moveToZone(c->id, ZoneType::Battlefield, ctx.controller);
    } else {
        // Execute the (copied) spell's effect immediately.
        EffectContext playCtx{ ctx.game, c, ctx.controller, {}, 0 };
        for (const auto& raw : c->rules->abilityLines) {
            auto ab = parseScriptLine(raw);
            if (ab.abilityType == "SP" || ab.abilityType == "DB") {
                executeEffectChain(ab, playCtx);
                break;
            }
        }
        // A real cast goes to the graveyard; a copy ceases to exist (the
        // imprinted original stays in exile, unchanged).
        if (!copyCard)
            ctx.game.moveToZone(c->id, ZoneType::Graveyard, c->ownerId);
    }
}

// DigUntil — reveal cards from library until a matching card is found.
// FoundDestination$ where to put matched cards (Hand/Battlefield/Exile, default Hand).
// RevealedDestination$ where to put the rest (Graveyard/Exile/Library, default Graveyard).
// Valid$ filter string for the "found" card type. Amount$ how many to find (default 1).
// MaxRevealed$ optional cap on cards revealed.
void effectDigUntil(const ScriptLine& s, EffectContext& ctx) {
    auto validFilter     = s.get("Valid",               "Card");
    auto foundDestStr    = s.get("FoundDestination",    "Hand");
    auto revealedDestStr = s.get("RevealedDestination", "Graveyard");
    int  amount          = s.getIntOrX("Amount", ctx.xValue, 1);
    int  maxRevealed     = s.getInt("MaxRevealed", 9999);

    ZoneType foundDest = ZoneType::Hand;
    if      (foundDestStr == "Battlefield") foundDest = ZoneType::Battlefield;
    else if (foundDestStr == "Exile")       foundDest = ZoneType::Exile;
    else if (foundDestStr == "Graveyard")   foundDest = ZoneType::Graveyard;

    ZoneType revealedDest = ZoneType::Graveyard;
    if      (revealedDestStr == "Exile")   revealedDest = ZoneType::Exile;
    else if (revealedDestStr == "Library") revealedDest = ZoneType::Library;
    else if (revealedDestStr == "Hand")    revealedDest = ZoneType::Hand;

    Player& p = ctx.game.player(ctx.controller);
    std::vector<ObjectId> found, others;
    int revealed = 0;

    // Collect card ids while iterating (can't mutate mid-loop)
    std::vector<ObjectId> libCards;
    for (const Card* c : p.library().cards())
        libCards.push_back(c->id);

    for (ObjectId id : libCards) {
        if ((int)found.size() >= amount || revealed >= maxRevealed) break;
        const Card* c = ctx.game.findCard(id);
        if (!c) continue;
        ++revealed;
        if ((int)found.size() < amount &&
            cardMatchesAnyFilter(*c, validFilter, ctx.controller, kInvalidId, ctx.source, &ctx.game))
            found.push_back(id);
        else
            others.push_back(id);
    }

    uint8_t destCtrl = (foundDest == ZoneType::Battlefield) ? ctx.controller
                                                             : ctx.controller;
    for (ObjectId id : found)
        ctx.game.moveToZone(id, foundDest, destCtrl);
    for (ObjectId id : others) {
        const Card* c = ctx.game.findCard(id);
        if (c) ctx.game.moveToZone(id, revealedDest, c->ownerId);
    }

    // Always shuffle after revealing from library (per rules)
    p.library().shuffle(ctx.game.rng());
}

// Reveal — reveal cards from a player's hand or library.
// Handles RevealValid$ + RememberRevealed$ for conditional "reveal if you have X" patterns.
void effectReveal(const ScriptLine& s, EffectContext& ctx) {
    auto revealValid = std::string(s.get("RevealValid", ""));
    bool rememberRev = (s.get("RememberRevealed", "") == "True");

    if (revealValid.empty() || !rememberRev) return; // pure information only

    // Find the first matching card in the controller's hand to reveal.
    const Card* revealed = nullptr;
    for (const Card* c : ctx.game.player(ctx.controller).hand().cards()) {
        if (cardMatchesAnyFilter(*c, revealValid, ctx.controller,
                                  ctx.source ? ctx.source->id : kInvalidId,
                                  ctx.source, &ctx.game)) {
            revealed = c;
            break;
        }
    }
    if (!revealed) return; // nothing to reveal

    ctx.remembered.push_back(revealed->id);
    ctx.game.rememberedSizeHint = static_cast<int>(ctx.remembered.size());
}

// PeekAndReveal — look at top PeekAmount$ cards of a library, optionally reveal matching ones.
// RememberRevealed$/RememberPeeked$ populates ctx.remembered for SubAbility chains.
void effectPeekAndReveal(const ScriptLine& s, EffectContext& ctx) {
    auto defined = s.get("Defined", "You");
    uint8_t pid = resolveDefinedPlayer(defined, ctx.controller, ctx.triggerPlayer);
    int peekNum  = s.getInt("PeekAmount", 1);
    bool noPeek  = (s.get("NoPeek",   "False") == "True");
    bool noReveal = (s.get("NoReveal", "False") == "True");
    bool remPeeked  = (s.get("RememberPeeked",   "False") == "True");
    bool remRevealed = (s.get("RememberRevealed", "False") == "True");
    bool revOptional = (s.get("RevealOptional",  "False") == "True"); // AI always reveals
    (void)revOptional;
    auto revealValid = std::string(s.get("RevealValid", ""));

    if (noPeek) return; // nothing to look at

    // Collect top peekNum cards from the library
    std::vector<Card*> peeked;
    auto& lib = ctx.game.player(pid).library();
    int idx = 0;
    for (Card* c : lib.cards()) {
        if (idx++ >= peekNum) break;
        peeked.push_back(c);
    }

    ObjectId selfId = ctx.source ? ctx.source->id : kInvalidId;

    for (Card* c : peeked) {
        // Looking at the top of the library makes those cards known to the
        // viewer until they're drawn/moved or the library is shuffled.
        c->revealedToOwner = true;
        bool matchesReveal = revealValid.empty()
            || cardMatchesAnyFilter(*c, revealValid, pid, selfId, ctx.source, &ctx.game);
        if (remPeeked)
            ctx.remembered.push_back(c->id);
        if (!noReveal && matchesReveal && remRevealed)
            ctx.remembered.push_back(c->id);
        else if (!noReveal && matchesReveal && !remPeeked) {
            // reveal only without a separate rememberPeeked — still track it
            ctx.remembered.push_back(c->id);
        }
    }
    ctx.game.rememberedSizeHint = static_cast<int>(ctx.remembered.size());
}

// MakeCard — create a named card and place it in a zone.
// Name$ or DefinedName$ specifies which card; Zone$ where to put it.
// Conjure$ True / TokenCard$ True = create from outside the game.
void effectMakeCard(const ScriptLine& s, EffectContext& ctx) {
    // Determine card name
    std::string cardName;
    auto nameStr   = s.get("Name",        "");
    auto defName   = s.get("DefinedName", "");
    if (!nameStr.empty()) {
        cardName = std::string(nameStr);
    } else if (!defName.empty()) {
        // "TriggeredCard" / "ValidLibrary Card.TopLibrary+..." etc. — resolve
        if ((defName == "TriggeredCard" || defName == "TriggeredCardLKI"
             || defName == "TriggeredCardLKICopy") && ctx.triggeredCardId != kInvalidId) {
            const Card* tc = ctx.game.findCard(ctx.triggeredCardId);
            if (tc) cardName = tc->name();
        } else if (defName == "Targeted" && !ctx.targets.empty() && ctx.targets[0].isCard()) {
            const Card* tc = ctx.game.findCard(ctx.targets[0].cardId);
            if (tc) cardName = tc->name();
        } else if (defName.size() > 13 && defName.substr(0, 13) == "ValidLibrary ") {
            // Take the top card of the library matching the filter
            auto filter = std::string(defName.substr(13));
            for (const Card* c : ctx.game.player(ctx.controller).library().cards()) {
                if (cardMatchesAnyFilter(*c, filter, ctx.controller,
                                          ctx.source ? ctx.source->id : kInvalidId,
                                          ctx.source, &ctx.game)) {
                    cardName = c->name(); break;
                }
            }
        }
    }
    if (cardName.empty()) return;

    // Determine destination zone
    auto zoneStr = s.get("Zone", "Hand");
    ZoneType dest = ZoneType::Hand;
    if (zoneStr == "Battlefield") dest = ZoneType::Battlefield;
    else if (zoneStr == "Graveyard") dest = ZoneType::Graveyard;
    else if (zoneStr == "Library")   dest = ZoneType::Library;
    else if (zoneStr == "Exile")     dest = ZoneType::Exile;

    int amount = s.getInt("Amount", 1);
    bool tapped = (s.get("Tapped", "False") == "True");
    bool rememberMade = (s.get("RememberMade", "False") == "True");

    // Look up card rules
    const CardRules* rules = ctx.game.findRules(cardName);
    if (!rules) return; // card not in database — skip

    for (int i = 0; i < amount; ++i) {
        // Create the card object and move it to the destination
        Card* created = ctx.game.createCard(rules, ctx.controller);
        if (!created) continue;
        Card* placed = ctx.game.moveToZone(created->id, dest, ctx.controller);
        if (placed && tapped) placed->tapped = true;
        if (placed && rememberMade) ctx.remembered.push_back(placed->id);
    }
    ctx.game.rememberedSizeHint = static_cast<int>(ctx.remembered.size());
}

// LookAt — look at the top N cards of a library (AI: no action needed).
void effectLookAt(const ScriptLine& s, EffectContext& ctx) {
    (void)s; (void)ctx;
    // AI already has perfect information; nothing to do here.
}

// FlipCoin — flip a coin; follow WinSubAbility$ or LoseSubAbility$ SVar chain.
// Amount$ how many times to flip (default 1).
// WinSubAbility$ / LoseSubAbility$ → SVar names in the source card.
void effectFlipCoin(const ScriptLine& s, EffectContext& ctx) {
    int times = s.getInt("Amount", 1);
    auto winSVar  = s.get("WinSubAbility",  "");
    auto loseSVar = s.get("LoseSubAbility", "");

    for (int i = 0; i < times; ++i) {
        bool win = (ctx.game.rng()() & 1) == 0; // 50/50
        auto branchSVar = win ? winSVar : loseSVar;
        if (branchSVar.empty() || !ctx.source) continue;
        auto it = ctx.source->rules->svars.find(std::string(branchSVar));
        if (it == ctx.source->rules->svars.end()) continue;
        auto sub = parseScriptLine(it->second);
        if (!sub.empty()) executeEffectChain(sub, ctx);
    }
}

// RepeatEach — execute a SubAbility once for each matching card on the battlefield.
// RepeatCards$ filter (or RepeatPlayers for player loops).
// RepeatSubAbility$ → SVar name.
void effectRepeatEach(const ScriptLine& s, EffectContext& ctx) {
    auto repeatCards   = s.get("RepeatCards",   "");
    auto repeatPlayers = s.get("RepeatPlayers", "");
    auto definedCards  = s.get("DefinedCards",  "");
    auto repeatSVar    = s.get("RepeatSubAbility", "");
    if (repeatSVar.empty() || !ctx.source) return;

    auto it = ctx.source->rules->svars.find(std::string(repeatSVar));
    if (it == ctx.source->rules->svars.end()) return;
    auto subScript = parseScriptLine(it->second);
    if (subScript.empty()) return;

    if (!repeatCards.empty()) {
        // Collect matching card ids first (executing may change battlefield)
        std::vector<ObjectId> matching;
        for (const Card* c : ctx.game.battlefield().cards())
            if (cardMatchesAnyFilter(*c, std::string(repeatCards), ctx.controller, kInvalidId, ctx.source, &ctx.game))
                matching.push_back(c->id);

        for (ObjectId id : matching) {
            Card* c = ctx.game.findCard(id);
            if (!c || !c->isOnBattlefield()) continue;
            EffectContext repCtx   = ctx;
            repCtx.triggeredCardId = id;
            repCtx.remembered      = { id };
            repCtx.targets         = { Target::forCard(id) };
            executeEffectChain(subScript, repCtx);
        }
    } else if (!definedCards.empty()) {
        // DefinedCards$ Remembered — loop over the current remembered list
        if (definedCards == "Remembered" || definedCards == "RememberedCards") {
            auto savedRem = ctx.remembered;
            for (ObjectId id : savedRem) {
                EffectContext repCtx   = ctx;
                repCtx.triggeredCardId = id;
                repCtx.remembered      = { id };
                repCtx.targets         = { Target::forCard(id) };
                executeEffectChain(subScript, repCtx);
            }
        }
    } else if (!repeatPlayers.empty()) {
        // Resolve RepeatPlayers$ to a set of player IDs
        bool includeCtrl = false, includeOpp = false;
        if (repeatPlayers.find("Opponent") != std::string_view::npos)
            includeOpp = true;
        else if (repeatPlayers == "You" || repeatPlayers == "Self")
            includeCtrl = true;
        else
            { includeCtrl = true; includeOpp = true; } // "Each", "Both", etc.

        if (includeCtrl) {
            EffectContext repCtx = ctx;
            executeEffectChain(subScript, repCtx);
        }
        if (includeOpp) {
            EffectContext repCtx = ctx;
            repCtx.controller    = ctx.controller ^ 1;
            executeEffectChain(subScript, repCtx);
        }
    }
}

// ChooseCard — AI heuristic: pick the highest-value card matching ValidCards$ from the
// specified zone. The chosen card is appended to ctx.remembered unless RememberChosen$ False.
void effectChooseCard(const ScriptLine& s, EffectContext& ctx) {
    auto validCards    = s.get("ValidCards", s.get("DefinedCards", ""));
    auto originStr     = s.get("Origin",     s.get("ZoneChoices", "Battlefield"));
    bool rememberChosen = (s.get("RememberChosen", "True") != "False");

    if (validCards.empty()) return;

    // Parse comma-separated zone list
    std::vector<ZoneType> zones;
    {
        std::string_view zv(originStr);
        while (!zv.empty()) {
            auto comma = zv.find(',');
            std::string_view tok = (comma == std::string_view::npos) ? zv : zv.substr(0, comma);
            while (!tok.empty() && tok.front() == ' ') tok.remove_prefix(1);
            while (!tok.empty() && tok.back()  == ' ') tok.remove_suffix(1);
            if      (tok == "Graveyard") zones.push_back(ZoneType::Graveyard);
            else if (tok == "Hand")      zones.push_back(ZoneType::Hand);
            else if (tok == "Library")   zones.push_back(ZoneType::Library);
            else if (tok == "Exile")     zones.push_back(ZoneType::Exile);
            else                         zones.push_back(ZoneType::Battlefield);
            zv = (comma == std::string_view::npos) ? std::string_view{} : zv.substr(comma + 1);
        }
    }
    if (zones.empty()) zones.push_back(ZoneType::Battlefield);

    ObjectId selfId   = ctx.source ? ctx.source->id : kInvalidId;
    uint8_t  filterCtrl = ctx.controller;

    // Heuristic score: creatures by P+T, everything else by CMC
    auto scoreCard = [](const Card* c) -> int {
        if (c->isCreature()) return effectivePower(*c) + effectiveToughness(*c);
        return static_cast<int>(c->rules->manaCost.cmc());
    };

    const Card* best = nullptr;
    int bestScore = -1;

    auto consider = [&](const Card* c) {
        if (!cardMatchesAnyFilter(*c, validCards, filterCtrl, selfId, ctx.source, &ctx.game))
            return;
        int sc = scoreCard(c);
        if (sc > bestScore || best == nullptr) { bestScore = sc; best = c; }
    };

    for (ZoneType zone : zones) {
        if (zone == ZoneType::Battlefield) {
            for (const Card* c : ctx.game.battlefield().cards()) consider(c);
        } else if (zone == ZoneType::Graveyard) {
            for (uint8_t p = 0; p < 2; ++p)
                for (const Card* c : ctx.game.player(p).graveyard().cards()) consider(c);
        } else if (zone == ZoneType::Hand) {
            for (uint8_t p = 0; p < 2; ++p)
                for (const Card* c : ctx.game.player(p).hand().cards()) consider(c);
        } else if (zone == ZoneType::Library) {
            for (uint8_t p = 0; p < 2; ++p)
                for (const Card* c : ctx.game.player(p).library().cards()) consider(c);
        } else if (zone == ZoneType::Exile) {
            for (const Card* c : ctx.game.exile().cards()) consider(c);
        }
    }

    if (best && rememberChosen)
        ctx.remembered.push_back(best->id);
}

// StoreSVar — compute a value and store it in the game's dynamic SVar table.
// SVar$ key, Type$ (Count/Number/Calculate/CountSVar), Expression$ value/expression.
void effectStoreSVar(const ScriptLine& s, EffectContext& ctx) {
    auto key  = s.get("SVar",       "X");
    auto type = s.get("Type",       "Number");
    auto expr = s.get("Expression", "0");

    int value = 0;
    if (type == "Number") {
        std::from_chars(expr.data(), expr.data() + expr.size(), value);
    } else if (type == "Count" || type == "Calculate" || type == "CountSVar") {
        std::string countBody = "Count$" + std::string(expr);
        ObjectId sid = ctx.source ? ctx.source->id : kInvalidId;
        value = ctx.game.evaluateCountExpr(countBody, ctx.controller, sid);
    } else if (type == "Targeted") {
        // Sum a stat (Power/Toughness/CMC) across targeted cards
        for (const auto& t : ctx.targets) {
            if (!t.isCard()) continue;
            const Card* c = ctx.game.findCard(t.cardId);
            if (!c) continue;
            if (expr == "CardPower")     value += effectivePower(*c);
            else if (expr == "CardToughness") value += effectiveToughness(*c);
            else if (expr == "CardManaCost") value += c->rules->manaCost.cmc();
        }
    }

    // Store in dynamic SVar table (checked first in evaluateSVar)
    ctx.game.dynamicSVars[std::string(key)] = value;
}

// ChoosePlayer — AI: always choose the opponent (hostile default).
// Stores the chosen player in game.chosenPlayerHint for downstream Defined$ ChosenPlayer.
void effectChoosePlayer(const ScriptLine& s, EffectContext& ctx) {
    auto defined = s.get("Defined", "Opponent");
    // Allow explicit "You" or controller-side choices; default to opponent
    uint8_t chosen;
    if (defined == "You" || defined == "Self")
        chosen = ctx.controller;
    else
        chosen = ctx.controller ^ 1; // Opponent / default
    ctx.game.chosenPlayerHint = chosen;
}

// ChooseType — AI: choose the most common/impactful type (Creature by default).
// Stores the result in game.chosenTypeName.
void effectChooseType(const ScriptLine& s, EffectContext& ctx) {
    (void)ctx;
    auto choices = s.get("Choices", "");
    // If choices restricts to a subset, pick the first one; otherwise default Creature
    if (!choices.empty() && choices.find("Creature") == std::string_view::npos) {
        // Parse first comma-separated option
        auto sep = choices.find(',');
        ctx.game.chosenTypeName = sep == std::string_view::npos
            ? std::string(choices) : std::string(choices.substr(0, sep));
    } else {
        ctx.game.chosenTypeName = "Creature";
    }
}

// RollDice — roll an N-sided die and store the result.
// ResultSVar$ X stores the result in dynamicSVars for downstream SVar evaluation.
// ResultSubAbilities$ ranges: "1-5:SVarA,6-20:SVarB" — execute the matching SVar body.
void effectRollDice(const ScriptLine& s, EffectContext& ctx) {
    int sides = s.getInt("Sides", 6);
    if (sides < 1) sides = 6;

    int result = 1 + static_cast<int>(ctx.game.rng()() % static_cast<unsigned>(sides));

    auto resultSVar = s.get("ResultSVar", "");
    if (!resultSVar.empty())
        ctx.game.dynamicSVars[std::string(resultSVar)] = result;

    // ResultSubAbilities$ "1-9:DBLibrary,10-20:DBHand"
    auto resultSubs = s.get("ResultSubAbilities", "");
    if (!resultSubs.empty() && ctx.source) {
        std::string sv(resultSubs);
        sv += ',';
        std::string tok;
        std::string matchedSVar;
        for (char ch : sv) {
            if (ch == ',') {
                if (!tok.empty()) {
                    auto colon = tok.find(':');
                    if (colon != std::string::npos) {
                        auto range    = tok.substr(0, colon);
                        auto svarName = tok.substr(colon + 1);
                        auto dash = range.find('-');
                        int lo = 1, hi = sides;
                        if (dash != std::string::npos) {
                            std::from_chars(range.data(), range.data() + dash, lo);
                            std::from_chars(range.data() + dash + 1, range.data() + range.size(), hi);
                        } else {
                            std::from_chars(range.data(), range.data() + range.size(), lo);
                            hi = lo;
                        }
                        if (result >= lo && result <= hi) {
                            matchedSVar = svarName;
                            break;
                        }
                    }
                    tok.clear();
                }
            } else tok += ch;
        }
        if (!matchedSVar.empty()) {
            auto it = ctx.source->rules->svars.find(matchedSVar);
            if (it != ctx.source->rules->svars.end()) {
                auto sub = parseScriptLine(it->second);
                if (!sub.empty()) executeEffectChain(sub, ctx);
                return;
            }
        }
    }
}

// Repeat — conditional loop sub-ability.
// Runs RepeatSubAbility at least once, then loops while RepeatPresent$/RepeatCheckSVar$ holds.
// MaxRepeat$ caps total iterations; RepeatOptional$ (AI: always declines to avoid runaway loops).
void effectRepeat(const ScriptLine& s, EffectContext& ctx) {
    if (!ctx.source) return;
    auto repeatSVar = s.get("RepeatSubAbility", "");
    if (repeatSVar.empty()) return;
    auto it = ctx.source->rules->svars.find(std::string(repeatSVar));
    if (it == ctx.source->rules->svars.end()) return;
    auto subScript = parseScriptLine(it->second);
    if (subScript.empty()) return;

    int maxRepeat = s.getInt("MaxRepeat", 50); // safety cap for infinite-loop prevention
    auto repeatPresent = s.get("RepeatPresent", "");
    auto repeatCheckSVar = s.get("RepeatCheckSVar", "");
    auto repeatSVarCmp   = s.get("RepeatSVarCompare", "GE1");
    bool repeatOptional  = (s.get("RepeatOptional", "False") == "True");

    int count = 0;
    do {
        executeEffectChain(subScript, ctx);
        ++count;
        if (count >= maxRepeat) break;

        // RepeatPresent$ — loop while matching card count passes the compare
        if (!repeatPresent.empty()) {
            int cardCount = 0;
            for (const Card* c : ctx.game.battlefield().cards())
                if (cardMatchesAnyFilter(*c, std::string(repeatPresent), ctx.controller, kInvalidId, ctx.source, &ctx.game))
                    ++cardCount;
            auto repeatCmp = s.get("RepeatCompare", "GE1");
            std::string cmpStr(repeatCmp);
            auto op = cmpStr.substr(0, 2);
            int threshold = 1;
            if (cmpStr.size() > 2)
                std::from_chars(cmpStr.data() + 2, cmpStr.data() + cmpStr.size(), threshold);
            bool pass = (op == "GE") ? (cardCount >= threshold) :
                        (op == "GT") ? (cardCount >  threshold) :
                        (op == "LE") ? (cardCount <= threshold) :
                        (op == "LT") ? (cardCount <  threshold) :
                        (op == "EQ") ? (cardCount == threshold) : (cardCount != threshold);
            if (!pass) break;
        }

        // RepeatCheckSVar$ — loop while SVar comparison holds
        if (!repeatCheckSVar.empty()) {
            const CardRules* rules = ctx.source ? ctx.source->rules : nullptr;
            ObjectId sid = ctx.source ? ctx.source->id : kInvalidId;
            int svarVal = ctx.game.evaluateSVar(std::string(repeatCheckSVar), ctx.controller, rules, sid);
            std::string cmpStr(repeatSVarCmp);
            auto op = cmpStr.substr(0, 2);
            int threshold = 1;
            if (cmpStr.size() > 2)
                std::from_chars(cmpStr.data() + 2, cmpStr.data() + cmpStr.size(), threshold);
            bool pass = (op == "GE") ? (svarVal >= threshold) :
                        (op == "GT") ? (svarVal >  threshold) :
                        (op == "LE") ? (svarVal <= threshold) :
                        (op == "LT") ? (svarVal <  threshold) :
                        (op == "EQ") ? (svarVal == threshold) : (svarVal != threshold);
            if (!pass) break;
        }

        // RepeatOptional$ — AI always declines to avoid runaway loops
        if (repeatOptional) break;

    } while (true);
}

// Goad — the target creature is goaded: must attack if able, can't attack the goading player.
// Duration$ UntilYourNextTurn (default). Defined$/ValidTgts$ for target.
void effectGoad(const ScriptLine& s, EffectContext& ctx) {
    (void)s;
    auto apply = [&](Card* c) {
        if (!c || !c->isOnBattlefield() || !c->isCreature()) return;
        c->goaded      = true;
        c->goadedBy    = ctx.controller;
        c->mustAttack  = true;  // goad forces attacking; goadedBy prevents attacking that player
    };
    if (!ctx.targets.empty()) {
        for (const auto& t : ctx.targets)
            if (t.isCard()) apply(ctx.game.findCard(t.cardId));
    } else if (Card* def = resolveDefinedCard(s, ctx)) {
        apply(def);
    }
}

// RemoveFromCombat — remove creatures from the current combat.
// Clears attacking/blocking flags on the Card and removes the entry from the active
// CombatState (exposed via GameState::activeCombat set by TurnManager).
void effectRemoveFromCombat(const ScriptLine& s, EffectContext& ctx) {
    // Collect target cards
    std::vector<Card*> targets;
    auto defined = s.get("Defined", "");
    if (!defined.empty()) {
        if (defined == "Remembered") {
            for (ObjectId rid : ctx.remembered)
                if (Card* c = ctx.game.findCard(rid)) targets.push_back(c);
        } else if (defined == "Targeted") {
            for (const auto& t : ctx.targets)
                if (t.isCard()) if (Card* c = ctx.game.findCard(t.cardId)) targets.push_back(c);
        } else {
            if (Card* c = resolveDefinedCard(s, ctx)) targets.push_back(c);
        }
    } else {
        for (const auto& t : ctx.targets)
            if (t.isCard()) if (Card* c = ctx.game.findCard(t.cardId)) targets.push_back(c);
    }

    CombatState* cs = ctx.game.activeCombat;
    for (Card* c : targets) {
        if (!c) continue;
        if (c->attacking) {
            c->attacking = false;
            if (cs) {
                auto& attacks = cs->attacks;
                attacks.erase(std::remove_if(attacks.begin(), attacks.end(),
                    [&](const CombatState::Attack& a) { return a.attackerId == c->id; }),
                    attacks.end());
            }
        }
        if (c->blocking) {
            c->blocking = false;
            if (cs) {
                // Remove this blocker from the attacker's blocker list
                for (auto& atk : cs->attacks) {
                    auto& blks = atk.blockerIds;
                    blks.erase(std::remove(blks.begin(), blks.end(), c->id), blks.end());
                    // If the attacker now has no blockers, mark it unblocked
                    if (blks.empty()) {
                        if (Card* atker = ctx.game.findCard(atk.attackerId))
                            atker->isBlocked = false;
                    }
                }
            }
        }
    }
}

// AddTurn — grant extra turn(s) to a player.
// NumTurns$ (default 1), Defined$ (You/Opponent).
void effectAddTurn(const ScriptLine& s, EffectContext& ctx) {
    int num     = s.getInt("NumTurns", 1);
    auto defined = s.get("Defined", "You");
    uint8_t pid = resolveDefinedPlayer(defined, ctx.controller, ctx.triggerPlayer);
    ctx.game.player(pid).addExtraTurn(num);
}

// SkipTurn — mark the target player to skip their next turn.
// SkipPhase is mapped here too but treated identically (skip whole turn).
void effectSkipTurn(const ScriptLine& s, EffectContext& ctx) {
    auto defined = s.get("Defined", "You");
    uint8_t pid = resolveDefinedPlayer(defined, ctx.controller, ctx.triggerPlayer);
    ctx.game.player(pid).addSkipTurn(1);
}

// LifeSet — set a player's life total to a specific value.
// LifeAmount$ (integer), Defined$ (You/Opponent, default Opponent).
void effectLifeSet(const ScriptLine& s, EffectContext& ctx) {
    int amount  = s.getIntOrX("LifeAmount", ctx.xValue, 0);
    auto defined = s.get("Defined", "Opponent");
    uint8_t pid = resolveDefinedPlayer(defined, ctx.controller, ctx.triggerPlayer);
    ctx.game.player(pid).setLife(amount);
}

// ControlExchange — two permanents swap controllers.
// Typically one is Defined$ and the other is the target.
void effectControlExchange(const ScriptLine& s, EffectContext& ctx) {
    Card* c1 = nullptr;
    Card* c2 = nullptr;

    auto defined = s.get("Defined", "");
    if (!defined.empty() && defined != "Targeted")
        c1 = resolveDefinedCard(s, ctx);

    if (!ctx.targets.empty() && ctx.targets[0].isCard())
        c2 = ctx.game.findCard(ctx.targets[0].cardId);
    if (!ctx.targets.empty() && ctx.targets.size() >= 2 && ctx.targets[1].isCard() && !c1)
        c1 = ctx.game.findCard(ctx.targets[1].cardId);

    if (!c1 || !c2 || !c1->isOnBattlefield() || !c2->isOnBattlefield()) return;
    std::swap(c1->controllerId, c2->controllerId);
}

// ZoneExchange — swap a card's zone with another card's zone.
// Very rare; simplified to a swap of the two target cards' positions.
void effectZoneExchange(const ScriptLine& s, EffectContext& ctx) {
    (void)s;
    if (ctx.targets.size() < 2) return;
    Card* c1 = ctx.game.findCard(ctx.targets[0].cardId);
    Card* c2 = ctx.game.findCard(ctx.targets[1].cardId);
    if (!c1 || !c2) return;

    ZoneType zone1 = c1->zone;
    ZoneType zone2 = c2->zone;
    // Move each to the other's old zone
    ctx.game.moveToZone(c1->id, zone2, c1->ownerId);
    // c1's id is now invalid; c2 is still live
    ctx.game.moveToZone(c2->id, zone1, c2->ownerId);
}

// Amass — put N +1/+1 counters on an Army token you control;
// if you don't have one, create a 0/0 Army creature token first.
// Num$ (default 1), Type$ (Army subtype, default Zombie).
void effectAmass(const ScriptLine& s, EffectContext& ctx) {
    int num = s.getIntOrX("Num", ctx.xValue, 1);
    auto type = s.get("Type", "Zombie");

    // Find existing Army token controlled by us
    Card* army = nullptr;
    for (Card* c : ctx.game.battlefield().cards()) {
        if (c->controllerId != ctx.controller) continue;
        if (!c->isCreature()) continue;
        if (c->rules->type.hasSubtype("Army")) { army = c; break; }
    }

    if (!army) {
        // Create a 0/0 black Army token of the given subtype
        std::string tokenType = "Creature " + std::string(type) + " Army";
        army = ctx.game.createToken(std::string(type) + " Army", tokenType,
                                    0x08 /*black*/, "0", "0", ctx.controller, {});
    }

    if (army)
        army->addCounter("+1/+1", num);
}

// Discover — reveal cards from the top of your library until you find a nonland card
// with CMC ≤ Num$. Cast it for free or put it in your hand; rest go on the bottom.
void effectDiscover(const ScriptLine& s, EffectContext& ctx) {
    int ceiling = s.getIntOrX("Num", ctx.xValue, 1);
    Player& p   = ctx.game.player(ctx.controller);

    std::vector<ObjectId> revealed;
    ObjectId foundId = kInvalidId;

    std::vector<ObjectId> libIds;
    for (const Card* c : p.library().cards())
        libIds.push_back(c->id);

    for (ObjectId id : libIds) {
        const Card* c = ctx.game.findCard(id);
        if (!c) continue;
        revealed.push_back(id);
        if (!c->isLand() && c->rules->cmc() <= ceiling) {
            foundId = id;
            break;
        }
    }

    // Remove found card from revealed list (it goes to battlefield/hand)
    if (foundId != kInvalidId) {
        revealed.erase(std::remove(revealed.begin(), revealed.end(), foundId),
                       revealed.end());
        Card* found = ctx.game.findCard(foundId);
        if (found) {
            if (found->isPermanent())
                ctx.game.moveToZone(foundId, ZoneType::Battlefield, ctx.controller);
            else {
                // Cast the spell for free (simplified: move to GY, execute effect)
                EffectContext playCtx{ ctx.game, found, ctx.controller, {}, 0 };
                for (const auto& raw : found->rules->abilityLines) {
                    auto ab = parseScriptLine(raw);
                    if (ab.abilityType == "SP" || ab.abilityType == "DB") {
                        executeEffectChain(ab, playCtx);
                        break;
                    }
                }
                ctx.game.moveToZone(foundId, ZoneType::Graveyard, found->ownerId);
            }
        }
    }

    // The other revealed cards go to the bottom of the library
    for (ObjectId id : revealed) {
        Card* c = ctx.game.findCard(id);
        if (c) {
            p.library().remove(id);
            p.library().addToBack(c);
        }
    }
}

// Manifest — put the top card of your library face-down as a 2/2 creature.
// NumCards$ (default 1).
void effectManifest(const ScriptLine& s, EffectContext& ctx) {
    int num = s.getInt("NumCards", 1);
    Player& p = ctx.game.player(ctx.controller);

    for (int i = 0; i < num && !p.library().empty(); ++i) {
        Card* top = p.library().front();
        if (!top) break;
        Card* manifested = ctx.game.moveToZone(top->id, ZoneType::Battlefield, ctx.controller);
        if (manifested) {
            // Override as 2/2 colorless creature (the "face-down" state)
            manifested->manifested       = true;
            manifested->basePowerOverride = 2;
            manifested->baseToughOverride = 2;
        }
    }
}

// Incubate — create an Incubator token with N +1/+1 counters.
// When transformed, becomes a 0/0 Phyrexian creature.
void effectIncubate(const ScriptLine& s, EffectContext& ctx) {
    int num = s.getIntOrX("Num", ctx.xValue, 1);
    // Create an artifact Incubator token with N +1/+1 counters
    Card* tok = ctx.game.createToken("Incubator", "Artifact Incubator", 0,
                                     "0", "0", ctx.controller, {});
    if (tok && num > 0)
        tok->addCounter("+1/+1", num);
}

// Learn — reveal a Lesson card from outside the game (sideboard) or draw+discard.
// AI: just draw a card (simplified: no sideboard available).
void effectLearn(const ScriptLine& s, EffectContext& ctx) {
    (void)s;
    // AI simplification: draw a card as the "discard and draw" mode
    Player& p = ctx.game.player(ctx.controller);
    if (!p.library().empty()) {
        Card* top = p.library().front();
        ctx.game.moveToZone(top->id, ZoneType::Hand, ctx.controller);
    }
}

// MultiplyCounter — double all counters of a given type on target(s).
// CounterType$ (default P1P1).
void effectMultiplyCounter(const ScriptLine& s, EffectContext& ctx) {
    auto ctype = s.get("CounterType", "P1P1");
    std::string key;
    if      (ctype == "P1P1")    key = "+1/+1";
    else if (ctype == "M1M1")    key = "-1/-1";
    else if (ctype == "CHARGE")  key = "charge";
    else if (ctype == "LOYALTY") key = "loyalty";
    else                         key = std::string(ctype);

    auto applyTo = [&](Card* c) {
        if (!c || !c->isOnBattlefield()) return;
        auto it = c->counters.find(key);
        if (it != c->counters.end() && it->second > 0)
            it->second *= 2;
    };

    if (!ctx.targets.empty()) {
        for (const auto& t : ctx.targets)
            if (t.isCard()) applyTo(ctx.game.findCard(t.cardId));
    } else if (Card* def = resolveDefinedCard(s, ctx)) {
        applyTo(def);
    } else {
        // Apply to all matching cards on battlefield
        auto validCards = s.get("ValidCards", "Permanent.YouCtrl");
        for (Card* c : ctx.game.battlefield().cards())
            if (cardMatchesAnyFilter(*c, validCards, ctx.controller, kInvalidId, ctx.source, &ctx.game))
                applyTo(c);
    }
}

// MoveCounter — move N counters of a type from source to target.
// CounterType$, CounterNum$ (default 1), Defined$ (from card), ValidTgts$ (to card).
void effectMoveCounter(const ScriptLine& s, EffectContext& ctx) {
    auto ctype = s.get("CounterType", "P1P1");
    int  num   = s.getInt("CounterNum", 1);

    std::string key;
    if      (ctype == "P1P1")    key = "+1/+1";
    else if (ctype == "M1M1")    key = "-1/-1";
    else if (ctype == "CHARGE")  key = "charge";
    else if (ctype == "LOYALTY") key = "loyalty";
    else                         key = std::string(ctype);

    Card* from = resolveDefinedCard(s, ctx);
    if (!from) from = ctx.source;
    Card* to   = nullptr;
    if (!ctx.targets.empty() && ctx.targets[0].isCard())
        to = ctx.game.findCard(ctx.targets[0].cardId);

    if (!from || !to || !from->isOnBattlefield() || !to->isOnBattlefield()) return;

    int have = from->counterCount(key);
    int actual = std::min(have, num);
    if (actual <= 0) return;
    from->removeCounter(key, actual);
    to->addCounter(key, actual);
}

// Unattach — detach equipment from its host creature.
void effectUnattach(const ScriptLine& s, EffectContext& ctx) {
    auto apply = [&](Card* eq) {
        if (!eq || !eq->isOnBattlefield()) return;
        if (eq->attachedTo == kInvalidId) return;
        Card* host = ctx.game.findCard(eq->attachedTo);
        if (host) {
            host->attachments.erase(
                std::remove(host->attachments.begin(), host->attachments.end(), eq->id),
                host->attachments.end());
        }
        eq->attachedTo = kInvalidId;
    };

    if (!ctx.targets.empty()) {
        for (const auto& t : ctx.targets)
            if (t.isCard()) apply(ctx.game.findCard(t.cardId));
    } else if (Card* def = resolveDefinedCard(s, ctx)) {
        apply(def);
    } else if (ctx.source) {
        apply(ctx.source);
    }
}

// Protect — grant protection from a color to target(s).
// Type$ (White/Blue/Black/Red/Green/All), Duration$ (EndOfTurn/Permanent).
void effectProtect(const ScriptLine& s, EffectContext& ctx) {
    auto typeStr = s.get("Type", "");
    auto dur     = s.get("Duration", "EndOfTurn");

    KeywordAbility prot = KeywordAbility::None;
    if      (typeStr == "White") prot = KeywordAbility::ProtectionWhite;
    else if (typeStr == "Blue")  prot = KeywordAbility::ProtectionBlue;
    else if (typeStr == "Black") prot = KeywordAbility::ProtectionBlack;
    else if (typeStr == "Red")   prot = KeywordAbility::ProtectionRed;
    else if (typeStr == "Green") prot = KeywordAbility::ProtectionGreen;
    else if (typeStr == "All")   prot = KeywordAbility::ProtectionAll;

    if (prot == KeywordAbility::None) return;

    auto applyTo = [&](Card* c) {
        if (!c || !c->isOnBattlefield()) return;
        if (dur == "Permanent")
            c->grantKeyword(prot);
        else {
            c->tempKeywords |= static_cast<uint32_t>(prot);
            c->keywordMask  |= static_cast<uint32_t>(prot);
        }
    };

    if (!ctx.targets.empty()) {
        for (const auto& t : ctx.targets)
            if (t.isCard()) applyTo(ctx.game.findCard(t.cardId));
    } else if (Card* def = resolveDefinedCard(s, ctx)) {
        applyTo(def);
    }
}

// AnimateAll — animate all matching permanents (make them creatures until EOT).
// ValidCards$, Power$, Toughness$, Keywords$, Duration$.
void effectAnimateAll(const ScriptLine& s, EffectContext& ctx) {
    auto validCards = s.get("ValidCards", "Artifact.YouCtrl");
    int  pow        = s.getInt("Power",     0);
    int  tgh        = s.getInt("Toughness", 0);
    auto kwStr      = std::string(s.get("Keywords", ""));
    if (kwStr.empty()) kwStr = std::string(s.get("KW", ""));
    auto dur        = s.get("Duration", "EndOfTurn");

    uint32_t kwMask = 0;
    if (!kwStr.empty()) {
        std::string tok;
        kwStr += '&';
        for (char ch : kwStr) {
            if (ch == '&') {
                while (!tok.empty() && tok.back()  == ' ') tok.pop_back();
                auto f = tok.find_first_not_of(' ');
                if (f != std::string::npos) tok = tok.substr(f);
                auto kw = parseKeyword(tok);
                if (kw != KeywordAbility::None) kwMask |= static_cast<uint32_t>(kw);
                tok.clear();
            } else {
                tok += ch;
            }
        }
    }

    for (Card* c : ctx.game.battlefield().cards()) {
        if (!cardMatchesAnyFilter(*c, validCards, ctx.controller, kInvalidId, ctx.source, &ctx.game)) continue;
        c->tempPower     += pow;
        c->tempToughness += tgh;
        c->tempIsCreature = true;
        if (kwMask && dur != "Permanent") {
            c->tempKeywords |= kwMask;
            c->keywordMask  |= kwMask;
        }
    }
}

// DigMultiple — dig up to N cards and put specific types in hand/battlefield.
// Like DigUntil but finds multiple cards (one of each FoundType).
void effectDigMultiple(const ScriptLine& s, EffectContext& ctx) {
    // Delegate to DigUntil with Amount$ = NumCards (typically 1)
    effectDigUntil(s, ctx);
}

// Balance — each player sacrifices permanents and discards until everyone has
// as many as the player with the fewest. (Simplified: each player discards/sacrifices
// to match the minimum hand size and permanent count.)
void effectBalance(const ScriptLine& s, EffectContext& ctx) {
    (void)s;
    // Count permanents each player controls
    int count0 = 0, count1 = 0;
    for (const Card* c : ctx.game.battlefield().cards()) {
        if (c->controllerId == 0) ++count0;
        else                      ++count1;
    }
    int minPerms = std::min(count0, count1);

    // Player with more permanents sacrifices the excess (AI: sacrifice the weakest)
    for (uint8_t pid = 0; pid < 2; ++pid) {
        int& myCount = (pid == 0) ? count0 : count1;
        std::vector<ObjectId> mine;
        for (const Card* c : ctx.game.battlefield().cards())
            if (c->controllerId == pid) mine.push_back(c->id);

        // Sacrifice from weakest first (back of list — arbitrary order)
        while ((int)mine.size() > minPerms && !mine.empty()) {
            ObjectId id = mine.back(); mine.pop_back();
            const Card* c = ctx.game.findCard(id);
            if (c) ctx.game.moveToZone(id, ZoneType::Graveyard, pid);
            --myCount;
        }
    }

    // Balance hand sizes
    int hand0 = static_cast<int>(ctx.game.player(0).hand().size());
    int hand1 = static_cast<int>(ctx.game.player(1).hand().size());
    int minHand = std::min(hand0, hand1);

    for (uint8_t pid = 0; pid < 2; ++pid) {
        Player& p = ctx.game.player(pid);
        while (static_cast<int>(p.hand().size()) > minHand && !p.hand().empty()) {
            Card* c = p.hand().back();
            if (!c) break;
            ctx.game.moveToZone(c->id, ZoneType::Graveyard, pid);
        }
    }
}

// Phasing — phase out target permanent until the beginning of its controller's
// next untap step. Simplified: set phasedOut flag; TurnManager clears it.
void effectPhasing(const ScriptLine& s, EffectContext& ctx) {
    auto apply = [&](Card* c) {
        if (!c || !c->isOnBattlefield()) return;
        c->phasedOut = true;
    };
    if (!ctx.targets.empty()) {
        for (const auto& t : ctx.targets)
            if (t.isCard()) apply(ctx.game.findCard(t.cardId));
    } else if (Card* def = resolveDefinedCard(s, ctx)) {
        apply(def);
    }
}

// Vote — each player votes for an option; the option with more votes wins.
// AI simplification: execute the first option (WinOption$).
// WinOption$ / LoseOption$ → SVar names for each outcome.
void effectVote(const ScriptLine& s, EffectContext& ctx) {
    // Simplified: AI always votes for option A and resolves the A effect
    auto optA = s.get("WinOption", "");
    if (optA.empty() || !ctx.source) return;
    auto it = ctx.source->rules->svars.find(std::string(optA));
    if (it == ctx.source->rules->svars.end()) return;
    auto sub = parseScriptLine(it->second);
    if (!sub.empty()) executeEffectChain(sub, ctx);
}

// ChooseColor — player chooses a color; stored in SVar for later effects.
// AI: picks the color most represented in their permanents.
// SVar$ key to store the chosen color.
void effectChooseColor(const ScriptLine& s, EffectContext& ctx) {
    auto svarKey = s.get("SVar", "ChosenColor");
    if (!ctx.source) return;

    // AI: count colors in our battlefield permanents
    int counts[5] = {};
    for (const Card* c : ctx.game.battlefield().cards()) {
        if (c->controllerId != ctx.controller) continue;
        uint8_t col = c->rules->manaCost.colorIdentity();
        if (col & 0x01) ++counts[0]; // W
        if (col & 0x02) ++counts[1]; // U
        if (col & 0x04) ++counts[2]; // B
        if (col & 0x08) ++counts[3]; // R
        if (col & 0x10) ++counts[4]; // G
    }
    const char* colorNames[] = { "White", "Blue", "Black", "Red", "Green" };
    int best = 0;
    for (int i = 1; i < 5; ++i)
        if (counts[i] > counts[best]) best = i;

    ctx.game.chosenColorName = colorNames[best];
    (void)svarKey; // stored in game state, not as an SVar
}

// NameCard — designates a named card stored directly on the source card object.
// Used by Anointed Peacekeeper, Alhammarret, etc. whose static abilities check Card.NamedCard.
// AI heuristic: pick the card most represented in opponent's hand + graveyard.
void effectNameCard(const ScriptLine& /*s*/, EffectContext& ctx) {
    if (!ctx.source) return;

    std::unordered_map<std::string, int> freq;
    uint8_t opp = ctx.controller ^ 1;
    for (const Card* c : ctx.game.player(opp).hand().cards())      ++freq[c->name()];
    for (const Card* c : ctx.game.player(opp).graveyard().cards()) ++freq[c->name()];

    std::string chosen;
    int best = 0;
    for (const auto& [nm, cnt] : freq)
        if (cnt > best) { best = cnt; chosen = nm; }

    if (chosen.empty()) {
        // Fallback: pick any card from their library (first card we can see)
        auto& lib = ctx.game.player(opp).library();
        if (!lib.cards().empty()) chosen = lib.cards().front()->name();
    }
    ctx.source->namedCard = chosen;
}

// ChooseName / ChooseCardName — player names a card; stored in SVar.
// AI: names the most-copied type from their graveyard.
void effectChooseName(const ScriptLine& s, EffectContext& ctx) {
    auto svarKey = s.get("SVar", "ChosenName");
    if (!ctx.source) return;

    // AI: pick the card name most represented in opponent's graveyard
    std::unordered_map<std::string,int> freq;
    for (const Card* c : ctx.game.player(ctx.controller ^ 1).graveyard().cards())
        ++freq[c->name()];

    std::string chosen = "Unknown";
    int best = 0;
    for (const auto& [nm, cnt] : freq)
        if (cnt > best) { best = cnt; chosen = nm; }

    const_cast<CardRules*>(ctx.source->rules)->svars[std::string(svarKey)] =
        "String$" + chosen;
}

// TwoPiles — split cards into two piles; a player chooses which pile goes where.
// AI: split remembered/target cards ~evenly; execute SubAbility on pile1, SubAbility2 on pile2.
void effectTwoPiles(const ScriptLine& s, EffectContext& ctx) {
    if (!ctx.source) return;
    const CardRules* rules = ctx.source->rules;

    // Gather cards: prefer remembered list, fall back to target cards
    std::vector<ObjectId> cards = ctx.remembered;
    if (cards.empty()) {
        for (const auto& t : ctx.targets)
            if (t.isCard()) cards.push_back(t.cardId);
    }
    if (cards.empty()) return;

    // Split: first ceil(n/2) cards → pile1, rest → pile2
    auto mid = (cards.size() + 1) / 2;
    std::vector<ObjectId> pile1(cards.begin(), cards.begin() + mid);
    std::vector<ObjectId> pile2(cards.begin() + mid, cards.end());

    auto runPile = [&](std::string_view svarName, std::vector<ObjectId>& pile) {
        if (svarName.empty()) return;
        auto it = rules->svars.find(std::string(svarName));
        if (it == rules->svars.end()) return;
        auto sub = parseScriptLine(it->second);
        if (sub.empty()) return;
        EffectContext pileCtx = ctx;
        pileCtx.remembered = pile;
        executeEffectChain(sub, pileCtx);
    };

    auto sub1 = s.get("SubAbility", "");
    auto sub2 = s.get("SubAbility2", sub1); // if no SubAbility2, same effect on both piles
    runPile(sub1, pile1);
    runPile(sub2, pile2);
}

// LifeExchange — swap life totals between players (or player and a value).
void effectLifeExchange(const ScriptLine& s, EffectContext& ctx) {
    (void)s;
    int life0 = ctx.game.player(0).life();
    int life1 = ctx.game.player(1).life();
    ctx.game.player(0).setLife(life1);
    ctx.game.player(1).setLife(life0);
}

// Detain — target permanent can't attack or block until your next turn.
// Duration$ UntilYourNextTurn (default). Uses cantAttack/cantBlock flags.
void effectDetain(const ScriptLine& s, EffectContext& ctx) {
    (void)s;
    auto apply = [&](Card* c) {
        if (!c || !c->isOnBattlefield()) return;
        c->cantAttack = true;
        c->cantBlock  = true;
    };
    if (!ctx.targets.empty()) {
        for (const auto& t : ctx.targets)
            if (t.isCard()) apply(ctx.game.findCard(t.cardId));
    } else if (Card* def = resolveDefinedCard(s, ctx)) {
        apply(def);
    }
}

// ImmediateTrigger — execute the listed SVar effect immediately (not on the stack).
// Execute$ SVar name.
void effectImmediateTrigger(const ScriptLine& s, EffectContext& ctx) {
    auto execName = s.get("Execute", "");
    if (execName.empty() || !ctx.source) return;
    auto it = ctx.source->rules->svars.find(std::string(execName));
    if (it == ctx.source->rules->svars.end()) return;
    auto sub = parseScriptLine(it->second);
    if (!sub.empty()) executeEffectChain(sub, ctx);
}

// DelayedTrigger — register a trigger to fire at a future game event.
// Simplified: execute the SubAbility immediately (exact timing rarely matters for AI).
void effectDelayedTrigger(const ScriptLine& s, EffectContext& ctx) {
    effectImmediateTrigger(s, ctx);
}

// Helper: sacrifice one card (fire triggers, then zone-change to GY).
static void sacrificeCardHelper(Card& c, EffectContext& ctx) {
    if (!c.isOnBattlefield()) return;
    {
        std::vector<PendingTrigger> t;
        TriggerSystem::onSacrificed(c, ctx.game, t);
        ctx.game.queueTriggers(std::move(t));
    }
    ctx.game.moveToZone(c.id, ZoneType::Graveyard, c.ownerId);
}

// SacrificeAll — force all matching permanents to be sacrificed.
// ValidCards$ filter. Separated from ChangeZoneAll so scripts can use either.
void effectSacrificeAll(const ScriptLine& s, EffectContext& ctx) {
    auto validCards = s.get("ValidCards", "Creature.YouCtrl");
    std::vector<ObjectId> toSac;
    for (const Card* c : ctx.game.battlefield().cards())
        if (cardMatchesAnyFilter(*c, validCards, ctx.controller, kInvalidId, ctx.source, &ctx.game))
            toSac.push_back(c->id);
    for (ObjectId id : toSac) {
        if (Card* c = ctx.game.findCard(id))
            sacrificeCardHelper(*c, ctx);
    }
}

// DestroyAll — destroy all matching permanents (Wrath of God, Plague Wind, etc.)
// Respects Indestructible. NoRegen$ True prevents regeneration (no-op at AI scope).
void effectDestroyAll(const ScriptLine& s, EffectContext& ctx) {
    auto validCards = s.get("ValidCards", "Permanent");
    ObjectId selfId = ctx.source ? ctx.source->id : kInvalidId;
    std::vector<ObjectId> toDestroy;
    for (const Card* c : ctx.game.battlefield().cards())
        if (cardMatchesAnyFilter(*c, validCards, ctx.controller, selfId, ctx.source, &ctx.game))
            toDestroy.push_back(c->id);
    for (ObjectId id : toDestroy) {
        Card* c = ctx.game.findCard(id);
        if (!c || !c->isOnBattlefield()) continue;
        if (c->hasKeyword(KeywordAbility::Indestructible)) continue;
        ctx.game.moveToZone(id, ZoneType::Graveyard, c->ownerId);
    }
}

// AlterAttribute — set or clear a game-mechanic attribute (Suspected, Saddled, Prepared, etc.)
// Activate$ False clears the attribute; anything else (or absent) sets it.
void effectAlterAttribute(const ScriptLine& s, EffectContext& ctx) {
    auto attrStr  = s.get("Attributes", "");
    if (attrStr.empty()) return;
    bool activate = (s.get("Activate", "True") != "False");

    // Collect target cards
    std::vector<Card*> targets;
    auto defined = s.get("Defined", "");
    if (defined.size() > 6 && defined.substr(0, 6) == "Valid ") {
        // "Valid <filter>" — all matching battlefield cards
        auto filter = std::string(defined.substr(6));
        ObjectId selfId = ctx.source ? ctx.source->id : kInvalidId;
        for (Card* c : ctx.game.battlefield().cards())
            if (cardMatchesAnyFilter(*c, filter, ctx.controller, selfId, ctx.source, &ctx.game))
                targets.push_back(c);
    } else if (!defined.empty()) {
        // Resolve via Defined$ (Self, Enchanted, Remembered, etc.)
        if (defined == "Remembered") {
            for (ObjectId rid : ctx.remembered)
                if (Card* c = ctx.game.findCard(rid)) targets.push_back(c);
        } else if (defined == "Targeted") {
            for (const auto& t : ctx.targets)
                if (t.isCard()) if (Card* c = ctx.game.findCard(t.cardId)) targets.push_back(c);
        } else {
            if (Card* c = resolveDefinedCard(s, ctx)) targets.push_back(c);
        }
    } else if (!ctx.targets.empty()) {
        for (const auto& t : ctx.targets)
            if (t.isCard()) if (Card* c = ctx.game.findCard(t.cardId)) targets.push_back(c);
    } else {
        if (ctx.source) targets.push_back(ctx.source);
    }

    // Apply attribute to each target card
    for (Card* c : targets) {
        if (!c) continue;
        if (activate)
            c->attributes.insert(std::string(attrStr));
        else
            c->attributes.erase(std::string(attrStr));
    }
}

// Populate — create a token copy of a creature token you control.
void effectPopulate(const ScriptLine& s, EffectContext& ctx) {
    // Delegate to CopyPermanent with Populate$ True
    effectCopyPermanent(s, ctx);
}

// CopySpell — copy a spell on the stack.
// Simplified: executes the top stack spell's effect a second time.
void effectCopySpell(const ScriptLine& s, EffectContext& ctx) {
    (void)s;
    // Find the top of the stack zone
    if (ctx.game.stack().empty()) return;
    Card* top = ctx.game.stack().back();
    if (!top) return;

    // Re-execute the effect for the controller
    EffectContext copyCtx{ ctx.game, top, ctx.controller, ctx.targets, ctx.xValue };
    for (const auto& raw : top->rules->abilityLines) {
        auto ab = parseScriptLine(raw);
        if (ab.abilityType == "SP" || ab.abilityType == "DB") {
            executeEffectChain(ab, copyCtx);
            break;
        }
    }
}

// AddPhase — add an additional combat or main phase this turn.
// Simplified: no-op (very rare and complex to implement correctly).
void effectAddPhase(const ScriptLine& s, EffectContext& ctx) {
    (void)s; (void)ctx;
}

// MustBlock — the target creature must block the source (or Defined$ attacker) next combat.
// Sets mustBlockTarget on the blocker card; checked by AiPlayer during blocker selection.
void effectMustBlock(const ScriptLine& s, EffectContext& ctx) {
    // The attacker that must be blocked is the source card (or Defined$ card)
    ObjectId forcedAttacker = kInvalidId;
    auto defined = s.get("Defined", "");
    if (!defined.empty() && defined != "Targeted") {
        if (Card* c = resolveDefinedCard(s, ctx)) forcedAttacker = c->id;
    } else if (ctx.source) {
        forcedAttacker = ctx.source->id;
    }
    if (forcedAttacker == kInvalidId) return;

    // Apply to targeted creatures
    for (const auto& t : ctx.targets) {
        if (!t.isCard()) continue;
        Card* c = ctx.game.findCard(t.cardId);
        if (c && c->isOnBattlefield() && c->isCreature())
            c->mustBlockTarget = forcedAttacker;
    }
}

// SeekEffect — reveal cards from library until finding a matching card; put in hand.
// Same mechanics as DigUntil with FoundDestination$ Hand and RevealedDestination$ Library.
void effectSeek(const ScriptLine& s, EffectContext& ctx) {
    // Seek keeps the library order intact (no reveal)
    auto validFilter = s.get("Valid", "Card");
    int  num         = s.getInt("Num", 1);
    Player& p        = ctx.game.player(ctx.controller);

    std::vector<ObjectId> libIds;
    for (const Card* c : p.library().cards())
        libIds.push_back(c->id);

    int found = 0;
    // Search randomly (shuffle then take)
    std::shuffle(libIds.begin(), libIds.end(), ctx.game.rng());
    for (ObjectId id : libIds) {
        if (found >= num) break;
        const Card* c = ctx.game.findCard(id);
        if (c && cardMatchesAnyFilter(*c, validFilter, ctx.controller, kInvalidId, ctx.source, &ctx.game)) {
            ctx.game.moveToZone(id, ZoneType::Hand, ctx.controller);
            ++found;
        }
    }
    p.library().shuffle(ctx.game.rng());
}

// If ctx.targets is populated those cards are sacrificed directly.
// If Defined$ Self the source card sacrifices itself.
void effectSacrifice(const ScriptLine& s, EffectContext& ctx) {
    // Defined$ Self — sacrifice the source card
    if (Card* def = resolveDefinedCard(s, ctx)) {
        sacrificeCardHelper(*def, ctx);
        return;
    }

    // Pre-selected targets
    if (!ctx.targets.empty()) {
        for (const auto& t : ctx.targets) {
            if (!t.isCard()) continue;
            if (Card* c = ctx.game.findCard(t.cardId))
                sacrificeCardHelper(*c, ctx);
        }
        return;
    }

    // Auto-select: find matching permanents and sacrifice the lowest-CMC ones first
    auto validCards = s.get("SacValid", "");
    if (validCards.empty()) validCards = s.get("Valid", "Permanent.YouCtrl");
    int amount = s.getInt("Amount", 0);
    if (amount <= 0) amount = s.getIntOrX("Num", ctx.xValue, 1);

    auto definedPl = s.get("Defined", "You");
    uint8_t pid = resolveDefinedPlayer(definedPl, ctx.controller);

    std::vector<const Card*> candidates;
    for (const Card* c : ctx.game.battlefield().cards())
        if (c->controllerId == pid && cardMatchesAnyFilter(*c, validCards, pid, kInvalidId, nullptr, &ctx.game))
            candidates.push_back(c);

    // Sacrifice lowest CMC first (AI heuristic: sacrifice worst card)
    std::sort(candidates.begin(), candidates.end(), [](const Card* a, const Card* b) {
        return a->rules->cmc() < b->rules->cmc();
    });

    int n = std::min(amount, (int)candidates.size());
    std::vector<ObjectId> toSac;
    toSac.reserve(n);
    for (int i = 0; i < n; ++i) toSac.push_back(candidates[i]->id);

    for (ObjectId id : toSac) {
        if (Card* c = ctx.game.findCard(id))
            sacrificeCardHelper(*c, ctx);
    }
}

// Bolster N — put N +1/+1 counters on the creature you control with the least toughness.
// Params: Num$ / Amount$ counter count.
void effectBolster(const ScriptLine& s, EffectContext& ctx) {
    int n = s.getIntOrX("Num", ctx.xValue, 1);
    if (n <= 0) n = s.getInt("Amount", 1);

    Card* weakest = nullptr;
    int lowestTough = INT_MAX;
    for (Card* c : ctx.game.battlefield().cards()) {
        if (!c->isCreature() || c->controllerId != ctx.controller) continue;
        int tough = effectiveToughness(*c);
        if (tough < lowestTough) {
            lowestTough = tough;
            weakest = c;
        }
    }
    if (weakest) weakest->addCounter("+1/+1", n);
}

// Monstrosity N — if this card is not monstrous, put N +1/+1 counters on it
// and set monstrous. Params: Amount$ counter count.
void effectMonstrosity(const ScriptLine& s, EffectContext& ctx) {
    Card* src = resolveDefinedCard(s, ctx);
    if (!src) src = ctx.source;
    if (!src || !src->isOnBattlefield()) return;
    if (src->monstrous) return; // already monstrous — effect does nothing
    int n = s.getIntOrX("Amount", ctx.xValue, 1);
    if (n <= 0) n = s.getIntOrX("Num", ctx.xValue, 1);
    src->addCounter("+1/+1", n);
    src->monstrous = true;
    {
        std::vector<PendingTrigger> trigs;
        TriggerSystem::onBecomesMonstrous(*src, ctx.game, trigs);
        ctx.game.queueTriggers(std::move(trigs));
    }
}

// RegenerateAll — regenerate every creature matching ValidCards$ filter.
void effectRegenerateAll(const ScriptLine& s, EffectContext& ctx) {
    auto validCards = s.get("ValidCards", "Creature.YouCtrl");
    for (Card* c : ctx.game.battlefield().cards()) {
        if (!c->isCreature()) continue;
        if (!cardMatchesAnyFilter(*c, validCards, ctx.controller, kInvalidId, ctx.source, &ctx.game)) continue;
        c->markedDamage    = 0;
        c->deathtouchDamage = false;
        if (c->tapped) c->tapped = false; // regeneration taps the creature
        c->grantKeyword(KeywordAbility::Indestructible); // approximation: grant indestructible EOT
        // In Forge, regeneration is a replacement — simplified here as damage reset + tap
    }
}

// Exert — the target creature doesn't untap during its controller's next untap step.
// Params: Defined$ target card (default Self).
void effectExert(const ScriptLine& s, EffectContext& ctx) {
    // Try Defined$ first, then ctx.targets, then source
    Card* target = resolveDefinedCard(s, ctx);
    if (!target && !ctx.targets.empty()) {
        for (const auto& t : ctx.targets) {
            if (t.isCard()) { target = ctx.game.findCard(t.cardId); break; }
        }
    }
    if (!target) target = ctx.source;
    if (target && target->isOnBattlefield())
        target->exerted = true;
}

// Transform — flip a DFC to its other face.
// The card's ownedRules is replaced with a copy of the back face (or front face if already
// transformed). After transforming, keywordMask and summoning-sickness are updated.
// Zone change always reverts to original face (new Card object has transformed=false).
void effectTransform(const ScriptLine& s, EffectContext& ctx) {
    Card* c = resolveDefinedCard(s, ctx);
    if (!c) c = ctx.source;
    if (!c || !c->isOnBattlefield()) return;

    const CardRules* other = c->rules->backFace;
    if (!other) return; // not a DFC — nothing to do

    c->ownedRules  = std::make_shared<CardRules>(*other);
    c->rules       = c->ownedRules.get();
    c->transformed = !c->transformed;
    c->keywordMask = buildKeywordMask(*c->rules);
    // Summoning sickness only applies on ETB; transform mid-turn preserves attack eligibility
    if (c->rules->isCreature() && !maskHas(c->keywordMask, KeywordAbility::Haste))
        c->summoningSickness = false; // preserve, don't re-set; was already false if on BF

    ctx.game.recomputeStaticBonuses();

    // Fire "when ~ transforms" triggers on the new face
    std::vector<PendingTrigger> trgBuf;
    TriggerSystem::onTransform(*c, ctx.game, trgBuf);
    ctx.game.queueTriggers(std::move(trgBuf));
}

// SetColor / ChangeColor — override the color identity of a card.
// NewColor$ space-separated color names: "White", "Blue", "Black", "Red", "Green", "Colorless".
// Defined$ targets (Targeted, Remembered, Self, EquippedBy, etc.)
void effectSetColor(const ScriptLine& s, EffectContext& ctx) {
    auto newColorStr = s.get("NewColor", "");
    uint8_t colorMask = 0;
    {
        std::string_view sv(newColorStr);
        while (!sv.empty()) {
            auto sp = sv.find(' ');
            std::string_view tok = (sp == std::string_view::npos) ? sv : sv.substr(0, sp);
            if      (tok == "White")     colorMask |= 0x01;
            else if (tok == "Blue")      colorMask |= 0x02;
            else if (tok == "Black")     colorMask |= 0x04;
            else if (tok == "Red")       colorMask |= 0x08;
            else if (tok == "Green")     colorMask |= 0x10;
            // "Colorless" leaves colorMask at 0 — intentionally no bits set
            sv = (sp == std::string_view::npos) ? std::string_view{} : sv.substr(sp + 1);
        }
    }
    // If NewColor$ is absent or "All", restore to rules default (clear override)
    bool clearOverride = newColorStr.empty() || newColorStr == "All";

    auto apply = [&](Card* c) {
        if (!c) return;
        c->colorIdOverride = clearOverride ? 0xFF : colorMask;
    };

    auto defined = s.get("Defined", "Targeted");
    if (defined == "Targeted" || defined.empty()) {
        for (const auto& t : ctx.targets)
            if (t.isCard()) apply(ctx.game.findCard(t.cardId));
    } else if (defined == "Remembered" || defined == "RememberedLKI") {
        for (ObjectId id : ctx.remembered) apply(ctx.game.findCard(id));
    } else if (defined == "Self" && ctx.source) {
        apply(ctx.game.findCard(ctx.source->id));
    } else if (defined == "EquippedBy" || defined == "EnchantedCard" || defined == "Enchanted") {
        if (ctx.source && ctx.source->attachedTo != kInvalidId)
            apply(ctx.game.findCard(ctx.source->attachedTo));
    }
}

// BecomeMonarch — designates a player as the Monarch.
// The Monarch draws a card at the beginning of their end step.
// A player loses the Monarch status when dealt combat damage by an opponent.
void effectBecomeMonarch(const ScriptLine& s, EffectContext& ctx) {
    auto defined = s.get("Defined", "You");
    uint8_t chosen = ctx.controller;
    if (defined == "Opponent" || defined == "Opp")
        chosen = ctx.controller ^ 1;
    ctx.game.monarchPlayer = chosen;
}

// Foretell — exile a card from the controller's hand face-down as a foretold card.
// The card can be cast on future turns for its foretell cost.
// Defined$ (default: Self/source) or Targeted.
void effectForetell(const ScriptLine& s, EffectContext& ctx) {
    Card* target = nullptr;
    auto defined = s.get("Defined", "Self");
    if (defined == "Targeted") {
        for (const auto& t : ctx.targets) {
            if (t.isCard()) { target = ctx.game.findCard(t.cardId); break; }
        }
    } else if (ctx.source) {
        target = ctx.game.findCard(ctx.source->id);
    }
    if (!target || target->zone != ZoneType::Hand) return;

    int turn = ctx.game.turnNumber();
    Card* exiled = ctx.game.moveToZone(target->id, ZoneType::Exile, target->controllerId);
    if (exiled) {
        exiled->foretold       = true;
        exiled->foretoldOnTurn = turn;
    }
}

// CleanUp — used at the end of SubAbility chains; resets remembered state.
void effectCleanup(const ScriptLine& s, EffectContext& ctx) {
    if (s.get("ClearRemembered", "") == "True") {
        ctx.remembered.clear();
        ctx.game.rememberedSizeHint   = 0;
        ctx.game.rememberedNumberHint = 0;
    }
    if (s.get("ClearChosenPlayer", "") == "True")
        ctx.game.chosenPlayerHint = 255;
    if (s.get("ClearChosenType", "") == "True")
        ctx.game.chosenTypeName.clear();
    if (s.get("ClearChosenColor", "") == "True")
        ctx.game.chosenColorName.clear();
}

// ── Mutate ────────────────────────────────────────────────────────────────────
// Simplified: the mutating card merges over the target creature, giving the
// merged permanent the top card's name/P/T while keeping the target's abilities.
// Both cards share the same Card object (target gains extra keywords/oracle text).
// ── ClassLevelUp ─────────────────────────────────────────────────────────────
// Advance a Class enchantment to its next level.  Classes have levels 1–3;
// each level adds new static/triggered abilities.  The level is tracked via a
// "level" counter on the card itself.
void effectClassLevelUp(const ScriptLine& /*s*/, EffectContext& ctx) {
    Card* cls = ctx.source;
    if (!cls || !cls->isOnBattlefield()) return;
    int cur = cls->counterCount("level");
    if (cur < 3) {   // max level 3
        cls->counters["level"] = cur + 1;
        // Re-apply static bonuses now that the level changed
        ctx.game.recomputeStaticBonuses();
    }
}

void effectMeld(const ScriptLine& /*s*/, EffectContext& ctx) {
    // Meld: source card and its partner must both be on the battlefield.
    // Exile both, then create the meld result on the battlefield.
    if (!ctx.source || !ctx.source->rules->hasMeld) return;
    const std::string& partnerName = ctx.source->rules->meldPartner;
    const std::string& resultName  = ctx.source->rules->meldResult;
    if (partnerName.empty() || resultName.empty()) return;

    // Find the partner on the battlefield under the same controller
    Card* partner = nullptr;
    for (Card* c : ctx.game.battlefield().cards()) {
        if (c->controllerId == ctx.controller && c->rules->name == partnerName) {
            partner = c; break;
        }
    }
    if (!partner) return;  // partner not on BF — meld fails

    // Exile both components
    ObjectId sourceId  = ctx.source->id;
    ObjectId partnerId = partner->id;
    ctx.game.moveToZone(sourceId,  ZoneType::Exile, ctx.controller);
    ctx.game.moveToZone(partnerId, ZoneType::Exile, ctx.controller);

    // Create the meld result from CardDb
    const CardRules* resultRules = ctx.game.findRules(resultName);
    if (!resultRules) {
        // If not in DB, fall back to creating a generic "Meld Result" token
        return;
    }
    Card* result = ctx.game.createCard(resultRules, ctx.controller);
    if (result) {
        ctx.game.moveToZone(result->id, ZoneType::Battlefield, ctx.controller);
    }
}

void effectMutate(const ScriptLine& s, EffectContext& ctx) {
    if (ctx.targets.empty()) return;
    const Target& tgt = ctx.targets[0];
    if (!tgt.isCard()) return;

    Card* host = ctx.game.findCard(tgt.cardId);
    Card* top  = ctx.source;
    if (!host || !top) return;
    if (!host->isOnBattlefield()) return;
    if (host->rules->type.hasSubtype("Human")) return;  // can't mutate onto Humans

    // Determine merge position: "Bottom" = host provides name/P/T, top provides abilities only
    bool onBottom = (s.get("Where", "Top") == "Bottom");

    // Build merged rules: always take the "face" from the chosen position
    const CardRules* faceSrc  = onBottom ? host->rules : top->rules;
    const CardRules* otherSrc = onBottom ? top->rules  : host->rules;

    auto merged = std::make_shared<CardRules>(*faceSrc);
    // Absorb triggered ability lines from the other source
    for (const auto& tl : otherSrc->triggerLines)
        merged->triggerLines.push_back(tl);
    for (const auto& al : otherSrc->staticAbilityLines)
        merged->staticAbilityLines.push_back(al);
    // Keywords from both
    for (const auto& kw : otherSrc->keywords)
        merged->keywords.push_back(kw);
    merged->oracleText += "\n[Mutated: " + otherSrc->name + "]";

    // Apply merged rules to host; keep host's counters, damage, attachments
    host->ownedRules  = merged;
    host->rules       = merged.get();
    host->keywordMask = buildKeywordMask(*merged);
    host->keywordMask |= host->continuousKeywords;

    ctx.game.recomputeStaticBonuses();

    // Fire Mutate ETB triggers
    std::vector<PendingTrigger> trigs;
    TriggerSystem::onZoneChangeGlobal(*host, ZoneType::Stack, ZoneType::Battlefield,
                                      ctx.game, trigs);
    ctx.game.queueTriggers(std::move(trigs));
}

// ── DayTime ───────────────────────────────────────────────────────────────────
// Effect used by some cards to explicitly set or flip day/night state.
void effectDayTime(const ScriptLine& s, EffectContext& ctx) {
    auto mode = s.get("Mode", "");
    if (mode == "Day")   ctx.game.dayNightState = 1;
    else if (mode == "Night") ctx.game.dayNightState = 2;
    else {
        // Flip current state
        if      (ctx.game.dayNightState == 1) ctx.game.dayNightState = 2;
        else if (ctx.game.dayNightState == 2) ctx.game.dayNightState = 1;
        else                                   ctx.game.dayNightState = 1;
    }
}

// ── Take the Initiative / Venture into the Dungeon ────────────────────────────
// Simplified dungeon state: track which room the initiative holder is in.
// Full Undercity dungeon (11 rooms) — we model 4 key rooms for MVP.
static const char* kUndercityRooms[] = {
    "Lost Depths",      // 0 - start
    "Forge",            // 1 - +1/+1 counter on creature you control
    "Trap",             // 2 - opponent discards a card
    "Throne of the Dead Three",  // 3 - create 3 treasure tokens
};
constexpr int kUndercityRoomCount = 4;

void effectTakeInitiative(const ScriptLine& /*s*/, EffectContext& ctx) {
    // Gain the initiative
    ctx.game.initiativeHolder = static_cast<int8_t>(ctx.controller);
    ctx.game.initiativeRoom   = 0;  // enter Lost Depths
}

void effectVenture(const ScriptLine& /*s*/, EffectContext& ctx) {
    if (ctx.game.initiativeHolder < 0) {
        // Nobody has initiative: take it
        ctx.game.initiativeHolder = static_cast<int8_t>(ctx.controller);
        ctx.game.initiativeRoom   = 0;
        return;
    }
    // Advance through the dungeon
    int next = std::min(ctx.game.initiativeRoom + 1, kUndercityRoomCount - 1);
    ctx.game.initiativeRoom = next;
    // Apply room effect
    switch (next) {
    case 1: {   // Forge: put a +1/+1 counter on a creature you control
        Card* best = nullptr; int bestVal = -1;
        for (Card* c : ctx.game.battlefield().cards()) {
            if (c->controllerId != ctx.controller || !c->isCreature()) continue;
            int val = effectivePower(*c) + effectiveToughness(*c);
            if (val > bestVal) { bestVal = val; best = c; }
        }
        if (best) {
            best->addCounter("+1/+1", 1);
            std::vector<PendingTrigger> t;
            TriggerSystem::onCounterAdded(*best, "+1/+1", 1, ctx.game, t);
            ctx.game.queueTriggers(std::move(t));
        }
        break;
    }
    case 2: {   // Trap: opponent discards a card
        uint8_t opp = ctx.controller ^ 1;
        Player& oppP = ctx.game.player(opp);
        if (!oppP.hand().empty()) {
            Card* toDiscard = oppP.hand().back();
            if (toDiscard)
                ctx.game.moveToZone(toDiscard->id, ZoneType::Graveyard, opp);
        }
        break;
    }
    case 3: {   // Throne: create 3 Treasure tokens
        for (int i = 0; i < 3; ++i)
            ctx.game.createToken("Treasure", "Artifact Treasure", 0, "0", "0", ctx.controller);
        break;
    }
    default: break;
    }
}

} // namespace mtg
