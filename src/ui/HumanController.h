#pragma once
#include "BoardRenderer.h"
#include "../game/GameState.h"
#include "../game/TurnManager.h"
#include "../game/ability/AbilityProcessor.h"
#include "../game/ai/AiPlayer.h"
#include "../game/StateBasedActions.h"
#include <set>

namespace ui {

// States the human player can be in during their turn
enum class HumanState {
    Idle,             // not the human's decision point
    MainPhase,        // play lands, tap mana, cast spells
    TargetSelect,     // selected a spell in hand — now click a target
    AbilityTarget,    // selected an activated ability — now click a target
    EquipSelect,      // selected an Equipment on battlefield — now click a creature
    DeclareAttack,    // click your creatures to toggle attack; Confirm to proceed
    NinjutsuSelect,   // after Bob declares blockers: click Ninja from hand, then unblocked attacker
    DeclareBlock,     // Bob is attacking; click your creature then his attacker
    OrderBlockers,    // click blockers in damage-assignment order for a human attacker
    DiscardChoice,    // hand > max size — click a hand card to discard
    TriggerTarget,    // a triggered ability needs a target — click to resolve
    ManaAbilityChoice,// land has multiple mana lines (Shivan Reef etc.) — pick one
    GameOver,         // game ended
};

// Manages Alice (player 0) through her complete turn, plus blocking during Bob's attacks.
class HumanController {
public:
    HumanController(mtg::GameState& game,
                    mtg::TurnManager& tm,
                    mtg::AbilityProcessor& abilities,
                    mtg::AiPlayer& bob);


    void startHumanTurn();

    void resetToMainPhase() noexcept;

    void startBlockPhase();

    // ── Input events 
    bool onCardClick(mtg::ObjectId id, mtg::ZoneType zone, uint8_t player);

    // The player clicked a player area (used for targeting — damage to face).
    bool onPlayerClick(uint8_t playerId);

    // "Pass / End Phase" button (or Space key) pressed.
    void onEndPhase();

    // "Confirm" button (or Enter key) pressed.
    void onConfirm();

    // ── State queries 
    HumanState state() const noexcept { return m_state; }

    // popup is showing). kInvalidId when no popup applies.
    mtg::ObjectId pendingSpellId() const noexcept { return m_pendingSpell; }

    bool cancelPendingSpell();

    // ── Cast mode options (popup) 
    // One row in the cast-options popup.
    enum class CastModeKind : uint8_t {
        Cast      = 0,  // pay manaCost, normal hand cast
        Foretell  = 1,  // pay {2}, exile face-down for next-turn cast
    };
    struct CastOption {
        CastModeKind kind;
        std::string  label;    // button label, e.g. "Cast" or "Foretell"
        std::string  costStr;  // pretty mana cost, e.g. "{2}{R}" or "{2}"
        bool         affordable; // does the current pool cover this cost?
    };
    // Modes available for the currently pendingSpellId, or empty if none.
    std::vector<CastOption> availableModes() const;

    // ── Multi-line mana ability picker 
    struct ManaAbilityOption {
        int          abilityIndex;   // index into the source's AB$ Mana lines
        std::string  label;          // "Add {C}" / "Add {U} or {R}  (1 life)"
    };
    // The source card whose abilities are being picked, or kInvalidId when
    // the picker is idle.
    mtg::ObjectId      manaAbilitySource() const noexcept { return m_manaAbilitySource; }
    const std::vector<ManaAbilityOption>& manaAbilityOptions() const noexcept {
        return m_manaAbilityOptions;
    }
    // Commit a picker choice: activates the chosen line on the source land.
    void chooseManaAbility(int abilityIndex);
    // Dismiss the picker without firing anything (clicked outside, Esc, etc.).
    void cancelManaAbility() noexcept;
    // Commit a chosen mode. For Cast it leaves the spell pending (existing
    // target-select flow). For Foretell it fires immediately and clears state.
    void chooseMode(CastModeKind kind);

