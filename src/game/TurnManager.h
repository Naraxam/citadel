#pragma once
#include "TurnStep.h"
#include "CombatState.h"
#include "StateBasedActions.h"
#include <cstdint>
#include <string_view>

namespace mtg {

class GameState;

// Drives the MTG turn sequence: steps, priority, state-based actions, combat.
//
// Typical game-loop usage:
//   1. Call beginStep() when entering a new step.
//   2. If stepGivesPriority(), give priority via givePriorityToActive()
//      and let players act; call passPriority() when they pass.
//   3. When passPriority() returns true (all passed, stack empty), call
//      endStep() then advanceStep() to move on.
//   4. Always run runSBAsUntilClean() before giving priority.
class TurnManager {
public:
    explicit TurnManager(GameState& game);

    // ── Step navigation ───────────────────────────────────────────────────
    // Reset to Turn 1 Untap — call after GameState::reset() for a new game.
    void reset() noexcept;

    // Grant an additional combat phase after the current one.
    // Called when effects like Aggravated Assault resolve.
    void grantExtraCombat() noexcept { ++m_extraCombats; }
    int  extraCombats()     const noexcept { return m_extraCombats; }
    void consumeExtraCombat() noexcept { if (m_extraCombats > 0) --m_extraCombats; }

    // Jump directly to a specific step (required for extra combat phases).
    // Does NOT fire advanceTurn() — only valid for steps on the same player's turn.
    void jumpToStep(TurnStep s) noexcept;

    TurnStep         currentStep()     const noexcept { return m_step; }
    std::string_view currentStepName() const noexcept { return stepName(m_step); }
    bool             stepGivesPriority()const noexcept { return mtg::stepGivesPriority(m_step); }

    // Execute automatic beginning-of-step actions:
    //   Untap  → untap active player's permanents; clear summoning sickness
    //   Draw   → active player draws one card
    //   Cleanup→ discard to hand size; remove damage from permanents
    void beginStep();

    // Clean up anything that expires at end of step (damage cleared in Cleanup).
    void endStep();

    // Advance to the next step; wraps around to next turn after Cleanup.
    void advanceStep();

    // ── Priority ──────────────────────────────────────────────────────────
    uint8_t priorityHolder() const noexcept { return m_priorityHolder; }

    // Reset priority tracking and give priority to the active player.
    void givePriorityToActive();

    // The current priority holder passes.
    // Returns true when all players have passed in succession with an empty stack
    // — caller should then call endStep() + advanceStep().
    bool passPriority(uint8_t playerId);

    // ── State-based actions ───────────────────────────────────────────────
    // Run SBAs in a loop until no more fire. Returns true if any fired.
    bool runSBAsUntilClean();

    // ── Combat ────────────────────────────────────────────────────────────
    // Declare a creature as an attacker. Taps it (unless vigilance).
    // Returns false if the creature can't attack (not a creature, has
    // summoning sickness, is already tapped, not on battlefield, etc.).
    bool declareAttacker(ObjectId creatureId, uint8_t defendingPlayerId);
    // Declare an attacker targeting a specific planeswalker (opponent must control it).
    bool declareAttackerVsPlaneswalker(ObjectId creatureId, ObjectId planeswalkerTargetId);

    // Assign a blocker to an attacker.
    // Returns false if the blocker can't block (tapped, not on battlefield, etc.).
    bool declareBlocker(ObjectId blockerId, ObjectId attackerId);

    // Returns true if any attacker or blocker has First Strike or Double Strike.
    // Use to decide whether to call dealCombatDamage(true) during FirstStrikeDamage.
    bool hasFirstStrikers() const noexcept;

    // Deal combat damage.
    // isFirstStrikeStep=true  → only first-strike/double-strike creatures deal damage.
    // isFirstStrikeStep=false → only non-first-strike creatures deal damage
    //                           (double-strikers deal damage again).
    void dealCombatDamage(bool isFirstStrikeStep = false);

    // Clear combat state (called at End of Combat).
    void endCombat();

    const CombatState& combatState()  const noexcept { return m_combat; }
    CombatState&       mutableCombatState() noexcept  { return m_combat; }

    // ── Snapshot / restore (for undo support in GameWindow) ───────────────
    struct Snapshot {
        TurnStep    step;
        int         stepIndex;
        uint8_t     priorityHolder;
        bool        passed[2];
        CombatState combat;
    };
    Snapshot saveSnapshot()  const noexcept;
    void     restoreSnapshot(const Snapshot&) noexcept;

    // ── Game-over ─────────────────────────────────────────────────────────
    bool    isGameOver() const noexcept;
    uint8_t winnerId()   const noexcept; // valid only when isGameOver()

private:
    GameState&  m_game;
    TurnStep    m_step           = TurnStep::Untap;
    int         m_stepIndex      = 0;
    uint8_t     m_priorityHolder = 0;
    bool        m_passed[2]      = {};
    CombatState m_combat;
    int         m_extraCombats   = 0;  // additional combat phases queued

    void resetPriorityPassed();

    // Move top of active player's library to their hand
    void drawCard(uint8_t playerId);
};

} // namespace mtg
