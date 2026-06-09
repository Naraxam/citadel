#include "GameState.h"
#include "KeywordAbility.h"
#include "CardFilter.h"
#include "CardStats.h"
#include "EquipSystem.h"
#include "LayerEngine.h"
#include "ability/ScriptLine.h"
#include "ability/Effects.h"
#include "ability/EffectContext.h"
#include "ability/Target.h"
#include <charconv>
#include <iostream>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <algorithm>
#include <vector>
#include <utility>
#include <cassert>

namespace mtg {

GameState::GameState()
    : m_players{ Player{0}, Player{1}, Player{2}, Player{3} }
{}

void GameState::reset() {
    m_objects.clear();
    m_dyingObjects.clear();
    m_tokenRules.clear();
    m_ownedRulesPool.clear();
    m_pendingTriggers.clear();
    m_battlefield = Zone{ZoneType::Battlefield};
    m_exile       = Zone{ZoneType::Exile};
    m_stack       = Zone{ZoneType::Stack};
    m_command     = Zone{ZoneType::Command};
    for (uint8_t i = 0; i < 4; ++i) m_players[i] = Player{i};
    m_turnNumber       = 1;
    m_activePlayerId   = 0;
    m_priorityPlayerId = 0;
    m_nextId           = 1;
    for (int i = 0; i < 4; ++i) {
        permanentLeftBattlefieldThisTurn[i] = false;
        attackedThisTurn[i] = false;
        lifeGainedThisTurn[i] = 0;
        lifeLostThisTurn[i]   = 0;
        cantGainLife[i]       = false;
        commanderCastCount[i] = 0;
        cardsDrawnThisTurn[i] = 0;
        spellsCastByPlayer[i] = 0;
        playerDamagedThisTurn[i] = false;
    }
    kickedHint             = false;
    rememberedSizeHint     = 0;
    rememberedNumberHint   = 0;
    chosenNumberHint       = 0;
    triggerAmountHint      = 0;
    chosenPlayerHint       = 255;
    monarchPlayer          = 255;
    chosenTypeName.clear();
    chosenColorName.clear();
    permanentsEnteredThisTurn.clear();
    dynamicSVars.clear();
    m_continuousEffects.clear();
    m_effectTimestamp = 0;
}

// ── Clone (Monte Carlo / simulation) ─────────────────────────────────────────

GameState GameState::clone() const {
    GameState c;

    // --- Scalars ---
    c.m_turnNumber              = m_turnNumber;
    c.m_activePlayerId          = m_activePlayerId;
    c.m_priorityPlayerId        = m_priorityPlayerId;
    c.m_nextId                  = m_nextId;
    c.spellsCastThisTurn        = spellsCastThisTurn;
    c.spellsCastByPlayer[0]     = spellsCastByPlayer[0];
    c.spellsCastByPlayer[1]     = spellsCastByPlayer[1];
    c.creaturesDiedThisTurn     = creaturesDiedThisTurn;
    c.etbXHint                  = etbXHint;
    c.preventAllCombatDamage    = preventAllCombatDamage;
    c.preventAllDamageToPlayer  = preventAllDamageToPlayer;
    c.playerDamagedThisTurn[0]              = playerDamagedThisTurn[0];
    c.playerDamagedThisTurn[1]              = playerDamagedThisTurn[1];
    c.lifeGainedThisTurn[0]                 = lifeGainedThisTurn[0];
    c.lifeGainedThisTurn[1]                 = lifeGainedThisTurn[1];
    c.lifeLostThisTurn[0]                   = lifeLostThisTurn[0];
    c.lifeLostThisTurn[1]                   = lifeLostThisTurn[1];
    c.kickedHint                            = kickedHint;
    c.rememberedSizeHint                    = rememberedSizeHint;
    c.rememberedNumberHint                  = rememberedNumberHint;
    c.chosenNumberHint                      = chosenNumberHint;
    c.triggerAmountHint                     = triggerAmountHint;
    c.cantGainLife[0]                       = cantGainLife[0];
    c.cantGainLife[1]                       = cantGainLife[1];
    c.tempCantGainLife[0]                   = tempCantGainLife[0];
    c.tempCantGainLife[1]                   = tempCantGainLife[1];
    c.tempCantActivate[0]                   = tempCantActivate[0];
    c.tempCantActivate[1]                   = tempCantActivate[1];
    c.commanderCastCount[0]                 = commanderCastCount[0];
    c.commanderCastCount[1]                 = commanderCastCount[1];
    c.cantPlayLand[0]                       = cantPlayLand[0];
    c.cantPlayLand[1]                       = cantPlayLand[1];
    for (int i = 0; i < 4; ++i) c.extraLandPlays[i] = extraLandPlays[i];
    c.tempExtraLandPlays[0] = tempExtraLandPlays[0];
    c.tempExtraLandPlays[1] = tempExtraLandPlays[1];
    c.tempMaxHandSize[0]    = tempMaxHandSize[0];
    c.tempMaxHandSize[1]    = tempMaxHandSize[1];
    c.chosenPlayerHint                      = chosenPlayerHint;
    c.monarchPlayer                         = monarchPlayer;
    c.chosenTypeName                        = chosenTypeName;
    c.chosenColorName                       = chosenColorName;
    c.turnControllerOf[0]                    = turnControllerOf[0];
    c.turnControllerOf[1]                    = turnControllerOf[1];
    c.tempCantCast                           = tempCantCast;
    c.tempCostMods                           = tempCostMods;
    c.pendingRetarget                       = pendingRetarget;
    c.pendingControlChange                  = pendingControlChange;
    c.dynamicSVars                          = dynamicSVars;
    c.permanentLeftBattlefieldThisTurn[0]   = permanentLeftBattlefieldThisTurn[0];
    c.permanentLeftBattlefieldThisTurn[1]   = permanentLeftBattlefieldThisTurn[1];
    c.attackedThisTurn[0]                   = attackedThisTurn[0];
    c.attackedThisTurn[1]                   = attackedThisTurn[1];
    c.cardsDrawnThisTurn[0]                 = cardsDrawnThisTurn[0];
    c.cardsDrawnThisTurn[1]                 = cardsDrawnThisTurn[1];
    c.dashedCards                 = dashedCards;
    c.unearthedCards              = unearthedCards;
    c.blitzedCards                = blitzedCards;
    c.permanentsEnteredThisTurn   = permanentsEnteredThisTurn;
    c.m_pendingTriggers         = m_pendingTriggers;
    c.m_pendingSearch           = m_pendingSearch;
    c.m_pendingRiot             = m_pendingRiot;
    c.m_pendingChooseType       = m_pendingChooseType;
    c.m_pendingPayLife          = m_pendingPayLife;
    c.m_pendingFabricate        = m_pendingFabricate;
    c.m_pendingCharm            = m_pendingCharm;
    c.m_pendingDiscard          = m_pendingDiscard;
    c.m_pendingManaChoice       = m_pendingManaChoice;
    c.m_pendingScry             = m_pendingScry;
    c.m_pendingMadnessCasts     = m_pendingMadnessCasts;
    c.m_cardDb                  = m_cardDb;
    c.m_continuousEffects       = m_continuousEffects;   // plain value copy — no pointers
    c.m_effectTimestamp         = m_effectTimestamp;
    c.m_emblems                 = m_emblems;             // emblems persist for the game
    c.m_ownedRulesPool          = m_ownedRulesPool;      // shared_ptrs: clone shares ownership
    // m_humanInteractive stays false in the clone (simulation mode)

    // --- Token rules: copy list, map old address → new address ---
    std::unordered_map<const CardRules*, const CardRules*> tokenMap;
    {
        c.m_tokenRules = m_tokenRules;  // deep copy (std::list copies all elements)
        auto oi = m_tokenRules.begin();
        auto ni = c.m_tokenRules.begin();
        for (; oi != m_tokenRules.end(); ++oi, ++ni)
            tokenMap[&*oi] = &*ni;
    }

    // --- Deep copy all Card objects ---
    for (const auto& [id, card] : m_objects) {
        auto nc = std::make_unique<Card>(*card);   // value copy of Card fields
        if (nc->ownedRules) {
            // Clone/copy cards keep their ownedRules shared_ptr; re-aim raw ptr.
            nc->rules = nc->ownedRules.get();
        } else if (auto it = tokenMap.find(nc->rules); it != tokenMap.end()) {
            // Token whose rules live in m_tokenRules — remap to copied list.
            nc->rules = it->second;
        }
        // else: points into the immutable CardDb — safe to keep as-is.
        c.m_objects.emplace(id, std::move(nc));
    }

    // --- Rebuild Zone card pointers to point at new Card objects ---
    auto lookup = [&](ObjectId oid) -> Card* {
        auto it = c.m_objects.find(oid);
        return it != c.m_objects.end() ? it->second.get() : nullptr;
    };

    // Player zones + non-zone state
    for (int p = 0; p < 2; ++p) {
        m_players[p].cloneStateTo(c.m_players[p]);
        c.m_players[p].library().rebuildPointers(lookup);
        c.m_players[p].hand().rebuildPointers(lookup);
        c.m_players[p].graveyard().rebuildPointers(lookup);
    }

    // Shared zones
    c.m_battlefield.copyCardsFrom(m_battlefield);
    c.m_exile.copyCardsFrom(m_exile);
    c.m_stack.copyCardsFrom(m_stack);
    c.m_command.copyCardsFrom(m_command);
    c.m_battlefield.rebuildPointers(lookup);
    c.m_exile.rebuildPointers(lookup);
    c.m_stack.rebuildPointers(lookup);
    c.m_command.rebuildPointers(lookup);

    return c;
}

Zone& GameState::zoneOf(ZoneType type, uint8_t controllerId) {
    switch (type) {
        case ZoneType::Library:     return m_players[controllerId].library();
        case ZoneType::Hand:        return m_players[controllerId].hand();
        case ZoneType::Graveyard:   return m_players[controllerId].graveyard();
        case ZoneType::Battlefield: return m_battlefield;
        case ZoneType::Exile:       return m_exile;
        case ZoneType::Stack:       return m_stack;
        case ZoneType::Command:     return m_command;
    }
    return m_battlefield; // unreachable
}

const Zone& GameState::zoneOf(ZoneType type, uint8_t controllerId) const {
    return const_cast<GameState*>(this)->zoneOf(type, controllerId);
}

Card* GameState::createCard(const CardRules* rules, uint8_t ownerId) {
    if (!rules) return nullptr;
    auto card       = std::make_unique<Card>();
    card->id        = allocId();
    card->rules         = rules;
    card->originalRules = rules;
    card->ownerId   = ownerId;
    card->controllerId = ownerId;
    card->zone      = ZoneType::Library;

    Card* ptr = card.get();
    m_objects.emplace(ptr->id, std::move(card));
    m_players[ownerId].library().addToBack(ptr);
    return ptr;
}

Card* GameState::findCard(ObjectId id) noexcept {
    auto it = m_objects.find(id);
    return it != m_objects.end() ? it->second.get() : nullptr;
}

const Card* GameState::findCard(ObjectId id) const noexcept {
    auto it = m_objects.find(id);
    return it != m_objects.end() ? it->second.get() : nullptr;
}

void GameState::removeFromCurrentZone(const Card& card) {
    switch (card.zone) {
        case ZoneType::Battlefield: m_battlefield.remove(card.id); break;
        case ZoneType::Exile:       m_exile.remove(card.id);       break;
        case ZoneType::Stack:       m_stack.remove(card.id);       break;
        case ZoneType::Command:     m_command.remove(card.id);     break;
        default:
            m_players[card.controllerId].zoneByType(card.zone)->remove(card.id);
            break;
    }
}

// ── Replacement effect helpers ────────────────────────────────────────────────

// Map a Forge zone name string to ZoneType (for R:Event$ Moved ReplaceWith$ redirects).
static ZoneType zoneTypeFromName(std::string_view name) {
    if (name == "Exile")       return ZoneType::Exile;
    if (name == "Hand")        return ZoneType::Hand;
    if (name == "Graveyard")   return ZoneType::Graveyard;
    if (name == "Library")     return ZoneType::Library;
    if (name == "Battlefield") return ZoneType::Battlefield;
    return ZoneType::Graveyard; // fallback
}

// Check one replacement line for a zone-redirect (Destination$ X → ReplaceWith$ Y).
// isSelfReplacement: true when the replacement line belongs to the moving card itself.
// Returns true if the destination should be changed to newDest.
static bool checkZoneRedirect(const std::string& rline, const Card& moving,
                               uint8_t sourceControllerId, ZoneType from, ZoneType dest,
                               ZoneType& newDest, bool isSelfReplacement) {
    auto s = parseScriptLine(rline);
    // Only handle R:Event$ Moved lines
    if (s.effectType != "Moved" && s.effectType != "ZoneChange") return false;
    // Origin check (optional)
    auto origin = s.get("Origin", "");
    if (!origin.empty() && origin != "Any" && origin != zoneName(from)) return false;
    // Destination must match (empty = any destination)
    auto destParam = s.get("Destination", "");
    if (!destParam.empty() && destParam != zoneName(dest)) return false;
    // ValidCard / ValidLKI check
    auto validCard = s.get("ValidCard", "");
    auto validLKI  = s.get("ValidLKI",  "");
    auto checkStr  = !validCard.empty() ? validCard : validLKI;
    if (!checkStr.empty()) {
        if (checkStr.find("Self") != std::string_view::npos) {
            // "Card.Self" means the replacement applies only to the card that owns it
            if (!isSelfReplacement) return false;
        } else if (!cardMatchesAnyFilter(moving, checkStr, sourceControllerId,
                                           kInvalidId, nullptr, nullptr)) {
            return false;
        }
    }
    // ReplaceWith must be a zone name (redirect type), not an SVar name
    auto replaceWith = s.get("ReplaceWith", "");
    if (replaceWith.empty()) return false;
    // Only treat as a zone redirect if the value looks like a zone name
    if (replaceWith == "Exile" || replaceWith == "Hand" ||
        replaceWith == "Library" || replaceWith == "Graveyard") {
        newDest = zoneTypeFromName(replaceWith);
        return true;
    }
    return false;
}

// Compute the final destination zone after applying any zone-redirect replacement effects.
// Checks: (1) the moving card's own R: lines, (2) all battlefield cards' R: lines,
//         (3) built-in Madness redirect (Hand→GY becomes Hand→Exile).
static ZoneType computeZoneRedirect(const Card& moving, ZoneType from, ZoneType dest,
                                     const Zone& battlefield) {
    // Self-replacement on the moving card ("Card.Self" lines)
    for (const auto& rline : moving.rules->replacementLines) {
        ZoneType newDest = dest;
        if (checkZoneRedirect(rline, moving, moving.controllerId, from, dest, newDest,
                               /*isSelfReplacement=*/true))
            return newDest;
    }
    // Global replacement from battlefield cards (other cards watching zone changes)
    for (const Card* src : battlefield.cards()) {
        if (src->id == moving.id) continue;
        for (const auto& rline : src->rules->replacementLines) {
            ZoneType newDest = dest;
            if (checkZoneRedirect(rline, moving, src->controllerId, from, dest, newDest,
                                   /*isSelfReplacement=*/false))
                return newDest;
        }
    }
    // Madness: discard from hand goes to exile instead of graveyard
    if (moving.rules->hasMadness && from == ZoneType::Hand && dest == ZoneType::Graveyard)
        return ZoneType::Exile;
    return dest;
}

// Forward declaration — evaluateCountSVar is defined later in this file.
static int evaluateCountSVar(std::string_view expr, const GameState& game,
                              uint8_t controller, ObjectId selfId = kInvalidId);

// Execute ETB replacement effects on a card that just entered the battlefield.
// Handles "enters tapped", "enters with counters", and similar state-modification replacements.
static void applyETBReplacements(Card* ptr, uint8_t controllerId, GameState& game) {
    const CardRules* rules = ptr->rules;

    // Helper: execute one ReplaceWith SVar effect on the entering card
    auto executeReplacement = [&](const CardRules* ownerRules, std::string_view replaceWith,
                                  uint8_t sourceControllerId) {
        // "ETBTapped" can be resolved directly (also handles the common SVar form)
        if (replaceWith == "ETBTapped") {
            ptr->tapped = true;
            return;
        }
        auto it = ownerRules->svars.find(std::string(replaceWith));
        if (it == ownerRules->svars.end()) return;
        auto effect = parseScriptLine(it->second);
        if (effect.empty()) return;
        // Source = entering card; target = entering card (Defined$ Self resolves correctly)
        EffectContext ctx{ game, ptr, sourceControllerId, { Target::forCard(ptr->id) }, 0, ptr->id };
        executeEffect(effect, ctx);
    };

    // Check the entering card's own ETB replacement lines
    for (const auto& rline : rules->replacementLines) {
        auto s = parseScriptLine(rline);
        if (s.effectType != "Moved" && s.effectType != "ZoneChange") continue;
        auto destParam = s.get("Destination", "");
        if (destParam != "Battlefield") continue;
        // Must be a state-modification ("Updated") or no result type specified
        auto result = s.get("ReplacementResult", "Updated");
        if (result != "Updated") continue;
        // ValidCard must match self (only process self-replacements here)
        auto validCard = s.get("ValidCard", "Card.Self");
        if (validCard.find("Self") == std::string_view::npos &&
            validCard.find("Card.Self") == std::string_view::npos) continue;
        auto replaceWith = s.get("ReplaceWith", "");
        if (replaceWith.empty()) continue;
        executeReplacement(rules, replaceWith, controllerId);
    }

    // Check all other battlefield cards for global ETB replacements
    // (e.g., "nonbasic lands enter tapped" from Magus of the Moon)
    for (const Card* watcher : game.battlefield().cards()) {
        if (watcher->id == ptr->id) continue;
        for (const auto& rline : watcher->rules->replacementLines) {
            auto s = parseScriptLine(rline);
            if (s.effectType != "Moved" && s.effectType != "ZoneChange") continue;
            auto destParam = s.get("Destination", "");
            if (destParam != "Battlefield") continue;
            auto result = s.get("ReplacementResult", "Updated");
            if (result != "Updated") continue;
            // ValidCard must match the entering card
            auto validCard = s.get("ValidCard", "");
            if (!validCard.empty() && !cardMatchesAnyFilter(*ptr, validCard, watcher->controllerId,
                                                               watcher->id, watcher, &game))
                continue;
            auto replaceWith = s.get("ReplaceWith", "");
            if (replaceWith.empty()) continue;
            executeReplacement(watcher->rules, replaceWith, watcher->controllerId);
        }
    }

    // K:etbCounter — apply N counters of the specified type when entering the battlefield
    for (const auto& entry : rules->etbCounters) {
        std::string key;
        if      (entry.counterType == "P1P1")   key = "+1/+1";
        else if (entry.counterType == "M1M1")   key = "-1/-1";
        else if (entry.counterType == "CHARGE")  key = "charge";
        else if (entry.counterType == "LOYALTY") key = "loyalty";
        else                                     key = entry.counterType;

        // Try to parse as a plain integer first
        int amount = 0;
        auto [p2, ec] = std::from_chars(entry.amountSVar.data(),
                                         entry.amountSVar.data() + entry.amountSVar.size(), amount);
        if (ec != std::errc{}) {
            // Not a plain number: treat as SVar name and evaluate
            auto it = rules->svars.find(entry.amountSVar);
            if (it != rules->svars.end())
                amount = evaluateCountSVar(it->second, game, controllerId, ptr->id);
        }
        if (amount > 0)
            ptr->addCounter(key, amount);
    }

    // K:Bloodthirst N — enter with N +1/+1 counters if an opponent was dealt damage this turn
    if (rules->hasBloodthirst && rules->bloodthirstAmount > 0) {
        if (game.playerDamagedThisTurn[controllerId ^ 1])
            ptr->addCounter("+1/+1", rules->bloodthirstAmount);
    }

    // K:Riot — ETB choice: haste or +1/+1 counter.
    // Human player defers via pending UI; AI always takes haste.
    if (rules->hasRiot) {
        if (controllerId == 0 && game.isHumanInteractive()) {
            game.setPendingRiot(ptr->id);
        } else {
            ptr->tempKeywords     |= static_cast<uint32_t>(KeywordAbility::Haste);
            ptr->keywordMask      |= static_cast<uint32_t>(KeywordAbility::Haste);
            ptr->summoningSickness = false;
        }
    }

    // K:Tribute N — opponent may put N +1/+1 counters on the entering creature.
    // Human opponent gets a choice overlay; AI always pays (prevents the bonus).
    if (rules->hasTribute && rules->tributeAmount > 0) {
        uint8_t opponent = controllerId ^ 1;
        if (game.isHumanInteractive() && opponent == 0) {
            // Human opponent chooses — defer to GameWindow overlay
            game.setPendingTribute(ptr->id, rules->tributeAmount, opponent);
        } else {
            // AI: always pays tribute to prevent the "not paid" bonus
            ptr->addCounter("+1/+1", rules->tributeAmount);
            ptr->tributed = true;
        }
    }

    // K:Champion — ETB exile a matching creature you control; track it on the card.
    if (rules->hasChampion && !rules->championFilter.empty()) {
        Card* victim = nullptr;
        for (Card* c : game.battlefield().cards()) {
            if (c->id == ptr->id || c->controllerId != controllerId || !c->isCreature()) continue;
            if (cardMatchesAnyFilter(*c, rules->championFilter, controllerId, ptr->id, ptr, &game)) {
                victim = c;
                break;
            }
        }
        if (victim) {
            Card* exiled = game.moveToZone(victim->id, ZoneType::Exile, controllerId);
            if (exiled) ptr->championedCreature = exiled->id;
        }
    }

    // K:ETBReplacement:Copy — Clone-type cards enter as a copy of a chosen permanent.
    if (rules->etbCopy.has_value()) {
        const auto& copyEntry = *rules->etbCopy;
        auto svarIt = rules->svars.find(copyEntry.svarName);
        if (svarIt != rules->svars.end()) {
            auto cloneLine      = parseScriptLine(svarIt->second);
            auto choicesFilter  = std::string(cloneLine.get("Choices", "Creature"));

            // AI: copy the highest-P/T creature matching the filter
            const Card* bestTarget = nullptr;
            int bestScore = -1;
            for (const Card* c : game.battlefield().cards()) {
                if (c->id == ptr->id) continue;
                if (!cardMatchesAnyFilter(*c, choicesFilter, controllerId, ptr->id, ptr, &game)) continue;
                int score = effectivePower(*c) + effectiveToughness(*c);
                if (score > bestScore) { bestScore = score; bestTarget = c; }
            }

            if (bestTarget) {
                ptr->ownedRules = std::make_shared<CardRules>(*bestTarget->rules);
                ptr->rules      = ptr->ownedRules.get();
                ptr->keywordMask     = buildKeywordMask(*ptr->rules);
                ptr->summoningSickness = ptr->rules->isCreature()
                    && !maskHas(ptr->keywordMask, KeywordAbility::Haste);
            }
        }
    }

    // K:ETBReplacement:Other — "As CARDNAME enters, choose a creature type"
    // (Herald's Horn, etc.). Run the named SVar when it's a creature ChooseType:
    // prompt the human with a focused list of types from their cards, or
    // auto-pick the most prominent creature type for the AI. The result is
    // stored on ptr->chosenType and read by the Creature.ChosenType filter.
    if (!rules->etbOtherSVar.empty()) {
        auto svIt = rules->svars.find(rules->etbOtherSVar);
        if (svIt != rules->svars.end()) {
            auto line = parseScriptLine(svIt->second);
            if (line.effectType == "ChooseType" &&
                line.get("Type", "Creature") == "Creature") {
                // Tally creature subtypes across the controller's own cards so
                // the choice list is short and relevant.
                std::unordered_map<std::string,int> tally;
                auto tallyCard = [&](const Card* cc) {
                    if (!cc || !cc->rules || cc->isToken) return;
                    if (!cc->rules->type.isCreature()) return;
                    for (const auto& sub : cc->rules->type.subtypes) ++tally[sub];
                };
                const Player& pl = game.player(controllerId);
                for (const Card* cc : pl.library().cards())   tallyCard(cc);
                for (const Card* cc : pl.hand().cards())       tallyCard(cc);
                for (const Card* cc : pl.graveyard().cards())  tallyCard(cc);
                for (const Card* cc : game.battlefield().cards())
                    if (cc && cc->controllerId == controllerId) tallyCard(cc);

                // Sort candidate types by frequency (desc), then name.
                std::vector<std::pair<std::string,int>> ranked(tally.begin(), tally.end());
                std::sort(ranked.begin(), ranked.end(),
                    [](const auto& a, const auto& b) {
                        return a.second != b.second ? a.second > b.second : a.first < b.first;
                    });

                if (controllerId == 0 && game.isHumanInteractive()) {
                    std::vector<std::string> opts;
                    for (auto& r : ranked) { opts.push_back(r.first); if (opts.size() >= 12) break; }
                    // Always offer a few staples so the player can pick even with
                    // an empty/creatureless deck on the battlefield.
                    for (const char* def : {"Human","Soldier","Goblin","Elf","Zombie","Dragon","Angel","Wizard"}) {
                        if (opts.size() >= 12) break;
                        if (std::find(opts.begin(), opts.end(), def) == opts.end())
                            opts.emplace_back(def);
                    }
                    game.setPendingChooseType(ptr->id, std::move(opts));
                } else {
                    // AI: MostProminentInComputerDeckNonToken → top tallied type.
                    if (!ranked.empty()) {
                        ptr->chosenType      = ranked.front().first;
                        game.chosenTypeName  = ranked.front().first;
                    }
                }
            }
        }
    }

    // K:Fabricate N — put N +1/+1 counters on self, or create N 1/1 colorless Servo tokens.
    // Human player defers via pending UI; AI always takes counters (simpler evaluation).
    if (rules->hasFabricate && rules->fabricateAmount > 0) {
        if (controllerId == 0 && game.isHumanInteractive()) {
            game.setPendingFabricate(ptr->id, rules->fabricateAmount);
        } else {
            ptr->addCounter("+1/+1", rules->fabricateAmount);
        }
    }

    // K:Exploit — on ETB, the controller may sacrifice a creature.
    // If they do, a bonus trigger fires (the card's T: line with Mode$Exploit checks this flag).
    // AI always exploits if there's a non-commander sacrifice target available.
    if (rules->hasExploit && ptr->isCreature()) {
        bool shouldExploit = false;
        if (game.isHumanInteractive() && controllerId == 0) {
            // Human: queue a pending exploit choice (similar to Fabricate)
            game.setPendingExploit(ptr->id, controllerId);
        } else {
            // AI: exploit the worst creature we control (lowest P+T, not commander, not this card)
            Card* worst = nullptr;
            int   worstScore = INT_MAX;
            for (Card* c : game.battlefield().cards()) {
                if (c->controllerId != controllerId) continue;
                if (c->id == ptr->id) continue;
                if (c->isCommander || !c->isCreature()) continue;
                int score = effectivePower(*c) + effectiveToughness(*c);
                if (score < worstScore) { worstScore = score; worst = c; }
            }
            if (worst) {
                game.moveToZone(worst->id, ZoneType::Graveyard, controllerId);
                shouldExploit = true;
            }
        }
        if (shouldExploit) ptr->exploited = true;
    }

    // K:Living Weapon — Equipment enters with a 0/0 black Phyrexian Germ creature token
    // attached to it. The token is created first, then the Equipment is attached.
    if (rules->hasKeyword("Living Weapon") && rules->type.hasSubtype("Equipment")) {
        Card* germ = game.createToken("Phyrexian Germ", "Creature Phyrexian Germ",
                                      static_cast<uint8_t>(ManaAtom::BLACK),
                                      "0", "0", controllerId);
        if (germ)
            attachEquipment(*ptr, *germ, game);
    }

    // Sagas: on ETB, immediately add the first lore counter and fire chapter I.
    // NOTE: triggerSagaChapter may sacrifice ptr (1-chapter sagas) — keep as last block.
    if (rules->saga.has_value())
        game.triggerSagaChapter(ptr);
}

Card* GameState::moveToZone(ObjectId oldId, ZoneType destination, uint8_t controllerId) {
    auto it = m_objects.find(oldId);
    if (it == m_objects.end()) return nullptr;

    Card* old = it->second.get();

    // Commander replacement effect: commanders return to the command zone instead of GY/Exile.
    bool isCommanderCard = old->isCommander;
    if (isCommanderCard &&
        (destination == ZoneType::Graveyard || destination == ZoneType::Exile))
        destination = ZoneType::Command;

    // Zone-redirect replacement effects (e.g. Rest in Peace → exile instead of GY)
    // Skip for commander redirects that already resolved to Command.
    if (destination != ZoneType::Command || !isCommanderCard)
        destination = computeZoneRedirect(*old, old->zone, destination, m_battlefield);

    // Snapshot what we need before invalidating old
    // Revert to original CardDb face: clones/transforms reset when changing zones.
    const CardRules* rules    = old->originalRules ? old->originalRules : old->rules;
    uint8_t          ownerId  = old->ownerId;
    uint8_t          oldCtrl  = old->controllerId;
    ZoneType         oldZone  = old->zone;  // saved for trigger firing

    // Revolt tracking: a permanent leaving the battlefield sets the flag for its controller.
    if (oldZone == ZoneType::Battlefield && old->isPermanent())
        permanentLeftBattlefieldThisTurn[oldCtrl] = true;

    // Morph/Manifest reveal (rule 707.9): when a face-down creature leaves the battlefield,
    // its true identity is revealed to all players. Log the reveal so both players know.
    if (oldZone == ZoneType::Battlefield && old->isFaceDown && old->rules) {
        // The real name is in originalRules (the rules pointer it was created from)
        const CardRules* trueRules = old->originalRules ? old->originalRules : old->rules;
        // We can't directly write to the game log here, but mark it in a game-state field
        // so GameWindow can pick it up. Using a simple pending reveal string.
        m_pendingReveal = trueRules->name;
    }

    // Equipment leaving the battlefield: detach from the equipped creature so
    // the creature's bonusPower/Toughness/Keywords are removed immediately.
    if (oldZone == ZoneType::Battlefield &&
        old->rules->type.hasSubtype("Equipment") &&
        old->attachedTo != kInvalidId) {
        detachEquipment(*old, *this);
    }

    // Remove from current zone
    removeFromCurrentZone(*old);

    // Per MTG rules, a zone change creates a new object
    auto newCard              = std::make_unique<Card>();
    newCard->id               = allocId();
    newCard->rules            = rules;
    newCard->originalRules    = rules;
    newCard->ownerId          = ownerId;
    newCard->controllerId     = controllerId;
    newCard->zone             = destination;
    newCard->keywordMask      = buildKeywordMask(*rules);

    newCard->isCommander = isCommanderCard;

    bool enteringBF = (destination == ZoneType::Battlefield);
    // Summoning sickness for creatures (cleared on their next untap)
    newCard->summoningSickness = enteringBF && rules->isCreature()
        && !maskHas(newCard->keywordMask, KeywordAbility::Haste);

    // Initialise loyalty counter for planeswalkers
    if (enteringBF && rules->type.isPlaneswalker() && rules->initialLoyalty > 0)
        newCard->counters["loyalty"] = rules->initialLoyalty;

    Card* ptr = newCard.get();
    ptr->zoneChangeSeq = ++m_zoneSeq;  // stamp chronological order
    m_objects.emplace(ptr->id, std::move(newCard));

    // Before retiring the old card, preserve any ownedRules in the game-level
    // pool. This prevents use-after-free when ctx.source->rules is accessed
    // after the card has been moved (e.g. during SubAbility$ chain execution).
    // The pool entries live for the game's lifetime and are never freed early.
    //
    // We then MOVE the old card's unique_ptr into m_dyingObjects rather than
    // freeing it. Any in-flight iteration that snapshotted m_battlefield (or
    // any other zone) still holds raw Card* pointers to this object; freeing
    // it here would turn those into dangling pointers and crash the next time
    // they are dereferenced. The dying card stays alive — only its zone entry
    // is gone — until the game ends and reset() drops the pool.
    {
        auto ownedIt = m_objects.find(oldId);
        if (ownedIt != m_objects.end()) {
            if (ownedIt->second->ownedRules)
                m_ownedRulesPool.push_back(ownedIt->second->ownedRules);
            m_dyingObjects.push_back(std::move(ownedIt->second));
        }
    }
    // Remove the map entry by key — emplace above may have rehashed the map,
    // invalidating the iterator 'it'. The unique_ptr is now empty (moved-from).
    m_objects.erase(oldId);

    // Add to destination zone
    zoneOf(destination, controllerId).add(ptr);

    // Capture id/type info before applyETBReplacements since a Saga that auto-sacrifices
    // on ETB (1-chapter saga) will recursively call moveToZone, destroy this Card object,
    // and make ptr dangling.  All subsequent uses go through newId/newIsPermanent or
    // re-fetch the card with findCard(newId).
    const ObjectId newId          = ptr->id;
    const bool     newIsPermanent = ptr->isPermanent();

    // ETB state-modification replacement effects (enters tapped, enters with counters, etc.)
    // Run BEFORE triggers fire so the card's state is correct when triggers resolve.
    if (enteringBF)
        applyETBReplacements(ptr, controllerId, *this);
    // ptr may now be dangling — use newId / findCard(newId) from here on.

    // Madness: card landed in Exile because it was discarded from Hand — queue cast decision
    if (destination == ZoneType::Exile && oldZone == ZoneType::Hand && rules->hasMadness)
        queueMadnessCast(newId, controllerId);

    // Track creature deaths for Morbid condition
    if (oldZone == ZoneType::Battlefield && destination == ZoneType::Graveyard && rules->isCreature())
        ++creaturesDiedThisTurn;

    // Track permanents entering the battlefield for Count$ThisTurnEntered_Battlefield_<filter>
    if (enteringBF && newIsPermanent) {
        permanentsEnteredThisTurn.push_back(newId);
        // Update City's Blessing: granted once a player controls 10+ permanents simultaneously
        checkCityBlessing(controllerId);
    }

    // Fire zone-change triggers for the card itself and any battlefield watchers.
    // Re-fetch ptr in case applyETBReplacements moved it; skip triggers if the card
    // is no longer in its expected zone (it was immediately re-moved by Saga/Evoke).
    Card* livePtr = findCard(newId);
    if (!livePtr) {
        // Card was moved again inside applyETBReplacements (e.g. 1-chapter Saga sacrifice).
        // Zone-change triggers for the original move already fired inside the recursive
        // moveToZone call; skip them here to avoid double-firing.
        if (destination == ZoneType::Battlefield || oldZone == ZoneType::Battlefield ||
            destination == ZoneType::Hand        || oldZone == ZoneType::Hand        ||
            destination == ZoneType::Graveyard   || oldZone == ZoneType::Graveyard) {
            recomputeStaticBonuses();
        }
        return nullptr;
    }

    // Snapshot the queue size so we can identify the just-added triggers below.
    size_t trigsBefore = m_pendingTriggers.size();
    TriggerSystem::onZoneChange(*livePtr, oldZone, destination, *this, m_pendingTriggers);
    if (destination == ZoneType::Battlefield || oldZone == ZoneType::Battlefield) {
        TriggerSystem::onZoneChangeGlobal(*livePtr, oldZone, destination,
                                          *this, m_pendingTriggers);
    }

    // Mode$ Panharmonicon — duplicate matching ETB/zone-change triggers.
    if (enteringBF && m_pendingTriggers.size() > trigsBefore) {
        for (const Card* bf : m_battlefield.cards()) {
            for (const auto& raw : bf->rules->staticAbilityLines) {
                auto s = parseScriptLine(raw);
                if (s.get("Mode", "") != "Panharmonicon") continue;

                // ValidCard$ — which permanent's triggers are doubled (usually Permanent.YouCtrl).
                auto validCard = s.get("ValidCard", "");
                // ValidCause$ — the entering card must match this filter (e.g. Land).
                auto validCause = s.get("ValidCause", "");
                if (!validCause.empty() &&
                    !cardMatchesAnyFilter(*livePtr, std::string(validCause),
                                          bf->controllerId, bf->id))
                    continue;

                // Collect triggers to duplicate (avoid modifying vector while iterating).
                std::vector<PendingTrigger> dups;
                for (size_t i = trigsBefore; i < m_pendingTriggers.size(); ++i) {
                    const PendingTrigger& pt = m_pendingTriggers[i];
                    const Card* trigSrc = findCard(pt.sourceCardId);
                    if (!trigSrc) continue;
                    // The trigger source must match ValidCard$ on the Panharmonicon ability.
                    if (!validCard.empty() &&
                        !cardMatchesAnyFilter(*trigSrc, std::string(validCard),
                                              bf->controllerId, bf->id))
                        continue;
                    dups.push_back(pt);
                }
                for (auto& dup : dups)
                    m_pendingTriggers.push_back(dup);
            }
        }
    }

    // Duration$ UntilHostLeavesPlay: when a host permanent leaves the battlefield,
    // return all cards in exile that it was holding (exiledBy == old->id / ptr's prior id).
    // We use oldId (the card's pre-zone-change id) as the key since the new object
    // has a fresh id. Scan exile for all players.
    if (oldZone == ZoneType::Battlefield && destination != ZoneType::Battlefield) {
        std::vector<ObjectId> toReturn;
        for (const Card* ec : m_exile.cards())
            if (ec->exiledBy == oldId) toReturn.push_back(ec->id);
        for (ObjectId rid : toReturn) {
            Card* rc = findCard(rid);
            if (rc) moveToZone(rid, ZoneType::Battlefield, rc->ownerId);
        }
    }
    // Recompute after any BF, hand, or graveyard change — Condition$-based static abilities
    // (Hellbent, Threshold, Metalcraft, Delirium) depend on non-battlefield zone sizes.
    if (destination == ZoneType::Battlefield || oldZone == ZoneType::Battlefield ||
        destination == ZoneType::Hand        || oldZone == ZoneType::Hand        ||
        destination == ZoneType::Graveyard   || oldZone == ZoneType::Graveyard) {
        m_staticBonusDirty = true;
        recomputeStaticBonuses();
    }

    return livePtr;
}

Card* GameState::createToken(const std::string& name,
                              const std::string& types,
                              uint8_t            colorMask,
                              const std::string& power,
                              const std::string& toughness,
                              uint8_t            controllerId,
                              const std::vector<std::string>& keywords) {
    // Build and store a CardRules for the token (kept alive in m_tokenRules)
    m_tokenRules.emplace_back();
    CardRules& rules = m_tokenRules.back();
    rules.name      = name;
    rules.type      = CardType::parse(types);
    rules.power     = power;
    rules.toughness = toughness;
    rules.keywords  = keywords;

    // ── Named-token ability injection ─────────────────────────────────────────
    // Standard token subtypes come with fixed activated abilities.  Inject them
    // here so the engine can activate them without per-card script files.
    if (name == "Treasure") {
        // {T}, Sacrifice this: Add one mana of any color.
        rules.abilityLines.push_back(
            "AB$ Mana | Cost$ T Sac<Self> | Produced$ Any | SpellDescription$ Add {C}.");
    } else if (name == "Clue") {
        // {2}, Sacrifice this: Draw a card.
        rules.abilityLines.push_back(
            "AB$ Draw | Cost$ 2 Sac<Self> | NumCards$ 1 | SpellDescription$ Draw a card.");
    } else if (name == "Food") {
        // {2}, {T}, Sacrifice this: You gain 3 life.
        rules.abilityLines.push_back(
            "AB$ GainLife | Cost$ 2 T Sac<Self> | LifeAmount$ 3 | SpellDescription$ Gain 3 life.");
    } else if (name == "Blood") {
        // {1}, {T}, Discard a card, Sacrifice this: Draw a card.
        rules.abilityLines.push_back(
            "AB$ Draw | Cost$ 1 T Discard<1> Sac<Self> | NumCards$ 1 | SpellDescription$ Discard, draw.");
    } else if (name == "Map") {
        // {1}, {T}, Sacrifice this: Search your library for a basic land, put it onto the battlefield.
        rules.abilityLines.push_back(
            "AB$ ChangeZone | Cost$ 1 T Sac<Self> | Origin$ Library | Destination$ Battlefield"
            " | ValidTgts$ Basic Land | SpellDescription$ Fetch a basic land.");
    } else if (name == "Shard") {
        // {T}: Add one mana of any color
        rules.abilityLines.push_back(
            "AB$ Mana | Cost$ T | Produced$ Any | SpellDescription$ Add {C}.");
    } else if (name == "Eldrazi Spawn" || name == "Eldrazi Scion") {
        // Sacrifice this creature: Add {C}. (Forge encodes this as the "_sac"
        // suffix on the c_0_1_eldrazi_spawn_sac / c_1_1_eldrazi_scion_sac script.)
        rules.abilityLines.push_back(
            "AB$ Mana | Cost$ Sac<Self> | Produced$ C | SpellDescription$ Sacrifice this creature: Add {C}.");
    }
    // ── Role tokens (Wilds of Eldraine enchantment auras) ─────────────────────
    // Role tokens are Aura Enchantments that grant bonuses to enchanted creatures.
    // Full aura-attachment is tracked via the standard Equipment/Aura system;
    // for AI purposes these behave like global enchantments with static effects.
    else if (name == "Wicked Role") {
        rules.oracleText = "Enchanted creature gets +1/+1. When enchanted creature dies, each opponent loses 1 life.";
        rules.staticAbilityLines.push_back(
            "S:Mode$ Continuous | Affected$ Creature.Enchanted | AddPower$ 1 | AddToughness$ 1");
    } else if (name == "Cursed Role") {
        rules.oracleText = "Enchanted creature becomes a 1/1.";
    } else if (name == "Monster Role") {
        rules.oracleText = "Enchanted creature gets +1/+1 and has trample.";
        rules.staticAbilityLines.push_back(
            "S:Mode$ Continuous | Affected$ Creature.Enchanted | AddKeyword$ Trample | AddPower$ 1 | AddToughness$ 1");
    } else if (name == "Royal Role") {
        rules.oracleText = "Enchanted creature gets +1/+1 and has ward {1}.";
    } else if (name == "Young Hero Role") {
        rules.oracleText = "Enchanted creature gets +1/+1. As long as enchanted creature has toughness 3 or less, it has lifelink.";
    } else if (name == "Sorcerer Role") {
        rules.oracleText = "Enchanted creature gets +1/+1 and has prowess.";
    }
    // Build a fake mana cost from the color mask so the renderer picks the
    // correct card background color (tokens have no real mana cost).
    {
        std::string costStr;
        if (colorMask & static_cast<uint8_t>(ManaAtom::WHITE)) costStr += "W ";
        if (colorMask & static_cast<uint8_t>(ManaAtom::BLUE))  costStr += "U ";
        if (colorMask & static_cast<uint8_t>(ManaAtom::BLACK)) costStr += "B ";
        if (colorMask & static_cast<uint8_t>(ManaAtom::RED))   costStr += "R ";
        if (colorMask & static_cast<uint8_t>(ManaAtom::GREEN)) costStr += "G ";
        if (!costStr.empty()) costStr.pop_back(); // strip trailing space
        rules.manaCost = ManaCost::parse(costStr.empty() ? "no cost" : costStr);
    }

    // Build the keyword mask
    auto card       = std::make_unique<Card>();
    card->id        = allocId();
    card->rules     = &rules;
    card->ownerId   = controllerId;
    card->controllerId = controllerId;
    card->zone      = ZoneType::Battlefield;
    card->isToken   = true;
    card->keywordMask = buildKeywordMask(rules);
    card->summoningSickness = rules.type.isCreature()
        && !maskHas(card->keywordMask, KeywordAbility::Haste);

    Card* ptr = card.get();
    m_objects.emplace(ptr->id, std::move(card));
    m_battlefield.addToBack(ptr);
    m_staticBonusDirty = true;
    recomputeStaticBonuses();
    return ptr;
}

// ── Variable P/T helpers ──────────────────────────────────────────────────────

// Evaluate a Forge Count$... SVar expression.
static int evaluateCountSVar(std::string_view expr, const GameState& game,
                              uint8_t controller, ObjectId selfId) {
    // PlayerCount<Set>$<Aggregation> [ <Filter>]
    // Handles PlayerCountOpponents$, PlayerCountPlayers$, PlayerCountOther$
    // In a 2-player game: Opponents/Other = the single opponent; Players = both.
    if (expr.size() > 12 && expr.substr(0, 12) == "PlayerCount") {
        auto rest = expr.substr(11);  // e.g. "Opponents$HighestLifeTotal"
        // Determine player set
        bool doOpponent = false, doSelf = false;
        if (rest.size() >= 9 && rest.substr(0, 9) == "Opponents") {
            rest = rest.substr(9);
            doOpponent = true; doSelf = false;
        } else if (rest.size() >= 5 && rest.substr(0, 5) == "Other") {
            rest = rest.substr(5);
            doOpponent = true; doSelf = false;
        } else if (rest.size() >= 7 && rest.substr(0, 7) == "Players") {
            rest = rest.substr(7);
            doOpponent = true; doSelf = true;
        } else {
            return 0;
        }
        if (rest.empty() || rest[0] != '$') return 0;
        rest = rest.substr(1); // skip '$'
        // Aggregation
        if (rest == "Amount") {
            return (doSelf ? 1 : 0) + (doOpponent ? 1 : 0);
        }
        if (rest == "HighestLifeTotal") {
            int best = doSelf ? game.player(controller).life() : INT_MIN;
            if (doOpponent) best = std::max(best, game.player(controller ^ 1).life());
            return best == INT_MIN ? 0 : best;
        }
        if (rest == "LowestLifeTotal") {
            int best = doSelf ? game.player(controller).life() : INT_MAX;
            if (doOpponent) best = std::min(best, game.player(controller ^ 1).life());
            return best == INT_MAX ? 0 : best;
        }
        // HighestValid <Filter> / LowestValid <Filter>
        if (rest.size() > 8 && (rest.substr(0, 7) == "Highest" || rest.substr(0, 6) == "Lowest")) {
            bool highest = (rest[0] == 'H');
            auto after = rest.substr(highest ? 7 : 6); // "Valid <Filter>"
            if (after.size() > 6 && after.substr(0, 5) == "Valid") {
                auto filterStr = after.substr(5);
                if (!filterStr.empty() && filterStr[0] == ' ') filterStr = filterStr.substr(1);
                auto countFor = [&](uint8_t pid) -> int {
                    int n = 0;
                    for (const Card* c : game.battlefield().cards())
                        if (cardMatchesFilter(*c, filterStr, pid, selfId, nullptr, &game)) ++n;
                    return n;
                };
                if (doSelf && doOpponent) {
                    int a = countFor(controller), b = countFor(controller ^ 1);
                    return highest ? std::max(a, b) : std::min(a, b);
                } else if (doOpponent) {
                    return countFor(controller ^ 1);
                } else {
                    return countFor(controller);
                }
            }
        }
        // ConditionGE<N> <Count$Expr>  — number of players in set where expr(player) >= N
        if (rest.size() > 11 && rest.substr(0, 11) == "ConditionGE") {
            auto numAndExpr = rest.substr(11);
            auto sp = numAndExpr.find(' ');
            int n = 1;
            std::string_view subExpr;
            if (sp != std::string_view::npos) {
                std::from_chars(numAndExpr.data(), numAndExpr.data() + sp, n);
                subExpr = numAndExpr.substr(sp + 1);
            }
            // Build a "Count$..." expression if not already prefixed
            std::string fullExpr = subExpr.empty() ? "" :
                (subExpr.substr(0, 6) == "Count$" ? std::string(subExpr)
                                                   : "Count$" + std::string(subExpr));
            int count = 0;
            if (doSelf && evaluateCountSVar(fullExpr, game, controller, selfId) >= n)
                ++count;
            if (doOpponent && evaluateCountSVar(fullExpr, game, controller ^ 1, selfId) >= n)
                ++count;
            return count;
        }
        return 0;
    }

    if (expr.size() < 6 || expr.substr(0, 6) != "Count$") return 0;
    auto type = expr.substr(6);

    // X paid when casting a spell (e.g. Walking Ballista, Hydroid Krasis)
    if (type == "xPaid") return game.etbXHint;

    if (type == "SpellsCastThisTurn" || type == "ThisTurnCast") return game.spellsCastThisTurn;
    if (type == "StormCount") return std::max(0, game.spellsCastThisTurn - 1);

    // Count$AttackersDeclared — number of attacking creatures (0 = none attacked yet)
    if (type == "AttackersDeclared") {
        int n = 0;
        for (const Card* c : game.battlefield().cards()) if (c->attacking) n++;
        return n;
    }

    // Count$Morbid — 1 if a creature died this turn, else 0
    if (type == "Morbid") return (game.creaturesDiedThisTurn > 0) ? 1 : 0;

    if (type == "CreatureYou") {
        int n = 0;
        for (const Card* c : game.battlefield().cards())
            if (c->controllerId == controller && c->isCreature()) n++;
        return n;
    }
    if (type == "CreatureOpp") {
        int n = 0;
        for (const Card* c : game.battlefield().cards())
            if (c->controllerId != controller && c->isCreature()) n++;
        return n;
    }
    if (type == "CreatureAll") {
        int n = 0;
        for (const Card* c : game.battlefield().cards())
            if (c->isCreature()) n++;
        return n;
    }
    if (type.substr(0, 7) == "LandYou") {
        // Supports optional subtype: "LandYou.Swamp"
        std::string_view sub;
        auto dot = type.find('.');
        if (dot != std::string_view::npos) sub = type.substr(dot + 1);
        int n = 0;
        for (const Card* c : game.battlefield().cards()) {
            if (c->controllerId != controller || !c->isLand()) continue;
            if (!sub.empty() && !c->rules->type.hasSubtype(sub)) continue;
            n++;
        }
        return n;
    }
    if (type == "TypeInGraveyard") {
        // Distinct card types across all graveyards (Tarmogoyf)
        std::set<std::string> types;
        for (uint8_t pid = 0; pid < 2; ++pid)
            for (const Card* c : game.player(pid).graveyard().cards())
                for (auto mt : c->rules->type.types)
                    types.insert(std::string(CardType::mainTypeName(mt)));
        return static_cast<int>(types.size());
    }
    if (type == "GraveyardYou" || type == "CardsInGraveyardYou") {
        return static_cast<int>(game.player(controller).graveyard().size());
    }
    if (type == "GraveyardOpp" || type == "CardsInGraveyardOpp") {
        return static_cast<int>(game.player(controller ^ 1).graveyard().size());
    }
    if (type == "GraveyardAll") {
        return static_cast<int>(game.player(0).graveyard().size() +
                                game.player(1).graveyard().size());
    }
    if (type == "HandYou") {
        return static_cast<int>(game.player(controller).hand().size());
    }
    if (type == "HandOpp") {
        return static_cast<int>(game.player(controller ^ 1).hand().size());
    }
    if (type == "HandAll") {
        return static_cast<int>(game.player(0).hand().size() + game.player(1).hand().size());
    }
    if (type == "LibraryYou" || type == "CardsInLibraryYou") {
        return static_cast<int>(game.player(controller).library().size());
    }
    if (type == "LibraryOpp" || type == "CardsInLibraryOpp") {
        return static_cast<int>(game.player(controller ^ 1).library().size());
    }
    if (type == "LibraryAll") {
        return static_cast<int>(game.player(0).library().size() + game.player(1).library().size());
    }
    if (type == "LifeYou" || type == "LifeTotal" || type == "YourLifeTotal") {
        return game.player(controller).life();
    }
    if (type == "LifeOpp") {
        return game.player(controller ^ 1).life();
    }
    if (type == "ArtifactsYou" || type == "Artifacts") {
        int n = 0;
        for (const Card* c : game.battlefield().cards())
            if (c->controllerId == controller && c->rules->type.isArtifact()) n++;
        return n;
    }
    if (type == "ArtifactsOpp") {
        int n = 0;
        for (const Card* c : game.battlefield().cards())
            if (c->controllerId != controller && c->rules->type.isArtifact()) n++;
        return n;
    }
    if (type == "ArtifactsAll") {
        int n = 0;
        for (const Card* c : game.battlefield().cards())
            if (c->rules->type.isArtifact()) n++;
        return n;
    }
    if (type == "EnchantsYou" || type == "EnchantersYou" || type == "EnchantmentsYou") {
        int n = 0;
        for (const Card* c : game.battlefield().cards())
            if (c->controllerId == controller && c->rules->type.isEnchantment()) n++;
        return n;
    }
    if (type == "EnchantmentsAll" || type == "EnchantsAll") {
        int n = 0;
        for (const Card* c : game.battlefield().cards())
            if (c->rules->type.isEnchantment()) n++;
        return n;
    }
    if (type.substr(0, 7) == "LandOpp") {
        std::string_view sub;
        auto dot = type.find('.');
        if (dot != std::string_view::npos) sub = type.substr(dot + 1);
        int n = 0;
        for (const Card* c : game.battlefield().cards()) {
            if (c->controllerId == controller || !c->isLand()) continue;
            if (!sub.empty() && !c->rules->type.hasSubtype(sub)) continue;
            n++;
        }
        return n;
    }
    if (type.substr(0, 7) == "LandAll") {
        std::string_view sub;
        auto dot = type.find('.');
        if (dot != std::string_view::npos) sub = type.substr(dot + 1);
        int n = 0;
        for (const Card* c : game.battlefield().cards()) {
            if (!c->isLand()) continue;
            if (!sub.empty() && !c->rules->type.hasSubtype(sub)) continue;
            n++;
        }
        return n;
    }
    if (type == "PermanentYou") {
        int n = 0;
        for (const Card* c : game.battlefield().cards())
            if (c->controllerId == controller && c->isPermanent()) n++;
        return n;
    }
    if (type == "PermanentOpp") {
        int n = 0;
        for (const Card* c : game.battlefield().cards())
            if (c->controllerId != controller && c->isPermanent()) n++;
        return n;
    }
    if (type == "PermanentAll") {
        return static_cast<int>(game.battlefield().size());
    }
    if (type == "TokensYou") {
        int n = 0;
        for (const Card* c : game.battlefield().cards())
            if (c->controllerId == controller && c->isToken) n++;
        return n;
    }
    if (type == "TokensOpp") {
        int n = 0;
        for (const Card* c : game.battlefield().cards())
            if (c->controllerId != controller && c->isToken) n++;
        return n;
    }
    if (type == "TokensAll") {
        int n = 0;
        for (const Card* c : game.battlefield().cards())
            if (c->isToken) n++;
        return n;
    }
    // Count$Xpower / Count$Xtoughness — effective P/T of the source card (selfId)
    if (type == "Xpower") {
        if (selfId == kInvalidId) return 0;
        const Card* src = game.findCard(selfId);
        return src ? effectivePower(*src) : 0;
    }
    if (type == "Xtoughness") {
        if (selfId == kInvalidId) return 0;
        const Card* src = game.findCard(selfId);
        return src ? effectiveToughness(*src) : 0;
    }
    // Count$CardCounters.TYPE or Count$Counters.TYPE — counters on the source card (selfId)
    if (type.size() > 12 && type.substr(0, 12) == "CardCounters.") {
        auto ctype = type.substr(12);
        if (selfId == kInvalidId) return 0;
        const Card* src = game.findCard(selfId);
        if (!src) return 0;
        std::string key;
        if      (ctype == "P1P1")    key = "+1/+1";
        else if (ctype == "M1M1")    key = "-1/-1";
        else if (ctype == "CHARGE")  key = "charge";
        else if (ctype == "LOYALTY") key = "loyalty";
        else if (ctype == "LEVEL")   key = "LEVEL";
        else                         key = std::string(ctype);
        return src->counterCount(key);
    }
    // Count$Counters.P1P1 etc. — counters on the source card (selfId)
    if (type.size() > 9 && type.substr(0, 9) == "Counters.") {
        auto ctype = type.substr(9);
        if (selfId == kInvalidId) return 0;
        const Card* src = game.findCard(selfId);
        if (!src) return 0;
        std::string key;
        if      (ctype == "P1P1")    key = "+1/+1";
        else if (ctype == "M1M1")    key = "-1/-1";
        else if (ctype == "CHARGE")  key = "charge";
        else if (ctype == "LOYALTY") key = "loyalty";
        else                         key = std::string(ctype);
        return src->counterCount(key);
    }
    // Count$OtherCreatureYou / OtherCreatureOpp — creatures controlled by you/opp
    // excluding the source card (selfId)
    if (type == "OtherCreatureYou") {
        int n = 0;
        for (const Card* c : game.battlefield().cards())
            if (c->controllerId == controller && c->isCreature() && c->id != selfId) n++;
        return n;
    }
    if (type == "OtherCreatureOpp") {
        int n = 0;
        for (const Card* c : game.battlefield().cards())
            if (c->controllerId != controller && c->isCreature() && c->id != selfId) n++;
        return n;
    }
    if (type == "OtherPermanentYou") {
        int n = 0;
        for (const Card* c : game.battlefield().cards())
            if (c->controllerId == controller && c->id != selfId) n++;
        return n;
    }
    // Count$Devotion.W / .BG / etc. — colored pips in your permanents' mana costs
    // Supports single and multi-color (e.g. Devotion.WU = devotion to white and blue).
    if (type.size() > 9 && type.substr(0, 9) == "Devotion.") {
        auto colorStr = type.substr(9);
        // Build a bitmask of all colors we care about from the color string
        uint32_t colorMask = 0;
        for (char ch : colorStr) {
            if      (ch == 'W') colorMask |= ManaAtom::WHITE;
            else if (ch == 'U') colorMask |= ManaAtom::BLUE;
            else if (ch == 'B') colorMask |= ManaAtom::BLACK;
            else if (ch == 'R') colorMask |= ManaAtom::RED;
            else if (ch == 'G') colorMask |= ManaAtom::GREEN;
        }
        // Handle long-form names (single color only: White, Blue, etc.)
        if (colorMask == 0) {
            if      (colorStr == "White") colorMask = ManaAtom::WHITE;
            else if (colorStr == "Blue")  colorMask = ManaAtom::BLUE;
            else if (colorStr == "Black") colorMask = ManaAtom::BLACK;
            else if (colorStr == "Red")   colorMask = ManaAtom::RED;
            else if (colorStr == "Green") colorMask = ManaAtom::GREEN;
        }
        if (colorMask == 0) return 0;
        int devotion = 0;
        for (const Card* c : game.battlefield().cards()) {
            if (c->controllerId != controller) continue;
            for (const auto& shard : c->rules->manaCost.shards())
                if (shard.atoms & colorMask) devotion++;
        }
        return devotion;
    }
    // Count$CreaturesAttacking / CreaturesBlocking — combat state counts
    if (type == "CreaturesAttacking") {
        int n = 0;
        for (const Card* c : game.battlefield().cards()) if (c->attacking) n++;
        return n;
    }
    if (type == "CreaturesAttackingYou") {
        int n = 0;
        for (const Card* c : game.battlefield().cards())
            if (c->attacking && c->controllerId != controller) n++;
        return n;
    }
    if (type == "CreaturesBlocking") {
        int n = 0;
        for (const Card* c : game.battlefield().cards()) if (c->blocking) n++;
        return n;
    }

    // Count$ValidHand[You|Opp|All] <filter> — cards in hand matching filter
    if (type.size() >= 9 && type.substr(0, 9) == "ValidHand") {
        auto rest = type.substr(9);
        bool doYou = true, doOpp = false;
        if (rest.size() >= 3 && rest.substr(0, 3) == "You")  { rest = rest.substr(3); doYou = true;  doOpp = false; }
        else if (rest.size() >= 3 && rest.substr(0, 3) == "Opp") { rest = rest.substr(3); doYou = false; doOpp = true;  }
        else if (rest.size() >= 3 && rest.substr(0, 3) == "All") { rest = rest.substr(3); doYou = true;  doOpp = true;  }
        if (!rest.empty() && rest[0] == ' ') rest = rest.substr(1);
        int n = 0;
        if (doYou) for (const Card* c : game.player(controller).hand().cards())
            if (cardMatchesFilter(*c, rest, controller, selfId, nullptr, &game)) n++;
        if (doOpp) for (const Card* c : game.player(controller ^ 1).hand().cards())
            if (cardMatchesFilter(*c, rest, controller, selfId, nullptr, &game)) n++;
        return n;
    }

    // Count$ValidExile <filter> — exiled cards matching filter
    if (type.size() >= 11 && type.substr(0, 11) == "ValidExile ") {
        std::string_view filterStr = type.substr(11);
        int n = 0;
        for (const Card* c : game.exile().cards())
            if (cardMatchesFilter(*c, filterStr, controller, selfId, nullptr, &game)) n++;
        return n;
    }
    if (type == "ValidExile") {
        return static_cast<int>(game.exile().size());
    }

    // Count$ValidBattlefield,<zone2> <filter> — count matching cards across multiple zones
    if (type.size() > 16 && type.substr(0, 16) == "ValidBattlefield") {
        // Parse comma-separated extra zones before the filter (e.g. "ValidBattlefield,Graveyard Elf")
        auto rest = type.substr(16);
        bool doGY = false, doHand = false, doExile = false, doLib = false;
        while (!rest.empty() && rest[0] == ',') {
            rest = rest.substr(1);
            if (rest.size() >= 9 && rest.substr(0, 9) == "Graveyard")  { doGY    = true; rest = rest.substr(9); }
            else if (rest.size() >= 4 && rest.substr(0, 4) == "Hand")  { doHand  = true; rest = rest.substr(4); }
            else if (rest.size() >= 5 && rest.substr(0, 5) == "Exile") { doExile = true; rest = rest.substr(5); }
            else if (rest.size() >= 7 && rest.substr(0, 7) == "Library"){ doLib   = true; rest = rest.substr(7); }
            else break;
        }
        if (!rest.empty() && rest[0] == ' ') rest = rest.substr(1);
        int n = 0;
        for (const Card* c : game.battlefield().cards())
            if (cardMatchesFilter(*c, rest, controller, selfId, nullptr, &game)) n++;
        if (doGY) {
            for (const Card* c : game.player(controller).graveyard().cards())
                if (cardMatchesFilter(*c, rest, controller, selfId, nullptr, &game)) n++;
            for (const Card* c : game.player(controller ^ 1).graveyard().cards())
                if (cardMatchesFilter(*c, rest, controller ^ 1, selfId, nullptr, &game)) n++;
        }
        if (doHand) {
            for (const Card* c : game.player(controller).hand().cards())
                if (cardMatchesFilter(*c, rest, controller, selfId, nullptr, &game)) n++;
        }
        return n;
    }

    // Count$Random.<min>.<max> — random integer in [min, max]
    if (type.size() > 7 && type.substr(0, 7) == "Random.") {
        auto rest = type.substr(7);
        auto dot  = rest.find('.');
        int lo = 0, hi = 1;
        if (dot != std::string_view::npos) {
            std::from_chars(rest.data(), rest.data() + dot, lo);
            auto hiStr = rest.substr(dot + 1);
            // hiStr may be a literal or SVar; try literal first
            if (!hiStr.empty() && std::isdigit(static_cast<unsigned char>(hiStr[0])))
                std::from_chars(hiStr.data(), hiStr.data() + hiStr.size(), hi);
            else if (!hiStr.empty())
                hi = const_cast<GameState&>(game).evaluateSVar(std::string(hiStr), controller, nullptr, selfId);
        }
        if (hi < lo) return lo;
        return lo + static_cast<int>(const_cast<GameState&>(game).rng()() % static_cast<unsigned>(hi - lo + 1));
    }

    // Count$Valid <filter>$DifferentColorPair — number of DISTINCT two-colour
    // pairs among matching permanents that are exactly two colours (Niv-Mizzet,
    // Guildpact). Up to 10 (WU, WB, …, RG).
    if (type.size() > 5 && type.substr(0, 5) == "Valid" &&
        type.find("$DifferentColorPair") != std::string_view::npos) {
        std::string_view rest = type.substr(5);
        if (!rest.empty() && rest[0] == ' ') rest = rest.substr(1);
        auto dollar = rest.find('$');
        std::string_view filterStr = rest.substr(0, dollar);
        static const uint8_t kPairs[10] = {
            ManaAtom::WHITE | ManaAtom::BLUE,  ManaAtom::WHITE | ManaAtom::BLACK,
            ManaAtom::WHITE | ManaAtom::RED,   ManaAtom::WHITE | ManaAtom::GREEN,
            ManaAtom::BLUE  | ManaAtom::BLACK, ManaAtom::BLUE  | ManaAtom::RED,
            ManaAtom::BLUE  | ManaAtom::GREEN, ManaAtom::BLACK | ManaAtom::RED,
            ManaAtom::BLACK | ManaAtom::GREEN, ManaAtom::RED   | ManaAtom::GREEN };
        uint16_t seen = 0;
        for (const Card* c : game.battlefield().cards()) {
            if (!c || !c->rules) continue;
            if (!cardMatchesFilter(*c, filterStr, controller, selfId, nullptr, &game)) continue;
            uint8_t cm = static_cast<uint8_t>(c->rules->manaCost.colorIdentity() &
                                              ManaAtom::COLORS_MASK);
            int bits = 0; for (uint8_t m = cm; m; m &= static_cast<uint8_t>(m - 1)) ++bits;
            if (bits != 2) continue;
            for (int i = 0; i < 10; ++i) if (kPairs[i] == cm) { seen |= static_cast<uint16_t>(1u << i); break; }
        }
        int n = 0; for (uint16_t v = seen; v; v &= static_cast<uint16_t>(v - 1)) ++n;
        return n;
    }

    // Count$Valid <filter> — count battlefield permanents matching filter
    // Must be checked BEFORE ValidGraveyard to avoid the 5-char prefix matching "ValidG..."
    // but AFTER ValidGraveyard itself (handled earlier and returned already).
    if (type.size() > 5 && type.substr(0, 5) == "Valid" &&
        (type.size() < 14 || type.substr(0, 14) != "ValidGraveyard")) {
        std::string_view filterStr = type.substr(5);
        if (!filterStr.empty() && filterStr[0] == ' ') filterStr = filterStr.substr(1);
        int n = 0;
        for (const Card* c : game.battlefield().cards())
            if (cardMatchesFilter(*c, filterStr, controller, selfId, nullptr, &game)) n++;
        return n;
    }

    // Count$ValidGraveyard[You|Opp] <filter> — count matching cards in graveyard(s)
    if (type.size() >= 14 && type.substr(0, 14) == "ValidGraveyard") {
        auto rest = type.substr(14);
        bool doYou = true, doOpp = true;
        if (rest.size() >= 3 && rest.substr(0, 3) == "You") { rest = rest.substr(3); doOpp = false; }
        else if (rest.size() >= 3 && rest.substr(0, 3) == "Opp") { rest = rest.substr(3); doYou = false; }
        if (!rest.empty() && rest[0] == ' ') rest = rest.substr(1);
        int n = 0;
        if (doYou) for (const Card* c : game.player(controller).graveyard().cards())
            if (cardMatchesFilter(*c, rest, controller, selfId, nullptr, &game)) n++;
        if (doOpp) for (const Card* c : game.player(controller ^ 1).graveyard().cards())
            if (cardMatchesFilter(*c, rest, controller, selfId, nullptr, &game)) n++;
        return n;
    }
    // Count$CardPower / Count$CardToughness — P/T of the source card (selfId)
    if (type == "CardPower") {
        if (selfId == kInvalidId) return 0;
        const Card* src = game.findCard(selfId);
        return src ? effectivePower(*src) : 0;
    }
    if (type == "CardToughness") {
        if (selfId == kInvalidId) return 0;
        const Card* src = game.findCard(selfId);
        return src ? effectiveToughness(*src) : 0;
    }

    // Count$Domain — number of distinct basic land subtypes you control (max 5)
    if (type == "Domain") {
        bool hasForest = false, hasIsland = false, hasSwamp = false,
             hasMountain = false, hasPlains = false;
        for (const Card* c : game.battlefield().cards()) {
            if (c->controllerId != controller || !c->isLand()) continue;
            if (c->rules->type.hasSubtype("Forest"))   hasForest   = true;
            if (c->rules->type.hasSubtype("Island"))   hasIsland   = true;
            if (c->rules->type.hasSubtype("Swamp"))    hasSwamp    = true;
            if (c->rules->type.hasSubtype("Mountain")) hasMountain = true;
            if (c->rules->type.hasSubtype("Plains"))   hasPlains   = true;
        }
        return (int)hasForest + (int)hasIsland + (int)hasSwamp +
               (int)hasMountain + (int)hasPlains;
    }

    // Count$ValidLibrary <filter> — cards in your library matching filter
    if (type.size() >= 12 && type.substr(0, 12) == "ValidLibrary") {
        auto rest = type.substr(12);
        if (!rest.empty() && rest[0] == ' ') rest = rest.substr(1);
        int n = 0;
        for (const Card* c : game.player(controller).library().cards())
            if (rest.empty() || cardMatchesFilter(*c, rest, controller, selfId, nullptr, &game)) n++;
        return n;
    }

    // Count$ThisTurnEntered_<Zone>_<filter> — permanents that entered a zone this turn.
    // Supported: ThisTurnEntered_Battlefield_<filter> (uses permanentsEnteredThisTurn)
    //            ThisTurnEntered_Graveyard_from_Battlefield_<filter> (uses creaturesDiedThisTurn)
    if (type.size() > 16 && type.substr(0, 16) == "ThisTurnEntered_") {
        auto rest = type.substr(16); // e.g. "Battlefield_Creature.YouCtrl"
        if (rest.size() >= 11 && rest.substr(0, 11) == "Battlefield") {
            auto filterPart = rest.substr(11);
            if (!filterPart.empty() && filterPart[0] == '_') filterPart = filterPart.substr(1);
            int n = 0;
            for (ObjectId oid : game.permanentsEnteredThisTurn) {
                const Card* c = game.findCard(oid);
                if (!c) continue;
                if (filterPart.empty() || cardMatchesFilter(*c, filterPart, controller, selfId, nullptr, &game))
                    ++n;
            }
            return n;
        }
        // "Graveyard_from_Battlefield_<filter>" — creatures that died this turn
        if (rest.size() >= 22 && rest.substr(0, 22) == "Graveyard_from_Battlef") {
            // Return creaturesDiedThisTurn as approximation (no per-filter GY entry log)
            return game.creaturesDiedThisTurn;
        }
    }

    // Count$LifeYouGainedThisTurn — life gained by the controller this turn
    if (type == "LifeYouGainedThisTurn" || type == "LifeGainedThisTurn")
        return game.lifeGainedThisTurn[controller];

    // Count$LifeOppsLostThisTurn — life lost by the opponent this turn
    if (type == "LifeOppsLostThisTurn")
        return game.lifeLostThisTurn[controller ^ 1];

    // Count$LifeYouLostThisTurn — life lost by the controller this turn
    if (type == "LifeYouLostThisTurn")
        return game.lifeLostThisTurn[controller];

    // Count$Party — number of distinct party class types you control (Cleric/Rogue/Warrior/Wizard)
    if (type == "Party") {
        bool hasCleric = false, hasRogue = false, hasWarrior = false, hasWizard = false;
        for (const Card* c : game.battlefield().cards()) {
            if (c->controllerId != controller || !c->isCreature()) continue;
            if (c->rules->type.hasSubtype("Cleric"))  hasCleric  = true;
            if (c->rules->type.hasSubtype("Rogue"))   hasRogue   = true;
            if (c->rules->type.hasSubtype("Warrior")) hasWarrior = true;
            if (c->rules->type.hasSubtype("Wizard"))  hasWizard  = true;
        }
        return (int)hasCleric + (int)hasRogue + (int)hasWarrior + (int)hasWizard;
    }

    // Count$Kicked.<trueValue>.<falseValue> — returns trueValue if kicked, falseValue otherwise
    if (type.size() > 7 && type.substr(0, 7) == "Kicked.") {
        auto rest = type.substr(7); // e.g. "1.0"
        auto sep = rest.find('.');
        int trueVal = 1, falseVal = 0;
        if (sep != std::string_view::npos) {
            std::from_chars(rest.data(), rest.data() + sep, trueVal);
            auto falseStr = rest.substr(sep + 1);
            std::from_chars(falseStr.data(), falseStr.data() + falseStr.size(), falseVal);
        }
        return game.kickedHint ? trueVal : falseVal;
    }

    // Count$ManaPool:color — amount of a given color in the mana pool
    if (type.size() > 9 && type.substr(0, 9) == "ManaPool:") {
        auto color = type.substr(9);
        const ManaPool& pool = game.player(controller).manaPool();
        if (color == "white")   return pool.available(ManaCostShard::WHITE);
        if (color == "blue")    return pool.available(ManaCostShard::BLUE);
        if (color == "black")   return pool.available(ManaCostShard::BLACK);
        if (color == "red")     return pool.available(ManaCostShard::RED);
        if (color == "green")   return pool.available(ManaCostShard::GREEN);
        if (color == "generic") return pool.total();
        return 0;
    }

    // Count$RememberedSize — number of cards currently in the remembered list.
    // Count$RememberedNumber — numeric value stashed from a prior effect (e.g. countered CMC).
    // Both are populated via game.rememberedSizeHint / rememberedNumberHint by Effects.cpp.
    if (type == "RememberedSize") return game.rememberedSizeHint;
    if (type == "RememberedNumber") return game.rememberedNumberHint;

    // Count$ChosenNumber — number chosen by a DB$ ChooseNumber effect this chain.
    if (type == "ChosenNumber") return game.chosenNumberHint;

    // Count$DamageAmount — amount from the most recent damage trigger.
    if (type == "DamageAmount") return game.triggerAmountHint;

    // Count$Compare <SVar> <OP><RHS>.<trueResult>.<falseResult>
    // Returns trueResult if comparison holds, falseResult otherwise.
    // RHS, trueResult, and falseResult may be numeric literals or SVar names.
    if (type.size() > 8 && type.substr(0, 8) == "Compare ") {
        auto rest = type.substr(8);
        auto spacePos = rest.find(' ');
        if (spacePos != std::string_view::npos) {
            auto svarName = std::string(rest.substr(0, spacePos));
            auto cmpExpr  = rest.substr(spacePos + 1);
            auto dot1 = cmpExpr.find('.');
            bool condMet = false;
            int trueR = 1, falseR = 0;
            if (dot1 != std::string_view::npos) {
                auto opVal  = cmpExpr.substr(0, dot1);
                auto rest2  = cmpExpr.substr(dot1 + 1);
                auto dot2   = rest2.find('.');
                const CardRules* rules = (selfId != kInvalidId && game.findCard(selfId))
                                         ? game.findCard(selfId)->rules : nullptr;
                // Resolve a token (number literal or SVar name)
                auto resolve = [&](std::string_view s) -> int {
                    if (s.empty()) return 0;
                    if (std::isdigit(static_cast<unsigned char>(s[0]))) {
                        int v = 0; std::from_chars(s.data(), s.data() + s.size(), v); return v;
                    }
                    return game.evaluateSVar(std::string(s), controller, rules, selfId);
                };
                if (dot2 != std::string_view::npos) {
                    trueR  = resolve(rest2.substr(0, dot2));
                    falseR = resolve(rest2.substr(dot2 + 1));
                }
                if (opVal.size() >= 3) {
                    auto op     = opVal.substr(0, 2);
                    auto rhsStr = opVal.substr(2);
                    int lhs = game.evaluateSVar(svarName, controller, rules, selfId);
                    int rhs = resolve(rhsStr);
                    if      (op == "GE") condMet = (lhs >= rhs);
                    else if (op == "GT") condMet = (lhs >  rhs);
                    else if (op == "LE") condMet = (lhs <= rhs);
                    else if (op == "LT") condMet = (lhs <  rhs);
                    else if (op == "EQ") condMet = (lhs == rhs);
                    else if (op == "NE") condMet = (lhs != rhs);
                }
            }
            return condMet ? trueR : falseR;
        }
    }

    // Count$LifeAmount / Count$Amount — also use the trigger amount hint for life gain/damage
    if (type == "LifeAmount" || type == "Amount") return game.triggerAmountHint;

    // Count$CardsInYourHand — same as HandYou
    if (type == "CardsInYourHand") return static_cast<int>(game.player(controller).hand().size());

    // Count$TimesKicked — for multi-kicker: how many times the spell was kicked.
    // Simplified: kickedHint is bool; return 1 if kicked.
    if (type == "TimesKicked") return game.kickedHint ? 1 : 0;

    // Count$Converge — number of distinct colors of mana spent.
    // Approximated as the number of distinct colored mana in the pool.
    if (type == "Converge") {
        int colorCount = 0;
        const ManaPool& pool = game.player(controller).manaPool();
        if (pool.available(ManaCostShard::WHITE) > 0) ++colorCount;
        if (pool.available(ManaCostShard::BLUE)  > 0) ++colorCount;
        if (pool.available(ManaCostShard::BLACK) > 0) ++colorCount;
        if (pool.available(ManaCostShard::RED)   > 0) ++colorCount;
        if (pool.available(ManaCostShard::GREEN) > 0) ++colorCount;
        return colorCount;
    }

    // Count$CastTotalManaSpent — total mana value of spells cast (approx: spellsCastThisTurn)
    if (type == "CastTotalManaSpent") return game.spellsCastThisTurn;

    // Count$ThisTurnCast_Card — cards cast this turn (approx: spellsCastThisTurn)
    if (type.size() >= 17 && type.substr(0, 17) == "ThisTurnCast_Card") return game.spellsCastThisTurn;

    // Count$YourCounters<Type> — player counter of the given type (Energy, Experience, etc.)
    if (type.size() > 12 && type.substr(0, 12) == "YourCounters") {
        auto counterType = std::string(type.substr(12));
        return game.player(controller).counterCount(counterType);
    }
    // Count$OppCounters<Type>
    if (type.size() > 11 && type.substr(0, 11) == "OppCounters") {
        auto counterType = std::string(type.substr(11));
        return game.player(controller ^ 1).counterCount(counterType);
    }

    // Count$ResolvedThisTurn — approximation: spells resolved by self this turn
    if (type == "ResolvedThisTurn") return game.spellsCastByPlayer[controller];

    // Count$YouDrewThisTurn — cards drawn this turn by the controller
    if (type == "YouDrewThisTurn") return game.cardsDrawnThisTurn[controller];

    // Count$YourStartingLife — standard starting life total
    if (type == "YourStartingLife") return 20;

    // Count$TriggerRememberAmount — same as RememberedSize in most contexts
    if (type == "TriggerRememberAmount") return game.rememberedSizeHint;

    // Count$Adamant — 1 if spell was cast with 3+ of a single mana color (simplified: kickedHint)
    // Real Adamant is about mana spent; approximation is safe (most Adamant cards have bonus effects)
    if (type == "Adamant") return 0; // conservative: never grant Adamant bonus in AI

    return 0;
}

// Parse a variable stat expression:  "*" → xVal,  "N+*" → N+xVal, "*+N" → xVal+N
static int parseVariableStat(std::string_view stat, int xVal) {
    if (stat == "*") return xVal;
    // "N+*" form
    auto plus = stat.find('+');
    if (plus != std::string_view::npos) {
        if (stat.substr(plus + 1) == "*") {
            int n = 0;
            std::from_chars(stat.data(), stat.data() + plus, n);
            return n + xVal;
        }
        if (stat.substr(0, plus) == "*") {
            int n = 0;
            auto rest = stat.substr(plus + 1);
            std::from_chars(rest.data(), rest.data() + rest.size(), n);
            return xVal + n;
        }
    }
    // Static number fallback
    int v = 0;
    std::from_chars(stat.data(), stat.data() + stat.size(), v);
    return v;
}

int GameState::evaluateSVar(const std::string& svarName, uint8_t controller,
                             const CardRules* rules, ObjectId selfId) const {
    // Dynamic SVar overrides take precedence (set by DB$ StoreSVar).
    {
        auto dit = dynamicSVars.find(svarName);
        if (dit != dynamicSVars.end()) return dit->second;
    }

    // First: look up as a named SVar in the card's rules.
    if (rules) {
        auto it = rules->svars.find(svarName);
        if (it != rules->svars.end())
            return evaluateCountSVar(it->second, *this, controller, selfId);
    }

    // /Plus. combinator: "ExprA/Plus.ExprB" → eval(ExprA) + eval(ExprB)
    auto plusPos = svarName.find("/Plus.");
    if (plusPos != std::string::npos) {
        auto left  = svarName.substr(0, plusPos);
        auto right = svarName.substr(plusPos + 6);
        return evaluateSVar(left,  controller, rules, selfId) +
               evaluateSVar(right, controller, rules, selfId);
    }

    // Direct Count$ expression passed inline (e.g. from ConditionCheckSVar).
    if (svarName.size() >= 6 && svarName.substr(0, 6) == "Count$")
        return evaluateCountSVar(svarName, *this, controller, selfId);

    return 0;
}

int GameState::evaluateCountExpr(std::string_view countExpr, uint8_t controller) const {
    return evaluateCountSVar(countExpr, *this, controller);
}

int GameState::evaluateCountExpr(std::string_view countExpr, uint8_t controller, ObjectId selfId) const {
    return evaluateCountSVar(countExpr, *this, controller, selfId);
}

void GameState::triggerSagaChapter(Card* saga) {
    if (!saga || !saga->rules->saga.has_value()) return;
    const auto& se = *saga->rules->saga;

    saga->addCounter("lore", 1);
    int lore = saga->counterCount("lore");

    // Execute the chapter effect for this lore count
    int chIdx = lore - 1;
    if (chIdx >= 0 && chIdx < static_cast<int>(se.chapterSVars.size())) {
        const auto& svarName = se.chapterSVars[chIdx];
        auto svarIt = saga->rules->svars.find(svarName);
        if (svarIt != saga->rules->svars.end()) {
            auto effect = parseScriptLine(svarIt->second);
            if (!effect.empty()) {
                EffectContext ctx{ *this, saga, saga->controllerId, {}, 0 };
                executeEffectChain(effect, ctx);
            }
        }
    }

    // After the last chapter: transform if the Saga has a back face, otherwise sacrifice.
    if (lore >= se.maxChapter) {
        ObjectId sid = saga->id;
        uint8_t  sc  = saga->controllerId;
        if (saga->rules->backFace) {
            // Transform Saga (e.g. Teferi's Ageless Insight → creature side)
            saga->ownedRules  = std::make_shared<CardRules>(*saga->rules->backFace);
            saga->rules       = saga->ownedRules.get();
            saga->transformed = true;
            saga->keywordMask = buildKeywordMask(*saga->rules);
            recomputeStaticBonuses();
            // Fire Transformed triggers
            std::vector<PendingTrigger> trBuf;
            TriggerSystem::onTransform(*saga, *this, trBuf);
            queueTriggers(std::move(trBuf));
        } else {
            moveToZone(sid, ZoneType::Graveyard, sc);
        }
    }
}

void GameState::recomputeStaticBonuses() {
    if (!m_staticBonusDirty) return;   // no battlefield changes since last call
    m_staticBonusDirty = false;

    // Step 1: clear existing continuous bonuses and rebuild the base keywordMask
    // Also reset per-player max hand size (static SetMaxHandSize effects re-apply below)
    m_players[0].setMaxHandSize(7);
    m_players[1].setMaxHandSize(7);
    cantGainLife[0] = false;
    cantGainLife[1] = false;
    cantPlayLand[0] = false;
    cantPlayLand[1] = false;
    for (int i = 0; i < 4; ++i) extraLandPlays[i] = 0;

    for (Card* c : m_battlefield.cards()) {
        if (!c->rules) continue;   // guard against stale ownedRules pointers
        c->keywordMask       |= c->removedKeywords;   // restore previously-stripped native keywords
        c->keywordMask       &= ~c->continuousKeywords;
        c->continuousPower    = 0;
        c->continuousToughness= 0;
        c->continuousKeywords = 0;
        c->removedKeywords    = 0;
        c->cantAttack                 = false;
        c->cantBlock                  = false;
        c->cantBeTargeted             = false;
        c->unblockable                = false;
        c->dealsDamageByToughness     = false;
        c->activateAbilityAsIfHaste   = false;
        c->allAbilitiesRemoved        = false;
        c->blockOnlyBy.clear();
        c->maxBlockerCount    = INT_MAX;
        c->setPower    = -1;
        c->setToughness= -1;
        c->swapPT      = false;
        c->colorIdOverride = 0xFF;   // reset to "use rules default" before layer 5 effects
        c->continuousSubtypes.clear();
        c->grantedAbilities.clear();
        c->textChanges.clear();

        // Level-up bands: override P/T based on LEVEL counter count
        if (c->rules->hasLevelUp && !c->rules->levelBands.empty()) {
            int lvl = c->counterCount("LEVEL");
            for (const auto& band : c->rules->levelBands) {
                if (lvl >= band.minLevel && lvl <= band.maxLevel) {
                    // Parse P/T strings (may be numeric)
                    auto parseStatStr = [](const std::string& s) -> int {
                        if (s.empty() || s == "*") return -1;
                        int v = 0;
                        std::from_chars(s.data(), s.data() + s.size(), v);
                        return v;
                    };
                    int bp = parseStatStr(band.power);
                    int bt = parseStatStr(band.toughness);
                    if (bp >= 0) c->setPower     = bp;
                    if (bt >= 0) c->setToughness = bt;
                    break;
                }
            }
        }

        // Recompute variable P/T for * creatures
        c->varPower    = -1;
        c->varToughness= -1;
        bool hasVarP = (c->rules->power.find('*')     != std::string::npos);
        bool hasVarT = (c->rules->toughness.find('*') != std::string::npos);
        if (hasVarP || hasVarT) {
            // Look for SVar X which defines the variable value
            int xVal = 0;
            auto it = c->rules->svars.find("X");
            if (it != c->rules->svars.end())
                xVal = evaluateCountSVar(it->second, *this, c->controllerId, c->id);
            if (hasVarP) c->varPower     = parseVariableStat(c->rules->power,     xVal);
            if (hasVarT) c->varToughness = parseVariableStat(c->rules->toughness, xVal);
        }
    }

    // Step 1.5: apply gameplay effects from game-mechanic attributes (AlterAttribute results)
    // Also apply keyword counters: a creature with a "flying counter" gains flying, etc.
    for (Card* c : m_battlefield.cards()) {
        if (c->hasAttribute("Suspected")) {
            c->cantBlock = true;
            c->keywordMask |= static_cast<uint32_t>(KeywordAbility::Menace);
        }
        // Keyword counters (rule 122.1k): each keyword-named counter grants the ability.
        static const std::pair<const char*, KeywordAbility> kKeywordCounters[] = {
            { "flying",          KeywordAbility::Flying      },
            { "first strike",    KeywordAbility::FirstStrike },
            { "double strike",   KeywordAbility::DoubleStrike},
            { "deathtouch",      KeywordAbility::Deathtouch  },
            { "haste",           KeywordAbility::Haste       },
            { "hexproof",        KeywordAbility::Hexproof    },
            { "indestructible",  KeywordAbility::Indestructible},
            { "lifelink",        KeywordAbility::Lifelink    },
            { "menace",          KeywordAbility::Menace      },
            { "reach",           KeywordAbility::Reach       },
            { "trample",         KeywordAbility::Trample     },
            { "vigilance",       KeywordAbility::Vigilance   },
        };
        for (const auto& [counterName, kw] : kKeywordCounters) {
            if (c->counterCount(counterName) > 0)
                c->keywordMask |= static_cast<uint32_t>(kw);
        }
    }

    // Helper to parse "&"-separated keyword list into a bitmask
    auto parseKwMask = [](std::string_view kwStr) -> uint32_t {
        uint32_t mask = 0;
        std::string tok;
        std::string s = std::string(kwStr);
        s += '&';
        for (char ch : s) {
            if (ch == '&') {
                if (!tok.empty()) {
                    auto kw = parseKeyword(tok);
                    if (kw != KeywordAbility::None)
                        mask |= static_cast<uint32_t>(kw);
                    tok.clear();
                }
            } else if (ch != ' ' || !tok.empty()) {
                tok += ch;
            }
        }
        return mask;
    };

    // Step 2: scan every battlefield card for static ability lines
    for (const Card* source : m_battlefield.cards()) {
        if (!source->rules) continue;   // guard against stale ownedRules pointers
        for (const auto& line : source->rules->staticAbilityLines) {
            auto s = parseScriptLine(line);

            // ── Continuous (power/toughness/keyword grant/revoke) ─────────
            bool isContinuous    = (s.effectType == "Continuous");
            bool isCantAtk       = (s.effectType == "CantAttack");
            bool isCantBlk       = (s.effectType == "CantBlock");
            bool isCantBlkBy     = (s.effectType == "CantBlockBy");
            bool isLoseAbility   = (s.effectType == "LoseAbility");
            bool isAddType       = (s.effectType == "AddType");
            bool isSetMaxHand    = (s.effectType == "SetMaxHandSize");
            bool isMinMaxBlocker = (s.effectType == "MinMaxBlocker");
            bool isCantTarget    = (s.effectType == "CantTarget");
            bool isCastWithFlash = (s.effectType == "CastWithFlash");
            bool isMustAttack    = (s.effectType == "MustAttack");
            bool isCantGainLife       = (s.effectType == "CantGainLife");
            bool isCantAtkUnless      = (s.effectType == "CantAttackUnless");
            bool isCantBlkUnless      = (s.effectType == "CantBlockUnless");
            bool isCombatDmgToughness = (s.effectType == "CombatDamageToughness");
            bool isActivateAsIfHaste  = (s.effectType == "ActivateAbilityAsIfHaste");
            // Combined mode: "CantAttack,CantBlock" — treat as both CantAttack and CantBlock
            if (s.effectType == "CantAttack,CantBlock" ||
                s.effectType == "CantAttack,CantBlock,CantBeActivated") {
                isCantAtk = true; isCantBlk = true;
            }
            bool isCantBeActivated = (s.effectType == "CantBeActivated" ||
                                      s.effectType == "CantAttack,CantBlock,CantBeActivated");
            bool isAssignDmgAsUnblocked = (s.effectType == "AssignCombatDamageAsUnblocked");
            bool isCantPlayLand         = (s.effectType == "CantPlayLand");
            if (!isContinuous && !isCantAtk && !isCantBlk && !isCantBlkBy &&
                !isLoseAbility && !isAddType && !isSetMaxHand && !isMinMaxBlocker &&
                !isCantTarget && !isCastWithFlash && !isMustAttack && !isCantGainLife &&
                !isCantAtkUnless && !isCantBlkUnless && !isCombatDmgToughness &&
                !isActivateAsIfHaste && !isCantBeActivated &&
                !isAssignDmgAsUnblocked && !isCantPlayLand) continue;

            // ── AdjustLandPlays: extra land plays per turn (Exploration, Azusa,
            //    Oracle of Mul Daya, Dryad of the Ilysian Grove, Mina & Denn, …) ──
            if (auto alp = s.get("AdjustLandPlays", ""); !alp.empty()) {
                int n = 0;
                bool neg = (!alp.empty() && alp[0] == '-');
                for (char ch : alp) if (ch >= '0' && ch <= '9') n = n * 10 + (ch - '0');
                if (neg) n = -n;
                auto aff = s.get("Affected", s.get("PlayerAffected", "You"));
                if (aff == "Each" || aff == "All" || aff == "Player") {
                    extraLandPlays[0] += n; extraLandPlays[1] += n;
                } else if (source->controllerId < 4) {   // "You"/"Controller"/default
                    extraLandPlays[source->controllerId] += n;
                }
                continue;
            }

            // ── SetMaxHandSize: no Affected$ needed — acts on players directly ──
            if (isSetMaxHand) {
                int newMax = s.getInt("Amount", 7);
                if (newMax < 0) newMax = 99; // -1 in Forge means "no maximum"
                auto playerAffected = s.get("PlayerAffected", "You");
                if (playerAffected == "Each" || playerAffected == "All") {
                    m_players[0].setMaxHandSize(std::max(m_players[0].maxHandSize(), newMax));
                    m_players[1].setMaxHandSize(std::max(m_players[1].maxHandSize(), newMax));
                } else {
                    uint8_t pid = (playerAffected == "Opponent")
                                  ? (source->controllerId ^ 1) : source->controllerId;
                    m_players[pid].setMaxHandSize(std::max(m_players[pid].maxHandSize(), newMax));
                }
                continue;
            }

            // ── Condition$ check — skip this static ability if condition is not met ──
            auto condition = s.get("Condition", "");
            if (!condition.empty()) {
                bool condMet = false;
                uint8_t ctrl = source->controllerId;
                if      (condition == "IsHellBent")
                    condMet = m_players[ctrl].hand().empty();
                else if (condition == "Threshold")
                    condMet = static_cast<int>(m_players[ctrl].graveyard().size()) >= 7;
                else if (condition == "Metalcraft") {
                    int arts = 0;
                    for (const Card* c : m_battlefield.cards())
                        if (c->controllerId == ctrl && c->rules->type.isArtifact()) arts++;
                    condMet = (arts >= 3);
                }
                else if (condition == "Morbid")
                    condMet = (creaturesDiedThisTurn > 0);
                else if (condition == "Delirium") {
                    std::set<std::string> types;
                    for (const Card* c : m_players[ctrl].graveyard().cards())
                        for (auto mt : c->rules->type.types)
                            types.insert(std::string(CardType::mainTypeName(mt)));
                    condMet = (types.size() >= 4);
                }
                else if (condition == "Ferocious") {
                    condMet = false;
                    for (const Card* c : m_battlefield.cards())
                        if (c->controllerId == ctrl && c->isCreature() && effectivePower(*c) >= 4)
                            { condMet = true; break; }
                }
                else if (condition == "SpellMastery") {
                    // True if you have 2 or more instant and/or sorcery cards in your GY
                    int spellCount = 0;
                    for (const Card* c : m_players[ctrl].graveyard().cards())
                        if (c->rules->type.isInstant() || c->rules->type.isSorcery())
                            if (++spellCount >= 2) break;
                    condMet = (spellCount >= 2);
                }
                else if (condition == "IsRevolt" || condition == "Revolt")
                    condMet = permanentLeftBattlefieldThisTurn[ctrl];
                else if (condition == "Raid" || condition == "IsRaid")
                    condMet = attackedThisTurn[ctrl];
                else {
                    condMet = true; // unknown condition: apply conservatively
                }
                if (!condMet) continue;
            }

            // IsPresent$ / PresentCompare$ — counts matching cards on the battlefield;
            // the ability only applies when the count matches (e.g. Level Up level checks).
            auto isPresent = s.get("IsPresent", "");
            if (!isPresent.empty()) {
                auto compare = s.get("PresentCompare", "EQ1");
                // compare is always "EQ1" for Level Up (exactly 1 matching card must exist)
                int needed = 1;
                bool gt = false, lt = false;
                std::string_view compSV(compare);
                if      (compSV.substr(0, 2) == "GE") { std::from_chars(compSV.data()+2, compSV.data()+compSV.size(), needed); gt = true; lt = true; }
                else if (compSV.substr(0, 2) == "GT") { std::from_chars(compSV.data()+2, compSV.data()+compSV.size(), needed); gt = true; }
                else if (compSV.substr(0, 2) == "LE") { std::from_chars(compSV.data()+2, compSV.data()+compSV.size(), needed); lt = true; }
                else if (compSV.substr(0, 2) == "EQ") { std::from_chars(compSV.data()+2, compSV.data()+compSV.size(), needed); }

                int count = 0;
                for (const Card* c : m_battlefield.cards()) {
                    if (cardMatchesAnyFilter(*c, std::string(isPresent), source->controllerId, source->id, source, this))
                        ++count;
                }
                bool ok = false;
                if (gt && lt)       ok = (count >= needed);    // GE
                else if (gt)        ok = (count > needed);     // GT
                else if (lt)        ok = (count <= needed);    // LE
                else /* EQ */       ok = (count == needed);
                if (!ok) continue;
            }

            // ── CantBlockBy: mark matching attackers as unblockable or filter-blocked ──
            // Uses ValidAttacker$/ValidBlocker$ instead of Affected$, so handled early.
            if (isCantBlkBy) {
                auto validAtk  = s.get("ValidAttacker", "");
                auto validBlk  = s.get("ValidBlocker", "");
                if (!validAtk.empty()) {
                    for (Card* target : m_battlefield.cards()) {
                        if (!cardMatchesAnyFilter(*target, validAtk,
                                                  source->controllerId, source->id, source, this))
                            continue;
                        if (validBlk.empty()) {
                            target->unblockable = true;
                        } else {
                            target->blockOnlyBy = std::string(validBlk);
                        }
                    }
                }
                continue;
            }

            // ── MinMaxBlocker: cap number of creatures that may block a matching permanent ──
            if (isMinMaxBlocker) {
                auto validCard = s.get("ValidCard", "Card.Self");
                int maxBlk = s.getInt("Max", INT_MAX);
                for (Card* target : m_battlefield.cards()) {
                    if (!cardMatchesAnyFilter(*target, validCard, source->controllerId, source->id, source, this))
                        continue;
                    if (maxBlk < target->maxBlockerCount)
                        target->maxBlockerCount = maxBlk;
                }
                continue;
            }

            // ── CantTarget: mark matching battlefield permanents as un-targetable ──
            if (isCantTarget) {
                auto validCard = s.get("ValidCard", s.get("Affected", ""));
                if (!validCard.empty()) {
                    for (Card* target : m_battlefield.cards()) {
                        if (!cardMatchesAnyFilter(*target, validCard, source->controllerId, source->id, source, this))
                            continue;
                        target->cantBeTargeted = true;
                    }
                }
                continue;
            }

            // ── CantGainLife: prevent life gain for matching players ──
            if (isCantGainLife) {
                auto playerAffected = s.get("PlayerAffected", "Opponent");
                if (playerAffected == "You" || playerAffected == "Controller")
                    cantGainLife[source->controllerId] = true;
                else if (playerAffected == "Each" || playerAffected == "All" || playerAffected == "Both")
                    { cantGainLife[0] = true; cantGainLife[1] = true; }
                else // Opponent (default for most "can't gain life" effects)
                    cantGainLife[source->controllerId ^ 1] = true;
                continue;
            }

            // ── CantAttackUnless / CantBlockUnless ────────────────────────────
            // In AI-only mode, treat these as unconditional CantAttack/CantBlock;
            // the AI won't pay optional costs to satisfy "unless" conditions.
            if (isCantAtkUnless) {
                auto validCard = s.get("ValidCard", s.get("Affected", "Creature.Self"));
                for (Card* target : m_battlefield.cards()) {
                    if (!cardMatchesAnyFilter(*target, validCard, source->controllerId, source->id, source, this))
                        continue;
                    target->cantAttack = true;
                }
                continue;
            }
            if (isCantBlkUnless) {
                auto validCard = s.get("ValidCard", s.get("Affected", "Creature.Self"));
                for (Card* target : m_battlefield.cards()) {
                    if (!cardMatchesAnyFilter(*target, validCard, source->controllerId, source->id, source, this))
                        continue;
                    target->cantBlock = true;
                }
                continue;
            }

            // ── CombatDamageToughness: matching creatures deal combat damage equal
            //    to toughness instead of power (e.g. Doran, the Siege Tower) ──────
            if (isCombatDmgToughness) {
                auto validCard = s.get("ValidCard", s.get("Affected", "Creature.All"));
                for (Card* target : m_battlefield.cards()) {
                    if (!target->isCreature()) continue;
                    if (!cardMatchesAnyFilter(*target, validCard, source->controllerId, source->id, source, this))
                        continue;
                    target->dealsDamageByToughness = true;
                }
                continue;
            }

            // ── CastWithFlash: handled at cast-time via GameState::hasFlashGrant() ──
            if (isCastWithFlash) continue;

            // ── ActivateAbilityAsIfHaste: matching permanents can use tap abilities
            //    even with summoning sickness (Thousand-Year Elixir, Tyvar, etc.) ──
            if (isActivateAsIfHaste) {
                auto validCard = s.get("ValidCard", s.get("Affected", "Creature.YouCtrl"));
                for (Card* target : m_battlefield.cards()) {
                    if (!cardMatchesAnyFilter(*target, validCard, source->controllerId,
                                             source->id, source, this))
                        continue;
                    target->activateAbilityAsIfHaste = true;
                }
                continue;
            }

            // ── CantBeActivated: matching permanents can't use activated abilities ──
            // Used by Pithing Needle, Suppression Field, etc.
            // For simplicity we model this as allAbilitiesRemoved (AI won't activate).
            if (isCantBeActivated) {
                auto validCard = s.get("ValidCard", s.get("Affected", ""));
                for (Card* target : m_battlefield.cards()) {
                    if (!validCard.empty() &&
                        !cardMatchesAnyFilter(*target, validCard, source->controllerId, source->id, source, this))
                        continue;
                    target->allAbilitiesRemoved = true;
                }
                continue;
            }

            // ── AssignCombatDamageAsUnblocked: matching creatures deal combat damage
            //    as though they were unblocked (like Shadow in a different dimension).
            //    Set unblockable so the AI / combat damage code treats them as unblocked. ──
            if (isAssignDmgAsUnblocked) {
                auto validCard = s.get("ValidCard", s.get("Affected", "Creature.Self"));
                for (Card* target : m_battlefield.cards()) {
                    if (!target->isCreature()) continue;
                    if (!cardMatchesAnyFilter(*target, validCard, source->controllerId, source->id, source, this))
                        continue;
                    target->unblockable = true;
                }
                continue;
            }

            // ── CantPlayLand: prevent a player from playing lands this turn ──
            if (isCantPlayLand) {
                // Tracked via cantPlayLand[] on the GameState (set below per player)
                auto playerAffected = s.get("PlayerAffected", "Opponent");
                if (playerAffected == "You" || playerAffected == "Controller")
                    cantPlayLand[source->controllerId] = true;
                else if (playerAffected == "Each" || playerAffected == "All")
                    { cantPlayLand[0] = true; cantPlayLand[1] = true; }
                else
                    cantPlayLand[source->controllerId ^ 1] = true;
                continue;
            }

            // ── MustAttack: force matching creatures to attack if able ──
            if (isMustAttack) {
                auto validCard = s.get("ValidCard", s.get("Affected", "Creature.OppCtrl"));
                // MustAttack$ You — direction of attack (who they must attack)
                auto mustAtkTgt = s.get("MustAttack", "");
                uint8_t targetPid = 255; // any player
                if (mustAtkTgt == "You") targetPid = source->controllerId;
                for (Card* target : m_battlefield.cards()) {
                    if (!target->isCreature()) continue;
                    if (!cardMatchesAnyFilter(*target, validCard, source->controllerId, source->id, source, this))
                        continue;
                    target->mustAttack       = true;
                    target->mustAttackTarget = targetPid;
                }
                continue;
            }

            auto affected = s.get("Affected", "");
            if (affected.empty()) continue;

            // AddPower$/AddToughness$ may be a plain int or an SVar name like "X"
            auto resolveSVarInt = [&](std::string_view key, int def) -> int {
                auto val = s.get(key, "");
                if (val.empty()) return def;
                int n = def;
                auto [p, ec] = std::from_chars(val.data(), val.data() + val.size(), n);
                if (ec == std::errc{}) return n;
                // SVar reference — look it up in the source card
                auto it = source->rules->svars.find(std::string(val));
                if (it == source->rules->svars.end()) return def;
                return evaluateCountSVar(it->second, *this, source->controllerId, source->id);
            };

            // ── AddType: grant creature subtypes to all matching battlefield cards ──
            // AddType$ may be multi-word (e.g. "Elf Warrior") — add each token separately.
            if (isAddType) {
                auto addTypeStr = s.get("AddType", "");
                if (addTypeStr.empty() || affected.empty()) continue;
                // Split by spaces into individual subtype tokens
                std::vector<std::string_view> typeTokens;
                {
                    std::string_view sv(addTypeStr);
                    while (!sv.empty()) {
                        auto sp = sv.find(' ');
                        typeTokens.push_back(sp == std::string_view::npos ? sv : sv.substr(0, sp));
                        sv = sp == std::string_view::npos ? std::string_view{} : sv.substr(sp + 1);
                    }
                }
                for (Card* target : m_battlefield.cards()) {
                    if (!cardMatchesAnyFilter(*target, affected, source->controllerId, source->id, source, this))
                        continue;
                    for (auto tok : typeTokens)
                        target->continuousSubtypes.emplace_back(tok);
                }
                continue;
            }

            // AddAbility$ <SVar> — grant an activated ability to matching
            // permanents (Chromatic Lantern: lands you control gain
            // "{T}: Add one mana of any colour"). The SVar value is the
            // ability-line string, parsed on activation just like a printed one.
            if (auto addAbil = s.get("AddAbility", ""); !addAbil.empty()) {
                auto svIt = source->rules->svars.find(std::string(addAbil));
                if (svIt != source->rules->svars.end() && !affected.empty()) {
                    for (Card* target : m_battlefield.cards()) {
                        if (!cardMatchesAnyFilter(*target, affected, source->controllerId,
                                                  source->id, source, this))
                            continue;
                        target->grantedAbilities.push_back(svIt->second);
                    }
                }
                continue;
            }

            int addPow        = resolveSVarInt("AddPower",     0);
            int addTgh        = resolveSVarInt("AddToughness", 0);
            int setPow        = s.getInt("SetPower",     -1);
            int setTgh        = s.getInt("SetToughness", -1);
            uint32_t kwMsk    = (isContinuous || isLoseAbility)
                                ? parseKwMask(s.get("AddKeyword",    "")) : 0;
            uint32_t removeKw = (isContinuous || isLoseAbility)
                                ? parseKwMask(s.get("RemoveKeyword", "")) : 0;
            bool removeAll    = isContinuous && (s.get("RemoveAllAbilities", "") == "True");
            // RemoveAllAbilities strips every keyword (all 32 bits)
            if (removeAll) removeKw = 0xFFFFFFFFu;

            auto applyToCard = [&](Card* t) {
                t->continuousPower     += addPow;
                t->continuousToughness += addTgh;
                t->continuousKeywords  |= kwMsk;
                t->keywordMask         |= kwMsk;
                if (removeKw) { t->removedKeywords |= removeKw; t->keywordMask &= ~removeKw; }
                if (removeAll) t->allAbilitiesRemoved = true;
                if (isCantAtk) t->cantAttack = true;
                if (isCantBlk) t->cantBlock  = true;
                if (setPow >= 0) t->setPower     = setPow;
                if (setTgh >= 0) t->setToughness = setTgh;
            };

            // EquippedBy / EnchantedCard: applies only to the attached card
            bool equipTargeting =
                (affected.find("EquippedBy")    != std::string_view::npos) ||
                (affected.find("EnchantedCard") != std::string_view::npos) ||
                (affected.find("Enchanted")     != std::string_view::npos);

            if (equipTargeting) {
                if (source->attachedTo == kInvalidId) continue;
                Card* t = findCard(source->attachedTo);
                if (!t || !t->isOnBattlefield()) continue;
                applyToCard(t);
                continue;
            }

            // General filter: apply to all matching battlefield cards
            for (Card* target : m_battlefield.cards()) {
                if (!cardMatchesAnyFilter(*target, affected, source->controllerId,
                                         source->id, source, this))
                    continue;
                applyToCard(target);
            }
        }
    }

    // Remove stale Static-duration effects from prior recomputes and regenerate them.
    // UntilEOT and Permanent effects are preserved — they are managed externally.
    m_continuousEffects.erase(
        std::remove_if(m_continuousEffects.begin(), m_continuousEffects.end(),
            [](const ContinuousEffect& e) {
                return e.duration == ContinuousEffect::Duration::Static;
            }),
        m_continuousEffects.end());

    // Apply Rule 613 layer-ordered effects from m_continuousEffects.
    LayerEngine::apply(*this);

    // Re-apply "loses keyword until end of turn" (Debuff) last, so a keyword the
    // creature would otherwise regain from a continuous grant stays removed.
    for (Card* c : m_battlefield.cards())
        if (c->tempRemovedKeywords) c->keywordMask &= ~c->tempRemovedKeywords;
}

bool GameState::hasFlashGrant(const Card& c) const noexcept {
    for (const Card* source : m_battlefield.cards()) {
        for (const auto& line : source->rules->staticAbilityLines) {
            auto s = parseScriptLine(line);
            if (s.effectType != "CastWithFlash") continue;
            auto affected = s.get("Affected", "");
            if (affected.empty()) continue;
            // Optional condition gate. Currently supported:
            //   Condition$ OpponentCastSpellThisTurn (Captain Mar-Vell's Cosmic Awareness).
            auto cond = s.get("Condition", "");
            if (cond == "OpponentCastSpellThisTurn") {
                uint8_t opp = source->controllerId ^ 1;
                if (spellsCastByPlayer[opp] <= 0) continue;
            }
            if (cardMatchesAnyFilter(c, std::string(affected), source->controllerId, source->id, source, this))
                return true;
        }
    }
    return false;
}

void GameState::printState() const {
    std::cout << "=== Game State (Turn " << m_turnNumber << ") ===\n";
    std::cout << "Active player: " << m_players[m_activePlayerId].name() << '\n';
    std::cout << '\n';

    for (const auto& p : m_players) {
        std::cout << p.name()
                  << "  Life=" << p.life()
                  << "  Poison=" << p.poisonCounters()
                  << "  Library=" << p.library().size()
                  << "  Hand=" << p.hand().size()
                  << "  Graveyard=" << p.graveyard().size()
                  << '\n';

        std::cout << "  Mana pool: " << p.manaPool().toString() << '\n';

        if (!p.hand().empty()) {
            std::cout << "  Hand:\n";
            for (const auto* c : p.hand().cards())
                std::cout << "    [" << c->id << "] " << c->name()
                          << "  " << c->rules->manaCost.toString() << '\n';
        }

        if (!p.graveyard().empty()) {
            std::cout << "  Graveyard:\n";
            for (const auto* c : p.graveyard().cards())
                std::cout << "    [" << c->id << "] " << c->name() << '\n';
        }
    }

    if (!m_battlefield.empty()) {
        std::cout << "\nBattlefield:\n";
        for (const auto* c : m_battlefield.cards()) {
            std::cout << "  [" << c->id << "] "
                      << m_players[c->controllerId].name() << " controls "
                      << c->name()
                      << (c->tapped ? " [tapped]" : "")
                      << (c->summoningSickness && c->isCreature() ? " [sick]" : "");
            if (c->isCreature())
                std::cout << "  " << c->rules->power << '/' << c->rules->toughness;
            if (c->markedDamage > 0)
                std::cout << "  damage=" << c->markedDamage;
            std::cout << '\n';
        }
    }

    if (!m_stack.empty()) {
        std::cout << "\nStack (top first):\n";
        for (const auto* c : m_stack.cards())
            std::cout << "  [" << c->id << "] " << c->name() << '\n';
    }

    std::cout << '\n';
}

} // namespace mtg
