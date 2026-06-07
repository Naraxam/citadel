#pragma once
#include "ZoneType.h"
#include "ObjectId.h"
#include "TurnStep.h"
#include "ability/ScriptLine.h"
#include "ability/Target.h"
#include <vector>
#include <cstdint>

namespace mtg {

class GameState;
struct Card;
struct CardRules;

// A trigger that fired and is waiting to be executed.
struct PendingTrigger {
    ObjectId   sourceCardId;               // the card whose trigger fired
    uint8_t    controllerId;
    ScriptLine effect;                     // the DB$ effect to execute when it resolves
    ObjectId   triggeredCardId = kInvalidId; // the card that caused the trigger
                                           // (e.g. the creature that ETB'd or died)
    uint8_t    triggerPlayer  = 255;       // player who caused the trigger (255 = none)
    int        triggerAmount  = 0;         // numeric amount (damage, life, etc.) for TriggerCount$
};

// TriggerSystem checks card trigger lines against game events and queues
// effects for the AbilityProcessor to drain.
//
// Supported trigger modes (Mode$ values):
//   ChangesZone — fires when a card enters/leaves a zone
//   Attacks     — fires when a creature attacks
//
// Each trigger check appends any matching PendingTriggers to the provided
// vector; the caller owns the vector and hands it to AbilityProcessor.
class TriggerSystem {
public:
    // Call after a card has moved to a new zone.
    // out receives any triggers that fired.
    static void onZoneChange(const Card& card,
                              ZoneType    from,
                              ZoneType    to,
                              const GameState& game,
                              std::vector<PendingTrigger>& out);

    // Call when a creature has been declared as an attacker.
    // Fires the attacker's own "whenever ~ attacks" triggers AND
    // any global "whenever a creature attacks" watchers on the battlefield.
    static void onAttack(const Card& attacker,
                          const GameState& game,
                          std::vector<PendingTrigger>& out);

    // Call when a creature has been declared as a blocker.
    // Fires the blocker's own "whenever ~ blocks" triggers AND
    // any global "whenever a creature blocks" watchers on the battlefield.
    static void onBlock(const Card& blocker,
                         const Card& attacker,
                         const GameState& game,
                         std::vector<PendingTrigger>& out);

    // Call when an attacker is confirmed unblocked (no legal blockers assigned).
    // Fires T:Mode$ AttackerUnblocked triggers for matching watchers.
    static void onAttackerUnblocked(const Card& attacker,
                                     const GameState& game,
                                     std::vector<PendingTrigger>& out);

    // Call at the beginning of each step/phase.
    // Fires "at the beginning of your upkeep / draw / end step…" triggers.
    static void onPhaseBegin(TurnStep step, uint8_t activePlayer,
                              const GameState& game,
                              std::vector<PendingTrigger>& out);

    // Call when a spell is cast (placed on the stack).
    // targets: optional list of targets chosen for the spell (used for Heroic).
    static void onSpellCast(const Card& spell, uint8_t controller,
                             const GameState& game,
                             std::vector<PendingTrigger>& out,
                             const std::vector<Target>* targets = nullptr);

    // Call after a creature becomes monstrous (effectMonstrosity sets the flag).
    // Fires T:Mode$ BecomesMonstrous triggers on the creature and global watchers.
    static void onBecomesMonstrous(const Card& source,
                                   const GameState& game,
                                   std::vector<PendingTrigger>& out);

    // Call when a source deals damage.
    // isCombat: true for combat damage, false for spell/ability damage.
    // toPlayer: true if target is a player; in that case targetPlayer is valid.
    //           false if target is a card; targetId is the damaged card's id.
    static void onDamageDone(const Card& source, int amount, bool isCombat,
                              bool toPlayer, ObjectId targetId, uint8_t targetPlayer,
                              const GameState& game,
                              std::vector<PendingTrigger>& out);

    // Scan *all* battlefield cards for triggers that match a zone-change event.
    // Used for "when another creature enters" patterns.
    static void onZoneChangeGlobal(const Card& movedCard,
                                    ZoneType from, ZoneType to,
                                    const GameState& game,
                                    std::vector<PendingTrigger>& out);

    // Call after a card is cycled. Fires T:Mode$ Cycled triggers on the cycled card's
    // rules (the card itself is already in the graveyard by the time this fires).
    static void onCycle(const CardRules& cycledRules, ObjectId cycledCardId,
                         uint8_t controller, const GameState& game,
                         std::vector<PendingTrigger>& out);

    // Call after a player gains life. Fires T:Mode$ GainsLife triggers.
    static void onGainLife(uint8_t player, int amount,
                            const GameState& game,
                            std::vector<PendingTrigger>& out);

    // Call after a card is discarded (moved from Hand to another zone by effect).
    // discarded is the new card object in the graveyard/exile. discardingPlayer is
    // the controller who discarded.
    static void onDiscard(const Card& discarded, uint8_t discardingPlayer,
                           const GameState& game,
                           std::vector<PendingTrigger>& out);

