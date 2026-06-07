#pragma once
#include "../GameState.h"
#include "../ability/AbilityProcessor.h"
#include "../ability/Target.h"
#include "../../core/mana/ManaCost.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace mtg {

class TurnManager;

struct MctsConfig {
    int   iterations     = 200;
    float c_puct         = 1.5f;
    float temperature    = 1.0f;
    int   tempCutoffTurn = 20;  // turns past this → argmax (τ=0)
};

struct MctsAction {
    enum class Kind { CastSpell, DeclareAttackers, AssignBlockers, Pass } kind = Kind::Pass;

    // CastSpell
    ObjectId              spellId      = kInvalidId;
    std::vector<Target>   spellTargets;
    ManaCost              spellCost;
    int                   spellTapCmc  = 0;

    // DeclareAttackers — IDs of creatures that will attack
    std::vector<ObjectId> attackerIds;

    // AssignBlockers — (blocker ID, attacker ID) pairs
    std::vector<std::pair<ObjectId, ObjectId>> blockerAssignments;
};

// Neural-net value function: returns value in [-1,+1] from playerId's perspective.
using ValueFn = std::function<float(const GameState&, uint8_t playerId)>;

// Batched value function: evaluates multiple (state, playerId) pairs in one GPU call.
// Returns a vector of values in the same order as the input pairs.
// When set, searchMainPhase uses this instead of ValueFn for leaf evaluation.
using BatchValueFn = std::function<
    std::vector<float>(const std::vector<std::pair<const GameState*, uint8_t>>&)>;

// Policy prior function: given a state and number of legal actions, returns a
// probability vector of length numActions used as PUCT priors.
// The network's policy head outputs POLICY_SIZE logits; this function converts
// them to per-action probabilities (softmax + truncate/pad to numActions).
using PolicyFn = std::function<std::vector<float>(const GameState&, uint8_t playerId, int numActions)>;

// Lightweight transposition table using a flat hash array.
// Maps a (life, board-size, hand-size) tuple to a cached value estimate,
// cutting redundant rollout evaluations when two spell orderings reach the
// same logical position.
struct TranspositionTable {
    static constexpr size_t kSize = 1u << 16;  // 64k entries, ~512 KB
    struct Entry { uint64_t key = 0; float value = 0.f; };
    std::array<Entry, kSize> data{};

    void store(uint64_t key, float value) noexcept {
        data[key % kSize] = {key, value};
    }
    bool probe(uint64_t key, float& value) const noexcept {
        const Entry& e = data[key % kSize];
        if (e.key == key) { value = e.value; return true; }
        return false;
    }
    static uint64_t hash(const GameState& gs, uint8_t pid) noexcept;
};

// Tree node for proper MCTS in searchMainPhase.
struct MctsNode {
    GameState                              state;
    std::vector<std::unique_ptr<MctsNode>> children;
    MctsAction                             action;     // action that led to this node
    MctsNode*                              parent = nullptr;
    int                                    visits = 0;
    float                                  totalValue = 0.f;
    float                                  prior  = 1.f;
    bool                                   expanded = false;

    float q()    const noexcept { return visits ? totalValue / visits : 0.f; }
};

// Flat PUCT search for three MTG decision points.
// Each call enumerates legal actions, allocates simulation budget via PUCT, and
// returns the selected action together with an optional visit-count distribution
// for use as a training policy target.
class MctsSearch {
public:
    // Main-phase spell sequencing (tree-based MCTS).
    // Returns the next action (Kind::Pass when no spell improves the position).
    // Caller executes the action and calls again until Pass is returned.
    //
    // persistRoot: when non-null, on each call the search checks whether the
    //   pointed-to node's state matches gs; if so it resumes from that subtree
    //   instead of rebuilding.  After picking an action, the chosen child is
    //   detached and stored back into *persistRoot for the next call.
    MctsAction searchMainPhase(
        const GameState& gs, const AbilityProcessor& srcAp, uint8_t playerId,
        const MctsConfig& cfg,
        std::vector<float>* visitDist = nullptr,
        const ValueFn& valueFn = {},
        const PolicyFn& policyFn = {},
        std::unique_ptr<MctsNode>* persistRoot = nullptr,
        const BatchValueFn& batchFn = {});

    // Attacker selection (Declare Attackers step).
    MctsAction searchAttackers(
        const GameState& gs, const AbilityProcessor& srcAp, uint8_t playerId,
        const MctsConfig& cfg,
        std::vector<float>* visitDist = nullptr,
        const ValueFn& valueFn = {},
        const PolicyFn& policyFn = {});

    // Blocker assignment (Declare Blockers step).
    // Reads the already-declared attacker list from `tm`.
    MctsAction searchBlockers(
        const GameState& gs, const AbilityProcessor& srcAp, TurnManager& tm,
        uint8_t playerId,
        const MctsConfig& cfg,
        std::vector<float>* visitDist = nullptr,
        const ValueFn& valueFn = {},
        const PolicyFn& policyFn = {});

private:
    struct ActionStats {
        int   visits     = 0;
        float totalValue = 0.f;
        float prior      = 1.f;  // policy network prior; uniform (1.f) when no PolicyFn set
    };

    // Action enumerators
    static std::vector<MctsAction> legalSpellActions   (const GameState& gs, uint8_t pid);
    static std::vector<MctsAction> legalAttackerActions(const GameState& gs, uint8_t pid);
    static std::vector<MctsAction> legalBlockerActions (const GameState& gs,
                                                        const CombatState& combat,
                                                        uint8_t pid);

    // Single-simulation evaluators (each clones gs internally)
    static float simulateSpell    (const GameState& gs, uint8_t pid,
                                   const MctsAction& a, const ValueFn& fn);
    static float simulateAttackers(const GameState& gs, uint8_t pid,
                                   const MctsAction& a, const ValueFn& fn);
    static float simulateBlockers (const GameState& gs, const CombatState& combat,
                                   uint8_t pid, const MctsAction& a, const ValueFn& fn);

    // Shared PUCT loop: runs unvisited actions first, then PUCT exploitation.
    // stats[i].prior is used as the policy prior P(i) in the PUCT formula.
    static void runFlatPuct(const std::vector<MctsAction>& actions,
                             std::vector<ActionStats>& stats,
                             int totalIterations, float c_puct,
                             const std::function<float(const MctsAction&)>& simFn);

    // Fill stats[i].prior from PolicyFn, or set uniform if policyFn is empty.
    static void applyPriors(std::vector<ActionStats>& stats,
                             const GameState& gs, uint8_t pid,
                             const PolicyFn& policyFn);

    // Tree MCTS helpers for searchMainPhase
    static MctsNode* selectLeaf(MctsNode* root, float c_puct);
    static void      expandNode(MctsNode* node, uint8_t pid,
                                 const PolicyFn& policyFn);
    static float     rolloutSpell(MctsNode* node, uint8_t pid, const ValueFn& fn);
    static void      backpropagate(MctsNode* node, float value);

    // Temperature-based action selection; fills visitDist if non-null.
    static MctsAction pickAction(const std::vector<MctsAction>& actions,
                                  const std::vector<ActionStats>& stats,
                                  float temperature, int turnNumber, int tempCutoffTurn,
                                  std::vector<float>* visitDist);

    // Eval of gs normalized to [-1,+1]; calls valueFn if set.
    // Uses a thread_local TT for caching repeated leaf evaluations.
    static float evalState(GameState& gs, uint8_t pid, const ValueFn& fn);
};

} // namespace mtg
