#pragma once

namespace mtg {

class GameState;

// Applies all ContinuousEffect objects stored in GameState::m_continuousEffects
// to battlefield cards in Rule 613 layer order (1 → 2 → 3 → 4 → 5 → 6 → 7a → 7b → 7c → 7e).
//
// Called at the end of GameState::recomputeStaticBonuses() after the existing
// static-ability scan has already handled player-level effects and non-layered restrictions.
// Initially m_continuousEffects is empty, so this is a no-op until callers populate it.
class LayerEngine {
public:
    static void apply(GameState& gs);
};

} // namespace mtg
