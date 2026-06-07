#include "TriggerSystem.h"
#include "GameState.h"
#include "CardFilter.h"
#include "CardStats.h"
#include "ZoneType.h"
#include "ability/ScriptLine.h"
#include "../core/card/CardRules.h"
#include <charconv>
#include <climits>
#include <set>
#include <utility>

namespace mtg {

// zoneName is already defined as inline in ZoneType.h — no local duplicate needed

// Returns false if the card's triggered abilities are suppressed (e.g. Humility, face-down morph).
static bool canFireTriggers(const Card& c) noexcept {
    return !c.allAbilitiesRemoved && !c.isFaceDown;
}

// Resolve the Execute$ SVar from a trigger line into a ScriptLine.
static ScriptLine resolveExecute(const Card& owner, std::string_view execName) {
    if (execName.empty()) return {};
    auto it = owner.rules->svars.find(std::string(execName));
    if (it == owner.rules->svars.end()) return {};
    return parseScriptLine(it->second);
}

// ── Zone-change trigger check ─────────────────────────────────────────────────

bool TriggerSystem::checkZoneChangeTrigger(const Card& triggerOwner,
                                            const ScriptLine& trig,
                                            const Card& movedCard,
                                            ZoneType from, ZoneType to,
                                            std::vector<PendingTrigger>& out,
                                            const GameState* game) {
    if (triggerOwner.allAbilitiesRemoved) return false;
    // T:Mode$ Dies is a shorthand for ChangesZone | Origin$ Battlefield | Destination$ Graveyard
    bool isDies = (trig.effectType == "Dies");
    if (!isDies && trig.effectType != "ChangesZone") return false;

    // Origin check (Any = wildcard)
    auto origin = isDies ? "Battlefield" : std::string(trig.get("Origin", "Any"));
    if (origin != "Any" && origin != zoneName(from)) return false;

    // Destination check
    auto dest = isDies ? "Graveyard" : std::string(trig.get("Destination", "Any"));
    if (dest != "Any" && dest != zoneName(to)) return false;

    // ValidCard$ check — does the moved card match?
    // Can be comma-separated groups (e.g. "Card.Self,Creature.Other"); any match fires.
    auto validCard = trig.get("ValidCard", "Card.Self");

    bool matches = false;
    std::string_view remaining = validCard;
    while (!remaining.empty() && !matches) {
        auto comma = remaining.find(',');
        std::string_view part = (comma == std::string_view::npos)
                                ? remaining : remaining.substr(0, comma);
        remaining = (comma == std::string_view::npos)
                    ? std::string_view{} : remaining.substr(comma + 1);

        if (part.find("Self") != std::string_view::npos) {
            // "Card.Self" → fires only when the trigger owner itself moves
            matches = (triggerOwner.id == movedCard.id);
        } else {
            // General filter — "Other" excludes the trigger owner
            ObjectId selfId = (part.find("Other") != std::string_view::npos)
                              ? triggerOwner.id : kInvalidId;
            matches = cardMatchesFilter(movedCard, part,
                                        triggerOwner.controllerId, selfId,
                                        &triggerOwner, game);
        }
    }

    if (!matches) return false;

    // TriggerCondition$ TribNotPaid / TribPaid — tribute conditional triggers
    auto tribCond = trig.get("TriggerCondition", "");
    if (tribCond == "TribNotPaid" && movedCard.tributed) return false;
    if (tribCond == "TribPaid"    && !movedCard.tributed) return false;

    // ActiveZones$ — the trigger owner must be (or have just been) in this zone.
    // For self-zone-change triggers (e.g. "when ~ dies"), the card has already
    // moved when we arrive here, so we also accept if it came FROM the required zone.
    auto activeZones = trig.get("ActiveZones", "Battlefield");
    if (activeZones.find("Battlefield") != std::string_view::npos) {
        bool wasOnBattlefield = triggerOwner.isOnBattlefield() ||
                                (triggerOwner.id == movedCard.id &&
                                 from == ZoneType::Battlefield);
        if (!wasOnBattlefield) return false;
    }

    // Resolve the Execute$ SVar to get the effect script
    auto execName = trig.get("Execute", "");
    auto effect   = resolveExecute(triggerOwner, execName);
    if (effect.empty()) return false;

    // Per-turn trigger limit (ActivationLimit$ N — "triggers only once each turn").
    int lim = trig.getInt("ActivationLimit", 0);
    if (lim > 0 && !triggerOwner.tryTriggerLimit(execName, lim)) return false;

    out.push_back({ triggerOwner.id, triggerOwner.controllerId, std::move(effect), movedCard.id });
    return true;
}

// ── Public API ────────────────────────────────────────────────────────────────

void TriggerSystem::onZoneChange(const Card& card,
                                  ZoneType from, ZoneType to,
                                  const GameState& game,
                                  std::vector<PendingTrigger>& out) {
    if (!canFireTriggers(card)) return;
    // ETB / departure triggers on the card itself
    for (const auto& rawLine : card.rules->triggerLines) {
        auto trig = parseScriptLine(rawLine);
        checkZoneChangeTrigger(card, trig, card, from, to, out, &game);

        // ChangesZoneAll self-trigger (e.g. "whenever one or more Vampires enter,
        // including this one"). Deduplicate against already-queued `out` entries.
        if (trig.effectType == "ChangesZoneAll") {
            auto origin = std::string(trig.get("Origin", "Any"));
            if (origin != "Any" && origin != zoneName(from)) continue;
            auto dest = std::string(trig.get("Destination", "Any"));
            if (dest != "Any" && dest != zoneName(to)) continue;
            auto validCards = std::string(trig.get("ValidCards", "Card"));
            if (!validCards.empty() && validCards != "Card" && validCards != "Any" &&
                !cardMatchesAnyFilter(card, validCards, card.controllerId, card.id, &card, &game))
                continue;
            auto execName = trig.get("Execute", "");
            auto effect   = resolveExecute(card, execName);
            if (effect.empty()) continue;
            bool duped = false;
            for (const auto& pt : out)
                if (pt.sourceCardId == card.id && pt.effect.effectType == effect.effectType)
                    { duped = true; break; }
            if (!duped)
                out.push_back({ card.id, card.controllerId, std::move(effect), card.id });
        }
    }
}

void TriggerSystem::onZoneChangeGlobal(const Card& movedCard,
                                        ZoneType from, ZoneType to,
                                        const GameState& game,
                                        std::vector<PendingTrigger>& out) {
    // Scan every battlefield card for "when another creature enters" patterns
    for (const Card* watcher : game.battlefield().cards()) {
        if (!watcher->rules) continue;  // guard against stale ownedRules
        if (watcher->id == movedCard.id) continue; // handled by onZoneChange
        if (!canFireTriggers(*watcher)) continue;
        for (const auto& rawLine : watcher->rules->triggerLines) {
            auto trig = parseScriptLine(rawLine);
            checkZoneChangeTrigger(*watcher, trig, movedCard, from, to, out, &game);

            // ChangesZoneAll: "whenever one or more matching cards enter/leave".
            // Fires once per watcher per batch; dedup against already-queued `out`.
            if (trig.effectType == "ChangesZoneAll") {
                auto origin = std::string(trig.get("Origin", "Any"));
                if (origin != "Any" && origin != zoneName(from)) continue;
                auto dest = std::string(trig.get("Destination", "Any"));
                if (dest != "Any" && dest != zoneName(to)) continue;
                auto validCards = std::string(trig.get("ValidCards", "Card"));
                if (!validCards.empty() && validCards != "Card" && validCards != "Any" &&
                    !cardMatchesAnyFilter(movedCard, validCards,
                                         watcher->controllerId, watcher->id, watcher, &game))
                    continue;
                auto execName = trig.get("Execute", "");
                auto effect   = resolveExecute(*watcher, execName);
                if (effect.empty()) continue;
                bool duped = false;
                for (const auto& pt : out)
                    if (pt.sourceCardId == watcher->id &&
                        pt.effect.effectType == effect.effectType)
                        { duped = true; break; }
                if (!duped)
                    out.push_back({ watcher->id, watcher->controllerId,
                                    std::move(effect), movedCard.id });
            }
        }

        // Graft: when another creature enters the BF, the Graft creature's controller
        // may move a +1/+1 counter from it to the entering creature.  AI always moves
        // one counter if it controls both creatures and the Graft creature has counters.
        if (watcher->rules->hasGraft &&
            to == ZoneType::Battlefield &&
            movedCard.rules->isCreature() &&
            movedCard.id != watcher->id &&
            watcher->counterCount("+1/+1") > 0) {
            // We handle the transfer as a direct effect here (no human choice for now)
            // by emitting a counter-move effect.
            auto effect = parseScriptLine(
                "DB$ MoveCounter | CounterType$ P1P1 | Source$ TriggeredCard | Dest$ Card.Self");
            // Use a simplified direct manipulation: queue the counter move
            // The actual transfer happens in the triggered ability handler;
            // for simplicity, handle it inline here.
            Card* mutableWatcher = const_cast<Card*>(watcher);
            if (mutableWatcher->counters.count("+1/+1") && mutableWatcher->counters["+1/+1"] > 0) {
                mutableWatcher->counters["+1/+1"]--;
                Card* entering = const_cast<Card*>(&movedCard);
                entering->addCounter("+1/+1", 1);
            }
        }

        // Evolve: when a creature with greater power or toughness enters the battlefield
        // under your control, put a +1/+1 counter on this creature.
        if (watcher->rules->hasEvolve &&
            to == ZoneType::Battlefield &&
            movedCard.rules->isCreature() &&
            watcher->controllerId == movedCard.controllerId) {
            // Use raw base stats from rules for the comparison (simplified)
            auto parseStat = [](const std::string& s) -> int {
                if (s.empty() || s == "*") return 0;
                int v = 0;
                std::from_chars(s.data(), s.data() + s.size(), v);
                return v;
            };
            int newPow = parseStat(movedCard.rules->power);
            int newTgh = parseStat(movedCard.rules->toughness);
            int myPow  = parseStat(watcher->rules->power) + watcher->counterCount("+1/+1")
                         - watcher->counterCount("-1/-1");
            int myTgh  = parseStat(watcher->rules->toughness) + watcher->counterCount("+1/+1")
                         - watcher->counterCount("-1/-1");
            if (newPow > myPow || newTgh > myTgh) {
                auto effect = parseScriptLine(
                    "DB$ PutCounter | CounterType$ P1P1 | CounterNum$ 1 | Defined$ TriggeredCard");
                out.push_back({ watcher->id, watcher->controllerId,
                                std::move(effect), watcher->id });
            }
        }
    }
}

void TriggerSystem::onAttack(const Card& attacker,
                              const GameState& game,
                              std::vector<PendingTrigger>& out) {
    // Attacker's own "whenever ~ attacks" triggers (ValidCard$ Card.Self)
    if (canFireTriggers(attacker)) {
        for (const auto& rawLine : attacker.rules->triggerLines) {
            auto trig = parseScriptLine(rawLine);
            if (trig.effectType != "Attacks") continue;
            auto validCard = trig.get("ValidCard", "Card.Self");
            if (validCard.find("Self") == std::string_view::npos) continue;
            // Skip count-conditional self-triggers — handled by onAttackersFinalized
            if (!trig.get("MaxAttackers", "").empty() ||
                !trig.get("MinAttackers", "").empty()) continue;
            auto execName = trig.get("Execute", "");
            auto effect   = resolveExecute(attacker, execName);
            if (effect.empty()) continue;
            out.push_back({ attacker.id, attacker.controllerId, std::move(effect), attacker.id });
        }
    }

    if (canFireTriggers(attacker)) {
        // Battle Cry: when this creature attacks, each other attacking creature gets +1/+0
        if (attacker.rules->hasBattleCry) {
            auto effect = parseScriptLine(
                "DB$ PumpAll | NumAtt$ 1 | NumDef$ 0 | ValidCards$ Creature.attacking+Other");
            out.push_back({ attacker.id, attacker.controllerId, std::move(effect), attacker.id });
        }

        // Myriad: when attacks, create a token copy attacking each other opponent.
        // In 2-player there are no "other opponents" so this fires only in 4-player.
        if (attacker.rules->hasMyriad && game.numPlayers() > 2) {
            uint8_t ctrl = attacker.controllerId;
            uint8_t ap   = game.activePlayerId();
            for (uint8_t p = 0; p < game.numPlayers(); ++p) {
                if (p == ap || p == ctrl) continue;  // not the active player or controller
                // Queue a token-creation effect for each other player
                auto effect = parseScriptLine(
                    "DB$ CopyPermanent | Defined$ TriggeredCard | CopyAndAttack$ True");
                out.push_back({ attacker.id, ctrl, std::move(effect), attacker.id });
            }
        }

        // Boast: mark this creature as having attacked (enables boast activations)
        if (attacker.rules->hasBoast) {
            Card* mut = const_cast<Card*>(&attacker);
            mut->attackedThisTurn = true;
        }
    }

    // Global battlefield watchers: "whenever a creature attacks" on other permanents
    for (const Card* watcher : game.battlefield().cards()) {
            if (!watcher->rules) continue;  // guard stale ownedRules
        if (watcher->id == attacker.id) continue;
        if (!canFireTriggers(*watcher)) continue;
        for (const auto& rawLine : watcher->rules->triggerLines) {
            auto trig = parseScriptLine(rawLine);
            if (trig.effectType != "Attacks") continue;

            // If ValidCard$ contains "Self", this is a self-trigger — already handled above
            auto validCard = std::string(trig.get("ValidCard", ""));
            if (validCard.empty() || validCard.find("Self") != std::string_view::npos) continue;

            // Skip count-conditional triggers — handled by onAttackersFinalized after
            // all attackers are known
            if (!trig.get("MaxAttackers", "").empty() ||
                !trig.get("MinAttackers", "").empty()) continue;

            if (!cardMatchesAnyFilter(attacker, validCard,
                                      watcher->controllerId, watcher->id, watcher, &game)) continue;

            // Optional controller restriction
            auto ctrlCond = trig.get("ControllerIs", "");
            if (ctrlCond.empty()) ctrlCond = trig.get("ValidActivatingPlayer", "");
            if (ctrlCond == "You"      && watcher->controllerId != attacker.controllerId) continue;
            if (ctrlCond == "Opponent" && watcher->controllerId == attacker.controllerId) continue;

            auto execName = trig.get("Execute", "");
            auto effect   = resolveExecute(*watcher, execName);
            if (effect.empty()) continue;
            out.push_back({ watcher->id, watcher->controllerId, std::move(effect), attacker.id });
        }
    }
}

void TriggerSystem::onAttackersFinalized(const GameState& game,
                                          std::vector<PendingTrigger>& out) {
    // Count attacking creatures (set by declareAttacker before we're called)
    int attackerCount = 0;
    std::vector<const Card*> attackers;
    for (const Card* c : game.battlefield().cards()) {
            if (!c->rules) continue;  // guard stale ownedRules
        if (c->attacking) {
            ++attackerCount;
            attackers.push_back(c);
        }
    }
    if (attackerCount == 0) return;

    // AttackersDeclared: fires once per watcher when the active player attacks.
    // AttackingPlayer$ You / Opponent — relative to the watcher's controller.
    uint8_t atkPlayer = game.activePlayerId();
    for (const Card* watcher : game.battlefield().cards()) {
        for (const auto& rawLine : watcher->rules->triggerLines) {
            auto trig = parseScriptLine(rawLine);
            if (trig.effectType != "AttackersDeclared") continue;

            auto atkCond = trig.get("AttackingPlayer", "You");
            bool watcherIsAttacker = (watcher->controllerId == atkPlayer);
            if (atkCond == "You"      && !watcherIsAttacker) continue;
            if (atkCond == "Opponent" &&  watcherIsAttacker) continue;

            auto execName = trig.get("Execute", "");
            auto effect   = resolveExecute(*watcher, execName);
            if (effect.empty()) continue;
            out.push_back({ watcher->id, watcher->controllerId, std::move(effect) });
        }
    }

    // Scan all battlefield cards for T:Mode$ Attacks triggers with
    // MaxAttackers$ or MinAttackers$ — these fire after all attackers are known.
    for (const Card* watcher : game.battlefield().cards()) {
        for (const auto& rawLine : watcher->rules->triggerLines) {
            auto trig = parseScriptLine(rawLine);
            if (trig.effectType != "Attacks") continue;

            auto maxAtkSv = trig.get("MaxAttackers", "");
            auto minAtkSv = trig.get("MinAttackers", "");
            if (maxAtkSv.empty() && minAtkSv.empty()) continue;

            int maxN = INT_MAX, minN = 0;
            if (!maxAtkSv.empty())
                std::from_chars(maxAtkSv.data(), maxAtkSv.data() + maxAtkSv.size(), maxN);
            if (!minAtkSv.empty())
                std::from_chars(minAtkSv.data(), minAtkSv.data() + minAtkSv.size(), minN);

            if (attackerCount < minN || attackerCount > maxN) continue;

            auto validCard = std::string(trig.get("ValidCard", ""));
            auto execName  = trig.get("Execute", "");

            // Optional controller restriction
            auto ctrlCond = trig.get("ControllerIs", "");
            if (ctrlCond.empty()) ctrlCond = trig.get("ValidActivatingPlayer", "");

            // Fire the trigger once per matching attacking creature
            for (const Card* atk : attackers) {
                if (ctrlCond == "You"      && watcher->controllerId != atk->controllerId) continue;
                if (ctrlCond == "Opponent" && watcher->controllerId == atk->controllerId) continue;

                if (!validCard.empty() &&
                    !cardMatchesAnyFilter(*atk, validCard, watcher->controllerId, watcher->id, watcher, &game))
                    continue;

                auto effect = resolveExecute(*watcher, execName);
                if (effect.empty()) continue;
                out.push_back({ watcher->id, watcher->controllerId, std::move(effect), atk->id });
            }
        }
    }

}

void TriggerSystem::onBlock(const Card& blocker, const Card& attacker,
                             const GameState& game,
                             std::vector<PendingTrigger>& out) {
    // Blocker's own "whenever ~ blocks" triggers
    for (const auto& rawLine : blocker.rules->triggerLines) {
        auto trig = parseScriptLine(rawLine);
        if (trig.effectType != "Blocks") continue;
        auto validCard = trig.get("ValidCard", "Card.Self");
        if (validCard.find("Self") == std::string_view::npos) continue;
        auto execName = trig.get("Execute", "");
        auto effect   = resolveExecute(blocker, execName);
        if (effect.empty()) continue;
        out.push_back({ blocker.id, blocker.controllerId, std::move(effect), blocker.id });
    }

    // Global battlefield watchers: "whenever a creature blocks" on other permanents
    for (const Card* watcher : game.battlefield().cards()) {
            if (!watcher->rules) continue;  // guard stale ownedRules
        if (watcher->id == blocker.id) continue;
        if (!canFireTriggers(*watcher)) continue;
        for (const auto& rawLine : watcher->rules->triggerLines) {
            auto trig = parseScriptLine(rawLine);
            if (trig.effectType != "Blocks") continue;
            auto validCard = std::string(trig.get("ValidCard", ""));
            if (validCard.empty() || validCard.find("Self") != std::string_view::npos) continue;
            if (!cardMatchesAnyFilter(blocker, validCard,
                                      watcher->controllerId, watcher->id, watcher, &game)) continue;
            auto ctrlCond = trig.get("ControllerIs", "");
            if (ctrlCond == "You"      && watcher->controllerId != blocker.controllerId) continue;
            if (ctrlCond == "Opponent" && watcher->controllerId == blocker.controllerId) continue;
            auto execName = trig.get("Execute", "");
            auto effect   = resolveExecute(*watcher, execName);
            if (effect.empty()) continue;
            out.push_back({ watcher->id, watcher->controllerId, std::move(effect), blocker.id });
        }
    }
    // Attacker's own "whenever ~ becomes blocked" triggers
    for (const auto& rawLine : attacker.rules->triggerLines) {
        auto trig = parseScriptLine(rawLine);
        if (trig.effectType != "BecomesBlocked") continue;
        auto validCard = trig.get("ValidAttacker", trig.get("ValidCard", "Card.Self"));
        if (validCard.find("Self") == std::string_view::npos) continue;
        auto execName = trig.get("Execute", "");
        auto effect   = resolveExecute(attacker, execName);
        if (effect.empty()) continue;
        out.push_back({ attacker.id, attacker.controllerId, std::move(effect), attacker.id });
    }
    // Global battlefield watchers: "whenever a creature becomes blocked"
    for (const Card* watcher : game.battlefield().cards()) {
            if (!watcher->rules) continue;  // guard stale ownedRules
        if (watcher->id == attacker.id || watcher->id == blocker.id) continue;
        if (!canFireTriggers(*watcher)) continue;
        for (const auto& rawLine : watcher->rules->triggerLines) {
            auto trig = parseScriptLine(rawLine);
            if (trig.effectType != "BecomesBlocked") continue;
            auto validCard = std::string(trig.get("ValidAttacker", trig.get("ValidCard", "")));
            if (validCard.empty() || validCard.find("Self") != std::string_view::npos) continue;
            if (!cardMatchesAnyFilter(attacker, validCard,
                                      watcher->controllerId, watcher->id, watcher, &game)) continue;
            auto execName = trig.get("Execute", "");
            auto effect   = resolveExecute(*watcher, execName);
            if (effect.empty()) continue;
            out.push_back({ watcher->id, watcher->controllerId, std::move(effect), attacker.id });
        }
    }

    // Bushido N: gets +N/+N whenever it blocks or is blocked.
    // Parse the N from "Bushido:N" keyword string.
    auto parseBushidoN = [](const CardRules* rules) -> int {
        for (const auto& kw : rules->keywords) {
            if (kw.size() > 8 && kw.substr(0, 8) == "Bushido:") {
                int n = 0;
                std::from_chars(kw.data() + 8, kw.data() + kw.size(), n);
                return n;
            }
        }
        return 0;
    };
    {
        int bBushido = parseBushidoN(blocker.rules);
        if (bBushido > 0 && canFireTriggers(blocker)) {
            auto effect = parseScriptLine(
                "DB$ Pump | Defined$ TriggeredCard | NumAtt$ " + std::to_string(bBushido) +
                " | NumDef$ " + std::to_string(bBushido));
            out.push_back({ blocker.id, blocker.controllerId, std::move(effect), blocker.id });
        }
        int aBushido = parseBushidoN(attacker.rules);
        if (aBushido > 0 && canFireTriggers(attacker)) {
            auto effect = parseScriptLine(
                "DB$ Pump | Defined$ TriggeredCard | NumAtt$ " + std::to_string(aBushido) +
                " | NumDef$ " + std::to_string(aBushido));
            out.push_back({ attacker.id, attacker.controllerId, std::move(effect), attacker.id });
        }
    }

    // ── AttackerBlocked / AttackerBlockedByCreature ───────────────────────────
    // These modes fire for the blocker-or-attacker that owns the trigger line,
    // using ValidCard$ (the "other" combatant) and ValidBlocker$ (the blocker).
    // Blocker's own trigger: ValidBlocker$ Card.Self, ValidCard$ matches attacker
    for (const auto& rawLine : blocker.rules->triggerLines) {
        auto trig = parseScriptLine(rawLine);
        if (trig.effectType != "AttackerBlocked" &&
            trig.effectType != "AttackerBlockedByCreature") continue;
        auto validBlocker = trig.get("ValidBlocker", "Card.Self");
        if (validBlocker.find("Self") == std::string_view::npos) {
            if (!cardMatchesAnyFilter(blocker, std::string(validBlocker),
                                      blocker.controllerId, blocker.id, &blocker, &game)) continue;
        }
        auto validCard = std::string(trig.get("ValidCard", "Creature"));
        if (validCard.find("Self") != std::string_view::npos) continue; // self = blocker itself, skip
        if (!validCard.empty() && validCard != "Any" && validCard != "Creature") {
            if (!cardMatchesAnyFilter(attacker, validCard, blocker.controllerId, blocker.id, &blocker, &game)) continue;
        }
        auto execName = trig.get("Execute", "");
        auto effect   = resolveExecute(blocker, execName);
        if (effect.empty()) continue;
        out.push_back({ blocker.id, blocker.controllerId, std::move(effect), attacker.id });
    }
    // Attacker's own trigger: ValidCard$ Card.Self, ValidBlocker$ matches blocker
    for (const auto& rawLine : attacker.rules->triggerLines) {
        auto trig = parseScriptLine(rawLine);
        if (trig.effectType != "AttackerBlocked" &&
            trig.effectType != "AttackerBlockedByCreature") continue;
        auto validCard = trig.get("ValidCard", "");
        if (validCard.find("Self") == std::string_view::npos) continue; // attacker must be "Self"
        auto validBlocker = std::string(trig.get("ValidBlocker", "Creature"));
        if (!validBlocker.empty() && validBlocker != "Any") {
            if (validBlocker.find("Self") != std::string_view::npos) continue;
            if (!cardMatchesAnyFilter(blocker, validBlocker, attacker.controllerId, attacker.id, &attacker, &game)) continue;
        }
        auto execName = trig.get("Execute", "");
        auto effect   = resolveExecute(attacker, execName);
        if (effect.empty()) continue;
        out.push_back({ attacker.id, attacker.controllerId, std::move(effect), blocker.id });
    }
}

// ── AttackerUnblocked triggers ────────────────────────────────────────────────

void TriggerSystem::onAttackerUnblocked(const Card& attacker,
                                         const GameState& game,
                                         std::vector<PendingTrigger>& out) {
    for (const Card* watcher : game.battlefield().cards()) {
        for (const auto& line : watcher->rules->triggerLines) {
            auto trig = parseScriptLine(line);
            if (trig.effectType != "AttackerUnblocked") continue;

            // ValidAttacker$ — defaults to Card.YouCtrl (any creature you control)
            auto validAtk = trig.get("ValidAttacker", "Creature.YouCtrl");
            if (!cardMatchesAnyFilter(attacker, validAtk, watcher->controllerId, watcher->id, watcher, &game))
                continue;

            // Controller condition
            auto ctrlCond = trig.get("ActivatingPlayer", "");
            if (ctrlCond == "You"      && watcher->controllerId != attacker.controllerId) continue;
            if (ctrlCond == "Opponent" && watcher->controllerId == attacker.controllerId) continue;

            auto execName = trig.get("Execute", "");
            auto effect   = resolveExecute(*watcher, execName);
            if (effect.empty()) continue;
            out.push_back({ watcher->id, watcher->controllerId, std::move(effect), attacker.id });
        }
    }
}

// ── Phase triggers ────────────────────────────────────────────────────────────

// ── Saga triggers ─────────────────────────────────────────────────────────────
// Handled in onPhaseBegin for Main1 (PreCombatMain) — put a lore counter and fire chapter.

void TriggerSystem::onPhaseBegin(TurnStep step, uint8_t activePlayer,
                                  const GameState& game,
                                  std::vector<PendingTrigger>& out) {
    // Map TurnStep → the Phase$ string Forge uses in trigger lines
    struct PhaseAlias { TurnStep step; const char* alias; };
    static const PhaseAlias kAliases[] = {
        {TurnStep::Untap,           "Untap"},
        {TurnStep::Upkeep,          "Upkeep"},
        {TurnStep::Draw,            "Draw"},
        {TurnStep::PreCombatMain,   "Main"},
        {TurnStep::PreCombatMain,   "Main1"},
        {TurnStep::BeginCombat,     "Combat"},
        {TurnStep::BeginCombat,     "BeginCombat"},
        {TurnStep::EndStep,         "End"},
        {TurnStep::EndStep,         "EndOfTurn"},
        {TurnStep::EndStep,         "EndTurn"},
        {TurnStep::PostCombatMain,  "Main"},
        {TurnStep::PostCombatMain,  "Main2"},
        {TurnStep::Cleanup,         "Cleanup"},
        {TurnStep::EndCombat,       "EndCombat"},
    };

    // Walk battlefield AND command zone so commanders fire their "while this
    // is in the command zone" triggers (Oloro's upkeep gain, etc.). For each
    // watcher, gate every trigger line by its TriggerZones$ field — default
    // "Battlefield" so non-commander cards retain the old behaviour.
    std::vector<const Card*> watchers;
    for (const Card* c : game.battlefield().cards()) watchers.push_back(c);
    for (const Card* c : game.command().cards())     watchers.push_back(c);

    // Track (watcher controller, execute SVar) tuples already queued this pass
    // so a card with BOTH a battlefield-zone and command-zone version of the
    // same effect (the Forge convention — see Edgar Markov) only fires once.
    std::set<std::pair<ObjectId, std::string>> queuedKeys;

    for (const Card* watcher : watchers) {
        if (!watcher->rules) continue;
        const std::string watcherZoneName =
            (watcher->zone == ZoneType::Battlefield) ? "Battlefield"
          : (watcher->zone == ZoneType::Command)     ? "Command"
          : "Other";

        for (const auto& rawLine : watcher->rules->triggerLines) {
            auto trig = parseScriptLine(rawLine);

            // Gate by TriggerZones$ — Forge writes this as a comma list, so
            // we just substring-match against our watcher's zone name.
            auto tzones = trig.get("TriggerZones", "Battlefield");
            if (tzones.find(watcherZoneName) == std::string::npos) continue;

            // TurnBegin fires at the start of Untap (beginning of turn)
            if (trig.effectType == "TurnBegin") {
                if (step != TurnStep::Untap) continue;
                auto validPlayer = trig.get("ValidPlayer", "Any");
                if (validPlayer == "You"      && watcher->controllerId != activePlayer) continue;
                if (validPlayer == "Opponent" && watcher->controllerId == activePlayer) continue;
                auto execName = std::string(trig.get("Execute", ""));
                auto effect   = resolveExecute(*watcher, execName);
                if (effect.empty()) continue;
                auto key = std::make_pair(watcher->id, execName);
                if (!queuedKeys.insert(key).second) continue;
                out.push_back({ watcher->id, watcher->controllerId, std::move(effect) });
                continue;
            }
            if (trig.effectType != "Phase") continue;

            // Match the Phase$ value against our step aliases
            auto phase = trig.get("Phase", "");
            bool phaseMatch = false;
            for (const auto& a : kAliases) {
                if (a.step == step && phase == a.alias) { phaseMatch = true; break; }
            }
            if (!phaseMatch) continue;

            // PlayerTurn$ or ValidPlayer$: "You" = active controller, "Opponent" = other
            auto playerTurn = trig.get("PlayerTurn", "");
            if (playerTurn.empty()) playerTurn = trig.get("ValidPlayer", "Any");
            if (playerTurn == "You"       && watcher->controllerId != activePlayer) continue;
            if (playerTurn == "Opponent"  && watcher->controllerId == activePlayer) continue;

            auto execName = std::string(trig.get("Execute", ""));
            auto effect   = resolveExecute(*watcher, execName);
            if (effect.empty()) continue;
            // De-dupe: a card with twin BF + Command triggers using the same
            // Execute$ SVar (Forge convention for Eminence-like effects) only
            // fires once per phase step.
            auto key = std::make_pair(watcher->id, execName);
            if (!queuedKeys.insert(key).second) continue;
            out.push_back({ watcher->id, watcher->controllerId, std::move(effect) });
        }
    }
}

// ── SpellCast triggers ────────────────────────────────────────────────────────

void TriggerSystem::onSpellCast(const Card& spell, uint8_t controller,
                                 const GameState& game,
                                 std::vector<PendingTrigger>& out,
                                 const std::vector<Target>* targets) {
    // Battlefield + command zone so Eminence-style commander-zone triggers
    // (Edgar Markov, etc.) actually fire when the source is in the command
    // zone. De-dupe by (watcher, execute SVar) since Forge writes Eminence as
    // a pair of triggers — one with TriggerZones$ Battlefield, one with
    // TriggerZones$ Command — so the card works in either zone.
    std::vector<const Card*> watchers;
    for (const Card* c : game.battlefield().cards()) watchers.push_back(c);
    for (const Card* c : game.command().cards())     watchers.push_back(c);

    std::set<std::pair<ObjectId, std::string>> queuedKeys;

    for (const Card* watcher : watchers) {
        if (!watcher->rules) continue;
        const std::string watcherZoneName =
            (watcher->zone == ZoneType::Battlefield) ? "Battlefield"
          : (watcher->zone == ZoneType::Command)     ? "Command"
          : "Other";

        for (const auto& rawLine : watcher->rules->triggerLines) {
            auto trig = parseScriptLine(rawLine);
            if (trig.effectType != "SpellCast" && trig.effectType != "SpellCastOrCopy") continue;

            // Gate by TriggerZones$ — default "Battlefield" preserves old
            // behaviour for non-commander cards.
            auto tzones = trig.get("TriggerZones", "Battlefield");
            if (tzones.find(watcherZoneName) == std::string::npos) continue;

            // Heroic: TargetsValid$ Card.Self — trigger fires only when the watcher
            // is a target of the spell. Skip if no target list, or watcher not in it.
            auto targetsValid = trig.get("TargetsValid", "");
            if (!targetsValid.empty()) {
                if (!targets) continue;
                bool isTargeted = false;
                for (const auto& t : *targets)
                    if (t.isCard() && t.cardId == watcher->id) { isTargeted = true; break; }
                if (!isTargeted) continue;
            }

            // Optional controller restriction (ControllerIs$ or ValidActivatingPlayer$)
            auto ctrlCond = trig.get("ControllerIs", "");
            if (ctrlCond.empty()) ctrlCond = trig.get("ValidActivatingPlayer", "");
            if (ctrlCond == "You"      && watcher->controllerId != controller) continue;
            if (ctrlCond == "Opponent" && watcher->controllerId == controller) continue;

            // OpponentTurn$ True — fires only on the opponent's turn
            auto oppTurn = trig.get("OpponentTurn", "");
            if (oppTurn == "True" && game.activePlayerId() == watcher->controllerId) continue;

            // ActivatorThisTurnCast$ EQ1 / EQ2 etc. — Nth spell cast by the controller
            auto actCast = std::string(trig.get("ActivatorThisTurnCast", ""));
            if (!actCast.empty()) {
                int needed = 0;
                bool eq = false, ge = false, le = false;
                if (actCast.size() >= 3) {
                    if (actCast[0] == 'E' && actCast[1] == 'Q') { eq = true; std::from_chars(actCast.data()+2, actCast.data()+actCast.size(), needed); }
                    else if (actCast[0] == 'G' && actCast[1] == 'E') { ge = true; std::from_chars(actCast.data()+2, actCast.data()+actCast.size(), needed); }
                    else if (actCast[0] == 'L' && actCast[1] == 'E') { le = true; std::from_chars(actCast.data()+2, actCast.data()+actCast.size(), needed); }
                }
                int count = game.spellsCastByPlayer[controller];
                if (eq && count != needed) continue;
                if (ge && count < needed) continue;
                if (le && count > needed) continue;
            }

            // ValidCard$ filter — use full filter system for accurate matching
            auto validCard = std::string(trig.get("ValidCard", "Spell"));
            if (validCard != "Spell" && validCard != "Card" && validCard != "Any") {
                if (!cardMatchesAnyFilter(spell, validCard, watcher->controllerId, kInvalidId, watcher, &game)) continue;
            }

            auto execName = std::string(trig.get("Execute", ""));
            auto effect   = resolveExecute(*watcher, execName);
            if (effect.empty()) continue;
            auto key = std::make_pair(watcher->id, execName);
            if (!queuedKeys.insert(key).second) continue;
            int lim = trig.getInt("ActivationLimit", 0);
            if (lim > 0 && !watcher->tryTriggerLimit(execName, lim)) continue;
            // triggeredCardId = the spell that was cast, so effects can inspect it
            // (e.g. Namor counting blue pips in the triggering spell's cost).
            out.push_back({ watcher->id, watcher->controllerId, std::move(effect), spell.id });
        }

        // Prowess: +1/+1 until EOT when controller casts a non-creature spell
        if (watcher->rules->hasProwess &&
            watcher->controllerId == controller &&
            !spell.rules->type.isCreature()) {
            auto effect = parseScriptLine("DB$ Pump | NumAtt$ 1 | NumDef$ 1 | Defined$ TriggeredCard");
            out.push_back({ watcher->id, watcher->controllerId, std::move(effect), watcher->id });
        }

        // Heroic: +1/+1 counter when this creature is targeted by a spell its controller casts
        if (watcher->rules->hasHeroic &&
            watcher->controllerId == controller &&
            targets) {
            for (const auto& t : *targets) {
                if (t.isCard() && t.cardId == watcher->id) {
                    auto effect = parseScriptLine("DB$ PutCounter | CounterType$ P1P1 | CounterNum$ 1 | Defined$ TriggeredCard");
                    out.push_back({ watcher->id, watcher->controllerId, std::move(effect), watcher->id });
                    break;
                }
            }
        }
    }
}

// ── BecomesMonstrous triggers ──────────────────────────────────────────────────

void TriggerSystem::onBecomesMonstrous(const Card& source,
                                        const GameState& game,
                                        std::vector<PendingTrigger>& out) {
    // Source's own BecomesMonstrous triggers (ValidCard$ Card.Self)
    for (const auto& rawLine : source.rules->triggerLines) {
        auto trig = parseScriptLine(rawLine);
        if (trig.effectType != "BecomesMonstrous") continue;
        auto validCard = trig.get("ValidCard", "Card.Self");
        if (validCard.find("Self") == std::string_view::npos) continue;
        auto execName = trig.get("Execute", "");
        auto effect   = resolveExecute(source, execName);
        if (effect.empty()) continue;
        out.push_back({ source.id, source.controllerId, std::move(effect), source.id });
    }
    // Global watchers ("whenever a creature becomes monstrous")
    for (const Card* watcher : game.battlefield().cards()) {
            if (!watcher->rules) continue;  // guard stale ownedRules
        if (watcher->id == source.id) continue;
        if (!canFireTriggers(*watcher)) continue;
        for (const auto& rawLine : watcher->rules->triggerLines) {
            auto trig = parseScriptLine(rawLine);
            if (trig.effectType != "BecomesMonstrous") continue;
            auto validCard = std::string(trig.get("ValidCard", ""));
            if (validCard.empty() || validCard.find("Self") != std::string_view::npos) continue;
            if (!cardMatchesAnyFilter(source, validCard,
                                      watcher->controllerId, watcher->id, watcher, &game)) continue;
            auto execName = trig.get("Execute", "");
            auto effect   = resolveExecute(*watcher, execName);
            if (effect.empty()) continue;
            out.push_back({ watcher->id, watcher->controllerId, std::move(effect), source.id });
        }
    }
}

// ── DamageDone triggers ───────────────────────────────────────────────────────

void TriggerSystem::onDamageDone(const Card& source, int amount, bool isCombat,
                                  bool toPlayer, ObjectId targetId, uint8_t targetPlayer,
                                  const GameState& game,
                                  std::vector<PendingTrigger>& out) {
    if (amount <= 0) return;

    // Scan every battlefield card for DamageDone trigger watchers.
    // This covers self-triggered cards (Ophidian Eye), aura watchers (Curiosity),
    // and equipment watchers (Sword of Fire and Ice).
    for (const Card* watcher : game.battlefield().cards()) {
        for (const auto& rawLine : watcher->rules->triggerLines) {
            auto trig = parseScriptLine(rawLine);
            if (trig.effectType != "DamageDone") continue;

            // CombatDamage$ True / False — optional combat restriction
            auto combatCond = trig.get("CombatDamage", "");
            if (combatCond == "True"  && !isCombat) continue;
            if (combatCond == "False" &&  isCombat) continue;

            // ValidTarget$ — what the damage was dealt to
            auto validTarget = trig.get("ValidTarget", "Any");
            if (validTarget == "Player"   && !toPlayer) continue;
            if (validTarget == "Creature" &&  toPlayer) continue;

            // ValidSource$ — which card must be the damage source
            auto validSource = trig.get("ValidSource", "Card.Self");
            bool sourceMatches = false;
            if (validSource.find("Self") != std::string_view::npos) {
                // "Self" → the watcher card is the source
                sourceMatches = (watcher->id == source.id && source.isOnBattlefield());
            } else if (validSource.find("EquippedBy")   != std::string_view::npos ||
                       validSource.find("EnchantedCard") != std::string_view::npos ||
                       validSource.find("Enchanted")    != std::string_view::npos) {
                // Attachment trigger: the source must be the card attached to the watcher
                sourceMatches = (watcher->attachedTo != kInvalidId &&
                                 watcher->attachedTo == source.id);
            } else {
                // General filter — check if source matches relative to watcher
                sourceMatches = cardMatchesAnyFilter(source, validSource,
                                                     watcher->controllerId, watcher->id, watcher, &game);
            }
            if (!sourceMatches) continue;

            // ControllerIs$ — optional: whose source we watch
            auto ctrlCond = trig.get("ControllerIs", "");
            if (ctrlCond == "You"      && source.controllerId != watcher->controllerId) continue;
            if (ctrlCond == "Opponent" && source.controllerId == watcher->controllerId) continue;

            auto execName = trig.get("Execute", "");
            auto effect   = resolveExecute(*watcher, execName);
            if (effect.empty()) continue;
            out.push_back({ watcher->id, watcher->controllerId, std::move(effect),
                            toPlayer ? kInvalidId : targetId });
        }
    }
    (void)targetPlayer;

    // Cipher: when the creature encoded with a cipher card deals combat damage to
    // a player, let the controller cast the encoded spell for free (as a copy).
    if (isCombat && toPlayer) {
        // Check exile zone for cards ciphered onto this source creature
        for (const Card* ec : game.exile().cards()) {
            if (ec->cipheredOnto != source.id) continue;
            if (!ec->rules || !ec->rules->hasCipher) continue;
            // Queue the free cast as a DB$ effect (approximated by executing the script directly)
            if (!ec->rules->abilityLines.empty()) {
                // The cipher card's first SP$ line is its spell effect
                for (const auto& raw : ec->rules->abilityLines) {
                    auto s = parseScriptLine(raw);
                    if (s.abilityType != "SP") continue;
                    PendingTrigger trig;
                    trig.sourceCardId  = ec->id;
                    trig.controllerId  = source.controllerId;
                    trig.effect        = s;
                    out.push_back(std::move(trig));
                    break;
                }
            }
        }
    }
}

// ── GainsLife triggers ───────────────────────────────────────────────────────

void TriggerSystem::onGainLife(uint8_t player, int amount,
                                const GameState& game,
                                std::vector<PendingTrigger>& out) {
    if (amount <= 0) return;
    for (const Card* watcher : game.battlefield().cards()) {
        for (const auto& rawLine : watcher->rules->triggerLines) {
            auto trig = parseScriptLine(rawLine);
            if (trig.effectType != "GainsLife" && trig.effectType != "GainLife" &&
                trig.effectType != "LifeGained") continue;

            // ControllerIs$ or ValidPlayer$ — whose life gain to watch (default: You)
            auto ctrlCond = trig.get("ControllerIs", "");
            if (ctrlCond.empty()) ctrlCond = trig.get("ValidPlayer", "You");
            if (ctrlCond == "You"      && watcher->controllerId != player) continue;
            if (ctrlCond == "Opponent" && watcher->controllerId == player) continue;
            if (ctrlCond == "Any"      || ctrlCond == "Either") { /* always match */ }

            // MinAmount$ N — only fire if the amount gained >= N
            int minAmt = trig.getInt("MinAmount", 1);
            if (amount < minAmt) continue;

            auto execName = trig.get("Execute", "");
            auto effect   = resolveExecute(*watcher, execName);
            if (effect.empty()) continue;
            out.push_back({ watcher->id, watcher->controllerId, std::move(effect) });
        }
    }
}

// ── Discards triggers ────────────────────────────────────────────────────────

void TriggerSystem::onDiscard(const Card& discarded, uint8_t discardingPlayer,
                               const GameState& game,
                               std::vector<PendingTrigger>& out) {
    for (const Card* watcher : game.battlefield().cards()) {
        for (const auto& rawLine : watcher->rules->triggerLines) {
            auto trig = parseScriptLine(rawLine);
            if (trig.effectType != "Discards" && trig.effectType != "Discard" &&
                trig.effectType != "Discarded") continue;

            // ControllerIs$ — whose discard to watch
            auto ctrlCond = trig.get("ControllerIs", "You");
            if (ctrlCond == "You"      && watcher->controllerId != discardingPlayer) continue;
            if (ctrlCond == "Opponent" && watcher->controllerId == discardingPlayer) continue;

            // ValidCard$ — does the discarded card match?
            auto validCard = std::string(trig.get("ValidCard", "Card"));
            if (!cardMatchesAnyFilter(discarded, validCard, discardingPlayer, kInvalidId, nullptr, &game)) continue;

            auto execName = trig.get("Execute", "");
            auto effect   = resolveExecute(*watcher, execName);
            if (effect.empty()) continue;
            out.push_back({ watcher->id, watcher->controllerId,
                            std::move(effect), discarded.id });
        }
    }
}

// ── Cycling triggers ─────────────────────────────────────────────────────────

void TriggerSystem::onCycle(const CardRules& cycledRules, ObjectId cycledCardId,
                             uint8_t controller, const GameState& /*game*/,
                             std::vector<PendingTrigger>& out) {
    // Check the cycled card's own T: lines for Mode$ Cycled triggers
    for (const auto& rawLine : cycledRules.triggerLines) {
        auto trig = parseScriptLine(rawLine);
        if (trig.effectType != "Cycled") continue;

        // ValidCard$ — typically Card.Self (the cycling trigger fires for this card)
        auto validCard = trig.get("ValidCard", "Card.Self");
        if (validCard.find("Self") == std::string_view::npos) continue;

        auto execName = trig.get("Execute", "");
        auto it       = cycledRules.svars.find(std::string(execName));
        if (it == cycledRules.svars.end()) continue;
        auto effect = parseScriptLine(it->second);
        if (effect.empty()) continue;

        out.push_back({ cycledCardId, controller, std::move(effect), cycledCardId });
    }
}

// ── Taps triggers ─────────────────────────────────────────────────────────────

void TriggerSystem::onTap(const Card& tapped, const GameState& game,
                           std::vector<PendingTrigger>& out) {
    // The tapped card's own "whenever ~ becomes tapped" triggers (Inspired pattern)
    for (const auto& rawLine : tapped.rules->triggerLines) {
        auto trig = parseScriptLine(rawLine);
        if (trig.effectType != "Taps" && trig.effectType != "BecomesTapped") continue;
        auto validCard = trig.get("ValidCard", "Card.Self");
        if (validCard.find("Self") == std::string_view::npos) continue;
        auto execName = trig.get("Execute", "");
        auto effect   = resolveExecute(tapped, execName);
        if (effect.empty()) continue;
        out.push_back({ tapped.id, tapped.controllerId, std::move(effect), tapped.id });
    }

    // Global battlefield watchers: "whenever a permanent/creature becomes tapped"
    for (const Card* watcher : game.battlefield().cards()) {
            if (!watcher->rules) continue;  // guard stale ownedRules
        if (watcher->id == tapped.id) continue;
        if (!canFireTriggers(*watcher)) continue;
        for (const auto& rawLine : watcher->rules->triggerLines) {
            auto trig = parseScriptLine(rawLine);
            if (trig.effectType != "Taps" && trig.effectType != "BecomesTapped") continue;
            auto validCard = std::string(trig.get("ValidCard", ""));
            if (validCard.empty() || validCard.find("Self") != std::string_view::npos) continue;
            if (!cardMatchesAnyFilter(tapped, validCard,
                                      watcher->controllerId, watcher->id, watcher, &game)) continue;
            auto ctrlCond = trig.get("ControllerIs", "");
            if (ctrlCond == "You"      && watcher->controllerId != tapped.controllerId) continue;
            if (ctrlCond == "Opponent" && watcher->controllerId == tapped.controllerId) continue;
            auto execName = trig.get("Execute", "");
            auto effect   = resolveExecute(*watcher, execName);
            if (effect.empty()) continue;
            out.push_back({ watcher->id, watcher->controllerId, std::move(effect), tapped.id });
        }
    }
}

// ── LosesLife triggers ────────────────────────────────────────────────────────

void TriggerSystem::onLoseLife(uint8_t player, int amount,
                                const GameState& game,
                                std::vector<PendingTrigger>& out) {
    if (amount <= 0) return;
    for (const Card* watcher : game.battlefield().cards()) {
        for (const auto& rawLine : watcher->rules->triggerLines) {
            auto trig = parseScriptLine(rawLine);
            if (trig.effectType != "LosesLife") continue;

            // ControllerIs$ — whose life loss to watch (empty = any player)
            auto ctrlCond = trig.get("ControllerIs", "");
            if (ctrlCond == "You"      && watcher->controllerId != player) continue;
            if (ctrlCond == "Opponent" && watcher->controllerId == player) continue;

            // MinAmount$ N — only fire if the amount lost >= N
            int minAmt = trig.getInt("MinAmount", 1);
            if (amount < minAmt) continue;

            auto execName = trig.get("Execute", "");
            auto effect   = resolveExecute(*watcher, execName);
            if (effect.empty()) continue;
            out.push_back({ watcher->id, watcher->controllerId, std::move(effect) });
        }
    }
}

// ── LandPlayed triggers ───────────────────────────────────────────────────────

void TriggerSystem::onLandPlayed(const Card& land, uint8_t controller,
                                  const GameState& game,
                                  std::vector<PendingTrigger>& out) {
    for (const Card* watcher : game.battlefield().cards()) {
        for (const auto& rawLine : watcher->rules->triggerLines) {
            auto trig = parseScriptLine(rawLine);
            if (trig.effectType != "LandPlayed") continue;

            // ControllerIs$ — whose land play to watch
            auto ctrlCond = trig.get("ControllerIs", "");
            if (ctrlCond == "You"      && watcher->controllerId != controller) continue;
            if (ctrlCond == "Opponent" && watcher->controllerId == controller) continue;

            // ValidCard$ — filter on the land that was played
            auto validCard = trig.get("ValidCard", "");
            if (!validCard.empty() &&
                !cardMatchesAnyFilter(land, validCard, watcher->controllerId, watcher->id, watcher, &game))
                continue;

            auto execName = trig.get("Execute", "");
            auto effect   = resolveExecute(*watcher, execName);
            if (effect.empty()) continue;
            out.push_back({ watcher->id, watcher->controllerId, std::move(effect), land.id });
        }
    }
}

// ── Sacrificed triggers ───────────────────────────────────────────────────────

void TriggerSystem::onSacrificed(const Card& sacrificed,
                                  const GameState& game,
                                  std::vector<PendingTrigger>& out) {
    for (const Card* watcher : game.battlefield().cards()) {
            if (!watcher->rules) continue;  // guard stale ownedRules
        if (watcher->id == sacrificed.id) continue;
        if (!canFireTriggers(*watcher)) continue;
        for (const auto& rawLine : watcher->rules->triggerLines) {
            auto trig = parseScriptLine(rawLine);
            if (trig.effectType != "Sacrificed") continue;

            // ControllerIs$ — whose sacrifice to watch
            auto ctrlCond = trig.get("ControllerIs", "");
            if (ctrlCond == "You"      && watcher->controllerId != sacrificed.controllerId) continue;
            if (ctrlCond == "Opponent" && watcher->controllerId == sacrificed.controllerId) continue;

            // ValidCard$ — filter on what was sacrificed
            auto validCard = trig.get("ValidCard", "");
            if (!validCard.empty() &&
                !cardMatchesAnyFilter(sacrificed, validCard, watcher->controllerId, watcher->id, watcher, &game))
                continue;

            auto execName = trig.get("Execute", "");
            auto effect   = resolveExecute(*watcher, execName);
            if (effect.empty()) continue;
            out.push_back({ watcher->id, watcher->controllerId, std::move(effect), sacrificed.id });
        }
    }
}

// ── CounterAdded triggers ─────────────────────────────────────────────────────

void TriggerSystem::onCounterAdded(const Card& target, std::string_view counterType,
                                    int amount, const GameState& game,
                                    std::vector<PendingTrigger>& out) {
    if (amount <= 0) return;
    // Convert internal counter key ("+1/+1") to Forge Type$ name ("P1P1")
    auto toForgeType = [](std::string_view key) -> std::string_view {
        if (key == "+1/+1") return "P1P1";
        if (key == "-1/-1") return "M1M1";
        if (key == "charge") return "CHARGE";
        if (key == "loyalty") return "LOYALTY";
        return key;
    };
    std::string_view forgeType = toForgeType(counterType);

    for (const Card* watcher : game.battlefield().cards()) {
            if (!watcher->rules) continue;  // guard stale ownedRules
        if (!canFireTriggers(*watcher)) continue;
        for (const auto& rawLine : watcher->rules->triggerLines) {
            auto trig = parseScriptLine(rawLine);
            bool isOnce = (trig.effectType == "CounterAddedOnce");
            if (trig.effectType != "CounterAdded" && !isOnce) continue;

            // CounterType$ P1P1 — optional counter-type filter
            auto typeFilter = trig.get("CounterType", "");
            if (!typeFilter.empty() && typeFilter != "Any" && typeFilter != forgeType) continue;

            // ValidCard$ — filter on the permanent that received the counter
            auto validCard = trig.get("ValidCard", "");
            if (!validCard.empty() && !cardMatchesAnyFilter(target, validCard,
                                                             watcher->controllerId, watcher->id, watcher, &game))
                continue;

            // Threshold$ N — fire only once the target's total counters of this type
            // reach N (e.g. "when the fifth plan counter is put on this enchantment").
            // counterCount reflects the post-add total since onCounterAdded runs after addCounter.
            int threshold = trig.getInt("Threshold", 0);
            if (threshold > 0 &&
                target.counterCount(std::string(counterType)) < threshold)
                continue;

            auto execName = trig.get("Execute", "");
            auto effect   = resolveExecute(*watcher, execName);
            if (effect.empty()) continue;

            // CounterAddedOnce: deduplicate — fire at most once per watcher per batch
            if (isOnce) {
                bool duped = false;
                for (const auto& pt : out)
                    if (pt.sourceCardId == watcher->id &&
                        pt.effect.effectType == effect.effectType)
                        { duped = true; break; }
                if (duped) continue;
            }

            out.push_back({ watcher->id, watcher->controllerId, std::move(effect), target.id });
        }
    }
}

// ── Untap triggers ────────────────────────────────────────────────────────────

void TriggerSystem::onUntap(const Card& untapped, const GameState& game,
                             std::vector<PendingTrigger>& out) {
    // The untapped card's own T:Mode$ Untap triggers (e.g. Inspired)
    for (const auto& rawLine : untapped.rules->triggerLines) {
        auto trig = parseScriptLine(rawLine);
        if (trig.effectType != "Untap" && trig.effectType != "BecomesUntapped") continue;
        auto validCard = trig.get("ValidCard", "Card.Self");
        if (validCard.find("Self") == std::string_view::npos) continue;
        auto execName = trig.get("Execute", "");
        auto effect   = resolveExecute(untapped, execName);
        if (effect.empty()) continue;
        out.push_back({ untapped.id, untapped.controllerId, std::move(effect), untapped.id });
    }

    // Inspired (keyword shorthand): +1/+1 counter when this creature untaps
    if (untapped.rules->hasInspired && untapped.isCreature()) {
        auto effect = parseScriptLine(
            "DB$ PutCounter | CounterType$ P1P1 | CounterNum$ 1 | Defined$ TriggeredCard");
        out.push_back({ untapped.id, untapped.controllerId, std::move(effect), untapped.id });
    }

    // Global watchers: "whenever a creature becomes untapped"
    for (const Card* watcher : game.battlefield().cards()) {
            if (!watcher->rules) continue;  // guard stale ownedRules
        if (watcher->id == untapped.id) continue;
        if (!canFireTriggers(*watcher)) continue;
        for (const auto& rawLine : watcher->rules->triggerLines) {
            auto trig = parseScriptLine(rawLine);
            if (trig.effectType != "Untap" && trig.effectType != "BecomesUntapped") continue;
            auto validCard = std::string(trig.get("ValidCard", ""));
            if (validCard.empty() || validCard.find("Self") != std::string_view::npos) continue;
            if (!cardMatchesAnyFilter(untapped, validCard,
                                      watcher->controllerId, watcher->id, watcher, &game)) continue;
            auto ctrlCond = trig.get("ControllerIs", "");
            if (ctrlCond == "You"      && watcher->controllerId != untapped.controllerId) continue;
            if (ctrlCond == "Opponent" && watcher->controllerId == untapped.controllerId) continue;
            auto execName = trig.get("Execute", "");
            auto effect   = resolveExecute(*watcher, execName);
            if (effect.empty()) continue;
            out.push_back({ watcher->id, watcher->controllerId, std::move(effect), untapped.id });
        }
    }
}

// ── Transformed triggers ──────────────────────────────────────────────────────

void TriggerSystem::onTransform(const Card& card,
                                 const GameState& game,
                                 std::vector<PendingTrigger>& out) {
    // card.rules is already the new face; check its triggerLines for Mode$ Transformed.
    for (const auto& rawLine : card.rules->triggerLines) {
        auto trig = parseScriptLine(rawLine);
        if (trig.effectType != "Transformed") continue;
        auto validCard = trig.get("ValidCard", "Card.Self");
        if (validCard.find("Self") == std::string_view::npos) continue;
        auto execName = trig.get("Execute", "");
        auto effect   = resolveExecute(card, execName);
        if (effect.empty()) continue;
        out.push_back({ card.id, card.controllerId, std::move(effect), card.id });
    }
    // Global watchers on other battlefield permanents
    for (const Card* watcher : game.battlefield().cards()) {
            if (!watcher->rules) continue;  // guard stale ownedRules
        if (watcher->id == card.id) continue;
        if (!canFireTriggers(*watcher)) continue;
        for (const auto& rawLine : watcher->rules->triggerLines) {
            auto trig = parseScriptLine(rawLine);
            if (trig.effectType != "Transformed") continue;
            auto validCard = std::string(trig.get("ValidCard", ""));
            if (validCard.empty() || validCard.find("Self") != std::string_view::npos) continue;
            if (!cardMatchesAnyFilter(card, validCard,
                                      watcher->controllerId, watcher->id, watcher, &game)) continue;
            auto execName = trig.get("Execute", "");
            auto effect   = resolveExecute(*watcher, execName);
            if (effect.empty()) continue;
            out.push_back({ watcher->id, watcher->controllerId, std::move(effect), card.id });
        }
    }
}

// ── TurnFaceUp triggers ───────────────────────────────────────────────────────

void TriggerSystem::onTurnFaceUp(const Card& card,
                                  const GameState& game,
                                  std::vector<PendingTrigger>& out) {
    // card.isFaceDown is already false; card.rules is the real face.
    for (const auto& rawLine : card.rules->triggerLines) {
        auto trig = parseScriptLine(rawLine);
        if (trig.effectType != "TurnFaceUp") continue;
        auto validCard = trig.get("ValidCard", "Card.Self");
        if (validCard.find("Self") == std::string_view::npos) continue;
        auto execName = trig.get("Execute", "");
        auto effect   = resolveExecute(card, execName);
        if (effect.empty()) continue;
        out.push_back({ card.id, card.controllerId, std::move(effect), card.id });
    }
    // Global watchers
    for (const Card* watcher : game.battlefield().cards()) {
            if (!watcher->rules) continue;  // guard stale ownedRules
        if (watcher->id == card.id) continue;
        if (!canFireTriggers(*watcher)) continue;
        for (const auto& rawLine : watcher->rules->triggerLines) {
            auto trig = parseScriptLine(rawLine);
            if (trig.effectType != "TurnFaceUp") continue;
            auto validCard = std::string(trig.get("ValidCard", ""));
            if (validCard.empty() || validCard.find("Self") != std::string_view::npos) continue;
            if (!cardMatchesAnyFilter(card, validCard,
                                      watcher->controllerId, watcher->id, watcher, &game)) continue;
            auto execName = trig.get("Execute", "");
            auto effect   = resolveExecute(*watcher, execName);
            if (effect.empty()) continue;
            out.push_back({ watcher->id, watcher->controllerId, std::move(effect), card.id });
        }
    }
}

// ── DamageDoneOnce triggers ───────────────────────────────────────────────────
// Fires once per (watcher, unique-target) when one or more sources matching
// ValidSource$ dealt combat damage to that target this combat step.

void TriggerSystem::onDamageDoneOnce(const std::vector<CombatDamageEvent>& events,
                                      const GameState& game,
                                      std::vector<PendingTrigger>& out) {
    if (events.empty()) return;

    for (const Card* watcher : game.battlefield().cards()) {
        for (const auto& rawLine : watcher->rules->triggerLines) {
            auto trig = parseScriptLine(rawLine);
            if (trig.effectType != "DamageDoneOnce") continue;

            // CombatDamage$ True / False — all events here ARE combat damage
            auto combatCond = trig.get("CombatDamage", "");
            if (combatCond == "False") continue;

            auto validSource = std::string(trig.get("ValidSource", "Card.Self"));
            auto validTarget = std::string(trig.get("ValidTarget", "Any"));
            auto execName    = trig.get("Execute", "");

            // Per-target total damage from matching sources.
            struct TargetTotal {
                bool     toPlayer;
                uint8_t  targetPlayer;
                ObjectId targetCardId;
                int      total = 0;
            };
            std::vector<TargetTotal> totals;

            auto findOrAdd = [&](bool tp, uint8_t pl, ObjectId cid) -> TargetTotal& {
                for (auto& t : totals) {
                    if (t.toPlayer == tp) {
                        if (tp && t.targetPlayer == pl) return t;
                        if (!tp && t.targetCardId == cid) return t;
                    }
                }
                totals.push_back({tp, pl, cid, 0});
                return totals.back();
            };

            for (const auto& ev : events) {
                if (ev.amount <= 0 || !ev.source) continue;

                // ValidSource$ check
                bool srcOk = false;
                if (validSource.find("Self") != std::string_view::npos) {
                    srcOk = (ev.source->id == watcher->id);
                } else if (validSource.empty() || validSource == "Any") {
                    srcOk = true;
                } else {
                    srcOk = cardMatchesAnyFilter(*ev.source, validSource,
                                                 watcher->controllerId, watcher->id, watcher, &game);
                }
                if (!srcOk) continue;

                // ValidTarget$ check
                bool tgtOk = false;
                if (ev.toPlayer) {
                    if (validTarget == "Any" || validTarget == "Player") {
                        tgtOk = true;
                    } else if (validTarget == "You") {
                        tgtOk = (ev.targetPlayer == watcher->controllerId);
                    } else if (validTarget == "Opponent") {
                        tgtOk = (ev.targetPlayer != watcher->controllerId);
                    }
                } else {
                    // target is a card
                    if (validTarget.find("Self") != std::string_view::npos) {
                        tgtOk = (ev.targetCardId == watcher->id);
                    } else if (validTarget == "Any" || validTarget == "Creature") {
                        tgtOk = true;
                    } else if (!validTarget.empty()) {
                        const Card* tc = game.findCard(ev.targetCardId);
                        if (tc) tgtOk = cardMatchesAnyFilter(*tc, validTarget,
                                                              watcher->controllerId, watcher->id, watcher, &game);
                    } else {
                        tgtOk = true;
                    }
                }
                if (!tgtOk) continue;

                findOrAdd(ev.toPlayer, ev.targetPlayer, ev.targetCardId).total += ev.amount;
            }

            // Fire once per unique target that received damage from a matching source
            for (const auto& entry : totals) {
                auto effect = resolveExecute(*watcher, execName);
                if (effect.empty()) continue;
                PendingTrigger pt;
                pt.sourceCardId    = watcher->id;
                pt.controllerId    = watcher->controllerId;
                pt.effect          = std::move(effect);
                pt.triggeredCardId = entry.toPlayer ? kInvalidId : entry.targetCardId;
                pt.triggerPlayer   = entry.toPlayer ? entry.targetPlayer : 255;
                pt.triggerAmount   = entry.total;
                out.push_back(std::move(pt));
            }
        }
    }
}

// ── Drawn triggers ────────────────────────────────────────────────────────────

void TriggerSystem::onDraw(uint8_t drawingPlayer, int drawNumber, ObjectId drawnCardId,
                            const GameState& game,
                            std::vector<PendingTrigger>& out) {
    for (const Card* watcher : game.battlefield().cards()) {
        for (const auto& rawLine : watcher->rules->triggerLines) {
            auto trig = parseScriptLine(rawLine);
            if (trig.effectType != "Drawn") continue;

            // ValidCard$ Card.YouCtrl|YouOwn / Card.OppCtrl|OppOwn — which player
            // drew. (Smothering Tithe uses Card.OppOwn; without OppOwn here it
            // matched neither branch and fired on the controller's own draws.)
            auto validCard = trig.get("ValidCard", "Card.YouCtrl");
            bool drawingIsYou = (watcher->controllerId == drawingPlayer);
            bool wantYou = validCard.find("YouCtrl") != std::string_view::npos ||
                           validCard.find("YouOwn")  != std::string_view::npos;
            bool wantOpp = validCard.find("OppCtrl") != std::string_view::npos ||
                           validCard.find("OppOwn")  != std::string_view::npos;
            if (wantYou && !drawingIsYou) continue;
            if (wantOpp &&  drawingIsYou) continue;

            // Number$ N — fire only on the Nth draw this turn (0/absent = any draw)
            int number = trig.getInt("Number", 0);
            if (number > 0 && drawNumber != number) continue;

            auto execName = trig.get("Execute", "");
            auto effect   = resolveExecute(*watcher, execName);
            if (effect.empty()) continue;
            out.push_back({ watcher->id, watcher->controllerId, std::move(effect), drawnCardId });
        }
    }
}

// ── BecomesTarget triggers ────────────────────────────────────────────────────

void TriggerSystem::onBecomesTarget(ObjectId targetId, ObjectId sourceId,
                                     bool sourceIsSpell, uint8_t sourceController,
                                     const GameState& game,
                                     std::vector<PendingTrigger>& out) {
    const Card* targeted = game.findCard(targetId);
    const Card* source   = game.findCard(sourceId);
    if (!targeted) return;

    for (const Card* watcher : game.battlefield().cards()) {
        if (!watcher->rules) continue;
        for (const auto& rawLine : watcher->rules->triggerLines) {
            auto trig = parseScriptLine(rawLine);
            if (trig.effectType != "BecomesTarget") continue;

            // ValidTarget$: the card being targeted must match the filter
            auto validTarget = trig.get("ValidTarget", "");
            if (!validTarget.empty()) {
                if (!cardMatchesAnyFilter(*targeted, std::string(validTarget),
                                          watcher->controllerId, watcher->id, watcher, &game))
                    continue;
            }

            // ValidSource$: the spell/ability doing the targeting must match
            auto validSource = std::string(trig.get("ValidSource", ""));
            if (!validSource.empty()) {
                bool srcOk = true;
                if (validSource.find("OppCtrl") != std::string::npos)
                    if (sourceController == watcher->controllerId) srcOk = false;
                if (validSource.find("YouCtrl") != std::string::npos)
                    if (sourceController != watcher->controllerId) srcOk = false;
                if (!sourceIsSpell &&
                    (validSource.find("Instant") != std::string::npos ||
                     validSource.find("Sorcery") != std::string::npos))
                    srcOk = false;
                if (source && !sourceIsSpell) {
                    // Filter on the source card type (e.g. "Instant,Sorcery")
                    if (validSource.find("Instant") != std::string::npos &&
                        !source->rules->type.isInstant()) srcOk = false;
                    if (validSource.find("Sorcery") != std::string::npos &&
                        !source->rules->type.isSorcery()) srcOk = false;
                }
                if (!srcOk) continue;
            }

            auto execName = trig.get("Execute", "");
            ScriptLine effect = resolveExecute(*watcher, execName);
            if (effect.empty()) continue;

            int lim = trig.getInt("ActivationLimit", 0);
            if (lim > 0 && !watcher->tryTriggerLimit(execName, lim)) continue;

            // triggeredCardId = the card that was targeted (for Defined$ TriggeredTarget)
            out.push_back({ watcher->id, watcher->controllerId, std::move(effect), targetId });
        }
    }
}

} // namespace mtg
