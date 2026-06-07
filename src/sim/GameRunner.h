#pragma once
#include "../game/GameState.h"
#include "../game/TurnManager.h"
#include "../game/ability/AbilityProcessor.h"
#include "../game/ai/AiPlayer.h"
#include "../game/ai/MctsSearch.h"
#include "../core/db/CardDb.h"
#include <array>
#include <functional>
#include <iosfwd>
#include <string>
#include <vector>

namespace mtg {

// Headless AI-vs-AI game runner used for self-play data collection.
//
// State encoding (264 floats, perspective of POV player) — Commander format:
//   Global [0..13]:
//     [0]  own life / 40        [1]  opp life / 40
//     [2]  own hand size / 7    [3]  opp hand size / 7
//     [4]  own library / 100    [5]  opp library / 100
//     [6]  own land count / 10  [7]  opp land count / 10
//     [8]  own GY size / 20     [9]  opp GY size / 20
//     [10] turn / 40            [11] own poison / 10
//     [12] opp poison / 10      [13] own available mana / 10
//   Own creatures [14..85]: 12 slots × 6 floats
//     power/10, toughness/10, keywords/128, +1/+1 counters/5,
//     tapped+sick flags (0|1|2|3), markedDamage/10
//   Opp creatures [86..157]: same structure
//   Own non-creature permanents [158..189]: 8 slots × 4 floats
//     cmc/10, type-bits/8, loyalty/5, tapped (0 or 1)
//   Opp non-creature permanents [190..221]: same structure
//   GY breakdown [222..229]:
//     own creature GY/10, own spell GY/10, opp creature GY/10, opp spell GY/10
//     own exile count/10, opp exile count/10, own land GY/5, opp land GY/5
//   Hand CMC [230..236]: own hand cards (up to 7), sorted CMC desc / 10
//   Own WUBRG land counts [237..241]: W U B R G / 10
//   Opp WUBRG land counts [242..246]: W U B R G / 10
//   Stack [247..254]: 4 slots × (cmc/10, is_pov_controller)
//   [255] own hand land count / 7
//   Commander [256..263]:
//     [256] own commander damage received / 21
//     [257] opp commander damage received / 21
//     [258] own commander cast count / 5   (tax indicator)
//     [259] opp commander cast count / 5
//     [260] own commander in command zone (0|1)
//     [261] opp commander in command zone (0|1)
//     [262] own commander CMC / 10
//     [263] opp commander CMC / 10
//   Extended hand breakdown [264..265]:
//     [264] own hand creature count / 7
//     [265] own hand spell count / 7
//   Commander on-battlefield P/T [266..267]:
//     [266] own commander power / 10 when on BF (0 if not in play)
//     [267] own commander toughness / 10 when on BF
class GameRunner {
public:
    static constexpr int kStateSize = 268;
    using StateVec = std::array<float, kStateSize>;

    // A full game episode recording: states, actions, and outcome.
    struct Episode {
        std::vector<StateVec>                states;   // board state before each main-phase decision
        std::vector<int>                     actions;  // 0 = normal play; N = combo index N-1 fired
        std::vector<float>                   rewards;  // +1.0 winner, -1.0 loser (set at end)
        std::vector<std::vector<float>>      policy;   // MCTS visit distribution per decision step
        int                                  winner = -1;
        int                                  turns  = 0;
    };

    // Construct a runner for the given pair of deck files.
    GameRunner(const CardDb& db,
               const std::string& deck0Path,
               const std::string& deck1Path);

    // Optional per-turn callback fired after each player's takeTurn() completes.
    // Receives the running turn counter (increments each half-turn).
    void setTurnCallback(std::function<void(int turn)> cb) { m_turnCb = std::move(cb); }

    // Enforce a minimum wall-clock duration per half-turn.
    // If a turn finishes faster, the thread sleeps for the remainder.
    // Default 0 = no throttle.
    void setMinTurnMs(int ms) { m_turnMinMs = ms; }

    // Attach a debug log stream. When set, GameRunner emits per-half-turn
    // timing lines and the AiPlayer emits loop-guard warnings.
    // Caller owns the stream lifetime.
    void setDebugLog(std::ostream* log) { m_debugLog = log; }

    // Enable / disable MCTS for both AI players in subsequent games.
    void setMcts(bool on, const MctsConfig& cfg = {}) noexcept {
        m_mctsEnabled = on; m_mctsConfig = cfg;
    }

    // Set the neural-net value function used by MCTS (shared across both players).
    // Pass an empty ValueFn to revert to the built-in heuristic evaluator.
    void setValueFn(ValueFn fn) noexcept { m_valueFn = std::move(fn); }

    // Per-player value functions for asymmetric evaluation (ELO matchups).
    // When set, these override m_valueFn for the respective player.
    void setValueFnP0(ValueFn fn) noexcept { m_valueFnP0 = std::move(fn); }
    void setValueFnP1(ValueFn fn) noexcept { m_valueFnP1 = std::move(fn); }

    // Policy prior function: guides PUCT action selection using the policy head.
    // Pass an empty PolicyFn to revert to uniform priors.
    void setPolicyFn(PolicyFn fn) noexcept { m_policyFn = std::move(fn); }

    // Batched value function — when set, MCTS evaluates leaves in groups of 8
    // and calls this function with the whole batch, enabling a single GPU forward
    // pass instead of N sequential calls.  Overrides setValueFn for leaf rollouts.
    void setBatchValueFn(BatchValueFn fn) noexcept { m_batchValueFn = std::move(fn); }

    // Play one complete game.  Returns the winner (0 or 1) or -1 if maxTurns
    // is reached without a winner (treated as a draw for training purposes).
    int runGame(int maxTurns = 200);

    int lastTurns()  const { return m_lastTurns; }
    int lastWinner() const { return m_lastWinner; }

    // Record a full episode (state/action/reward triples) for one game.
    Episode runEpisode(int maxTurns = 200);

    // Play `games` games, writing one CSV row per game to outPath.
    // Columns: winner, turns.
    void runBatch(int games, const std::string& outPath, int maxTurns = 200);

    // Encode the current game state from `pov` player's perspective.
    static StateVec encodeState(const GameState& game, uint8_t pov);

    // Print a one-line summary of the last game result.
    void printLastResult() const;

private:
    const CardDb&   m_db;
    std::string     m_deck0;
    std::string     m_deck1;

    std::function<void(int)> m_turnCb;
    int           m_turnMinMs  = 0;
    std::ostream* m_debugLog   = nullptr;
    bool          m_mctsEnabled = false;
    MctsConfig    m_mctsConfig;
    ValueFn       m_valueFn;
    ValueFn       m_valueFnP0;  // overrides m_valueFn for P0 when set
    ValueFn       m_valueFnP1;  // overrides m_valueFn for P1 when set
    PolicyFn      m_policyFn;
    BatchValueFn  m_batchValueFn;  // GPU-batched leaf evaluator

    // State from the last game (filled by runGame / runEpisode).
    int m_lastWinner = -1;
    int m_lastTurns  = 0;

    // Set up a fresh game in the provided objects.
    bool setupGame(GameState& game,
                   AbilityProcessor& abilities,
                   TurnManager& tm) const;
};

} // namespace mtg
