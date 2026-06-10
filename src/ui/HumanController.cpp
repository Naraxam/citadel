#include "HumanController.h"
#include "../game/CardFilter.h"
#include "../game/CardStats.h"
#include "../game/TurnStep.h"
#include "../game/EquipSystem.h"
#include "../game/TriggerSystem.h"
#include "../game/ability/ScriptLine.h"
#include <algorithm>

namespace ui {

using namespace mtg;

// Extract just the mana portion of an activated-ability Cost$ string (dropping
// {T}, Sac<…>, PayLife<…>, Discard<…>, etc.) so we can check affordability and
// show the player what mana an ability needs.
static mtg::ManaCost abilityManaCost(std::string_view costStr) {
    std::string mana;
    size_t i = 0;
    while (i < costStr.size()) {
        size_t sp = costStr.find(' ', i);
        std::string_view tok = costStr.substr(i, (sp == std::string_view::npos
                                                  ? costStr.size() : sp) - i);
        i = (sp == std::string_view::npos) ? costStr.size() : sp + 1;
        bool isMana = !tok.empty();
        for (char ch : tok) {
            bool ok = (ch >= '0' && ch <= '9') || ch == 'X' || ch == '/' ||
                      ch == 'W' || ch == 'U' || ch == 'B' || ch == 'R' ||
                      ch == 'G' || ch == 'C' || ch == 'S' || ch == 'P';
            if (!ok) { isMana = false; break; }
        }
        if (isMana) { if (!mana.empty()) mana += ' '; mana += std::string(tok); }
    }
    return mtg::ManaCost::parse(mana);
}

HumanController::HumanController(GameState& game, TurnManager& tm,
                                   AbilityProcessor& abilities, AiPlayer& bob)
    : m_game(game), m_tm(tm), m_abilities(abilities), m_bob(bob)
{}

void HumanController::runSBAs() {
    while (StateBasedActions::run(m_game)) {}
}

void HumanController::autoAdvanceToMain() {
    // ── Untap — automatic, no triggers ───────────────────────────────────────
    m_tm.beginStep();
    m_tm.endStep();
    if (m_tm.isGameOver()) { m_state = HumanState::GameOver; return; }
    m_tm.advanceStep();

    // ── Upkeep ────────────────────────────────────────────────────────────────
    m_tm.beginStep();
    m_abilities.processSuspendUpkeep(m_game.activePlayerId());
    m_abilities.processReboundUpkeep(m_game.activePlayerId());
    m_abilities.processEchoUpkeep(m_game.activePlayerId());
    m_abilities.processCumulativeUpkeep(m_game.activePlayerId());
    m_abilities.firePhaseTriggersAndDrain(TurnStep::Upkeep, m_game.activePlayerId());
    if (m_tm.isGameOver()) { m_state = HumanState::GameOver; return; }
    m_tm.endStep();
    if (m_tm.isGameOver()) { m_state = HumanState::GameOver; return; }
    m_tm.advanceStep();

    // Check for Forecast cards in hand: if present, pause at upkeep so the
    // human can optionally activate Forecast: {cost}, Reveal this card.
    {
        bool hasForecast = false;
        for (const auto* c : m_game.player(0).hand().cards()) {
            if (c->rules->hasForecast) { hasForecast = true; break; }
        }
        if (hasForecast && !m_stops.upkeep) {
            // Auto-pause so the human sees the Forecast opportunity
            m_stepPauseCont = StepPauseCont::AfterUpkeep;
            m_instantOnly   = true;
            m_state         = HumanState::MainPhase;
            return;
        }
    }

    if (m_stops.upkeep) {
        // Pause here — player can cast instants; End Phase resumes to Draw
        m_stepPauseCont = StepPauseCont::AfterUpkeep;
        m_instantOnly   = true;
        m_state         = HumanState::MainPhase;
        return;
    }

    // ── Draw ─────────────────────────────────────────────────────────────────
    m_tm.beginStep();
    m_abilities.firePhaseTriggersAndDrain(TurnStep::Draw, m_game.activePlayerId());
    if (m_tm.isGameOver()) { m_state = HumanState::GameOver; return; }
    m_tm.endStep();
    if (m_tm.isGameOver()) { m_state = HumanState::GameOver; return; }
    m_tm.advanceStep();

    // ── Begin Pre-Combat Main ─────────────────────────────────────────────────
    m_inPostCombatMain = false;
    m_tm.beginStep();
    m_abilities.firePhaseTriggersAndDrain(TurnStep::PreCombatMain, m_game.activePlayerId());
    m_state = HumanState::MainPhase;
}

// ── Public API ────────────────────────────────────────────────────────────────

void HumanController::resetToMainPhase() noexcept {
    m_state              = HumanState::MainPhase;
    m_savedState         = HumanState::MainPhase;
    m_instantOnly        = false;
    m_stepPauseCont      = StepPauseCont::None;
    m_inPostCombatMain   = false;
    m_pendingSpell       = kInvalidId;
    m_pendingEquip       = kInvalidId;
    m_pendingAbilityCard = kInvalidId;
    m_pendingBlocker     = kInvalidId;
    m_pendingNinja       = kInvalidId;
    m_pendingPW          = kInvalidId;
    m_attackers.clear();
    m_orderingAttackers.clear();
    m_orderedBlockers.clear();
    m_remainingBlockers.clear();
    m_cleanupPending     = false;
}

void HumanController::castMiracleCard(mtg::ObjectId cardId) {
    mtg::Card* c = m_game.findCard(cardId);
    if (!c || !c->rules->hasMiracle) return;
    m_pendingSpell = cardId;
    m_state = HumanState::TargetSelect;
}

void HumanController::startHumanTurn() {
    m_attackers.clear();
    m_pendingSpell      = kInvalidId;
    m_pendingBlocker    = kInvalidId;
    m_orderingAttackers.clear();
    m_orderingIdx       = 0;
    m_orderedBlockers.clear();
    m_remainingBlockers.clear();
    autoAdvanceToMain();
}

void HumanController::startBlockPhase() {
    m_pendingBlocker = kInvalidId;
    m_state = HumanState::DeclareBlock;
}

bool HumanController::checkAndHandleTriggers() {
    if (!m_abilities.hasPendingHumanTrigger()) return false;
    m_state = HumanState::TriggerTarget;
    return true;
}

void HumanController::beginResponsePhase() {
    m_instantOnly = true;
    m_state = HumanState::MainPhase;
}

void HumanController::chooseManaAbility(int abilityIndex) {
    if (m_manaAbilitySource == kInvalidId) return;
    ObjectId src = m_manaAbilitySource;
    m_abilities.activateManaAbility(src, 0, abilityIndex);
    runSBAs();
    checkAndHandleTriggers();
    m_manaAbilitySource = kInvalidId;
    m_manaAbilityOptions.clear();
    // Return to the saved state — TargetSelect when the picker was opened
    // mid-cast (so the cast popup stays up after picking a colour), else
    // MainPhase as normal.
    if (m_savedState != HumanState::Idle) {
        m_state      = m_savedState;
        m_savedState = HumanState::Idle;
    } else {
        m_state = HumanState::MainPhase;
    }
}

void HumanController::cancelManaAbility() noexcept {
    m_manaAbilitySource = kInvalidId;
    m_manaAbilityOptions.clear();
    if (m_savedState != HumanState::Idle) {
        m_state      = m_savedState;
        m_savedState = HumanState::Idle;
    } else {
        m_state = HumanState::MainPhase;
    }
}

bool HumanController::cancelPendingSpell() {
    if (m_pendingSpell == mtg::kInvalidId) return false;
    m_pendingSpell       = mtg::kInvalidId;
    m_pendingEquip       = mtg::kInvalidId;
    m_pendingAbilityCard = mtg::kInvalidId;
    // Re-enter the right MainPhase variant depending on whether we're at
    // instant speed (post-block / opponent response window) or sorcery speed.
    m_state = HumanState::MainPhase;
    return true;
}

std::vector<HumanController::CastOption>
HumanController::availableModes() const {
    std::vector<CastOption> out;
    if (m_pendingSpell == kInvalidId) return out;
    const Card* c = m_game.findCard(m_pendingSpell);
    if (!c || !c->rules) return out;
    const ManaPool& pool = m_game.player(0).manaPool();

    // Cast (always offered; cost depends on commander-zone, alternate-cost
    // selection happens inside castSpell — we surface the printed manaCost).
    // Apply any ReduceCost/RaiseCost statics so the cost shown and the
    // affordability check match what castSpell will actually charge.
    ManaCost castManaCost = c->rules->manaCost.reduceGeneric(
        m_abilities.genericReductionFor(*c, 0));
    std::string castCost = castManaCost.toString();
    if (castCost.empty()) castCost = "{0}";
    out.push_back({CastModeKind::Cast, "Cast", castCost,
                   pool.canPay(castManaCost) || castManaCost.isNoCost()});

    // Foretell (hand only, sorcery speed, not already foretold).
    if (c->rules->hasForetell && c->zone == ZoneType::Hand && !m_instantOnly) {
        out.push_back({CastModeKind::Foretell, "Foretell", "{2}",
                       pool.total() >= 2});
    }
    return out;
}

void HumanController::chooseMode(CastModeKind kind) {
    if (m_pendingSpell == kInvalidId) return;
    ObjectId id = m_pendingSpell;

    switch (kind) {
    case CastModeKind::Cast: {
        // If the spell has no SP$ line that requires a target, commit
        // immediately. Otherwise enter TargetSelect so the player can click a
        // target on the battlefield/stack/graveyard.
        const Card* c = m_game.findCard(id);
        bool needsTarget = false;
        if (c && c->rules) {
            // Auras must choose what to enchant on cast (they attach to the
            // chosen target when they enter).
            if (c->rules->type.hasSubtype("Aura")) needsTarget = true;
            for (const auto& raw : c->rules->abilityLines) {
                auto s = parseScriptLine(raw);
                if (s.abilityType != "SP") continue;
                if (!s.get("ValidTgts", "").empty()) { needsTarget = true; break; }
            }
        }
        if (!needsTarget) {
            if (m_abilities.castSpell(id, 0, {})) {
                bool inCombat = (m_savedState == HumanState::DeclareAttack ||
                                 m_savedState == HumanState::DeclareBlock);
                if (inCombat) m_abilities.resolveTop();
                runSBAs();
                m_pendingSpell = kInvalidId;
                if (!checkAndHandleTriggers()) {
                    m_state      = (m_savedState != HumanState::Idle)
                                   ? m_savedState : HumanState::MainPhase;
                    m_savedState = HumanState::Idle;
                }
                return;
            }
            // castSpell failed — record the most-likely reason so GameWindow
            // can log a meaningful message rather than silent rejection. Leave
            // the spell pending so the player can tap more lands; show the
            // target-select state to keep the popup visible.
            if (c) {
                const ManaPool& pool = m_game.player(0).manaPool();
                std::string nm = c->rules->name;
                std::string need = c->rules->manaCost.toString();
                if (need.empty()) need = "{0}";
                std::string have = pool.toString();
                if (have.empty()) have = "(empty)";
                if (!pool.canPay(c->rules->manaCost) &&
                    !c->rules->manaCost.isNoCost()) {
                    m_castError = "Cannot cast " + nm + ": need " + need +
                                  ", pool has " + have +
                                  ". Tap lands to add mana.";
                } else {
                    m_castError = "Cannot cast " + nm +
                                  " right now (timing, restriction, or no legal target).";
                }
            }
        }
        m_state = HumanState::TargetSelect;
        return;
    }

    case CastModeKind::Foretell:
        if (m_abilities.activateForetell(id, 0)) {
            runSBAs();
            checkAndHandleTriggers();
        }
        m_pendingSpell = kInvalidId;
        m_state        = (m_savedState != HumanState::Idle)
                         ? m_savedState : HumanState::MainPhase;
        m_savedState   = HumanState::Idle;
        return;
    }
}

void HumanController::endResponsePhase() {
    m_instantOnly = false;
    m_pendingSpell       = kInvalidId;
    m_pendingEquip       = kInvalidId;
    m_pendingAbilityCard = kInvalidId;
    m_state = HumanState::Idle;
}

// ── Input handling ────────────────────────────────────────────────────────────

bool HumanController::onCardClick(ObjectId id, ZoneType zone, uint8_t player) {
    switch (m_state) {

    case HumanState::MainPhase: {
        // Command zone click — your commander. Open the cast popup so the
        // player can cast it (paying the commander tax) the same way they
        // would a hand card. Sorcery-speed only.
        if (player == 0 && zone == ZoneType::Command) {
            if (m_instantOnly) return false;
            Card* c = m_game.findCard(id);
            if (!c || !c->isCommander || c->controllerId != 0) return false;
            m_pendingSpell = id;
            m_state        = HumanState::TargetSelect;
            return true;
        }

        // Graveyard click in main phase — accept any cast-from-graveyard
        // mechanic the engine knows how to resolve, plus the GY-activated
        // abilities (Embalm / Eternalize / Scavenge). castSpell auto-selects
        // the right alt-cost for the cast path; the activated ones get their
        // dedicated activate* entry points.
        if (player == 0 && zone == ZoneType::Graveyard) {
            Card* c = m_game.findCard(id);
            if (!c || !c->rules) return false;
            const auto& r = *c->rules;

            // Embalm: pay embalmCost → exile this and put a token copy onto
            // the battlefield. Sorcery speed.
            if (r.hasEmbalm && !m_instantOnly) {
                if (m_abilities.activateEmbalm(id, 0)) {
                    runSBAs();
                    checkAndHandleTriggers();
                }
                return true;
            }
            // Eternalize: same shape as Embalm but the token is a 4/4 zombie.
            if (r.hasEternalize && !m_instantOnly) {
                if (m_abilities.activateEternalize(id, 0)) {
                    runSBAs();
                    checkAndHandleTriggers();
                }
                return true;
            }
            // Scavenge: pay scavengeCost, exile this, put N +1/+1 counters
            // on a target creature. Needs a target on the battlefield.
            if (r.hasScavenge && !m_instantOnly) {
                m_pendingAbilityCard = id;
                m_pendingAbilityIdx  = -1;        // sentinel: not an AB$ line
                m_state              = HumanState::AbilityTarget;
                return true;
            }

            bool canCastFromGY =
                r.hasFlashback || r.hasRetrace   || r.hasUnearth  ||
                r.hasEscape    || r.hasJumpStart || r.hasDisturb  ||
                r.hasAftermath;
            if (!canCastFromGY) return false;
            // Instant-speed gate: only Flashback / instant cards can fire
            // at instant speed. Sorcery-speed mechanics are sorcery only.
            if (m_instantOnly) {
                bool instantOK =
                    r.type.isInstant() ||
                    c->hasKeyword(KeywordAbility::Flash) ||
                    m_game.hasFlashGrant(*c) ||
                    r.hasFlashback;
                if (!instantOK) return false;
            }
            m_pendingSpell = id;
            m_state        = HumanState::TargetSelect;
            return true;
        }

        // Exile click — for now we only support casting a Foretold card on a
        // later turn than the one it was foretold. The cast popup opens just
        // like a hand card; castSpell handles the foretell branch internally.
        if (player == 0 && zone == ZoneType::Exile) {
            Card* c = m_game.findCard(id);
            if (!c || !c->rules) return false;
            if (c->foretold && c->rules->hasForetell &&
                m_game.turnNumber() > c->foretoldOnTurn) {
                if (m_instantOnly && !c->rules->type.isInstant() &&
                    !c->hasKeyword(KeywordAbility::Flash))
                    return false;
                m_pendingSpell = id;
                m_state        = HumanState::TargetSelect;
                return true;
            }
            return false;
        }

        if (player == 0 && zone == ZoneType::Hand) {
            Card* c = m_game.findCard(id);
            if (!c) return false;

            // Suspend: exile with time counters instead of casting (sorcery speed).
            // For Suspend X, default X=2 (no number-entry UI yet).
            if (c->rules->hasSuspend && !m_instantOnly) {
                int xVal = c->rules->suspendCountIsX ? 2 : 0;
                if (m_abilities.activateSuspend(id, 0, xVal)) return true;
            }

            if (c->rules->type.isLand()) {
                if (m_instantOnly) return false;
                if (m_game.canPlayLand(0)) {   // respects extra land plays (Exploration, etc.)
                    Card* landed = m_game.moveToZone(id, ZoneType::Battlefield, 0);
                    m_game.player(0).incLandsPlayed();
                    if (landed) {
                        std::vector<PendingTrigger> t;
                        TriggerSystem::onLandPlayed(*landed, 0, m_game, t);
                        m_game.queueTriggers(std::move(t));
                        m_abilities.processHideaway(*landed);  // Mosswort Bridge, etc.
                    }
                }
                return true;
            }

            if (m_instantOnly &&
                !c->rules->type.isInstant() &&
                !c->hasKeyword(KeywordAbility::Flash) &&
                !m_game.hasFlashGrant(*c))
                return false;

            // Forecast: activate during upkeep pause (m_instantOnly=true, at upkeep step)
            if (c->rules->hasForecast && m_instantOnly) {
                if (m_abilities.activateForecast(id, 0)) {
                    runSBAs();
                    checkAndHandleTriggers();
                }
                return true;
            }

            // Check for ActivationZone$ Hand abilities (cycling, etc.) — activate before casting
            for (int i = 0; i < static_cast<int>(c->rules->abilityLines.size()); ++i) {
                auto s = mtg::parseScriptLine(c->rules->abilityLines[i]);
                if (s.abilityType != "AB" || s.get("ActivationZone", "") != "Hand") continue;
                auto validTgts = s.get("ValidTgts", "");
                if (!validTgts.empty()) {
                    m_pendingAbilityCard = id;
                    m_pendingAbilityIdx  = i;
                    m_state = HumanState::AbilityTarget;
                } else {
                    // No target — activate (cycling goes on stack; [Pass] resolves it)
                    if (m_abilities.activateAbility(id, i, 0, {})) {
                        runSBAs();
                        checkAndHandleTriggers();
                    }
                }
                return true;
            }

            // Count Phyrexian shards — show a payment-choice overlay before targeting
            {
                int phyrCount = 0;
                for (const auto& shard : c->rules->manaCost.shards())
                    if (shard.isPhyrexian()) ++phyrCount;
                if (phyrCount > 0 && !m_game.hasPendingPhyrexian()) {
                    m_game.setPendingPhyrexian(phyrCount);
                    m_pendingSpell = id;
                    return true;  // GameWindow shows the choice; on confirm we'll advance state
                }
            }

            m_pendingSpell = id;
            m_state        = HumanState::TargetSelect;
            return true;
        }

        if (player == 0 && zone == ZoneType::Battlefield) {
            Card* c = m_game.findCard(id);
            if (!c) return false;

            // Cancel any pending PW overlay if clicking a different card
            if (m_pendingPW != kInvalidId && id != m_pendingPW)
                cancelPWChoice();

            // Planeswalker: collect loyalty abilities and request choice overlay
            if (c->rules->type.isPlaneswalker()) {
                m_pwAbilIdxs.clear();
                m_pwAbilLabels.clear();
                for (int i = 0; i < static_cast<int>(c->rules->abilityLines.size()); ++i) {
                    auto s = mtg::parseScriptLine(c->rules->abilityLines[i]);
                    if (s.abilityType != "AB" ||
                        s.effectType == "Mana" || s.effectType == "ManaReflected") continue;
                    m_pwAbilIdxs.push_back(i);
                    m_pwAbilLabels.push_back(loyaltyCostLabel(std::string(s.get("Cost", ""))));
                }
                if (!m_pwAbilIdxs.empty()) {
                    m_pendingPW = id;
                    return true; // GameWindow will display the overlay
                }
            }

            // 1a. Morph / Megamorph: click a face-down creature to turn it face up
            if (!m_instantOnly && c->isFaceDown &&
                (c->rules->hasMorph || c->rules->hasMegamorph)) {
                if (m_abilities.activateMorph(id, 0)) {
                    runSBAs();
                    checkAndHandleTriggers();
                }
                return true;
            }

            // 1b. Check for Level Up keyword — sorcery-speed, pay cost → add LEVEL counter
            if (!m_instantOnly && c->rules->hasLevelUp) {
                if (m_abilities.activateLevelUp(id, 0)) {
                    runSBAs();
                    checkAndHandleTriggers();
                }
                return true;
            }

            // 2. Check for Equip keyword — enter EquipSelect
            for (const auto& kw : c->rules->keywords) {
                if (parseEquipCost(kw) >= 0) {
                    m_pendingEquip = id;
                    m_state = HumanState::EquipSelect;
                    return true;
                }
            }

            // 3. Non-mana activated abilities. Collect every line so that a
            //    card with 2+ activated abilities opens a picker (the same
            //    overlay the planeswalker option box uses) instead of always
            //    auto-firing only the first ability.
            {
                std::vector<int>         abilIdxs;
                std::vector<std::string> abilLabels;
                for (int i = 0; i < static_cast<int>(c->rules->abilityLines.size()); ++i) {
                    auto s = mtg::parseScriptLine(c->rules->abilityLines[i]);
                    if (s.abilityType != "AB" ||
                        s.effectType == "Mana" || s.effectType == "ManaReflected") continue;

                    // Build a short label: prefer the spell description,
                    // else combine effect type + cost so the player can tell
                    // similar abilities apart.
                    std::string label = std::string(s.get("SpellDescription", ""));
                    if (label.empty()) {
                        std::string cost = std::string(s.get("Cost", ""));
                        std::string eff  = std::string(s.effectType);
                        if (!cost.empty()) label = cost + ": " + eff;
                        else               label = eff.empty() ? std::string("Activate") : eff;
                    }
                    if (label.size() > 60) label = label.substr(0, 57) + "...";
                    abilIdxs.push_back(i);
                    abilLabels.push_back(std::move(label));
                }

                if (abilIdxs.size() >= 2) {
                    // Multiple abilities — reuse the existing PW overlay
                    // mechanism (m_pendingPW / m_pwAbilIdxs / m_pwAbilLabels)
                    // since GameWindow already renders + dispatches it; this
                    // way the overlay works for any card.
                    m_pendingPW    = id;
                    m_pwAbilIdxs   = std::move(abilIdxs);
                    m_pwAbilLabels = std::move(abilLabels);
                    return true;
                }

                if (abilIdxs.size() == 1) {
                    int i = abilIdxs[0];
                    auto s = mtg::parseScriptLine(c->rules->abilityLines[i]);

                    // Activation-cost indicator: parse the mana portion of the
                    // ability's Cost and, if the player can't pay it right now,
                    // tell them the cost instead of silently doing nothing (the
                    // "why won't my ability fire?" / "is it summoning sick?"
                    // confusion). They can tap lands and click again.
                    mtg::ManaCost mc = abilityManaCost(s.get("Cost", ""));
                    const ManaPool& pool = m_game.player(0).manaPool();
                    if (!mc.isNoCost() && !pool.canPay(mc)) {
                        std::string have = pool.toString();
                        if (have.empty()) have = "(empty)";
                        m_castError = "Activate " + c->rules->name + ": need " +
                                      mc.toString() + " — pool has " + have +
                                      ". Tap lands for mana, then click it again.";
                        return true;
                    }

                    auto validTgts = s.get("ValidTgts", "");
                    if (!validTgts.empty()) {
                        m_pendingAbilityCard = id;
                        m_pendingAbilityIdx  = i;
                        m_state = HumanState::AbilityTarget;
                    } else if (!m_abilities.activateAbility(id, i, 0, {})) {
                        m_castError = "Couldn't activate " + c->rules->name +
                                      " right now (timing, restriction, or already used).";
                    } else {
                        runSBAs();
                        checkAndHandleTriggers();
                    }
                    return true;
                }
                // 0 non-mana abilities → fall through to mana-ability path.
            }

            // 4. Fall back to mana ability. Count mana lines on this card:
            //    1 → activate directly UNLESS the cost destroys the source
            //    (sacrifice / discard self) — those get the picker as a
            //    confirmation step.
            //    2+ → always open the picker so the player chooses
            //    (Shivan Reef, Temple of Malady, City of Brass, etc.).
            std::vector<ManaAbilityOption> opts;
            bool hasDestructiveCost = false;
            int manaIdx = 0;
            std::vector<const std::string*> manaLines;
            for (const auto& l : c->rules->abilityLines) manaLines.push_back(&l);
            for (const auto& l : c->grantedAbilities)    manaLines.push_back(&l);
            for (const auto* lp : manaLines) {
                const auto& raw = *lp;
                auto s = parseScriptLine(raw);
                if (s.abilityType != "AB" ||
                    (s.effectType != "Mana" && s.effectType != "ManaReflected")) continue;
                // Reflected mana (Exotic Orchard): the colour is chosen after the
                // ability resolves (a pending-mana-choice overlay), so just label it.
                if (s.effectType == "ManaReflected") {
                    std::string lbl = std::string(s.get("SpellDescription", "Add any color"));
                    if (lbl.size() > 60) lbl = lbl.substr(0, 57) + "...";
                    opts.push_back({manaIdx, std::move(lbl)});
                    ++manaIdx;
                    continue;
                }
                std::string produced = std::string(s.get("Produced", "C"));
                if (produced == "Any" || produced == "AnyColor") {
                    opts.push_back({manaIdx, "Add any color"});
                    ++manaIdx;
                    continue;
                }
                // Prefer the line's authored SpellDescription — it already has
                // the {C}/{W}/{U} mana tokens (rendered as pips by the picker)
                // AND any rider text like "CARDNAME deals 1 damage to you"
                // (Adarkar Wastes and the other pain lands). Fall back to a label
                // reconstructed from Produced$ when there's no description.
                std::string label;
                std::string desc = std::string(s.get("SpellDescription", ""));
                if (!desc.empty()) {
                    label = desc;
                    for (size_t pos; (pos = label.find("CARDNAME")) != std::string::npos; )
                        label.replace(pos, 8, c->rules->name);
                } else {
                    label = "Add ";
                    std::string colorsForLabel;
                    if (produced.size() > 6 && produced.substr(0, 6) == "Combo ") {
                        std::string tail = produced.substr(6);
                        if (tail.find("ColorIdentity") != std::string::npos) {
                            uint8_t mask = 0;
                            for (const Card* cmd : m_game.command().cards())
                                if (cmd && cmd->isCommander && cmd->ownerId == 0)
                                    mask |= cmd->rules->manaCost.colorIdentity();
                            for (const Card* bf : m_game.battlefield().cards())
                                if (bf && bf->isCommander && bf->ownerId == 0)
                                    mask |= bf->rules->manaCost.colorIdentity();
                            if (mask == 0) mask = ManaAtom::WHITE | ManaAtom::BLUE |
                                                  ManaAtom::BLACK | ManaAtom::RED |
                                                  ManaAtom::GREEN;
                            if (mask & ManaAtom::WHITE) colorsForLabel += 'W';
                            if (mask & ManaAtom::BLUE)  colorsForLabel += 'U';
                            if (mask & ManaAtom::BLACK) colorsForLabel += 'B';
                            if (mask & ManaAtom::RED)   colorsForLabel += 'R';
                            if (mask & ManaAtom::GREEN) colorsForLabel += 'G';
                        } else {
                            for (char ch : tail)
                                if (ch == 'W' || ch == 'U' || ch == 'B' ||
                                    ch == 'R' || ch == 'G' || ch == 'C')
                                    colorsForLabel += ch;
                        }
                        bool first = true;
                        for (char ch : colorsForLabel) {
                            if (!first) label += " or ";
                            label += "{"; label += ch; label += "}";
                            first = false;
                        }
                    } else {
                        for (char ch : produced)
                            if (ch != ' ') { label += "{"; label += ch; label += "}"; }
                    }
                }
                std::string costStr = std::string(s.get("Cost", ""));
                if (costStr.find("PayLife<1>") != std::string::npos)
                    label += "  (1 life)";
                else if (costStr.find("PayLife<2>") != std::string::npos)
                    label += "  (2 life)";

                // Destructive cost annotation — make sure the player sees
                // they're sacrificing the source / a permanent / a card from
                // hand before the ability fires. Set hasDestructive so we
                // force the picker open even for single-line cards.
                bool destructive = false;
                if (costStr.find("Sac<Self>")        != std::string::npos ||
                    costStr.find("SacrificeSource") != std::string::npos) {
                    label = "Sacrifice " + c->rules->name + " — " + label;
                    destructive = true;
                } else if (costStr.find("Sac<") != std::string::npos) {
                    auto pos = costStr.find("Sac<");
                    auto end = costStr.find('>', pos);
                    std::string inner = (end == std::string::npos) ? "" :
                        costStr.substr(pos + 4, end - pos - 4);
                    if (inner.find("CARDNAME") != std::string::npos) {
                        label = "Sacrifice " + c->rules->name + " — " + label;
                    } else {
                        label = "Sacrifice (" + inner + ") — " + label;
                    }
                    destructive = true;
                }
                if (costStr.find("Discard<") != std::string::npos)
                    destructive = true;

                opts.push_back({manaIdx, std::move(label)});
                if (destructive) hasDestructiveCost = true;
                ++manaIdx;
            }
            // Dual-typed lands (Plains+Island, shock/gain/check lands, etc.) make
            // mana from basic land subtypes, not A:Mana lines — enumerate those
            // colours so the player can choose. Index matches activateManaAbility.
            if (opts.empty() && c->rules->type.isLand()) {
                int ci = 0;
                auto addCol = [&](const char* sub, char sym) {
                    if (c->rules->type.hasSubtype(sub)) {
                        std::string lbl = "Add {"; lbl += sym; lbl += "}";
                        opts.push_back({ci++, lbl});
                    }
                };
                addCol("Plains",'W'); addCol("Island",'U'); addCol("Swamp",'B');
                addCol("Mountain",'R'); addCol("Forest",'G');
            }
            // Show the picker when there's a real choice OR whenever any
            // option pays a destructive cost — gives the player a guaranteed
            // chance to back out before losing a permanent (Blood Pet,
            // Lotus Petal, Skirk Prospector, etc.).
            if (opts.size() >= 2 || (opts.size() == 1 && hasDestructiveCost)) {
                m_manaAbilitySource  = id;
                m_manaAbilityOptions = std::move(opts);
                m_state              = HumanState::ManaAbilityChoice;
                return true;
            }
            m_abilities.activateManaAbility(id, 0);
            checkAndHandleTriggers();
            return true;
        }
        return false;
    }

    case HumanState::ManaAbilityChoice: {
        // While the picker is open: clicking the same source cancels; any
        // other click is ignored (handler in GameWindow consumes the button
        // hits directly via chooseManaAbility / cancelManaAbility).
        if (player == 0 && zone == ZoneType::Battlefield && id == m_manaAbilitySource)
            cancelManaAbility();
        return true;
    }

    case HumanState::TargetSelect: {
        // Tap-for-mana while the cast popup is up: clicking an own,
        // untapped, mana-producing permanent doesn't try to target it with
        // the pending spell — it activates its mana ability so the player
        // can pay the cost without dismissing the popup first.
        if (player == 0 && zone == ZoneType::Battlefield) {
            Card* clicked = m_game.findCard(id);
            if (clicked && !clicked->tapped && id != m_pendingSpell) {
                std::vector<ManaAbilityOption> opts;
                bool hasDestructiveCost = false;
                int manaIdx = 0;
                std::vector<const std::string*> manaLines;
                for (const auto& l : clicked->rules->abilityLines) manaLines.push_back(&l);
                for (const auto& l : clicked->grantedAbilities)    manaLines.push_back(&l);
                for (const auto* lp : manaLines) {
                    const auto& raw = *lp;
                    auto s = parseScriptLine(raw);
                    if (s.abilityType != "AB" ||
                        (s.effectType != "Mana" && s.effectType != "ManaReflected")) continue;
                    std::string costStr = std::string(s.get("Cost", ""));
                    std::string lbl;
                    if (s.effectType == "ManaReflected")
                        lbl = std::string(s.get("SpellDescription", "Add any color"));
                    if (std::string(s.get("Produced","")) == "Any" ||
                        std::string(s.get("Produced","")) == "AnyColor")
                        lbl = "Add any color";
                    if (costStr.find("Sac<Self>")        != std::string::npos ||
                        costStr.find("SacrificeSource") != std::string::npos) {
                        lbl = "Sacrifice " + clicked->rules->name;
                        hasDestructiveCost = true;
                    } else if (costStr.find("Sac<") != std::string::npos) {
                        auto pos = costStr.find("Sac<");
                        auto end = costStr.find('>', pos);
                        std::string inner = (end == std::string::npos) ? "" :
                            costStr.substr(pos + 4, end - pos - 4);
                        if (inner.find("CARDNAME") != std::string::npos) {
                            lbl = "Sacrifice " + clicked->rules->name;
                            hasDestructiveCost = true;
                        }
                    }
                    if (costStr.find("Discard<") != std::string::npos)
                        hasDestructiveCost = true;
                    // Use the authored description (pips + rider text like
                    // pain-land damage) when we don't have a more specific label.
                    if (lbl.empty()) {
                        std::string desc = std::string(s.get("SpellDescription", ""));
                        if (!desc.empty()) {
                            lbl = desc;
                            for (size_t p; (p = lbl.find("CARDNAME")) != std::string::npos; )
                                lbl.replace(p, 8, clicked->rules->name);
                        } else {
                            lbl = "Tap for mana";
                        }
                    }
                    opts.push_back({manaIdx, std::move(lbl)});
                    ++manaIdx;
                }
                // Subtype-based duals (Plains+Island shocks/duals) — enumerate
                // basic-land-type colours so the picker offers each one.
                if (opts.empty() && clicked->rules->type.isLand()) {
                    int ci = 0;
                    auto addCol = [&](const char* sub, char sym) {
                        if (clicked->rules->type.hasSubtype(sub)) {
                            std::string lbl = "Add {"; lbl += sym; lbl += "}";
                            opts.push_back({ci++, lbl});
                        }
                    };
                    addCol("Plains",'W'); addCol("Island",'U'); addCol("Swamp",'B');
                    addCol("Mountain",'R'); addCol("Forest",'G');
                }
                bool isBasic = (opts.size() == 1) && clicked->rules->type.isLand();
                if (opts.size() >= 2 || (opts.size() == 1 && hasDestructiveCost)) {
                    // Multi-line OR destructive-cost source — open the picker
                    // so the player can back out before losing the permanent
                    // (Blood Pet, Lotus Petal, Skirk Prospector, etc.). Stash
                    // the pre-cast state so the picker returns to it.
                    m_savedState         = m_state;
                    m_manaAbilitySource  = id;
                    m_manaAbilityOptions = std::move(opts);
                    m_state              = HumanState::ManaAbilityChoice;
                    return true;
                }
                if (opts.size() == 1 || isBasic) {
                    m_abilities.activateManaAbility(id, 0, isBasic ? -1 : 0);
                    checkAndHandleTriggers();
                    return true;  // popup stays up; pool now has the new mana
                }
                // Falls through to target-click logic for non-mana sources.
            }
        }

        if (zone == ZoneType::Battlefield || zone == ZoneType::Stack ||
            zone == ZoneType::Graveyard) {
            std::vector<Target> tgts = { Target::forCard(id) };
            if (m_abilities.castSpell(m_pendingSpell, 0, tgts)) {
                // In combat states (attack/block declaration) resolve immediately.
                // In normal main phase, leave on stack — [Pass] resolves it.
                bool inCombat = (m_savedState == HumanState::DeclareAttack ||
                                 m_savedState == HumanState::DeclareBlock);
                if (inCombat) m_abilities.resolveTop();
                runSBAs();
            }
            m_pendingSpell = kInvalidId;
            if (!checkAndHandleTriggers()) {
                m_state      = (m_savedState != HumanState::Idle)
                               ? m_savedState : HumanState::MainPhase;
                m_savedState = HumanState::Idle;
            }
            return true;
        }
        if (player == 0 && zone == ZoneType::Hand) {
            m_pendingSpell = kInvalidId;
            m_state      = (m_savedState != HumanState::Idle)
                           ? m_savedState : HumanState::MainPhase;
            m_savedState = HumanState::Idle;
            return true;
        }
        return false;
    }

    case HumanState::TriggerTarget: {
        if (zone == ZoneType::Battlefield || zone == ZoneType::Stack ||
            zone == ZoneType::Graveyard) {
            // Validate that the clicked card is a legal target for the pending trigger
            if (m_abilities.hasPendingHumanTrigger()) {
                const auto& trig = m_abilities.topHumanTrigger();
                auto validTgts = std::string(trig.effect.get("ValidTgts", ""));
                if (!validTgts.empty()) {
                    Card* clicked = m_game.findCard(id);
                    if (!clicked) return false;
                    if (!cardMatchesAnyFilter(*clicked, validTgts, trig.controllerId))
                        return false;
                    if (clicked->hasKeyword(KeywordAbility::Shroud)) return false;
                    if (clicked->hasKeyword(KeywordAbility::Hexproof) &&
                        clicked->controllerId == trig.controllerId) return false;
                }
            }
            m_abilities.resolveHumanTrigger({ Target::forCard(id) });
            runSBAs();
            if (!checkAndHandleTriggers())
                m_state = HumanState::MainPhase;
        }
        return true;
    }

    case HumanState::DiscardChoice: {
        // Discard a card from hand until at or below max hand size
        if (player == 0 && zone == ZoneType::Hand) {
            m_game.moveToZone(id, ZoneType::Graveyard, 0);
            m_abilities.drainPendingTriggers(); // handles Madness, etc.
            // If a pending overlay was deferred (e.g. Madness), wait for it to resolve.
            if (m_game.hasPendingMadnessCast()) {
                m_cleanupPending = true; // GameWindow will call checkResumeCleanup() after
                return true;
            }
            // If now at or below max hand size, finish the turn
            if (static_cast<int>(m_game.player(0).hand().size()) <=
                m_game.player(0).maxHandSize()) {
                doCleanupAndIdle();
            }
        }
        return true;
    }

    case HumanState::EquipSelect: {
        // Click an own creature to attach the selected equipment
        if (player == 0 && zone == ZoneType::Battlefield && id != m_pendingEquip) {
            m_abilities.activateEquip(m_pendingEquip, id, 0);
            runSBAs();
        }
        m_pendingEquip = mtg::kInvalidId;
        m_state = HumanState::MainPhase;
        return true;
    }

    case HumanState::AbilityTarget: {
        // Scavenge sentinel: m_pendingAbilityIdx == -1 means we're picking
        // a target creature on the battlefield to receive +1/+1 counters
        // from a card in the controller's graveyard. activateScavenge takes
        // (graveyard card, target creature, controller) directly.
        if (m_pendingAbilityIdx == -1) {
            if (zone == ZoneType::Battlefield) {
                if (m_abilities.activateScavenge(m_pendingAbilityCard, id, 0)) {
                    runSBAs();
                    checkAndHandleTriggers();
                }
            }
            m_pendingAbilityCard = mtg::kInvalidId;
            m_pendingAbilityIdx  = 0;
            m_state = HumanState::MainPhase;
            return true;
        }

        // Battlefield and stack cards can be targeted
        if (zone == ZoneType::Battlefield || zone == ZoneType::Stack) {
            std::vector<mtg::Target> tgts = { mtg::Target::forCard(id) };
            if (m_abilities.activateAbility(m_pendingAbilityCard,
                                             m_pendingAbilityIdx, 0, tgts)) {
                runSBAs(); // ability sits on stack — [Pass] resolves it
            }
        }
        m_pendingAbilityCard = mtg::kInvalidId;
        m_state = HumanState::MainPhase;
        return true;
    }

    case HumanState::DeclareAttack: {
        // Allow casting instant-speed spells from hand during combat declaration
        if (player == 0 && zone == ZoneType::Hand) {
            Card* c = m_game.findCard(id);
            if (c && (c->rules->type.isInstant() || c->hasKeyword(KeywordAbility::Flash) ||
                      c->rules->hasFlashback)) {
                m_savedState   = HumanState::DeclareAttack;
                m_pendingSpell = id;
                m_state        = HumanState::TargetSelect;
                return true;
            }
        }
        if (player == 0 && zone == ZoneType::Battlefield) {
            if (m_attackers.count(id)) m_attackers.erase(id);
            else                        m_attackers.insert(id);
            return true;
        }
        return false;
    }

    case HumanState::OrderBlockers: {
        // Click a remaining blocker to place it next in the damage order
        if (zone == ZoneType::Battlefield) {
            auto it = std::find(m_remainingBlockers.begin(), m_remainingBlockers.end(), id);
            if (it != m_remainingBlockers.end()) {
                m_orderedBlockers.push_back(id);
                m_remainingBlockers.erase(it);
                if (m_remainingBlockers.empty())
                    finishCurrentBlockerOrder();
                return true;
            }
        }
        return false;
    }

    case HumanState::NinjutsuSelect: {
        // Step 1: click a Ninja from hand to select it
        if (player == 0 && zone == ZoneType::Hand) {
            Card* c = m_game.findCard(id);
            if (c && c->rules->hasNinjutsu) {
                m_pendingNinja = id;
                return true;
            }
        }
        // Step 2: click an unblocked attacker to activate Ninjutsu
        if (player == 0 && zone == ZoneType::Battlefield && m_pendingNinja != kInvalidId) {
            const CombatState::Attack* a = m_tm.combatState().findAttack(id);
            if (a && a->blockerIds.empty()) {
                if (m_abilities.activateNinjutsu(m_pendingNinja, id, 0, m_tm)) {
                    runSBAs();
                    // Stay in NinjutsuSelect to allow chaining multiple Ninjutsu
                    m_pendingNinja = kInvalidId;
                    if (!maybeEnterNinjutsuWindow())
                        startBlockerOrdering();
                }
                return true;
            }
        }
        return false;
    }

    case HumanState::DeclareBlock: {
        // Allow casting instant-speed spells from hand during block declaration
        if (player == 0 && zone == ZoneType::Hand) {
            Card* c = m_game.findCard(id);
            if (c && (c->rules->type.isInstant() || c->hasKeyword(KeywordAbility::Flash) ||
                      c->rules->hasFlashback)) {
                m_savedState   = HumanState::DeclareBlock;
                m_pendingSpell = id;
                m_state        = HumanState::TargetSelect;
                return true;
            }
        }
        if (player == 0 && zone == ZoneType::Battlefield) {
            m_pendingBlocker = id;   // your creature (summoning-sick ones may block)
            return true;
        }
        // Click an attacker to assign the pending blocker. The attacker is any
        // OPPONENT's creature (player != 0), not just player 1 — so blocking
        // works in multiplayer too. declareBlocker validates it's in combat.
        if (player != 0 && zone == ZoneType::Battlefield && m_pendingBlocker != kInvalidId) {
            m_tm.declareBlocker(m_pendingBlocker, id);
            m_pendingBlocker = kInvalidId;
            return true;
        }
        return false;
    }

    default:
        return false;
    }
}

bool HumanController::onPlayerClick(uint8_t playerId) {
    // During attacker declaration, clicking a living opponent re-targets the
    // attack at that player (so you can choose which opponent to attack).
    if (m_state == HumanState::DeclareAttack) {
        if (playerId != 0 && playerId < m_game.numPlayers() &&
            !m_game.player(playerId).hasLost()) {
            m_attackDefender = playerId;
            return true;
        }
        return false;
    }
    if (m_state == HumanState::TargetSelect && m_pendingSpell != kInvalidId) {
        // Target a player with a spell (Lightning Bolt to the face, etc.). Only
        // if the spell actually allows a player target; "Opponent" filters can't
        // hit yourself (the human is player 0).
        const Card* c = m_game.findCard(m_pendingSpell);
        bool allowsAny = false, allowsOpp = false;
        if (c && c->rules)
            for (const auto& raw : c->rules->abilityLines) {
                auto s = parseScriptLine(raw);
                if (s.abilityType != "SP") continue;
                auto vt = std::string(s.get("ValidTgts", ""));
                if (vt.find("Player") != std::string::npos ||
                    vt.find("Any")    != std::string::npos) allowsAny = true;
                if (vt.find("Opponent") != std::string::npos) allowsOpp = true;
            }
        bool ok = allowsAny || (allowsOpp && playerId != 0);
        if (!ok) return false;   // not a legal player target — let the click fall through
        std::vector<Target> tgts = { Target::forPlayer(playerId) };
        if (m_abilities.castSpell(m_pendingSpell, 0, tgts)) {
            bool inCombat = (m_savedState == HumanState::DeclareAttack ||
                             m_savedState == HumanState::DeclareBlock);
            if (inCombat) m_abilities.resolveTop();
            runSBAs();
        }
        m_pendingSpell = kInvalidId;
        if (!checkAndHandleTriggers()) {
            m_state      = (m_savedState != HumanState::Idle)
                           ? m_savedState : HumanState::MainPhase;
            m_savedState = HumanState::Idle;
        }
        return true;
    }
    if (m_state == HumanState::AbilityTarget) {
        std::vector<Target> tgts = { Target::forPlayer(playerId) };
        if (m_abilities.activateAbility(m_pendingAbilityCard,
                                         m_pendingAbilityIdx, 0, tgts)) {
            runSBAs(); // ability on stack — [Pass] resolves it
        }
        m_pendingAbilityCard = kInvalidId;
        if (!checkAndHandleTriggers())
            m_state = HumanState::MainPhase;
        return true;
    }
    if (m_state == HumanState::TriggerTarget) {
        m_abilities.resolveHumanTrigger({ Target::forPlayer(playerId) });
        runSBAs();
        if (!checkAndHandleTriggers())
            m_state = HumanState::MainPhase;
        return true;
    }
    return false;
}

void HumanController::onEndPhase() {
    switch (m_state) {

    case HumanState::TriggerTarget: {
        // Human pressed Pass without clicking a target — auto-pick the best target
        if (m_abilities.hasPendingHumanTrigger()) {
            const auto& trig = m_abilities.topHumanTrigger();
            auto validTgts = std::string(trig.effect.get("ValidTgts", ""));
            std::vector<mtg::Target> autoPicked;
            // Try to pick any matching card
            for (const mtg::Card* c : m_game.battlefield().cards()) {
                if (mtg::cardMatchesAnyFilter(*c, validTgts, trig.controllerId)) {
                    autoPicked = { mtg::Target::forCard(c->id) };
                    break;
                }
            }
            m_abilities.resolveHumanTrigger(autoPicked);
            runSBAs();
        }
        if (!checkAndHandleTriggers())
            m_state = HumanState::MainPhase;
        break;
    }

    case HumanState::MainPhase:
        if (!m_abilities.stackEmpty()) {
            m_abilities.resolveTop();
            runSBAs();
            checkAndHandleTriggers();
        } else if (m_stepPauseCont != StepPauseCont::None) {
            resumeFromStepPause();
        } else if (m_inPostCombatMain) {
            endTurn();
        } else if (!m_instantOnly) {
            advanceToAttackers();
        }
        // In instant-only response mode with empty stack: no-op (GameWindow handles exit).
        break;

    case HumanState::DeclareAttack:
        // End Phase with no attackers declared — skip all combat steps
        m_tm.endStep();
        m_tm.advanceStep(); // → DeclareBlockers
        // Skip DeclareBlockers, FirstStrike, CombatDamage, EndCombat (4 steps)
        for (int i = 0; i < 4; ++i) {
            m_tm.beginStep(); m_tm.endStep();
            if (m_tm.isGameOver()) { m_state = HumanState::GameOver; return; }
            m_tm.advanceStep();
        }
        // → Post-Combat Main
        m_inPostCombatMain = true;
        m_tm.beginStep();
        m_state = HumanState::MainPhase;
        break;

    case HumanState::NinjutsuSelect:
        // Pass — skip Ninjutsu window
        m_pendingNinja = kInvalidId;
        startBlockerOrdering();
        break;

    case HumanState::DeclareBlock:
        // Skip blocking — confirm with no blocks
        onConfirm();
        break;

    case HumanState::OrderBlockers:
        // Pass/Space = accept current ordering (auto-appends remaining)
        finishCurrentBlockerOrder();
        break;

    default:
        break;
    }
}

void HumanController::onConfirm() {
    switch (m_state) {

    case HumanState::DeclareAttack: {
        // Declare all selected attackers at the chosen defending opponent
        // (m_attackDefender, default = first living opponent; the player can
        // click an opponent during attacker declaration to retarget).
        if (m_game.player(m_attackDefender).hasLost() || m_attackDefender == 0)
            m_attackDefender = firstLivingOpponent();
        for (ObjectId id : m_attackers)
            m_tm.declareAttacker(id, m_attackDefender);
        m_attackers.clear();

        m_tm.endStep(); m_tm.advanceStep(); // → Declare Blockers
        m_tm.beginStep();
        // Bob declares his blocks
        m_bob.declareBlockers(m_tm);
        m_tm.endStep(); m_tm.advanceStep(); // → First Strike Damage

        // Give Alice a Ninjutsu window if she has a Ninja and unblocked attackers.
        // If not (or after she passes/uses it), fall through to blocker ordering.
        if (maybeEnterNinjutsuWindow()) break;
        startBlockerOrdering();
        break;
    }

    case HumanState::NinjutsuSelect: {
        // "Confirm" or "Pass" — skip Ninjutsu and proceed to blocker ordering.
        m_pendingNinja = kInvalidId;
        startBlockerOrdering();
        break;
    }

    case HumanState::OrderBlockers:
        finishCurrentBlockerOrder();
        break;

    case HumanState::DeclareBlock: {
        // Blocks already registered; advance through damage
        m_pendingBlocker = kInvalidId;
        // Damage and remaining combat steps are handled by the caller (GameWindow)
        m_state = HumanState::Idle; // signal GameWindow to continue Bob's turn
        break;
    }

    case HumanState::MainPhase: {
        // "Confirm" in main phase = same as "End Phase"
        onEndPhase();
        break;
    }

    case HumanState::TargetSelect: {
        // Cast button in the popup — commit with no manual target. The engine
        // filters illegal targets and auto-picks for required ValidTgts when
        // possible. If the cost can't be paid, castSpell returns false and the
        // spell remains pending so the player can tap more lands.
        if (m_pendingSpell == kInvalidId) break;
        ObjectId id = m_pendingSpell;
        const Card* c = m_game.findCard(id);
        if (m_abilities.castSpell(id, 0, {})) {
            bool inCombat = (m_savedState == HumanState::DeclareAttack ||
                             m_savedState == HumanState::DeclareBlock);
            if (inCombat) m_abilities.resolveTop();
            runSBAs();
            m_pendingSpell = kInvalidId;
            if (!checkAndHandleTriggers()) {
                m_state      = (m_savedState != HumanState::Idle)
                               ? m_savedState : HumanState::MainPhase;
                m_savedState = HumanState::Idle;
            }
        } else if (c) {
            // On failure, stay in TargetSelect with the spell still pending
            // and surface the most-likely reason.
            const ManaPool& pool = m_game.player(0).manaPool();
            std::string nm = c->rules->name;
            std::string need = c->rules->manaCost.toString();
            if (need.empty()) need = "{0}";
            std::string have = pool.toString();
            if (have.empty()) have = "(empty)";
            if (!pool.canPay(c->rules->manaCost) &&
                !c->rules->manaCost.isNoCost()) {
                m_castError = "Cannot cast " + nm + ": need " + need +
                              ", pool has " + have + ". Tap lands to add mana.";
            } else {
                m_castError = "Cannot cast " + nm +
                              " right now (timing, restriction, or no legal target).";
            }
        }
        break;
    }

    default:
        break;
    }
}

// ── Private helpers ───────────────────────────────────────────────────────────

bool HumanController::maybeEnterNinjutsuWindow() {
    // Check for an unblocked attacker controlled by Alice
    bool hasUnblocked = false;
    for (const auto& atk : m_tm.combatState().attacks) {
        const Card* c = m_game.findCard(atk.attackerId);
        if (c && c->controllerId == 0 && atk.blockerIds.empty()) {
            hasUnblocked = true;
            break;
        }
    }
    if (!hasUnblocked) return false;

    // Check for a Ninja in Alice's hand
    bool hasNinja = false;
    for (const Card* c : m_game.player(0).hand().cards()) {
        if (c->rules->hasNinjutsu) { hasNinja = true; break; }
    }
    if (!hasNinja) return false;

    m_pendingNinja = kInvalidId;
    m_state = HumanState::NinjutsuSelect;
    return true;
}

uint8_t HumanController::firstLivingOpponent() const {
    for (uint8_t pid = 1; pid < m_game.numPlayers(); ++pid)
        if (!m_game.player(pid).hasLost()) return pid;
    return 1;   // fallback
}

void HumanController::advanceToAttackers() {
    m_inPostCombatMain = false;
    m_attackDefender = firstLivingOpponent();   // default: next living opponent
    // End current main phase → Begin Combat
    m_tm.endStep(); m_tm.advanceStep();

    if (m_stops.beginCombat) {
        // Pause at Begin Combat step
        m_tm.beginStep();
        m_stepPauseCont = StepPauseCont::AfterBeginCombat;
        m_instantOnly   = true;
        m_state         = HumanState::MainPhase;
        return;
    }

    m_tm.beginStep(); m_tm.endStep(); m_tm.advanceStep(); // skip Begin Combat
    // Begin Declare Attackers
    m_tm.beginStep();
    m_attackers.clear();
    m_state = HumanState::DeclareAttack;
}

void HumanController::advanceThroughCombat() {
    // ── First Strike Damage ───────────────────────────────────────────────────
    m_tm.beginStep();
    if (m_stops.firstStrike) {
        m_stepPauseCont = StepPauseCont::AfterFirstStrike;
        m_instantOnly   = true;
        m_state         = HumanState::MainPhase;
        return;
    }
    if (m_tm.hasFirstStrikers()) {
        m_tm.dealCombatDamage(true);
        runSBAs();
    }
    m_tm.endStep();
    if (m_tm.isGameOver()) { m_state = HumanState::GameOver; return; }
    m_tm.advanceStep(); // → Regular Combat Damage

    // ── Regular Combat Damage ─────────────────────────────────────────────────
    m_tm.beginStep();
    if (m_stops.combatDmg) {
        m_stepPauseCont = StepPauseCont::AfterCombatDmg;
        m_instantOnly   = true;
        m_state         = HumanState::MainPhase;
        return;
    }
    if (!m_tm.combatState().empty()) {
        m_tm.dealCombatDamage(false);
        runSBAs();
    }
    m_tm.endStep();
    if (m_tm.isGameOver()) { m_state = HumanState::GameOver; return; }
    m_tm.advanceStep(); // → End of Combat

    // ── End of Combat ─────────────────────────────────────────────────────────
    m_tm.beginStep();
    if (m_stops.endCombat) {
        m_stepPauseCont = StepPauseCont::AfterEndCombat;
        m_instantOnly   = true;
        m_state         = HumanState::MainPhase;
        return;
    }
    m_tm.endStep();

    // Extra combat phase: jump back to Declare Attackers instead of advancing.
    if (m_tm.extraCombats() > 0) {
        m_tm.consumeExtraCombat();
        // Skip to Declare Attackers (BeginCombat handled implicitly by jump)
        m_tm.jumpToStep(TurnStep::BeginCombat);
        m_tm.beginStep(); m_tm.endStep();              // skip Begin Combat
        m_tm.jumpToStep(TurnStep::DeclareAttackers);
        m_tm.beginStep();
        m_attackers.clear();
        m_state = HumanState::DeclareAttack;
        return;
    }

    m_tm.advanceStep(); // → Post-Combat Main

    // ── Post-Combat Main ──────────────────────────────────────────────────────
    m_inPostCombatMain = true;
    m_tm.beginStep();
    m_state = HumanState::MainPhase;
}

// ── Blocker ordering helpers ──────────────────────────────────────────────────

mtg::ObjectId HumanController::orderingAttackerId() const noexcept {
    if (m_state != HumanState::OrderBlockers ||
        m_orderingIdx >= m_orderingAttackers.size())
        return kInvalidId;
    return m_orderingAttackers[m_orderingIdx];
}

void HumanController::startBlockerOrdering() {
    m_orderingAttackers.clear();
    m_orderingIdx = 0;

    for (const auto& atk : m_tm.combatState().attacks) {
        if (atk.blockerIds.size() < 2) continue;
        const Card* attacker = m_game.findCard(atk.attackerId);
        if (!attacker || attacker->controllerId != 0) continue;
        m_orderingAttackers.push_back(atk.attackerId);
    }

    if (m_orderingAttackers.empty()) {
        advanceThroughCombat();
        return;
    }
    enterOrderingForCurrentAttack();
}

void HumanController::enterOrderingForCurrentAttack() {
    m_orderedBlockers.clear();
    m_remainingBlockers.clear();

    ObjectId atkId = m_orderingAttackers[m_orderingIdx];
    for (const auto& atk : m_tm.combatState().attacks) {
        if (atk.attackerId == atkId) {
            m_remainingBlockers = atk.blockerIds;
            break;
        }
    }
    m_state = HumanState::OrderBlockers;
}

void HumanController::finishCurrentBlockerOrder() {
    // Auto-append any blockers the player didn't click
    for (ObjectId bid : m_remainingBlockers)
        m_orderedBlockers.push_back(bid);
    m_remainingBlockers.clear();

    // Write the chosen order back into combat state
    ObjectId atkId = m_orderingAttackers[m_orderingIdx];
    for (auto& atk : m_tm.mutableCombatState().attacks) {
        if (atk.attackerId == atkId) {
            atk.blockerIds = m_orderedBlockers;
            break;
        }
    }
    m_orderedBlockers.clear();

    ++m_orderingIdx;
    if (m_orderingIdx < m_orderingAttackers.size()) {
        enterOrderingForCurrentAttack();
    } else {
        m_orderingAttackers.clear();
        m_orderingIdx = 0;
        advanceThroughCombat();
    }
}

void HumanController::checkResumeCleanup() {
    if (!m_cleanupPending) return;
    if (m_game.hasPendingMadnessCast()) return; // still waiting
    m_cleanupPending = false;
    if (static_cast<int>(m_game.player(0).hand().size()) >
        m_game.player(0).maxHandSize()) {
        return; // stay in DiscardChoice; player still needs to discard
    }
    doCleanupAndIdle();
}

void HumanController::endTurn() {
    m_inPostCombatMain = false;
    m_tm.endStep(); m_tm.advanceStep(); // Post-Combat Main → End Step

    m_tm.beginStep();
    m_abilities.firePhaseTriggersAndDrain(TurnStep::EndStep, m_game.activePlayerId());
    if (m_tm.isGameOver()) { m_state = HumanState::GameOver; return; }

    if (m_stops.endStep) {
        m_tm.endStep(); m_tm.advanceStep(); // End Step → Cleanup
        if (m_tm.isGameOver()) { m_state = HumanState::GameOver; return; }
        m_stepPauseCont = StepPauseCont::AfterEndStep;
        m_instantOnly   = true;
        m_state         = HumanState::MainPhase;
        return;
    }

    m_tm.endStep(); m_tm.advanceStep(); // End Step → Cleanup
    if (m_tm.isGameOver()) { m_state = HumanState::GameOver; return; }
    doCleanupAndIdle();
}

void HumanController::doCleanupAndIdle() {
    // Discard down to hand limit before TurnManager auto-discards
    if (static_cast<int>(m_game.player(0).hand().size()) >
        m_game.player(0).maxHandSize()) {
        m_state = HumanState::DiscardChoice;
        return;
    }
    m_tm.beginStep(); m_tm.endStep(); // Cleanup
    if (m_tm.isGameOver()) { m_state = HumanState::GameOver; return; }
    m_tm.advanceStep();
    m_state = HumanState::Idle;
}

// ── Phase-stop continuation ───────────────────────────────────────────────────

void HumanController::resumeFromStepPause() {
    m_instantOnly = false;
    auto cont = m_stepPauseCont;
    m_stepPauseCont = StepPauseCont::None;

    switch (cont) {
    case StepPauseCont::AfterUpkeep: {
        // Resume from Upkeep pause → Draw then Main1
        m_tm.beginStep();
        m_abilities.firePhaseTriggersAndDrain(TurnStep::Draw, m_game.activePlayerId());
        if (m_tm.isGameOver()) { m_state = HumanState::GameOver; return; }
        m_tm.endStep();
        if (m_tm.isGameOver()) { m_state = HumanState::GameOver; return; }
        m_tm.advanceStep();
        m_inPostCombatMain = false;
        m_tm.beginStep();
        m_abilities.firePhaseTriggersAndDrain(TurnStep::PreCombatMain, m_game.activePlayerId());
        m_state = HumanState::MainPhase;
        break;
    }
    case StepPauseCont::AfterBeginCombat:
        // Resume from Begin Combat pause → Declare Attackers
        m_tm.endStep(); m_tm.advanceStep();
        m_tm.beginStep();
        m_attackers.clear();
        m_state = HumanState::DeclareAttack;
        break;

    case StepPauseCont::AfterFirstStrike:
        // Deal first strike damage and continue to regular damage
        if (m_tm.hasFirstStrikers()) { m_tm.dealCombatDamage(true); runSBAs(); }
        m_tm.endStep();
        if (m_tm.isGameOver()) { m_state = HumanState::GameOver; return; }
        m_tm.advanceStep();
        m_tm.beginStep();
        // Force a stop at combat damage whenever combat is actually happening
        // (attackers/blockers on the board), even if the player unchecked the
        // combat-damage stop — so combat is always manually played out.
        if (m_stops.combatDmg || !m_tm.combatState().empty()) {
            m_stepPauseCont = StepPauseCont::AfterCombatDmg;
            m_instantOnly   = true;
            m_state         = HumanState::MainPhase;
            return;
        }
        if (!m_tm.combatState().empty()) { m_tm.dealCombatDamage(false); runSBAs(); }
        m_tm.endStep();
        if (m_tm.isGameOver()) { m_state = HumanState::GameOver; return; }
        m_tm.advanceStep();
        m_inPostCombatMain = true;
        m_tm.beginStep(); // EndCombat
        if (m_stops.endCombat) {
            m_stepPauseCont = StepPauseCont::AfterEndCombat;
            m_instantOnly   = true;
            m_state         = HumanState::MainPhase;
            return;
        }
        m_tm.endStep(); m_tm.advanceStep();
        m_tm.beginStep(); // PostCombatMain
        m_state = HumanState::MainPhase;
        break;

    case StepPauseCont::AfterCombatDmg:
        // Regular damage was already paused-before; deal it now then continue
        if (!m_tm.combatState().empty()) { m_tm.dealCombatDamage(false); runSBAs(); }
        m_tm.endStep();
        if (m_tm.isGameOver()) { m_state = HumanState::GameOver; return; }
        m_tm.advanceStep();
        m_tm.beginStep(); // EndCombat
        if (m_stops.endCombat) {
            m_inPostCombatMain = true;
            m_stepPauseCont    = StepPauseCont::AfterEndCombat;
            m_instantOnly      = true;
            m_state            = HumanState::MainPhase;
            return;
        }
        m_tm.endStep(); m_tm.advanceStep();
        m_inPostCombatMain = true;
        m_tm.beginStep(); // PostCombatMain
        m_state = HumanState::MainPhase;
        break;

    case StepPauseCont::AfterEndCombat:
        // End of Combat → either extra combat or Post-Combat Main
        m_tm.endStep();
        if (m_tm.extraCombats() > 0) {
            m_tm.consumeExtraCombat();
            m_tm.jumpToStep(TurnStep::BeginCombat);
            m_tm.beginStep(); m_tm.endStep();
            m_tm.jumpToStep(TurnStep::DeclareAttackers);
            m_tm.beginStep();
            m_attackers.clear();
            m_state = HumanState::DeclareAttack;
        } else {
            m_tm.advanceStep();
            m_inPostCombatMain = true;
            m_tm.beginStep();
            m_state = HumanState::MainPhase;
        }
        break;

    case StepPauseCont::AfterEndStep:
        // EndStep was paused; now do Cleanup
        doCleanupAndIdle();
        break;

    default:
        break;
    }
}

// ── Planeswalker ability selection ────────────────────────────────────────────

std::string HumanController::loyaltyCostLabel(const std::string& costStr) {
    if (costStr.find("AddCounter<") != std::string::npos &&
        costStr.find("LOYALTY") != std::string::npos) {
        auto lt = costStr.find('<'), sl = costStr.find('/');
        if (lt != std::string::npos && sl != std::string::npos)
            return "+" + costStr.substr(lt + 1, sl - lt - 1);
    }
    if (costStr.find("SubCounter<") != std::string::npos &&
        costStr.find("LOYALTY") != std::string::npos) {
        auto lt = costStr.find('<'), sl = costStr.find('/');
        if (lt != std::string::npos && sl != std::string::npos)
            return "-" + costStr.substr(lt + 1, sl - lt - 1);
    }
    return "0";
}

void HumanController::activatePWAbility(int choiceIdx) {
    if (m_pendingPW == kInvalidId ||
        choiceIdx < 0 || choiceIdx >= static_cast<int>(m_pwAbilIdxs.size()))
        return;
    ObjectId pwId      = m_pendingPW;
    int      abilityIdx = m_pwAbilIdxs[choiceIdx];
    cancelPWChoice();

    Card* pw = m_game.findCard(pwId);
    if (!pw) return;
    auto s = mtg::parseScriptLine(pw->rules->abilityLines[abilityIdx]);
    auto validTgts = std::string(s.get("ValidTgts", ""));
    if (!validTgts.empty()) {
        m_pendingAbilityCard = pwId;
        m_pendingAbilityIdx  = abilityIdx;
        m_state = HumanState::AbilityTarget;
    } else {
        if (m_abilities.activateAbility(pwId, abilityIdx, 0, {})) {
            runSBAs(); // ability on stack — [Pass] resolves it
            checkAndHandleTriggers();
        }
    }
}

void HumanController::cancelPWChoice() {
    m_pendingPW = kInvalidId;
    m_pwAbilIdxs.clear();
    m_pwAbilLabels.clear();
}

// ── RenderHints ───────────────────────────────────────────────────────────────

RenderHints HumanController::buildHints() const {
    RenderHints h;
    h.phase       = std::string(m_tm.currentStepName());
    h.selectedCards = m_attackers; // highlight selected attackers

    if (m_pendingSpell != kInvalidId) {
        h.selectedCards.insert(m_pendingSpell);
        h.pendingSpell = m_pendingSpell;
    }
    if (m_pendingBlocker != kInvalidId)
        h.selectedCards.insert(m_pendingBlocker);

    for (const auto& atk : m_tm.combatState().attacks)
        h.attackers.insert(atk.attackerId);

    if (m_pendingEquip       != mtg::kInvalidId) h.selectedCards.insert(m_pendingEquip);
    if (m_pendingAbilityCard != mtg::kInvalidId) h.selectedCards.insert(m_pendingAbilityCard);
    if (m_pendingPW          != mtg::kInvalidId) h.selectedCards.insert(m_pendingPW);
    if (m_pendingNinja       != mtg::kInvalidId) h.selectedCards.insert(m_pendingNinja);

    // PW overlay: override instruction before state switch
    if (m_pendingPW != mtg::kInvalidId) {
        h.instruction = "Choose a loyalty ability.";
        return h;
    }

    // Pending Connive discard
    if (m_game.hasPendingConnive()) {
        const auto& pc = m_game.pendingConnive;
        int remaining = pc.amount - pc.discardsDone;
        h.instruction = "Connive — discard " + std::to_string(remaining)
                      + " card" + (remaining == 1 ? "" : "s")
                      + " (nonland → +1/+1 counter). Click a hand card.";
        return h;
    }
    // Pending discard from an opponent's spell
    if (m_game.hasPendingDiscard()) {
        int n = m_game.pendingDiscardCount();
        h.instruction = "Discard " + std::to_string(n) +
                        (n == 1 ? " card" : " cards") + " — click a hand card.";
        return h;
    }

    switch (m_state) {
    case HumanState::MainPhase: {
        if (!m_abilities.stackEmpty()) {
            const auto* top = m_abilities.top();
            std::string topName = "Spell";
            if (top) {
                const mtg::Card* src = m_game.findCard(top->sourceCardId);
                if (src) topName = src->name();
            }
            int n = m_abilities.stackSize();
            std::string info = topName + (n > 1 ? " (+" + std::to_string(n-1) + " more)" : "");
            h.instruction = info + " on stack — [Pass] to resolve, or cast more instants.";
        } else {
            h.instruction = m_instantOnly
                ? "Opponent's turn — cast instants/Flash or [Pass]."
                : "Click hand card to play. Click equip/ability on battlefield. [Pass] to attack.";
        }
        break;
    }
    case HumanState::TargetSelect: {
        h.instruction = "Click a target card or opponent name.";
        // Populate valid targets with all legal targets for the pending spell/ability
        const mtg::CardRules* pendingRules = nullptr;
        if (m_pendingSpell != kInvalidId) {
            const mtg::Card* ps = m_game.findCard(m_pendingSpell);
            if (ps) pendingRules = ps->rules;
        } else if (m_pendingAbilityCard != kInvalidId) {
            const mtg::Card* pa = m_game.findCard(m_pendingAbilityCard);
            if (pa) pendingRules = pa->rules;
        }
        if (pendingRules) {
            for (const auto& raw : pendingRules->abilityLines) {
                auto sl = mtg::parseScriptLine(raw);
                if (sl.abilityType != "SP" && sl.abilityType != "AB" && sl.abilityType != "DB") continue;
                auto vt = std::string(sl.get("ValidTgts", ""));
                if (vt.empty()) continue;
                for (const mtg::Card* c : m_game.battlefield().cards()) {
                    if (c->cantBeTargeted) continue;
                    if (c->hasKeyword(mtg::KeywordAbility::Shroud)) continue;
                    if (c->hasKeyword(mtg::KeywordAbility::Hexproof) && c->controllerId == 0) continue;
                    if (!cardMatchesAnyFilter(*c, vt, 0, kInvalidId, nullptr, &m_game)) continue;
                    h.validTargets.insert(c->id);
                }
                break;
            }
        }
        break;
    }
        break;
    case HumanState::EquipSelect:
        h.instruction = "Click a creature to attach the equipment to.";
        break;
    case HumanState::AbilityTarget:
        h.instruction = "Click a target for the activated ability.";
        break;
    case HumanState::DeclareAttack: {
        // Compute expected damage from currently toggled attackers
        int previewDmg = 0;
        for (ObjectId aid : m_attackers) {
            const Card* a = m_game.findCard(aid);
            if (a) previewDmg += std::max(0, effectivePower(*a));
        }
        h.combatDamagePreview = previewDmg;
        std::string dmgStr = previewDmg > 0
            ? "  [" + std::to_string(previewDmg) + " dmg]" : "";
        h.instruction = "Click creatures to attack." + dmgStr + " [Confirm] to proceed.";
        break;
    }
    case HumanState::NinjutsuSelect:
        if (m_pendingNinja != mtg::kInvalidId)
            h.instruction = "Ninjutsu: click an unblocked attacker to swap it out. [Pass] to skip.";
        else
            h.instruction = "Ninjutsu available! Click a Ninja from hand, then an unblocked attacker. [Pass] to skip.";
        break;
    case HumanState::DeclareBlock:
        h.instruction = "Click YOUR creature, then click an attacker to block.";
        break;
    case HumanState::OrderBlockers: {
        // Show the current attacker in orange (attackers set), blockers in gold (selectedCards)
        if (m_orderingIdx < m_orderingAttackers.size())
            h.attackers.insert(m_orderingAttackers[m_orderingIdx]);
        for (ObjectId bid : m_orderedBlockers)   h.selectedCards.insert(bid);
        for (ObjectId bid : m_remainingBlockers) h.selectedCards.insert(bid);
        // Number overlay for already-placed blockers
        for (int i = 0; i < static_cast<int>(m_orderedBlockers.size()); ++i)
            h.blockerOrder[m_orderedBlockers[i]] = i + 1;
        int queued = static_cast<int>(m_orderingAttackers.size()) - static_cast<int>(m_orderingIdx);
        h.instruction = "Click blockers in damage order (1st = takes damage first)."
                        " [Pass]/[Confirm] to accept.";
        if (queued > 1)
            h.instruction += " (" + std::to_string(queued) + " attackers left)";
        break;
    }
    case HumanState::TriggerTarget: {
        std::string srcName = "triggered ability";
        if (m_abilities.hasPendingHumanTrigger()) {
            const auto& trig = m_abilities.topHumanTrigger();
            const mtg::Card* src = m_game.findCard(trig.sourceCardId);
            if (src) srcName = src->name() + "'s ability";
        }
        h.instruction = "Choose a target for " + srcName + ".";
        break;
    }
    case HumanState::DiscardChoice: {
        int excess = static_cast<int>(m_game.player(0).hand().size())
                   - m_game.player(0).maxHandSize();
        h.instruction = "Discard " + std::to_string(excess)
                      + " card(s) — click a hand card.";
        break;
    }
    default:
        h.instruction = "";
        break;
    }
    return h;
}

} // namespace ui
