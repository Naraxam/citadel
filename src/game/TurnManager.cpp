#include "TurnManager.h"
#include "CardFilter.h"
#include "GameState.h"
#include "CardStats.h"
#include "TriggerSystem.h"
#include "KeywordAbility.h"
#include "ability/Effects.h"
#include <algorithm>
#include <charconv>
#include <memory>
#include <vector>

namespace mtg {

TurnManager::TurnManager(GameState& game)
    : m_game(game)
    , m_step(TurnStep::Untap)
    , m_stepIndex(0)
    , m_priorityHolder(game.activePlayerId())
{}

void TurnManager::reset() noexcept {
    m_step           = TurnStep::Untap;
    m_stepIndex      = 0;
    m_priorityHolder = m_game.activePlayerId();
    m_passed[0] = m_passed[1] = false;
    m_combat.clear();
}

// ── Step navigation ───────────────────────────────────────────────────────────

void TurnManager::beginStep() {
    const uint8_t ap = m_game.activePlayerId();

    switch (m_step) {

    case TurnStep::Untap: {
        // Snapshot cards that were tapped — these are candidates for Inspired triggers
        std::vector<ObjectId> wasTapped;
        for (Card* c : m_game.battlefield().cards()) {
            if (c->controllerId == ap && c->tapped && !c->exerted)
                wasTapped.push_back(c->id);
        }
        for (Card* c : m_game.battlefield().cards()) {
            if (c->controllerId != ap) continue;
            c->summoningSickness = false;
            c->goaded            = false;
            c->goadedBy          = 255;

            if (c->rules->hasPhasing) {
                // Phasing: phase out if in, phase in if out.  Phased-out cards
                // are treated as if they don't exist until they phase back in.
                c->phasedOut = !c->phasedOut;
                if (c->phasedOut) continue;  // don't untap a card phasing out
            } else {
                c->phasedOut = false;  // non-phasing cards always phase back in
            }

            if (c->exerted) {
                c->exerted = false;
            } else {
                c->tapped = false;
            }
        }
        // Fire Inspired (onUntap) for each creature that actually untapped
        for (ObjectId id : wasTapped) {
            Card* c = m_game.findCard(id);
            if (!c || c->tapped || !c->isCreature()) continue;
            std::vector<PendingTrigger> t;
            TriggerSystem::onUntap(*c, m_game, t);
            m_game.queueTriggers(std::move(t));
        }
        break;
    }

    case TurnStep::Draw:
        drawCard(ap);
        {
            // Sagas: after the owner's draw step, add a lore counter to each Saga they control.
            // Snapshot IDs first — triggerSagaChapter may sacrifice and invalidate pointers.
            std::vector<ObjectId> sagaIds;
            for (const Card* c : m_game.battlefield().cards())
                if (c->controllerId == ap && c->rules->saga.has_value())
                    sagaIds.push_back(c->id);
            for (ObjectId sid : sagaIds) {
                Card* c = m_game.findCard(sid);
                if (c && c->rules->saga.has_value())
                    m_game.triggerSagaChapter(c);
            }
        }
        break;

    case TurnStep::Cleanup: {
        // ControlPlayer lasts exactly the controlled player's turn — release it here.
        m_game.clearTurnController(ap);
        m_game.tempCantCast.clear();  // "can't cast this turn" restrictions wear off
        m_game.tempCostMods.clear();  // "spells cost less this turn" reductions wear off
        m_game.tempCantActivate[0] = false;  // "can't activate abilities this turn" wears off
        m_game.tempCantActivate[1] = false;
        m_game.tempCantGainLife[0] = false;  // "can't gain life this turn" wears off
        m_game.tempCantGainLife[1] = false;
        m_game.tempExtraLandPlays[0] = 0;    // extra land plays this turn wear off
        m_game.tempExtraLandPlays[1] = 0;
        Player& p = m_game.player(ap);
        int maxHand = (ap < 2 && m_game.tempMaxHandSize[ap] >= 0)
                      ? m_game.tempMaxHandSize[ap] : p.maxHandSize();
        if (ap < 2) { m_game.tempMaxHandSize[0] = -1; m_game.tempMaxHandSize[1] = -1; }
        while (static_cast<int>(p.hand().size()) > maxHand) {
            Card* last = p.hand().back();
            if (!last) break;
            m_game.moveToZone(last->id, ZoneType::Graveyard, ap);
        }
        for (Card* c : m_game.battlefield().cards()) {
            c->markedDamage       = 0;
            c->deathtouchDamage   = false;
            c->damageShield       = 0;
            c->loyaltyUsedThisTurn= false;  // rule 606.3: reset PW loyalty use each turn
            // Remove "until end of turn" stat/keyword/type bonuses
            c->tempPower      = 0;
            c->tempToughness  = 0;
            c->enlistBonus    = 0;
            c->keywordMask   &= ~c->tempKeywords;
            c->tempKeywords   = 0;
            c->keywordMask        |= c->tempRemovedKeywords;  // Debuff wears off
            c->tempRemovedKeywords = 0;
            c->tempIsCreature = false;
            c->tempUnblockable = false;   // "can't be blocked this turn" wears off
            c->tempBlockOnlyBy.clear();
            c->tempCantBlock = false;     // "can't block this turn" wears off
            c->tempCantAttack = false;    // "can't attack this turn" wears off
            c->tempCantRegenerate = false;// "can't be regenerated this turn" wears off
            // Detain (cantAttack/cantBlock) wears off at the detaining player's
            // next turn — simplification: clear at end of any turn
            c->cantAttack        = false;
            c->cantBlock         = false;
            c->setPower          = -1;
            c->setToughness      = -1;
            c->mustAttack        = false;
            c->mustAttackTarget  = 255;
            c->mustBlockTarget   = kInvalidId;
            c->mustBlockAny      = false;
            c->tempCantActivate  = false;
        }
        // Revert GainControl Duration$ EndOfTurn effects
        for (Card* c : m_game.battlefield().cards()) {
            if (c->originalControllerId != 0xFF) {
                c->controllerId         = c->originalControllerId;
                c->originalControllerId = 0xFF;
            }
        }

        m_game.spellsCastThisTurn       = 0;
        m_game.spellsCastByPlayer[0]    = 0;
        m_game.spellsCastByPlayer[1]    = 0;
        m_game.creaturesDiedThisTurn    = 0;
        // Reset per-turn "triggers only once each turn" counters (ActivationLimit$).
        for (Card* c : m_game.battlefield().cards()) c->triggerFiresThisTurn.clear();
        for (Card* c : m_game.command().cards())     c->triggerFiresThisTurn.clear();
        m_game.recomputeStaticBonuses(); // refresh Morbid/etc. now that death counter is cleared
        m_game.preventAllCombatDamage      = false;
        m_game.preventAllDamageToPlayer    = false;
        m_game.playerDamagedThisTurn[0]             = false;
        m_game.playerDamagedThisTurn[1]             = false;
        m_game.prowlActive[0]                       = false;
        m_game.prowlActive[1]                       = false;
        m_game.permanentLeftBattlefieldThisTurn[0]  = false;
        m_game.permanentLeftBattlefieldThisTurn[1]  = false;
        m_game.attackedThisTurn[0]                  = false;
        m_game.attackedThisTurn[1]                  = false;
        m_game.cardsDrawnThisTurn[0]                = 0;
        m_game.cardsDrawnThisTurn[1]                = 0;
        m_game.lifeGainedThisTurn[0]                = 0;
        m_game.lifeGainedThisTurn[1]                = 0;
        m_game.lifeLostThisTurn[0]                  = 0;
        m_game.lifeLostThisTurn[1]                  = 0;
        m_game.permanentsEnteredThisTurn.clear();
        m_game.player(0).clearDamageShield();
        m_game.player(1).clearDamageShield();
        m_game.player(0).manaPool().empty();
        m_game.player(1).manaPool().empty();

        // ── Day/Night transition (Innistrad daybound/nightbound) ──────────────
        // Rule 726.2: during Cleanup, if day/night is active:
        //   - It becomes Night if the active player cast 0 spells this turn.
        //   - It becomes Day if the active player cast 2+ spells this turn.
        {
            uint8_t dnAp = m_game.activePlayerId();
            int spells = m_game.spellsCastByPlayer[dnAp];
            bool becomeDay   = (m_game.dayNightState != 0 && spells >= 2);
            bool becomeNight = (m_game.dayNightState != 0 && spells == 0);
            // Helper: does this card's current rules have a given K: keyword?
            auto hasKw = [](const Card* c, const char* kw) -> bool {
                if (!c->rules) return false;
                for (const auto& k : c->rules->keywords)
                    if (k == kw) return true;
                return false;
            };
            if (becomeDay && m_game.dayNightState != 1) {
                m_game.dayNightState = 1;   // → Day
                // Transform cards currently showing their Nightbound (back) face back to day
                for (Card* c : m_game.battlefield().cards())
                    if (c->transformed && c->rules && c->rules->backFace
                        && hasKw(c, "Nightbound")) {
                        // Reset to original (day) face
                        c->ownedRules  = nullptr;
                        c->rules       = c->originalRules;
                        c->transformed = false;
                        c->keywordMask = buildKeywordMask(*c->rules);
                    }
            } else if (becomeNight && m_game.dayNightState != 2) {
                m_game.dayNightState = 2;   // → Night
                // Transform cards currently showing their Daybound (front) face to night
                for (Card* c : m_game.battlefield().cards())
                    if (!c->transformed && c->rules && c->rules->backFace
                        && hasKw(c, "Daybound")) {
                        c->ownedRules  = std::make_shared<CardRules>(*c->rules->backFace);
                        c->rules       = c->ownedRules.get();
                        c->transformed = true;
                        c->keywordMask = buildKeywordMask(*c->rules);
                    }
            }
        }
        break;
    }

    case TurnStep::EndStep: {
        // Monarch: the Monarch draws a card at the beginning of their end step.
        if (m_game.monarchPlayer == m_game.activePlayerId())
            drawCard(m_game.monarchPlayer);

        // Initiative: the holder ventures into the dungeon at the beginning of
        // THEIR end step.  This models the "at the beginning of your upkeep" text.
        if (m_game.initiativeHolder == static_cast<int8_t>(m_game.activePlayerId())) {
            // Simplified venture: advance room (same logic as effectVenture)
            int next = std::min(m_game.initiativeRoom + 1, 3);
            m_game.initiativeRoom = next;
        }

        // Dash: return dashed creatures to their owner's hand at the beginning of end step.
        std::vector<ObjectId> toReturn = std::move(m_game.dashedCards);
        m_game.dashedCards.clear();
        for (ObjectId id : toReturn) {
            Card* dc = m_game.findCard(id);
            if (dc && dc->isOnBattlefield())
                m_game.moveToZone(id, ZoneType::Hand, dc->controllerId);
        }
        // Unearth: exile creatures that entered via Unearth.
        std::vector<ObjectId> toExile = std::move(m_game.unearthedCards);
        m_game.unearthedCards.clear();
        for (ObjectId id : toExile) {
            Card* uc = m_game.findCard(id);
            if (uc && uc->isOnBattlefield())
                m_game.moveToZone(id, ZoneType::Exile, uc->controllerId);
        }
        // Blitz: sacrifice creatures that were cast via Blitz.
        // The T:Mode$ Dies trigger on the card fires draw-a-card automatically.
        {
            std::vector<ObjectId> toSac = std::move(m_game.blitzedCards);
            m_game.blitzedCards.clear();
            for (ObjectId id : toSac) {
                Card* bc = m_game.findCard(id);
                if (bc && bc->isOnBattlefield()) {
                    std::vector<PendingTrigger> sacTrigs;
                    TriggerSystem::onSacrificed(*bc, m_game, sacTrigs);
                    m_game.queueTriggers(std::move(sacTrigs));
                    m_game.moveToZone(id, ZoneType::Graveyard, bc->ownerId);
                }
            }
        }
        break;
    }

    default:
        break;
    }
}

void TurnManager::endStep() {
    m_game.player(0).manaPool().empty();
    m_game.player(1).manaPool().empty();
    if (m_step == TurnStep::DeclareAttackers) {
        // Fire Exalted-style triggers that depend on knowing the total attacker count.
        std::vector<PendingTrigger> t;
        TriggerSystem::onAttackersFinalized(m_game, t);
        if (!t.empty()) m_game.queueTriggers(std::move(t));

        // Mentor: for each attacking creature with Mentor, put a +1/+1 counter on
        // another attacking creature you control with strictly less power.
        for (const Card* mentor : m_game.battlefield().cards()) {
            if (!mentor->attacking || !mentor->rules->hasMentor) continue;
            int mentorPow = effectivePower(*mentor);
            const Card* best = nullptr;
            int bestPow = -1;
            for (const Card* other : m_game.battlefield().cards()) {
                if (other->id == mentor->id || !other->attacking) continue;
                if (other->controllerId != mentor->controllerId) continue;
                int p = effectivePower(*other);
                if (p < mentorPow && p > bestPow) { best = other; bestPow = p; }
            }
            if (best) {
                Card* target = m_game.findCard(best->id);
                if (target) {
                    target->addCounter("+1/+1", 1);
                    std::vector<PendingTrigger> ct;
                    TriggerSystem::onCounterAdded(*target, "+1/+1", 1, m_game, ct);
                    m_game.queueTriggers(std::move(ct));
                }
            }
        }

        // Exalted: if exactly one creature is attacking, give it +1/+1 until end of turn
        // for each Exalted ability its controller has on the battlefield.
        {
            std::vector<Card*> atkers;
            for (Card* c : m_game.battlefield().cards())
                if (c->attacking) atkers.push_back(c);
            if (atkers.size() == 1) {
                Card* sole    = atkers[0];
                uint8_t ctrl  = sole->controllerId;
                int exaltedN  = 0;
                for (const Card* c : m_game.battlefield().cards())
                    if (c->controllerId == ctrl && c->rules->hasExalted) ++exaltedN;
                if (exaltedN > 0) {
                    sole->tempPower     += exaltedN;
                    sole->tempToughness += exaltedN;
                }
            }
        }

        // Training: for each attacking Training creature, if any co-attacking creature
        // has strictly greater power, put a +1/+1 counter on it.
        for (Card* trainer : m_game.battlefield().cards()) {
            if (!trainer->attacking || !trainer->rules->hasTraining) continue;
            int tp = effectivePower(*trainer);
            bool biggerAlly = false;
            for (const Card* other : m_game.battlefield().cards()) {
                if (other->id == trainer->id || !other->attacking) continue;
                if (other->controllerId != trainer->controllerId) continue;
                if (effectivePower(*other) > tp) { biggerAlly = true; break; }
            }
            if (biggerAlly) {
                trainer->addCounter("+1/+1", 1);
                std::vector<PendingTrigger> tct;
                TriggerSystem::onCounterAdded(*trainer, "+1/+1", 1, m_game, tct);
                m_game.queueTriggers(std::move(tct));
            }
        }
    }
    if (m_step == TurnStep::EndCombat)
        endCombat();
}

void TurnManager::advanceStep() {
    m_stepIndex = (m_stepIndex + 1) % kStepCount;
    m_step      = kStepOrder[m_stepIndex];
    if (m_stepIndex == 0) {
        // If the active player has extra turns queued, consume one and repeat their turn.
        if (m_game.activePlayer().consumeExtraTurn()) {
            m_game.incrementTurnNumber();
            m_game.activePlayer().resetTurnState();
        } else {
            m_game.advanceTurn();
            // SkipTurn: if the new active player has skip-turns pending, consume one and
            // advance again (giving the other player another turn).
            while (m_game.activePlayer().consumeSkipTurn()) {
                m_game.advanceTurn();
            }
        }
        m_priorityHolder = m_game.activePlayerId();
    }
    resetPriorityPassed();
}

// ── Extra combat / step jump ──────────────────────────────────────────────────

void TurnManager::jumpToStep(TurnStep s) noexcept {
    for (int i = 0; i < kStepCount; ++i) {
        if (kStepOrder[i] == s) {
            m_stepIndex = i;
            m_step      = s;
            resetPriorityPassed();
            return;
        }
    }
}

// ── Snapshot / restore ───────────────────────────────────────────────────────

TurnManager::Snapshot TurnManager::saveSnapshot() const noexcept {
    return { m_step, m_stepIndex, m_priorityHolder,
             { m_passed[0], m_passed[1] }, m_combat };
}

void TurnManager::restoreSnapshot(const Snapshot& s) noexcept {
    m_step           = s.step;
    m_stepIndex      = s.stepIndex;
    m_priorityHolder = s.priorityHolder;
    m_passed[0]      = s.passed[0];
    m_passed[1]      = s.passed[1];
    m_combat         = s.combat;
}

// ── Priority ──────────────────────────────────────────────────────────────────

void TurnManager::resetPriorityPassed() {
    m_passed[0] = false;
    m_passed[1] = false;
}

void TurnManager::givePriorityToActive() {
    m_priorityHolder = m_game.activePlayerId();
    resetPriorityPassed();
}

bool TurnManager::passPriority(uint8_t playerId) {
    if (playerId >= 2) return false;
    m_passed[playerId] = true;
    if (m_passed[0] && m_passed[1] && m_game.stack().empty())
        return true;
    m_priorityHolder           = playerId ^ 1;
    m_passed[m_priorityHolder] = false;
    return false;
}

// ── State-based actions ───────────────────────────────────────────────────────

bool TurnManager::runSBAsUntilClean() {
    bool anyEver = false;
    while (StateBasedActions::run(m_game))
        anyEver = true;
    return anyEver;
}

// ── Combat ────────────────────────────────────────────────────────────────────

bool TurnManager::declareAttacker(ObjectId creatureId, uint8_t defendingPlayerId) {
    Card* c = m_game.findCard(creatureId);
    if (!c)                                                    return false;
    if (!c->isCreature())                                      return false;
    if (!c->isOnBattlefield())                                 return false;
    if (c->tapped)                                             return false;
    if (c->summoningSickness)                                  return false;
    if (c->controllerId != m_game.activePlayerId())            return false;
    if (c->cantAttack || c->tempCantAttack)                    return false;
    // Goad: can't attack the player who goaded it
    if (c->goaded && c->goadedBy == defendingPlayerId)         return false;

    // Vigilance: attacker doesn't tap
    if (!c->hasKeyword(KeywordAbility::Vigilance)) {
        c->tapped = true;
        // Fire "whenever ~ becomes tapped" (Inspired) triggers
        std::vector<PendingTrigger> tapTrigs;
        TriggerSystem::onTap(*c, m_game, tapTrigs);
        m_game.queueTriggers(std::move(tapTrigs));
    }

    // Enlist: if this attacker has Enlist, tap one untapped non-attacking creature the
    // controller controls and add its power to this creature's power until end of turn.
    if (c->rules->hasEnlist) {
        Card* best = nullptr;
        int   bestPower = 0;
        for (Card* other : m_game.battlefield().cards()) {
            if (other->id == creatureId) continue;
            if (other->controllerId != c->controllerId) continue;
            if (!other->isCreature() || other->tapped || other->attacking) continue;
            if (other->summoningSickness) continue;
            int p = effectivePower(*other);
            if (p > bestPower) { bestPower = p; best = other; }
        }
        if (best && bestPower > 0) {
            best->tapped = true;
            // Store the bonus as a temporary power modifier on the attacker
            c->enlistBonus = bestPower;
        }
    }

    m_combat.attacks.push_back({ creatureId, defendingPlayerId, kInvalidId, {}, {}, false });
    c->attacking = true;
    m_game.attackedThisTurn[c->controllerId] = true;
    m_game.activeCombat = &m_combat;

    // Fire attack triggers (e.g. "whenever ~ attacks, draw a card")
    {
        std::vector<PendingTrigger> atk;
        TriggerSystem::onAttack(*c, m_game, atk);
        m_game.queueTriggers(std::move(atk));
    }

    // Annihilator N: defending player immediately sacrifices N permanents
    if (c->rules->hasAnnihilator && c->rules->annihilatorCount > 0) {
        int n = c->rules->annihilatorCount;
        std::vector<const Card*> perms;
        for (const Card* p : m_game.battlefield().cards())
            if (p->controllerId == defendingPlayerId)
                perms.push_back(p);
        // Sacrifice lowest-CMC first (AI heuristic: lose the least valuable)
        std::sort(perms.begin(), perms.end(), [](const Card* a, const Card* b) {
            return a->rules->cmc() < b->rules->cmc();
        });
        int toSac = std::min(n, (int)perms.size());
        std::vector<ObjectId> ids;
        ids.reserve(toSac);
        for (int i = 0; i < toSac; ++i) ids.push_back(perms[i]->id);
        for (ObjectId id : ids) {
            Card* p = m_game.findCard(id);
            if (!p || !p->isOnBattlefield()) continue;
            std::vector<PendingTrigger> t;
            TriggerSystem::onSacrificed(*p, m_game, t);
            m_game.queueTriggers(std::move(t));
            m_game.moveToZone(id, ZoneType::Graveyard, p->ownerId);
        }
        while (StateBasedActions::run(m_game)) {}
    }

    return true;
}

bool TurnManager::declareAttackerVsPlaneswalker(ObjectId creatureId,
                                                 ObjectId planeswalkerTargetId) {
    const Card* pw = m_game.findCard(planeswalkerTargetId);
    if (!pw || !pw->rules || !pw->rules->type.isPlaneswalker()) return false;
    uint8_t pwController = pw->controllerId;
    // Target must be controlled by an opponent
    if (pwController == m_game.activePlayerId()) return false;

    if (!declareAttacker(creatureId, pwController)) return false;

    // Set the planeswalker target on the last-added attack
    if (!m_combat.attacks.empty())
        m_combat.attacks.back().defendingPlaneswalker = planeswalkerTargetId;
    return true;
}

bool TurnManager::declareBlocker(ObjectId blockerId, ObjectId attackerId) {
    Card* blocker = m_game.findCard(blockerId);
    if (!blocker)                                              return false;
    if (!blocker->isCreature())                                return false;
    if (!blocker->isOnBattlefield())                           return false;
    if (blocker->tapped)                                       return false;
    if (blocker->cantBlock || blocker->tempCantBlock)          return false;
    if (blocker->controllerId == m_game.activePlayerId())      return false;

    auto* attack = m_combat.findAttack(attackerId);
    if (!attack) return false;

    Card* attacker = m_game.findCard(attackerId);
    if (!attacker) return false;

    // MinMaxBlocker: attacker already has the maximum number of blockers assigned
    if ((int)attack->blockerIds.size() >= attacker->maxBlockerCount) return false;

    // CantBlockBy: attacker is unblockable or can only be blocked by a filter
    // (permanent statics, plus DB$ Effect "can't be blocked this turn").
    if (attacker->unblockable || attacker->tempUnblockable) return false;
    for (const std::string* filt : { &attacker->blockOnlyBy, &attacker->tempBlockOnlyBy }) {
        if (!filt->empty() &&
            !cardMatchesAnyFilter(*blocker, *filt, blocker->controllerId, attacker->id))
            return false;
    }

    // Flying restriction
    if (attacker->hasKeyword(KeywordAbility::Flying)) {
        if (!blocker->hasKeyword(KeywordAbility::Flying) &&
            !blocker->hasKeyword(KeywordAbility::Reach))
            return false;
    }

    // Shadow — can only block/be blocked by other shadow creatures
    if (attacker->hasKeyword(KeywordAbility::Shadow) !=
        blocker->hasKeyword(KeywordAbility::Shadow))
        return false;

    // Horsemanship — can only be blocked by creatures with Horsemanship
    if (attacker->rules->hasKeyword("Horsemanship") &&
        !blocker->rules->hasKeyword("Horsemanship"))
        return false;

    // Intimidate — can only be blocked by artifact creatures or creatures sharing a color
    if (attacker->rules->hasKeyword("Intimidate")) {
        bool isArtifact  = blocker->rules->type.isArtifact();
        uint8_t atkColor = (attacker->colorIdOverride != 0xFF)
                           ? attacker->colorIdOverride
                           : attacker->rules->manaCost.colorIdentity();
        uint8_t blkColor = (blocker->colorIdOverride != 0xFF)
                           ? blocker->colorIdOverride
                           : blocker->rules->manaCost.colorIdentity();
        bool sharesColor = (atkColor & blkColor) != 0;
        if (!isArtifact && !sharesColor) return false;
    }

    // Fear — can only be blocked by black or artifact creatures
    if (attacker->hasKeyword(KeywordAbility::Fear)) {
        uint8_t blkColor = blocker->rules->manaCost.colorIdentity();
        bool isBlack    = (blkColor & 0x04) != 0;
        bool isArtifact = blocker->rules->type.isArtifact();
        if (!isBlack && !isArtifact) return false;
    }

    // Landwalk — check if defending player controls the matching land type
    auto defPid  = static_cast<uint8_t>(blocker->controllerId);
    auto& defLands = m_game.player(defPid);
    auto hasLandSubtype = [&](std::string_view subtype) {
        for (const Card* c : m_game.battlefield().cards())
            if (c->controllerId == defPid && c->isLand() &&
                c->rules->type.hasSubtype(subtype)) return true;
        return false;
    };
    if (attacker->hasKeyword(KeywordAbility::Swampwalk)    && hasLandSubtype("Swamp"))    return false;
    if (attacker->hasKeyword(KeywordAbility::Islandwalk)   && hasLandSubtype("Island"))   return false;
    if (attacker->hasKeyword(KeywordAbility::Mountainwalk) && hasLandSubtype("Mountain")) return false;
    if (attacker->hasKeyword(KeywordAbility::Forestwalk)   && hasLandSubtype("Forest"))   return false;
    if (attacker->hasKeyword(KeywordAbility::Plainswalk)   && hasLandSubtype("Plains"))   return false;

    // Protection from everything — the attacker can't be blocked at all
    if (maskHas(attacker->keywordMask, KeywordAbility::ProtectionAll)) return false;
    // Type-based protection on the attacker: can't be blocked by creatures (a common form)
    if ((attacker->rules->protectionTypeMask & 0x04) && blocker->isCreature()) return false;
    if ((attacker->rules->protectionTypeMask & 0xFF) == 0xFF) return false;
    // Protection from a specific color — can't be blocked by sources of that color
    uint8_t blockerColor = blocker->rules->manaCost.colorIdentity();
    if (hasProtectionFrom(attacker->keywordMask, blockerColor)) return false;
    (void)defLands;

    attack->blockerIds.push_back(blockerId);
    blocker->blocking  = true;
    attacker->isBlocked = true;

    // Flanking: if the attacker has Flanking and the blocker does not,
    // the blocker gets -1/-1 until end of turn.
    if ((attacker->rules->hasFlanking || attacker->rules->hasKeyword("Flanking")) &&
        !blocker->rules->hasFlanking && !blocker->rules->hasKeyword("Flanking")) {
        blocker->tempPower     -= 1;
        blocker->tempToughness -= 1;
    }

    // Rampage N: for each blocker beyond the first, the attacker gets +N/+N until EOT.
    if ((int)attack->blockerIds.size() >= 2) {
        int rn = attacker->rules->hasRampage ? attacker->rules->rampageAmount : 0;
        if (rn == 0) {
            for (const auto& kw : attacker->rules->keywords) {
                if (kw.size() > 8 && kw.substr(0, 8) == "Rampage:") {
                    std::from_chars(kw.data() + 8, kw.data() + kw.size(), rn);
                    break;
                }
            }
        }
        if (rn > 0) {
            attacker->tempPower     += rn;
            attacker->tempToughness += rn;
        }
    }

    // Fire "whenever ~ blocks" and "whenever ~ becomes blocked" triggers
    {
        std::vector<PendingTrigger> blkTrigs;
        TriggerSystem::onBlock(*blocker, *attacker, m_game, blkTrigs);
        m_game.queueTriggers(std::move(blkTrigs));
    }
    return true;
}

bool TurnManager::hasFirstStrikers() const noexcept {
    for (const auto& attack : m_combat.attacks) {
        const Card* att = m_game.findCard(attack.attackerId);
        if (att && (att->hasKeyword(KeywordAbility::FirstStrike) ||
                    att->hasKeyword(KeywordAbility::DoubleStrike)))
            return true;
        for (ObjectId bid : attack.blockerIds) {
            const Card* blk = m_game.findCard(bid);
            if (blk && (blk->hasKeyword(KeywordAbility::FirstStrike) ||
                        blk->hasKeyword(KeywordAbility::DoubleStrike)))
                return true;
        }
    }
    return false;
}

// Helper: deal damage to a creature, respecting deathtouch / infect / wither / protection.
// sourceColor: ManaAtom color bitmask of the damage source (0 = colorless).
static void applyDamageToCreature(Card& target, int amount, bool fromDeathtouch,
                                   bool fromInfect, bool fromWither,
                                   uint8_t sourceColor,
                                   GameState& game, const Card* source) {
    // Protection prevents damage from sources of the matching color
    if (hasProtectionFrom(target.keywordMask, sourceColor)) return;

    // R:Event$ DamageDone prevention replacements (Daunting Defender, Cover of Winter…)
    amount = applyDamageReplacements(amount, source, &target, -1, game);
    if (amount <= 0) return;

    // Per-card damage prevention shield
    if (target.damageShield > 0) {
        int prevented = std::min(amount, target.damageShield);
        target.damageShield -= prevented;
        amount -= prevented;
    }
    if (amount <= 0) return;

    if (fromInfect || fromWither) {
        target.addCounter("-1/-1", amount);
    } else {
        target.markedDamage += amount;
        if (fromDeathtouch && amount > 0)
            target.deathtouchDamage = true;
    }
}

void TurnManager::dealCombatDamage(bool isFirstStrikeStep) {
    if (m_game.preventAllCombatDamage) return; // Fog effect active

    // Accumulate all damage events for DamageDoneOnce trigger processing at the end.
    std::vector<TriggerSystem::CombatDamageEvent> damageEvents;

    for (auto& attack : m_combat.attacks) {
        Card* attacker = m_game.findCard(attack.attackerId);
        if (!attacker) continue;

        bool atFS = attacker->hasKeyword(KeywordAbility::FirstStrike);
        bool atDS = attacker->hasKeyword(KeywordAbility::DoubleStrike);

        // Does the attacker deal damage this step?
        // First-strike step : FS or DS creatures deal damage
        // Regular step      : non-FS creatures deal damage; DS creatures deal again
        bool attackerDeals = isFirstStrikeStep ? (atFS || atDS) : (!atFS || atDS);

        if (attackerDeals) {
            // Auto-sort blockers by toughness for AI attackers (weakest first).
            // Human attackers: player already ordered blockers in the UI.
            if (attack.blockerIds.size() > 1 && attacker->controllerId != 0) {
                std::sort(attack.blockerIds.begin(), attack.blockerIds.end(),
                    [&](ObjectId a, ObjectId b) {
                        const Card* ca = m_game.findCard(a);
                        const Card* cb = m_game.findCard(b);
                        if (!ca || !cb) return false;
                        int ta = effectiveToughness(*ca) - ca->markedDamage;
                        int tb = effectiveToughness(*cb) - cb->markedDamage;
                        return ta < tb; // weakest first
                    });
            }

            int  power     = attacker->dealsDamageByToughness
                                 ? effectiveToughness(*attacker) : effectivePower(*attacker);
            bool trample   = attacker->hasKeyword(KeywordAbility::Trample);
            bool dtouch    = attacker->hasKeyword(KeywordAbility::Deathtouch);
            bool lifelink  = attacker->hasKeyword(KeywordAbility::Lifelink);
            bool menace    = attacker->hasKeyword(KeywordAbility::Menace);
            bool infect    = attacker->hasKeyword(KeywordAbility::Infect);
            bool wither    = attacker->hasKeyword(KeywordAbility::Wither);
            uint8_t atkColor = attacker->rules->manaCost.colorIdentity();

            // Menace: must be blocked by 2+ creatures; fewer = illegal block = unblocked
            bool effectivelyUnblocked = attack.blockerIds.empty() ||
                                        (menace && attack.blockerIds.size() < 2);

            if (effectivelyUnblocked) {
                // Fire AttackerUnblocked triggers (only on first damage step, not first-strike repeat)
                if (!isFirstStrikeStep) {
                    std::vector<PendingTrigger> ubt;
                    TriggerSystem::onAttackerUnblocked(*attacker, m_game, ubt);
                    m_game.queueTriggers(std::move(ubt));
                }

                // Planeswalker target: deal damage to loyalty counters instead of player life
                if (attack.defendingPlaneswalker != kInvalidId) {
                    Card* pw = m_game.findCard(attack.defendingPlaneswalker);
                    if (pw && pw->isOnBattlefield() && pw->rules->type.isPlaneswalker()) {
                        int loy = pw->counterCount("loyalty");
                        int newLoy = loy - power;
                        pw->counters["loyalty"] = newLoy;
                        if (lifelink && power > 0)
                            m_game.gainLife(attacker->controllerId, power);
                        // Planeswalker death handled by SBAs (loyalty ≤ 0)
                    }
                    continue;  // skip normal player-damage code
                }

                // R:Event$ DamageDone prevention to the player (Guardian Seraph…),
                // then the per-player damage prevention shield.
                int damToPlayer = applyDamageReplacements(
                    power, attacker, nullptr,
                    static_cast<int>(attack.defendingPlayerId), m_game);
                {
                    Player& defender = m_game.player(attack.defendingPlayerId);
                    if (defender.damageShield() > 0) {
                        int prevented = std::min(damToPlayer, defender.damageShield());
                        defender.addDamageShield(-prevented);
                        damToPlayer -= prevented;
                    }
                }
                // Infect deals to players as poison counters instead of life loss
                if (damToPlayer > 0) {
                    if (infect) {
                        m_game.player(attack.defendingPlayerId).addPoison(damToPlayer);
                    } else {
                        m_game.loseLife(attack.defendingPlayerId, damToPlayer);
                        m_game.playerDamagedThisTurn[attack.defendingPlayerId] = true;
                        {
                            std::vector<PendingTrigger> llt;
                            TriggerSystem::onLoseLife(attack.defendingPlayerId, damToPlayer, m_game, llt);
                            m_game.queueTriggers(std::move(llt));
                        }
                    }
                }
                if (lifelink && damToPlayer > 0)
                    m_game.gainLife(attacker->controllerId, damToPlayer);
                // Renown: first time this creature deals combat damage to a player
                if (damToPlayer > 0 && !attacker->renowned &&
                    attacker->rules->hasRenown && attacker->rules->renownAmount > 0) {
                    attacker->renowned = true;
                    int n = attacker->rules->renownAmount;
                    attacker->addCounter("+1/+1", n);
                    std::vector<PendingTrigger> rct;
                    TriggerSystem::onCounterAdded(*attacker, "+1/+1", n, m_game, rct);
                    m_game.queueTriggers(std::move(rct));
                }
                // Toxic N: deal N poison counters in addition to normal combat damage
                if (damToPlayer > 0 && attacker->rules->hasToxic && !infect) {
                    m_game.player(attack.defendingPlayerId).addPoison(attacker->rules->toxicAmount);
                }
                // Monarch: if the defending player was the Monarch, transfer the crown.
                if (damToPlayer > 0 &&
                    m_game.monarchPlayer == attack.defendingPlayerId)
                    m_game.monarchPlayer = attacker->controllerId;

                // Commander damage: 21 or more from a single commander = loss.
                if (attacker->isCommander && damToPlayer > 0)
                    m_game.recordCommanderDamage(attack.defendingPlayerId,
                                                 attacker->controllerId, damToPlayer);

                // Prowl: if this creature has Prowl, mark prowlActive so the caster can
                // use the alternate Prowl cost later in the turn (rule 702.75).
                if (damToPlayer > 0 && attacker->rules->hasProwl)
                    m_game.prowlActive[attacker->controllerId] = true;

                // Initiative: initiative holder advances dungeon on combat damage to a player.
                if (damToPlayer > 0 &&
                    m_game.initiativeHolder == static_cast<int8_t>(attacker->controllerId)) {
                    constexpr int kRoomMax = 9;
                    m_game.initiativeRoom = std::min(m_game.initiativeRoom + 1, kRoomMax);
                }

                // DamageDone triggers
                {
                    std::vector<PendingTrigger> ddt;
                    TriggerSystem::onDamageDone(*attacker, power, true, true,
                                                kInvalidId, attack.defendingPlayerId,
                                                m_game, ddt);
                    m_game.queueTriggers(std::move(ddt));
                }
                if (damToPlayer > 0)
                    damageEvents.push_back({attacker, true, attack.defendingPlayerId, kInvalidId, damToPlayer});
            } else if (!effectivelyUnblocked) {
                // Afflict: defending player loses N life whenever this creature becomes blocked.
                // Fire once per attack regardless of first-strike step.
                if (!isFirstStrikeStep && attacker->rules->hasAfflict) {
                    int n = attacker->rules->afflictAmount;
                    m_game.loseLife(attack.defendingPlayerId, n);
                    m_game.playerDamagedThisTurn[attack.defendingPlayerId] = true;
                    std::vector<PendingTrigger> alt;
                    TriggerSystem::onLoseLife(attack.defendingPlayerId, n, m_game, alt);
                    m_game.queueTriggers(std::move(alt));
                }
                int remaining = power;
                for (size_t i = 0; i < attack.blockerIds.size() && remaining > 0; ++i) {
                    Card* blk = m_game.findCard(attack.blockerIds[i]);
                    if (!blk) continue;
                    bool isLast = (i + 1 == attack.blockerIds.size());
                    // Lethal = damage needed to kill this blocker
                    int lethal = dtouch ? 1
                                       : std::max(0, effectiveToughness(*blk) - blk->markedDamage);
                    // With trample: assign only lethal to every blocker (even the last)
                    // so the remainder can overflow to the player.
                    // Without trample: assign all remaining to the last blocker.
                    int assign = (trample || !isLast) ? std::min(remaining, lethal)
                                                      : remaining;
                    applyDamageToCreature(*blk, assign, dtouch, infect, wither, atkColor, m_game, attacker);
                    if (assign > 0) {
                        if (lifelink) m_game.gainLife(attacker->controllerId, assign);
                        std::vector<PendingTrigger> ddt;
                        TriggerSystem::onDamageDone(*attacker, assign, true, false,
                                                    blk->id, 0, m_game, ddt);
                        m_game.queueTriggers(std::move(ddt));
                        damageEvents.push_back({attacker, false, 0, blk->id, assign});
                    }
                    remaining -= assign;
                }
                // Trample: leftover damage hits defending player
                if (trample && remaining > 0) {
                    Player& defender = m_game.player(attack.defendingPlayerId);
                    remaining = applyDamageReplacements(
                        remaining, attacker, nullptr,
                        static_cast<int>(attack.defendingPlayerId), m_game);
                    if (defender.damageShield() > 0) {
                        int prevented = std::min(remaining, defender.damageShield());
                        defender.addDamageShield(-prevented);
                        remaining -= prevented;
                    }
                    if (remaining > 0) {
                        if (infect) {
                            defender.addPoison(remaining);
                        } else {
                            m_game.loseLife(attack.defendingPlayerId, remaining);
                            {
                                std::vector<PendingTrigger> llt;
                                TriggerSystem::onLoseLife(attack.defendingPlayerId, remaining, m_game, llt);
                                m_game.queueTriggers(std::move(llt));
                            }
                        }
                        if (lifelink)
                            m_game.gainLife(attacker->controllerId, remaining);
                        // Commander damage via trample
                        if (attacker->isCommander && !infect)
                            m_game.recordCommanderDamage(attack.defendingPlayerId,
                                                         attacker->controllerId, remaining);
                        std::vector<PendingTrigger> ddt;
                        TriggerSystem::onDamageDone(*attacker, remaining, true, true,
                                                    kInvalidId, attack.defendingPlayerId,
                                                    m_game, ddt);
                        m_game.queueTriggers(std::move(ddt));
                        damageEvents.push_back({attacker, true, attack.defendingPlayerId, kInvalidId, remaining});
                    }
                }
            }
        }

        // Blockers deal damage back to attacker (or band if attacker has Banding)
        for (ObjectId bid : attack.blockerIds) {
            Card* blk = m_game.findCard(bid);
            if (!blk) continue;
            bool blkFS = blk->hasKeyword(KeywordAbility::FirstStrike);
            bool blkDS = blk->hasKeyword(KeywordAbility::DoubleStrike);
            bool blockerDeals = isFirstStrikeStep ? (blkFS || blkDS) : (!blkFS || blkDS);
            if (!blockerDeals) continue;

            int power      = blk->dealsDamageByToughness
                                 ? effectiveToughness(*blk) : effectivePower(*blk);
            bool blkInfect = blk->hasKeyword(KeywordAbility::Infect);
            bool blkWither = blk->hasKeyword(KeywordAbility::Wither);
            bool blkDtouch = blk->hasKeyword(KeywordAbility::Deathtouch);
            uint8_t blkColor = blk->rules->manaCost.colorIdentity();

            // Banding: attacker distributes blocker damage across band members
            // (simplified: split evenly across band, remainder to attacker)
            if (attacker->rules->hasBanding && !attack.bandIds.empty()) {
                int bandSize = 1 + static_cast<int>(attack.bandIds.size());
                int each = power / bandSize;
                int rem  = power % bandSize;
                applyDamageToCreature(*attacker, each + rem, blkDtouch, blkInfect, blkWither, blkColor, m_game, blk);
                for (ObjectId membId : attack.bandIds) {
                    Card* mem = m_game.findCard(membId);
                    if (mem) applyDamageToCreature(*mem, each, blkDtouch, blkInfect, blkWither, blkColor, m_game, blk);
                }
            } else {
                applyDamageToCreature(*attacker, power, blkDtouch, blkInfect, blkWither, blkColor, m_game, blk);
            }
            if (power > 0) {
                if (blk->hasKeyword(KeywordAbility::Lifelink))
                    m_game.gainLife(blk->controllerId, power);
                std::vector<PendingTrigger> ddt;
                TriggerSystem::onDamageDone(*blk, power, true, false,
                                            attacker->id, 0, m_game, ddt);
                m_game.queueTriggers(std::move(ddt));
                damageEvents.push_back({blk, false, 0, attacker->id, power});
            }
        }   // for (ObjectId bid)
    }       // for (auto& attack)

    // Fire DamageDoneOnce triggers once per (watcher, target) after all combat damage
    if (!damageEvents.empty()) {
        std::vector<PendingTrigger> once;
        TriggerSystem::onDamageDoneOnce(damageEvents, m_game, once);
        m_game.queueTriggers(std::move(once));
    }
}

void TurnManager::endCombat() {
    for (Card* c : m_game.battlefield().cards()) {
        c->attacking  = false;
        c->blocking   = false;
        c->isBlocked  = false;
    }
    m_combat.clear();
    m_game.activeCombat = nullptr;
}

// ── Game-over ─────────────────────────────────────────────────────────────────

bool TurnManager::isGameOver() const noexcept {
    uint8_t n = m_game.numPlayers();
    int alive = 0;
    for (uint8_t i = 0; i < n; ++i)
        if (!m_game.player(i).hasLost()) ++alive;
    return alive <= 1;
}

uint8_t TurnManager::winnerId() const noexcept {
    uint8_t n = m_game.numPlayers();
    for (uint8_t i = 0; i < n; ++i)
        if (!m_game.player(i).hasLost()) return i;
    return 0;  // all lost (draw)
}

// ── Private helpers ───────────────────────────────────────────────────────────

void TurnManager::drawCard(uint8_t playerId) {
    Player& p = m_game.player(playerId);

    // Dredge replacement: AI always Dredges if available and library has enough cards
    for (Card* c : p.graveyard().cards()) {
        if (!c->rules->hasDredge || c->rules->dredgeAmount <= 0) continue;
        int n = c->rules->dredgeAmount;
        if (static_cast<int>(p.library().size()) < n) continue;
        // Mill N cards from library
        ObjectId dredgeId = c->id;
        for (int i = 0; i < n; ++i) {
            Card* top = p.library().front();
            if (!top) break;
            m_game.moveToZone(top->id, ZoneType::Graveyard, playerId);
        }
        // Return Dredge card to hand
        if (Card* dc = m_game.findCard(dredgeId))
            m_game.moveToZone(dc->id, ZoneType::Hand, playerId);
        return; // replaced draw
    }

    if (p.library().empty()) {
        p.lose();
        return;
    }
    Card* top = p.library().front();
    bool isFirstDraw = (m_game.cardsDrawnThisTurn[playerId] == 0);
    Card* drawn = m_game.moveToZone(top->id, ZoneType::Hand, playerId);
    ++m_game.cardsDrawnThisTurn[playerId];
    if (isFirstDraw && drawn && drawn->rules->hasMiracle)
        drawn->miracleEligible = true;

    // Fire Drawn triggers (e.g. "whenever you draw a card", "whenever you draw your Nth card")
    {
        std::vector<PendingTrigger> trigs;
        TriggerSystem::onDraw(playerId, m_game.cardsDrawnThisTurn[playerId],
                              drawn ? drawn->id : kInvalidId, m_game, trigs);
        m_game.queueTriggers(std::move(trigs));
    }
}

} // namespace mtg
