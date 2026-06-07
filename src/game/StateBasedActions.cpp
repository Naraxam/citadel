#include "StateBasedActions.h"
#include "GameState.h"
#include "CardFilter.h"
#include "CardStats.h"
#include "EquipSystem.h"
#include "TriggerSystem.h"
#include "ability/ScriptLine.h"
#include <algorithm>
#include <charconv>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace {

using namespace mtg;

// When a creature dies, fire any haunt triggers from exiled cards haunting it.
static void checkHauntTriggers(GameState& game, ObjectId dyingId) {
    for (const Card* exiled : game.exile().cards()) {
        if (!exiled->rules->hasHaunt) continue;
        if (exiled->hauntedCreatureId != dyingId) continue;
        for (const auto& raw : exiled->rules->triggerLines) {
            auto s = parseScriptLine(raw);
            if (s.get("Mode", "") != "Haunt") continue;
            auto execName = s.get("Execute", "");
            if (execName.empty()) continue;
            auto it = exiled->rules->svars.find(std::string(execName));
            if (it == exiled->rules->svars.end()) continue;
            PendingTrigger trig;
            trig.sourceCardId    = exiled->id;
            trig.controllerId    = exiled->controllerId;
            trig.effect          = parseScriptLine(it->second);
            trig.triggeredCardId = dyingId;
            game.queueTriggers({trig});
        }
    }
}

// Convert a Forge chapter designator ("I","II","III","1","2", …) to an integer.
int parseChapterNum(std::string_view s) {
    if (s == "I"   || s == "i")   return 1;
    if (s == "II"  || s == "ii")  return 2;
    if (s == "III" || s == "iii") return 3;
    if (s == "IV"  || s == "iv")  return 4;
    if (s == "V"   || s == "v")   return 5;
    int v = 0;
    std::from_chars(s.data(), s.data() + s.size(), v);
    return v;
}
} // namespace

