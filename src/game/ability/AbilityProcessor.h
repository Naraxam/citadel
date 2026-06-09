#pragma once
#include "StackAbility.h"
#include "Target.h"
#include "../ObjectId.h"
#include "../TurnStep.h"
#include "../TriggerSystem.h"
#include "../../core/mana/ManaCost.h"
#include <cstdint>
#include <deque>
#include <vector>
#include <string>

namespace mtg {

class GameState;
class TurnManager;
struct Card;

// Manages casting spells, activating abilities, and resolving the stack.
//
// Casting a spell:
//   1. Pay its mana cost from the controller's pool.
//   2. Choose targets.
//   3. Move card Hand → Stack (via GameState::moveToZone).
//   4. Push a StackAbility describing what happens on resolution.
//
// Resolving:
//   1. Pop the top StackAbility.
//   2. Execute the effect(s) via Effects::executeEffect.
//   3. Move the source card to its destination zone (graveyard for non-permanents,
//      battlefield for permanents).
//
// Mana abilities (AB$ Mana) bypass the stack entirely — they are activated
// immediately with no priority window.
class AbilityProcessor {
public:
    explicit AbilityProcessor(GameState& game);

    // ── Mana abilities ────────────────────────────────────────────────────
    // Tap a land or artifact to add mana. abilityIndex selects which mana
    // line to fire (0-based among AB$ Mana lines). -1 = activate all matching
    // lines as a single bundle, which is the legacy behaviour for the AI's
    // tap-for-cost path and basic lands; the UI uses an explicit index when
    // the player picks from the multi-mana-ability popup (Shivan Reef has
    // two: {C}, and {U} or {R} for 1 life).
    bool activateManaAbility(ObjectId sourceId, uint8_t controller,
                              int abilityIndex = -1);

    // ── General activated abilities ───────────────────────────────────────
    // Activate the N-th AB$ ability line (0-indexed) that is not a mana ability.
    // Pays the Cost$, then either executes immediately (mana) or pushes to stack.
    // Returns false if the ability can't be activated.
    bool activateAbility(ObjectId sourceId, int abilityIndex,
                          uint8_t controller,
                          const std::vector<Target>& targets = {});

    // Activate an Equipment's equip ability (from K:Equip:N keyword).
    // Attaches the equipment to targetId after paying the equip cost.
    bool activateEquip(ObjectId equipId, ObjectId targetId, uint8_t controller);

    // Encore: exile this from GY, create haste token copies attacking each opponent.
    // In 2-player Commander, creates one copy that attacks the opponent.
    bool activateEncore(ObjectId cardId, uint8_t controller);

    // Activate the cycling ability of a card in hand.
    // Pays the cycling cost, discards the card, draws a replacement (or searches
    // for a land for TypeCycling variants). Returns false if cost can't be paid.
    bool activateCycling(ObjectId cardId, uint8_t controller);
    // Forecast: {cost}, Reveal this card from hand → trigger its Forecast effect.
    // Usable only during the controller's upkeep.
    bool activateForecast(ObjectId cardId, uint8_t controller);
    bool activateTransmute(ObjectId cardId, uint8_t controller);
    bool activateFortify(ObjectId fortId, ObjectId targetLandId, uint8_t controller);

    // Crew a Vehicle: tap the given creatures (total power >= crewCost) and
    // make the Vehicle an artifact creature until end of turn.
    // Returns false if the vehicle has no Crew keyword, the creatures don't
    // have enough total power, or any creature is already tapped.
    bool crewVehicle(ObjectId vehicleId,
                     const std::vector<ObjectId>& crewIds,
                     uint8_t controller);

    // Scavenge: exile a creature card from the graveyard (it must have K:Scavenge),
    // pay its scavenge cost, and put +1/+1 counters equal to its power on targetId.
    // Returns false if any precondition fails.
    bool activateScavenge(ObjectId cardId, ObjectId targetId, uint8_t controller);

    // Monstrosity: pay the card's K:Monstrosity:N:COST, put N +1/+1 counters on it, and
    // set its monstrous flag. Returns false if already monstrous, not on battlefield, or
    // cost can't be paid.
    bool activateMonstrosity(ObjectId cardId, uint8_t controller);

    // Embalm: exile a creature card from the graveyard (it must have K:Embalm), pay its
    // embalm cost, and create a white Zombie token copy of it on the battlefield.
    // Returns false if any precondition fails.
    bool activateEmbalm(ObjectId cardId, uint8_t controller);

