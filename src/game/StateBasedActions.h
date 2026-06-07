#pragma once

namespace mtg {

class GameState;

// State-based actions (SBAs) are game rules that apply automatically before
// any player receives priority. They must be checked repeatedly until none
// fire in a single pass.
//
// Implemented:
//   - Creature with toughness <= 0 dies (ignores Indestructible)
//   - Creature with lethal damage or deathtouch damage dies (respects Indestructible / Regeneration)
//   - Player at 0 or less life loses
//   - Player with 10+ poison counters loses
//   - Planeswalker with 0 loyalty dies
//   - Legendary rule (player keeps the most recently placed copy)
//   - Aura falls off when its attached permanent leaves the battlefield
//   - Saga sacrificed when lore counters reach or exceed the final chapter
//   - +1/+1 and -1/-1 counters on the same permanent cancel each other
//   - Undying / Persist return-from-death handling
//   - Modular counter transfer on death
//   - Champion return on death
//   - Soulshift Spirit return on death
class StateBasedActions {
public:
    // Run one pass of all SBAs on the game state.
    // Returns true if at least one action fired; caller should loop until false.
    static bool run(GameState& game);

    // Like run() but skips checkAlwaysTriggers.
    // Use inside trigger-drain loops to prevent Always-trigger accumulation:
    // drainPendingTriggers() clears pending status before iterating, so every
    // Always-trigger card becomes re-eligible on each SBA pass, causing O(M)
    // new triggers per trigger resolved. runBasic() breaks that cycle while
    // still catching player death, creature death, etc.
    static bool runBasic(GameState& game);

private:
    static bool checkCreatureDeath(GameState& game);
    static bool checkPlaneswalkerDeath(GameState& game);
    static bool checkPlayerLoss(GameState& game);
    // +1/+1 and -1/-1 counters on the same permanent cancel each other
    static bool checkCounterCancellation(GameState& game);
    // Legendary rule: each player can only control one legendary permanent with a given name
    static bool checkLegendaryRule(GameState& game);
    // Aura SBA: an Aura whose enchanted permanent is no longer on the battlefield goes to GY
    static bool checkAuraFallOff(GameState& game);
    // Saga SBA: a Saga whose lore counters equal or exceed its final chapter is sacrificed
    static bool checkSagaSacrifice(GameState& game);
    // Mode$ Always triggers: queue effects for cards whose state conditions are currently met
    // (e.g. "sacrifice when you control no Swamps", "sacrifice when no +1/+1 counters").
    static bool checkAlwaysTriggers(GameState& game);
};

} // namespace mtg