namespace mtg {

bool StateBasedActions::run(GameState& game) {
    bool any = false;
    any |= checkCounterCancellation(game);
    any |= checkLegendaryRule(game);
    any |= checkAuraFallOff(game);
    any |= checkSagaSacrifice(game);
    any |= checkCreatureDeath(game);
    any |= checkPlaneswalkerDeath(game);
    any |= checkPlayerLoss(game);
    any |= checkAlwaysTriggers(game);
    return any;
}

bool StateBasedActions::runBasic(GameState& game) {
    bool any = false;
    any |= checkCounterCancellation(game);
    any |= checkLegendaryRule(game);
    any |= checkAuraFallOff(game);
    any |= checkSagaSacrifice(game);
    any |= checkCreatureDeath(game);
    any |= checkPlaneswalkerDeath(game);
    any |= checkPlayerLoss(game);
    // checkAlwaysTriggers intentionally omitted: calling it from inside
    // drainPendingTriggers() causes O(M) accumulation because drainTriggers()
    // clears pending status, making every Always-trigger card re-eligible
    // on each SBA pass. Always triggers fire from the explicit SBA loops
    // in AiPlayer::takeTurn() instead.
    return any;
}

bool StateBasedActions::checkCreatureDeath(GameState& game) {
    std::vector<ObjectId> toDestroy;

    for (const Card* card : game.battlefield().cards()) {
        if (!card->isCreature()) continue;

        int toughness = effectiveToughness(*card);

        // Toughness <= 0 kills even indestructible creatures (not a "destroy" effect)
        if (toughness <= 0) {
            toDestroy.push_back(card->id);
            continue;
        }

        if (card->hasKeyword(KeywordAbility::Indestructible)) continue;

        // Shield counter: prevents one instance of lethal damage or destruction
        bool wouldDieNow = (card->markedDamage >= toughness) ||
                           (card->deathtouchDamage && card->markedDamage > 0);
        if (wouldDieNow && card->counterCount("shield") > 0) {
            const_cast<Card*>(card)->removeCounter("shield", 1);
            const_cast<Card*>(card)->markedDamage     = 0;
            const_cast<Card*>(card)->deathtouchDamage = false;
            continue;
        }

        // Regeneration shield absorbs one lethal-damage event
        bool wouldDie = (card->markedDamage >= toughness) ||
                        (card->deathtouchDamage && card->markedDamage > 0);
        if (wouldDie && card->counterCount("regen") > 0) {
            // Pop the regen shield: tap the creature, clear damage
            const_cast<Card*>(card)->removeCounter("regen");
            const_cast<Card*>(card)->markedDamage     = 0;
            const_cast<Card*>(card)->deathtouchDamage = false;
            const_cast<Card*>(card)->tapped           = true;
            continue;
        }

        // Lethal damage
        if (card->markedDamage >= toughness) {
            toDestroy.push_back(card->id);
            continue;
        }

        // Any damage from a deathtouch source is lethal
        if (card->deathtouchDamage && card->markedDamage > 0)
            toDestroy.push_back(card->id);
    }

    // Collect Undying/Persist/Modular/Champion info before zone changes invalidate pointers
    struct DyingInfo {
        ObjectId id;
        uint8_t  ownerId;
        bool     undying;
        bool     persist;
        int      modularCounters;      // >0 if Modular and had +1/+1 counters
        ObjectId championedCreature;   // kInvalidId if no champion
        bool     hasSoulshift;
        int      soulshiftAmount;
    };
    std::vector<DyingInfo> dying;
    dying.reserve(toDestroy.size());

    for (ObjectId id : toDestroy) {
        Card* c = game.findCard(id);
        if (!c) continue;
        DyingInfo info;
        info.id      = id;
        info.ownerId = c->ownerId;
        info.undying = c->hasKeyword(KeywordAbility::Undying) &&
                       c->counterCount("+1/+1") == 0;
        info.persist = c->hasKeyword(KeywordAbility::Persist) &&
                       c->counterCount("-1/-1") == 0;
        info.modularCounters    = c->rules->hasModular ? c->counterCount("+1/+1") : 0;
        info.championedCreature = c->championedCreature;
        info.hasSoulshift       = c->rules->hasSoulshift;
        info.soulshiftAmount    = c->rules->soulshiftAmount;

        // Detach equipment and send auras to GY
        // Collect attachment IDs first to avoid iterator invalidation
        std::vector<ObjectId> attachIds(c->attachments.begin(), c->attachments.end());
        for (ObjectId eqId : attachIds) {
            Card* eq = game.findCard(eqId);
            if (!eq) continue;
            if (eq->rules->type.isEnchantment()) {
                eq->attachedTo = kInvalidId;
                // Bestow: when the host leaves the BF, the bestow card becomes a
                // creature on the battlefield instead of going to the graveyard.
                if (eq->rules->hasBestow) {
                    // Flip face-up as a creature (it was on BF as an aura; stays as creature)
                    // The card type is already Enchantment Creature; just detach it.
                    // No zone change needed — it remains on the BF.
                } else {
                    // Normal aura: goes to GY
                    game.moveToZone(eqId, ZoneType::Graveyard, eq->ownerId);
                }
            } else {
                // Equipment: just unlinks, stays on battlefield
                detachEquipment(*eq, game);
            }
        }
        if (c->attachedTo != kInvalidId) {
            Card* equip = game.findCard(c->attachedTo);
            if (equip && !equip->rules->type.isEnchantment())
                detachEquipment(*equip, game);
            const_cast<Card*>(c)->attachedTo = kInvalidId;
        }
        game.moveToZone(id, ZoneType::Graveyard, c->ownerId);
        dying.push_back(info);
        // Fire haunt triggers for any exile cards haunting this creature
        checkHauntTriggers(game, id);
    }

    // Return Undying/Persist creatures — they enter the battlefield from the graveyard
    for (const auto& info : dying) {
        if (!info.undying && !info.persist) continue;
        // Find the card in the graveyard (moveToZone created a new object)
        Card* inGy = nullptr;
        for (Card* c : game.player(info.ownerId).graveyard().cards()) {
            if (c->ownerId == info.ownerId) {
                // The graveyard card is the newest addition — it's first (front)
                inGy = c;
                break;
            }
        }
        if (!inGy) continue;
        Card* returned = game.moveToZone(inGy->id, ZoneType::Battlefield, info.ownerId);
        if (!returned) continue;
        if (info.undying) returned->addCounter("+1/+1", 1);
        if (info.persist)  returned->addCounter("-1/-1", 1);
    }

    // Modular: transfer +1/+1 counters from the dying creature to an artifact creature.
    for (const auto& info : dying) {
        if (info.modularCounters <= 0) continue;
        // AI: target the first artifact creature we control
        for (Card* c : game.battlefield().cards()) {
            if (c->controllerId != info.ownerId) continue;
            if (!c->isCreature() || !c->rules->type.isArtifact()) continue;
            c->addCounter("+1/+1", info.modularCounters);
            break;
        }
    }

    // Champion: when a champion creature dies, return the championed exile to battlefield.
    for (const auto& info : dying) {
        if (info.championedCreature == kInvalidId) continue;
        Card* exiled = game.findCard(info.championedCreature);
        if (exiled && exiled->zone == ZoneType::Exile)
            game.moveToZone(info.championedCreature, ZoneType::Battlefield, info.ownerId);
    }

    // Soulshift N: when a Spirit creature dies, return a Spirit with CMC ≤ N from GY to hand.
    for (const auto& info : dying) {
        if (!info.hasSoulshift || info.soulshiftAmount <= 0) continue;
        // Find the most expensive Spirit in GY with CMC ≤ soulshiftAmount
        Card* best = nullptr;
        int bestCmc = -1;
        for (Card* c : game.player(info.ownerId).graveyard().cards()) {
            if (!c->rules->type.isCreature()) continue;
            if (!c->rules->type.hasSubtype("Spirit")) continue;
            int cmc = c->rules->cmc();
            if (cmc > info.soulshiftAmount) continue;
            if (cmc > bestCmc) { bestCmc = cmc; best = c; }
        }
        if (best)
            game.moveToZone(best->id, ZoneType::Hand, info.ownerId);
    }

    return !toDestroy.empty();
}

bool StateBasedActions::checkPlaneswalkerDeath(GameState& game) {
    std::vector<ObjectId> toDestroy;
    for (const Card* card : game.battlefield().cards()) {
        if (!card->rules->type.isPlaneswalker()) continue;
        if (card->counterCount("loyalty") <= 0)
            toDestroy.push_back(card->id);
    }
    for (ObjectId id : toDestroy) {
        const Card* c = game.findCard(id);
        if (!c) continue;
        game.moveToZone(id, ZoneType::Graveyard, c->ownerId);
    }
    return !toDestroy.empty();
}

bool StateBasedActions::checkSagaSacrifice(GameState& game) {
    // A Saga whose lore counters ≥ its final chapter count is put into the graveyard.
    bool any = false;
    std::vector<ObjectId> toSacrifice;
    for (const Card* c : game.battlefield().cards()) {
        if (!c->rules->type.isEnchantment()) continue;
        if (!c->rules->type.hasSubtype("Saga")) continue;
        int lore = c->counterCount("LORE");
        if (lore == 0) continue;
        // Find the highest Chapter$ number across all ability lines
        int maxChapter = 0;
        for (const auto& raw : c->rules->abilityLines) {
            auto s = parseScriptLine(raw);
            auto chStr = s.get("Chapter", "");
            if (chStr.empty()) continue;
            maxChapter = std::max(maxChapter, parseChapterNum(chStr));
        }
        if (maxChapter > 0 && lore >= maxChapter)
            toSacrifice.push_back(c->id);
    }
    for (ObjectId id : toSacrifice) {
        const Card* saga = game.findCard(id);
        if (saga) { game.moveToZone(id, ZoneType::Graveyard, saga->ownerId); any = true; }
    }
    return any;
}

bool StateBasedActions::checkAuraFallOff(GameState& game) {
    // An Aura on the battlefield whose attached permanent no longer exists
    // (moved to another zone) goes to the graveyard.
    bool any = false;
    std::vector<ObjectId> toGY;
    for (const Card* c : game.battlefield().cards()) {
        if (!c->rules->type.isEnchantment()) continue;
        if (c->attachedTo == kInvalidId) continue; // not an Aura (or detached)
        const Card* attached = game.findCard(c->attachedTo);
        if (!attached || !attached->isOnBattlefield())
            toGY.push_back(c->id);
    }
    for (ObjectId id : toGY) {
        const Card* aura = game.findCard(id);
        if (aura) { game.moveToZone(id, ZoneType::Graveyard, aura->ownerId); any = true; }
    }
    return any;
}

bool StateBasedActions::checkLegendaryRule(GameState& game) {
    bool any = false;
    for (uint8_t pid = 0; pid < 2; ++pid) {
        // Group legendary permanents controlled by this player by name
        std::unordered_map<std::string, std::vector<ObjectId>> byName;
        for (const Card* c : game.battlefield().cards()) {
            if (c->controllerId != pid) continue;
            if (!c->rules->type.isLegendary()) continue;
            byName[c->rules->name].push_back(c->id);
        }
        // Destroy all but the most recently added (last in vector) for each duplicate
        for (auto& [name, ids] : byName) {
            if (ids.size() <= 1) continue;
            for (size_t i = 0; i + 1 < ids.size(); ++i) {
                const Card* c = game.findCard(ids[i]);
                if (c) game.moveToZone(ids[i], ZoneType::Graveyard, c->ownerId);
                any = true;
            }
        }
    }
    return any;
}

bool StateBasedActions::checkCounterCancellation(GameState& game) {
    bool any = false;
    for (Card* c : game.battlefield().cards()) {
        int plus  = c->counterCount("+1/+1");
        int minus = c->counterCount("-1/-1");
        if (plus > 0 && minus > 0) {
            int cancel = std::min(plus, minus);
            c->addCounter("+1/+1", -cancel);
            c->addCounter("-1/-1", -cancel);
            any = true;
        }
    }
    return any;
}

bool StateBasedActions::checkPlayerLoss(GameState& game) {
    bool any = false;
    for (uint8_t i = 0; i < 2; ++i) {
        Player& p = game.player(i);
        if (p.hasLost()) continue;
        if (p.life() <= 0 || p.poisonCounters() >= 10 ||
            p.commanderDamageFrom(0) >= 21 || p.commanderDamageFrom(1) >= 21) {
            p.lose();
            any = true;
        }
    }
    return any;
}

bool StateBasedActions::checkAlwaysTriggers(GameState& game) {
    bool any = false;

    for (const Card* card : game.battlefield().cards()) {
        if (card->rules->abilityLines.empty()) continue;
        // Don't queue another trigger from this card while one is still pending
        if (game.hasPendingTriggerFrom(card->id)) continue;

        for (const auto& raw : card->rules->abilityLines) {
            auto line = parseScriptLine(raw);
            if (line.get("Mode", "") != "Always") continue;

            auto zones = line.get("TriggerZones", "Battlefield");
            if (zones.find("Battlefield") == std::string::npos) continue;

            std::string execSvar{line.get("Execute", "")};
            if (execSvar.empty()) continue;

            auto svarIt = card->rules->svars.find(execSvar);
            if (svarIt == card->rules->svars.end()) continue;

            bool conditionMet = false;

            // --- IsPresent$ condition ---
            auto isPresent  = line.get("IsPresent", "");
            auto presentCmp = line.get("PresentCompare", "");

            if (!isPresent.empty()) {
                int count = 0;
                for (const Card* c2 : game.battlefield().cards()) {
                    if (cardMatchesAnyFilter(*c2, isPresent,
                                            card->controllerId, card->id))
                        ++count;
                }
                if (presentCmp.empty()) {
                    conditionMet = (count >= 1);
                } else if (presentCmp.size() >= 3) {
                    auto op = presentCmp.substr(0, 2);
                    int n = 0;
                    std::from_chars(presentCmp.data() + 2,
                                    presentCmp.data() + presentCmp.size(), n);
                    if      (op == "GE") conditionMet = (count >= n);
                    else if (op == "LE") conditionMet = (count <= n);
                    else if (op == "EQ") conditionMet = (count == n);
                    else if (op == "GT") conditionMet = (count >  n);
                    else if (op == "LT") conditionMet = (count <  n);
                }
            }

            // --- CheckSVar$ condition ---
            auto checkSvar = line.get("CheckSVar", "");
            auto svarCmp   = line.get("SVarCompare", "");
            if (!checkSvar.empty() && !svarCmp.empty()) {
                int val = game.evaluateSVar(std::string(checkSvar), card->controllerId,
                                            card->rules, card->id);
                if (svarCmp.size() >= 3) {
                    auto op = svarCmp.substr(0, 2);
                    int n = 0;
                    std::from_chars(svarCmp.data() + 2,
                                    svarCmp.data() + svarCmp.size(), n);
                    if      (op == "GE") conditionMet = (val >= n);
                    else if (op == "LE") conditionMet = (val <= n);
                    else if (op == "EQ") conditionMet = (val == n);
                    else if (op == "GT") conditionMet = (val >  n);
                    else if (op == "LT") conditionMet = (val <  n);
                }
            }

            if (!conditionMet) continue;

            PendingTrigger trig;
            trig.sourceCardId    = card->id;
            trig.controllerId    = card->controllerId;
            trig.effect          = parseScriptLine(svarIt->second);
            trig.triggeredCardId = card->id;

            std::vector<PendingTrigger> v;
            v.push_back(std::move(trig));
            game.queueTriggers(std::move(v));
            any = true;
            break; // one trigger per card per SBA pass; next pass picks up remaining
        }
    }
    return any;
}

} // namespace mtg
