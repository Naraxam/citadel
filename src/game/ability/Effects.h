#pragma once
#include "ScriptLine.h"
#include "EffectContext.h"

namespace mtg {

// Dispatches a parsed ScriptLine to the appropriate handler and executes it.
// Returns false if the effect type is unrecognised.
bool executeEffect(const ScriptLine& script, EffectContext& ctx);

// Execute script and follow any SubAbility$ chain through the source card's SVars.
void executeEffectChain(const ScriptLine& script, EffectContext& ctx);

// ── Individual handlers (public so tests can call them directly) ──────────────

void effectDealDamage   (const ScriptLine& s, EffectContext& ctx);
void effectMana         (const ScriptLine& s, EffectContext& ctx);
void effectDraw         (const ScriptLine& s, EffectContext& ctx);
void effectGainLife     (const ScriptLine& s, EffectContext& ctx);
void effectDestroy      (const ScriptLine& s, EffectContext& ctx);
void effectCounter      (const ScriptLine& s, EffectContext& ctx);
void effectPump         (const ScriptLine& s, EffectContext& ctx);
void effectChangeZone   (const ScriptLine& s, EffectContext& ctx);

// Phase 8 additions
void effectToken        (const ScriptLine& s, EffectContext& ctx);
void effectPutCounter   (const ScriptLine& s, EffectContext& ctx);
void effectPumpAll      (const ScriptLine& s, EffectContext& ctx);
void effectChangeZoneAll(const ScriptLine& s, EffectContext& ctx);
void effectDamageAll    (const ScriptLine& s, EffectContext& ctx);
void effectDiscard      (const ScriptLine& s, EffectContext& ctx);
void effectTap          (const ScriptLine& s, EffectContext& ctx);
void effectCharm        (const ScriptLine& s, EffectContext& ctx);
void effectDig          (const ScriptLine& s, EffectContext& ctx);
void effectScry         (const ScriptLine& s, EffectContext& ctx);
void effectRegenerate   (const ScriptLine& s, EffectContext& ctx);
void effectAttach       (const ScriptLine& s, EffectContext& ctx);
void effectEffect       (const ScriptLine& s, EffectContext& ctx);

// Phase 10 additions
void effectMill          (const ScriptLine& s, EffectContext& ctx);
void effectGainAbility   (const ScriptLine& s, EffectContext& ctx);
void effectGainControl   (const ScriptLine& s, EffectContext& ctx);
void effectShuffle       (const ScriptLine& s, EffectContext& ctx);
void effectPreventDamage (const ScriptLine& s, EffectContext& ctx);
void effectFight         (const ScriptLine& s, EffectContext& ctx);
void effectSetState      (const ScriptLine& s, EffectContext& ctx); // mass Tap/Untap
void effectRemoveCounter (const ScriptLine& s, EffectContext& ctx);
void effectExplore       (const ScriptLine& s, EffectContext& ctx); // Ixalan Explore

// Phase 12 additions
void effectSurveil                (const ScriptLine& s, EffectContext& ctx); // look at top N, optionally GY
void effectProliferate            (const ScriptLine& s, EffectContext& ctx); // add one of each counter
void effectPutCounterAll          (const ScriptLine& s, EffectContext& ctx); // put counter on all matching
void effectRearrangeTopOfLibrary  (const ScriptLine& s, EffectContext& ctx); // Ponder-style look + reorder

// Phase 13 additions
void effectBranch       (const ScriptLine& s, EffectContext& ctx); // conditional SubAbility fork
void effectAnimate      (const ScriptLine& s, EffectContext& ctx); // turn permanent into creature EOT
void effectConnive      (const ScriptLine& s, EffectContext& ctx); // draw+discard, counter if nonland

// Phase 14+ additions
void effectInvestigate      (const ScriptLine& s, EffectContext& ctx); // create Clue tokens
void effectCopyPermanent    (const ScriptLine& s, EffectContext& ctx); // token copy of permanent (Populate)
void effectPlay             (const ScriptLine& s, EffectContext& ctx); // cast card for free
void effectDigUntil         (const ScriptLine& s, EffectContext& ctx); // reveal until type found
void effectReveal           (const ScriptLine& s, EffectContext& ctx); // reveal cards from hand
void effectLookAt           (const ScriptLine& s, EffectContext& ctx); // look at top N cards (AI no-op)
void effectFlipCoin         (const ScriptLine& s, EffectContext& ctx); // 50/50 WinSub/LoseSub
void effectRepeatEach       (const ScriptLine& s, EffectContext& ctx); // repeat SubAbility per card
void effectStoreSVar        (const ScriptLine& s, EffectContext& ctx); // store count into SVar
void effectChoosePlayer     (const ScriptLine& s, EffectContext& ctx); // AI: choose a player (default: opponent)
void effectChooseType       (const ScriptLine& s, EffectContext& ctx); // AI: choose a card type (default: Creature)
void effectRollDice         (const ScriptLine& s, EffectContext& ctx); // roll N-sided die, store result
void effectRepeat           (const ScriptLine& s, EffectContext& ctx); // conditional loop sub-ability
void effectGoad             (const ScriptLine& s, EffectContext& ctx); // goad a creature
void effectRemoveFromCombat (const ScriptLine& s, EffectContext& ctx); // remove from combat
void effectAddTurn          (const ScriptLine& s, EffectContext& ctx); // grant extra turn(s)
void effectSkipTurn         (const ScriptLine& s, EffectContext& ctx); // skip a turn
void effectLifeSet          (const ScriptLine& s, EffectContext& ctx); // set life total
void effectControlExchange  (const ScriptLine& s, EffectContext& ctx); // swap controllers
void effectZoneExchange     (const ScriptLine& s, EffectContext& ctx); // swap two cards' zones
void effectAmass            (const ScriptLine& s, EffectContext& ctx); // Amass N Army counters
void effectDiscover         (const ScriptLine& s, EffectContext& ctx); // Discover N
void effectManifest         (const ScriptLine& s, EffectContext& ctx); // face-down 2/2 from library
void effectIncubate         (const ScriptLine& s, EffectContext& ctx); // create Incubator token
void effectLearn            (const ScriptLine& s, EffectContext& ctx); // Learn mechanic
void effectMultiplyCounter  (const ScriptLine& s, EffectContext& ctx); // double counters
void effectMoveCounter      (const ScriptLine& s, EffectContext& ctx); // move counters between cards
void effectUnattach         (const ScriptLine& s, EffectContext& ctx); // detach equipment
void effectProtect          (const ScriptLine& s, EffectContext& ctx); // grant protection from color
void effectAnimateAll       (const ScriptLine& s, EffectContext& ctx); // mass animate permanents
void effectBalance          (const ScriptLine& s, EffectContext& ctx); // sacrifice to minimum count
void effectPhasing          (const ScriptLine& s, EffectContext& ctx); // phase out a permanent
void effectVote             (const ScriptLine& s, EffectContext& ctx); // vote mechanic (simplified)
void effectChooseColor      (const ScriptLine& s, EffectContext& ctx); // choose a color
void effectChooseName       (const ScriptLine& s, EffectContext& ctx); // choose a card name
void effectNameCard         (const ScriptLine& s, EffectContext& ctx); // name a card (Anointed Peacekeeper)
void effectTwoPiles         (const ScriptLine& s, EffectContext& ctx); // split into two piles
void effectLifeExchange     (const ScriptLine& s, EffectContext& ctx); // swap life totals
void effectDetain           (const ScriptLine& s, EffectContext& ctx); // detain a permanent
void effectImmediateTrigger (const ScriptLine& s, EffectContext& ctx); // execute trigger immediately
void effectDelayedTrigger   (const ScriptLine& s, EffectContext& ctx); // register delayed trigger
void effectSacrificeAll     (const ScriptLine& s, EffectContext& ctx); // sacrifice matching permanents
void effectDestroyAll       (const ScriptLine& s, EffectContext& ctx); // destroy matching permanents
void effectAlterAttribute   (const ScriptLine& s, EffectContext& ctx); // set/clear game-mechanic attributes
void effectMakeCard         (const ScriptLine& s, EffectContext& ctx); // conjure/create a named card
void effectPeekAndReveal    (const ScriptLine& s, EffectContext& ctx); // peek at library top, reveal matching
void effectPopulate         (const ScriptLine& s, EffectContext& ctx); // copy creature token
void effectCopySpell        (const ScriptLine& s, EffectContext& ctx); // copy a spell on the stack
void effectAddPhase         (const ScriptLine& s, EffectContext& ctx); // add extra phase
void effectMustBlock        (const ScriptLine& s, EffectContext& ctx); // force block
void effectSeek             (const ScriptLine& s, EffectContext& ctx); // random library search

// Phase 15+ additions
void effectSacrifice        (const ScriptLine& s, EffectContext& ctx); // sacrifice matching permanents
void effectBolster          (const ScriptLine& s, EffectContext& ctx); // +N/+N on lowest-toughness creature
void effectMonstrosity      (const ScriptLine& s, EffectContext& ctx); // put counters + set monstrous
void effectRegenerateAll    (const ScriptLine& s, EffectContext& ctx); // regenerate all matching creatures
void effectExert            (const ScriptLine& s, EffectContext& ctx); // card doesn't untap next turn
void effectTransform        (const ScriptLine& s, EffectContext& ctx); // flip DFC to other face
void effectCleanup          (const ScriptLine& s, EffectContext& ctx); // SubAbility chain cleanup (ClearRemembered$)
void effectChooseCard       (const ScriptLine& s, EffectContext& ctx); // AI: choose best card matching filter
void effectSetColor         (const ScriptLine& s, EffectContext& ctx); // override a card's color identity
void effectBecomeMonarch    (const ScriptLine& s, EffectContext& ctx); // set the Monarch
void effectForetell         (const ScriptLine& s, EffectContext& ctx); // exile card from hand as foretold
void effectClassLevelUp     (const ScriptLine& s, EffectContext& ctx); // advance a Class enchantment's level
void effectMeld             (const ScriptLine& s, EffectContext& ctx); // exile 2 components, create meld permanent
void effectMutate           (const ScriptLine& s, EffectContext& ctx); // merge mutate spell onto target
void effectDayTime          (const ScriptLine& s, EffectContext& ctx); // set/flip day/night state
void effectTakeInitiative   (const ScriptLine& s, EffectContext& ctx); // take the initiative
void effectVenture          (const ScriptLine& s, EffectContext& ctx); // venture into the dungeon
void effectClone            (const ScriptLine& s, EffectContext& ctx); // a permanent becomes a copy of another

} // namespace mtg