    // Call after all combat damage has been dealt in a single combat damage step.
    // Fires T:Mode$ DamageDoneOnce triggers once per (watcher, target) pair where at
    // least one source matching ValidSource$ dealt damage to the target.
    // Each event records one source→target damage assignment from this combat step.
    struct CombatDamageEvent {
        const Card* source;      // the attacking/blocking card that dealt damage
        bool        toPlayer;
        uint8_t     targetPlayer;   // valid when toPlayer
        ObjectId    targetCardId;   // valid when !toPlayer
        int         amount;
    };
    static void onDamageDoneOnce(const std::vector<CombatDamageEvent>& events,
                                  const GameState& game,
                                  std::vector<PendingTrigger>& out);

    // Call after a player draws a card. drawNumber is cardsDrawnThisTurn AFTER increment.
    // drawnCardId may be kInvalidId if the drawn card is unknown (e.g. dredge replacement).
    // Fires T:Mode$ Drawn triggers filtered by ValidCard$ and Number$.
    static void onDraw(uint8_t drawingPlayer, int drawNumber, ObjectId drawnCardId,
                       const GameState& game,
                       std::vector<PendingTrigger>& out);

    // Call after a player loses life. Fires T:Mode$ LosesLife triggers.
    // player: the player who lost life. amount: how much life was lost.
    static void onLoseLife(uint8_t player, int amount,
                            const GameState& game,
                            std::vector<PendingTrigger>& out);

    // Call after a permanent becomes tapped (by any means). Fires T:Mode$ Taps triggers.
    static void onTap(const Card& tapped, const GameState& game,
                      std::vector<PendingTrigger>& out);

    // Call after a permanent becomes untapped (during untap step or by effect).
    // Fires T:Mode$ Untap triggers. Covers Inspired ("whenever this creature becomes untapped").
    static void onUntap(const Card& untapped, const GameState& game,
                        std::vector<PendingTrigger>& out);

    // Call after one or more counters are added to a permanent. Fires T:Mode$ CounterAdded.
    // target: the permanent that received counters. counterType: e.g. "+1/+1", "charge".
    // amount: number of counters added (>= 1).
    static void onCounterAdded(const Card& target, std::string_view counterType, int amount,
                               const GameState& game,
                               std::vector<PendingTrigger>& out);

    // Call after a land is played (moved from hand to battlefield as a land drop).
    // land: the land card now on the battlefield. controller: the player who played it.
    // Fires T:Mode$ LandPlayed triggers.
    static void onLandPlayed(const Card& land, uint8_t controller,
                             const GameState& game,
                             std::vector<PendingTrigger>& out);

    // Call after a permanent is sacrificed (just before or after zone change to GY).
    // sacrificed: the card being sacrificed. Fires T:Mode$ Sacrificed triggers.
    static void onSacrificed(const Card& sacrificed,
                             const GameState& game,
                             std::vector<PendingTrigger>& out);

    // Call after ALL attackers have been declared (end of Declare Attackers step).
    // Fires T:Mode$ Attacks triggers that include MaxAttackers$ or MinAttackers$
    // (e.g. Exalted: "fires when you attack with only one creature").
    static void onAttackersFinalized(const GameState& game,
                                     std::vector<PendingTrigger>& out);

    // Call after a DFC transforms (effectTransform changes the active face).
    // Fires T:Mode$ Transformed triggers on the card itself and global watchers.
    // The card is passed in its NEW face state (rules == back face after transform).
    static void onTransform(const Card& card,
                            const GameState& game,
                            std::vector<PendingTrigger>& out);

    // Call after a morph/megamorph creature is turned face up (isFaceDown set to false).
    // Fires T:Mode$ TurnFaceUp triggers on the card itself and global watchers.
    static void onTurnFaceUp(const Card& card,
                             const GameState& game,
                             std::vector<PendingTrigger>& out);

    // Call after a spell or ability is cast/activated that targets targetId.
    // sourceId: the spell/ability card (on the Stack or Battlefield).
    // sourceIsSpell: true if source is an Instant/Sorcery/etc. spell on the stack.
    // sourceController: the player who controls the targeting spell/ability.
    // Fires T:Mode$ BecomesTarget triggers for matching battlefield watchers.
    static void onBecomesTarget(ObjectId targetId, ObjectId sourceId,
                                bool sourceIsSpell, uint8_t sourceController,
                                const GameState& game,
                                std::vector<PendingTrigger>& out);

private:
    // Check whether a single trigger line matches the zone-change event and
    // appends to out if so.  Returns true if a trigger was found.
    static bool checkZoneChangeTrigger(const Card& triggerOwner,
                                        const ScriptLine& trig,
                                        const Card& movedCard,
                                        ZoneType from, ZoneType to,
                                        std::vector<PendingTrigger>& out,
                                        const GameState* game = nullptr);
};

} // namespace mtg