    // Eternalize: like Embalm but the token is always 4/4 regardless of original P/T.
    bool activateEternalize(ObjectId cardId, uint8_t controller);

    // Foretell: during your main phase, pay {2} and exile a card from hand face-down.
    // The card may then be cast from exile on a later turn for its foretell cost.
    // Returns false if the card has no Foretell keyword, is not in hand, or cost fails.
    bool activateForetell(ObjectId cardId, uint8_t controller);

    // Suspend: pay suspend cost from hand, exile with N time counters.
    // Returns false if prerequisites fail (no suspend, not in hand, can't pay).
    // xValue is used when the suspend count is X.
    bool activateSuspend(ObjectId cardId, uint8_t controller, int xValue = 0);

    // Ninjutsu: pay the ninja's ninjutsuCost, return unblocked attacker attackerId to its
    // owner's hand, and put ninjaId from hand onto the battlefield tapped and attacking
    // in the same combat slot.  tm updates the live CombatState.
    // Returns false if any precondition fails (wrong zone, already blocked, can't pay).
    bool activateNinjutsu(ObjectId ninjaId, ObjectId attackerId, uint8_t controller,
                          TurnManager& tm);

    // Level Up: pay the levelUpCost and put one LEVEL counter on the creature (sorcery speed).
    // Returns false if any precondition fails (not on BF, wrong controller, can't pay).
    bool activateLevelUp(ObjectId cardId, uint8_t controller);

    // Outlast: tap + pay outlastCost → put one +1/+1 counter (sorcery speed).
    bool activateOutlast(ObjectId cardId, uint8_t controller);

    // Adapt N: pay adaptCost → put N +1/+1 counters if the creature has none.
    bool activateAdapt(ObjectId cardId, uint8_t controller);

    // Morph / Megamorph: pay the face-up cost to turn a face-down creature face up.
    // Sets isFaceDown = false, rebuilds keywordMask, adds +1/+1 counter for Megamorph,
    // fires TurnFaceUp triggers.
    // Returns false if the card is not face-down on the battlefield or cost can't be paid.
    bool activateMorph(ObjectId cardId, uint8_t controller);

    // Called at the start of the active player's upkeep.
    // Removes one TIME counter from each suspended card in exile.
    // When the last counter is removed, casts the card for free.
    // Returns true if any card was cast this way.
    bool processSuspendUpkeep(uint8_t playerId);

    // Rebound upkeep: cast any exiled Rebound cards for free.
    // Returns true if any card was cast this way.
    bool processReboundUpkeep(uint8_t playerId);

    // Echo upkeep: for each Echo permanent controlled by playerId that has seen one
    // full upkeep since ETB, the controller must pay the echo cost or it is sacrificed.
    void processEchoUpkeep(uint8_t playerId);

    // Hideaway N (Mosswort Bridge, Windbrisk Heights, …): when a permanent with
    // K:Hideaway enters, look at the top N cards of its controller's library and
    // exile the most expensive one face-down, linked to the source (exiledBy).
    // The card can later be played for free via the source's AB$ Play | ExiledWith.
    // No-op for permanents without the keyword. Call once on ETB.
    void processHideaway(Card& source);

    // Cumulative Upkeep: add one age counter to each affected permanent, then player
    // must pay age-count × upkeepCost or sacrifice.
    void processCumulativeUpkeep(uint8_t playerId);

    // ── Spell casting ─────────────────────────────────────────────────────
    // Try to cast a spell from a player's hand.
    //   - Pays the mana cost from the controller's pool.
    //   - Moves the card to the Stack zone.
    //   - Pushes a StackAbility with the provided targets.
    // Returns false if the cost can't be paid or the card can't be cast.
    bool castSpell(ObjectId cardId, uint8_t controller,
                   const std::vector<Target>& targets = {});

    // Net generic-mana cost reduction available to `controller` when casting
    // `spell`, summed over all S:Mode$ ReduceCost / RaiseCost statics (the
    // spell's own plus every battlefield permanent). Positive = cheaper.
    // Shared by castSpell and the affordability gates (UI / AI) so a reduced
    // spell is treated as affordable for its reduced cost — otherwise the
    // reducer "did nothing" because the cost shown/checked was still full.
    int genericReductionFor(const Card& spell, uint8_t controller) const;

    // ── Stack resolution ──────────────────────────────────────────────────
    // Resolve the top ability on the stack.
    void resolveTop();