    // Populated when a cast attempt (chooseMode or onConfirm) fails — empty
    // otherwise. GameWindow polls + clears each tick so the message is logged
    // exactly once. Format: "Cannot cast <name>: <reason>".
    const std::string& consumeCastError() noexcept {
        m_castError.swap(m_castErrorOut);
        m_castError.clear();
        return m_castErrorOut;
    }

    bool needsInput() const noexcept {
        switch (m_state) {
            case HumanState::Idle:
            case HumanState::GameOver:
                return false;
            default:
                return true;
        }
    }
    bool isDone()        const noexcept { return m_state == HumanState::Idle ||
                                                 m_state == HumanState::GameOver; }
    bool isGameOver()    const noexcept { return m_state == HumanState::GameOver; }
    bool inResponseMode() const noexcept { return m_instantOnly; }

    // Called after the human resolves a Phyrexian mana payment choice overlay.
    void advanceAfterPhyrexianChoice(bool /*paidLife*/) {
        m_state = HumanState::TargetSelect;
    }

    // Cast a hand card at its miracle cost (called by GameWindow after miracle choice).
    void castMiracleCard(mtg::ObjectId cardId);

    void beginResponsePhase();
    void endResponsePhase();

    // Fill in a RenderHints for the current state
    RenderHints buildHints() const;

    // Call after any game action — enters TriggerTarget if a trigger needs human input.
    // Returns true if a pending trigger was found.
    bool checkAndHandleTriggers();

    struct PhaseStops {
        bool untap        = false;
        bool upkeep       = false;
        bool draw         = false;
        bool main1        = true;
        bool beginCombat  = false;
        bool attackers    = true;
        bool blockers     = true;
        bool firstStrike  = false;
        bool combatDmg    = false;
        bool endCombat    = false;
        bool main2        = true;
        bool endStep      = false;
        bool cleanup      = false;
    };
    void setPhaseStops(const PhaseStops& s) { m_stops = s; }
    const PhaseStops& phaseStops() const noexcept { return m_stops; }

    mtg::ObjectId            pendingPlaneswalker()  const noexcept { return m_pendingPW; }
    const std::vector<int>&  pwAbilIdxs()           const noexcept { return m_pwAbilIdxs; }
    const std::vector<std::string>& pwAbilLabels()  const noexcept { return m_pwAbilLabels; }
    // Called by GameWindow when the human clicks an ability button.
    void activatePWAbility(int choiceIdx);
    // Called by GameWindow when the human clicks away from the PW overlay.
    void cancelPWChoice();

    bool inBlockerOrdering() const noexcept { return m_state == HumanState::OrderBlockers; }
    // The attacker currently being ordered (kInvalidId when not ordering).
    mtg::ObjectId orderingAttackerId() const noexcept;
    // Blockers placed so far (first = takes damage first).
    const std::vector<mtg::ObjectId>& orderedBlockers()   const noexcept { return m_orderedBlockers; }
    // Blockers not yet placed (clickable).
    const std::vector<mtg::ObjectId>& remainingBlockers() const noexcept { return m_remainingBlockers; }

private:
    mtg::GameState&        m_game;
    mtg::TurnManager&      m_tm;
    mtg::AbilityProcessor& m_abilities;
    mtg::AiPlayer&         m_bob;

    HumanState             m_state      = HumanState::Idle;
    HumanState             m_savedState = HumanState::Idle; // state to restore after TargetSelect
    bool                   m_instantOnly = false;
    mtg::ObjectId          m_pendingSpell   = mtg::kInvalidId;
    mtg::ObjectId          m_pendingEquip   = mtg::kInvalidId; // equipment waiting for creature target
    mtg::ObjectId          m_pendingAbilityCard = mtg::kInvalidId; // card whose ability needs a target
    int                    m_pendingAbilityIdx  = 0;              // which ability index on that card
    std::set<mtg::ObjectId> m_attackers;  // toggled attackers
    uint8_t m_attackDefender = 1;         // which opponent the attackers target
    uint8_t firstLivingOpponent() const;  // default attack target (skips dead players)
    mtg::ObjectId          m_pendingBlocker = mtg::kInvalidId; // blocker selected, waiting for attacker click
    mtg::ObjectId          m_pendingNinja   = mtg::kInvalidId; // Ninja selected for Ninjutsu swap