    // True if there is anything on the stack.
    bool stackEmpty() const noexcept { return m_stack.empty(); }
    int  stackSize()  const noexcept { return static_cast<int>(m_stack.size()); }
    // The full stack (spells AND activated/triggered abilities), bottom→top.
    // Used by the renderer so activated abilities are visible on the stack.
    const std::vector<StackAbility>& stackAbilities() const noexcept { return m_stack; }

    // ── Human trigger queue ───────────────────────────────────────────────
    // Triggered abilities owned by player 0 that have ValidTgts$ and need
    // the human to choose a target before they can resolve.
    bool hasPendingHumanTrigger() const noexcept { return !m_humanTriggers.empty(); }
    int  pendingHumanTriggerCount() const noexcept { return static_cast<int>(m_humanTriggers.size()); }
    const PendingTrigger& topHumanTrigger() const noexcept { return m_humanTriggers.front(); }
    const std::deque<PendingTrigger>& humanTriggerQueue() const noexcept { return m_humanTriggers; }

    // Move trigger at index `from` to index `to` in the queue (for ordering UI).
    void reorderHumanTrigger(int from, int to) noexcept {
        if (from < 0 || to < 0 || from >= (int)m_humanTriggers.size() ||
            to >= (int)m_humanTriggers.size() || from == to) return;
        auto it = m_humanTriggers.begin();
        PendingTrigger tmp = std::move(m_humanTriggers[static_cast<size_t>(from)]);
        m_humanTriggers.erase(it + from);
        m_humanTriggers.insert(m_humanTriggers.begin() + to, std::move(tmp));
    }

    // Resolve the front queued human trigger with the given targets, then drain SBAs.
    void resolveHumanTrigger(const std::vector<Target>& targets);

    // Drain and immediately resolve any pending triggers queued in GameState.
    // Called automatically after each stack resolution and after mana abilities.
    void drainPendingTriggers();

    // Cast a madness card from exile for its madness cost (or free if cost is 0).
    // On failure (can't pay), moves the card to the graveyard instead.
    // Used by GameWindow after the human player confirms they want to cast.
    void castMadnessCard(ObjectId id, uint8_t ctrl);

    // Fire phase-begin triggers for the given step and drain them immediately.
    // Call this after TurnManager::beginStep() for each step that gives priority.
    void firePhaseTriggersAndDrain(TurnStep step, uint8_t activePlayer);

    // Peek at the top of the stack (for display / targeting).
    const StackAbility* top() const noexcept {
        return m_stack.empty() ? nullptr : &m_stack.back();
    }

    // Pay an activation cost string (e.g. "T", "R T", "2 G Sac<1/CARDNAME>").
    // Returns false if the cost can't be met (pool check, tap state, etc.).
    bool payActivationCost(const std::string& costStr, Card& source, uint8_t controller);

    // Companion special action: once per game, pay {3} from the player's mana
    // pool to move the set-aside companion into their hand. Returns false if the
    // player has no companion, already used it, or can't pay {3} from pool.
    bool activateCompanion(uint8_t pid);

    // Creates a new AbilityProcessor bound to newGame with the same stack and
    // trigger queue state.  Use together with GameState::clone() for simulation.
    AbilityProcessor cloneFor(GameState& newGame) const;

private:
    GameState&                m_game;
    std::vector<StackAbility> m_stack;         // LIFO — back() is the top
    std::deque<PendingTrigger> m_humanTriggers; // triggers awaiting human target input
    int m_triggerDepth = 0;  // recursion guard for drainPendingTriggers

    // Rewrite a spell's targets toward the redirector's opponent (ChangeTargets).
    // Player targets become that opponent; card targets become the opponent's most
    // valuable permanent matching the spell's ValidTgts (best for a harmful spell).
    void redirectSpellTargets(StackAbility& ability, uint8_t redirector);

    // Attempt to pay a mana cost from the player's pool.
    // Returns false without modifying the pool if it can't be paid.
    bool payCost(const ManaCost& cost, uint8_t controller);

    // After a non-permanent spell resolves, move its card to the graveyard.
    void moveResolvedSpellToGraveyard(ObjectId cardId, uint8_t ownerId);

    // Cascade: reveal cards from top until finding one with CMC < cascadingCmc,
    // then cast it for free. Remaining revealed cards go to the bottom.
    void processCascade(int cascadingCmc, uint8_t controller);
};

} // namespace mtg