    // Ability-choice overlay state. Originally just planeswalker loyalty
    // abilities; now also used for any permanent that offers more than one way
    // to activate it (Eiganjo Castle: tap for {W} OR prevent damage;
    // Shadowspear: equip OR its {1} ability). m_pwAbilKinds runs parallel to
    // m_pwAbilIdxs: 0 = non-mana activated ability (idx into abilityLines),
    // 1 = mana ability (idx into the source's AB$ Mana lines), 2 = Equip.
    mtg::ObjectId              m_pendingPW       = mtg::kInvalidId;
    std::vector<int>           m_pwAbilIdxs;
    std::vector<int>           m_pwAbilKinds;
    std::vector<std::string>   m_pwAbilLabels;
    // Dispatch a single chosen activation option on a permanent (see kinds above).
    bool activatePermanentChoice(mtg::ObjectId id, int kind, int idx);

    // Blocker ordering state (HumanState::OrderBlockers)
    std::vector<mtg::ObjectId> m_orderingAttackers;    // human attackers with 2+ blockers
    size_t                     m_orderingIdx     = 0;  // current index into above
    std::vector<mtg::ObjectId> m_orderedBlockers;      // placed in damage order so far
    std::vector<mtg::ObjectId> m_remainingBlockers;    // not yet placed

    bool m_cleanupPending = false;

    // ── Phase-stop state 
    PhaseStops m_stops;
    bool m_inPostCombatMain = false;
    std::string m_castError;
    mutable std::string m_castErrorOut;

    // Multi-line mana ability picker state. Populated when the player clicks
    // a land/source with 2+ AB$ Mana lines; cleared on choose or cancel.
    mtg::ObjectId                   m_manaAbilitySource = mtg::kInvalidId;
    std::vector<ManaAbilityOption>  m_manaAbilityOptions;

    // Continuation tag set when pausing at an auto-advance step.
    enum class StepPauseCont : uint8_t {
        None,
        AfterUpkeep,
        AfterBeginCombat,
        AfterFirstStrike,
        AfterCombatDmg,
        AfterEndCombat,
        AfterEndStep,
    };
    StepPauseCont m_stepPauseCont = StepPauseCont::None;

public:
    // Called by GameWindow after any pending-overlay choice completes.
    void checkResumeCleanup();

private:
    // Run SBAs until clean
    void runSBAs();

    // Enter NinjutsuSelect state if Alice has a Ninja in hand and an unblocked attacker.
    // Returns true if the state was entered (caller should break out of the confirm flow).
    bool maybeEnterNinjutsuWindow();

    // Extract "+N" / "0" / "-N" label from a loyalty ability Cost$ string
    static std::string loyaltyCostLabel(const std::string& costStr);

    // Advance through Untap, Upkeep, Draw automatically
    void autoAdvanceToMain();

    // Advance from Pre-Combat Main through to the next interactive point
    // (Declare Attackers), running Begin Combat automatically.
    void advanceToAttackers();

    // Advance through damage steps and post-combat main, then end the turn.
    void advanceThroughCombat();

    // End the post-combat main phase and cleanup
    void endTurn();

    // Resume after a phase-stop pause (called by onEndPhase when m_stepPauseCont != None)
    void resumeFromStepPause();

    // Complete the Cleanup step and set state to Idle (shared by endTurn and discard paths)
    void doCleanupAndIdle();

    // Blocker ordering helpers
    void startBlockerOrdering();           // enter OrderBlockers or skip to combat
    void enterOrderingForCurrentAttack();  // set up m_orderedBlockers / m_remainingBlockers
    void finishCurrentBlockerOrder();      // apply order, advance to next or to combat
};

} // namespace ui
