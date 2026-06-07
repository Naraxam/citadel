#include "MctsSearch.h"
#include "AiPlayer.h"
#include "../CardStats.h"
#include "../CombatState.h"
#include "../KeywordAbility.h"
#include "../StateBasedActions.h"
#include "../TurnManager.h"
#include "../ability/ScriptLine.h"
#include <algorithm>
#include <cassert>
#include <future>
#include <mutex>
#include <cmath>
#include <memory>
#include <random>

namespace mtg {

// ── Internal helpers ───────────────────────────────────────────────────────────

// Normalized static eval in [-1,+1] from pid's perspective.
// Creates a throwaway AbilityProcessor / AiPlayer so we can call staticEval().
uint64_t TranspositionTable::hash(const GameState& gs, uint8_t pid) noexcept {
    // Very lightweight hash: encode life totals, board size, hand size, and turn.
    // Collisions are fine — the TT is a soft cache, not a correctness mechanism.
    uint64_t h = static_cast<uint64_t>(gs.player(pid).life()) * 1000003ULL
               ^ static_cast<uint64_t>(gs.player(pid ^ 1).life()) * 999983ULL
               ^ static_cast<uint64_t>(gs.battlefield().size())   * 998981ULL
               ^ static_cast<uint64_t>(gs.player(pid).hand().size()) * 997967ULL
               ^ static_cast<uint64_t>(gs.turnNumber())           * 99991ULL;
    return h;
}

float MctsSearch::evalState(GameState& gs, uint8_t pid, const ValueFn& fn) {
    // Thread-local TT: persists across sequential searchMainPhase calls on the
    // same thread (one MCTS worker per thread in self-play).
    thread_local TranspositionTable s_tt;

    uint64_t key = TranspositionTable::hash(gs, pid);
    float cached;
    if (s_tt.probe(key, cached)) return cached;

    float value;
    if (fn) {
        // Python ValueFn callback (training mode): use the live transformer.
        value = fn(const_cast<const GameState&>(gs), pid);
    } else if (AiPlayer::hasValueNet()) {
        // No Python callback (standalone game): use the C++ MLP distilled from
        // the trained transformer (data/value_net.bin, loaded once at startup).
        AbilityProcessor ap(gs);
        AiPlayer ai(pid, gs, ap);
        value = std::clamp(ai.netEval(), -1.f, 1.f);
    } else {
        // No learned signal at all: fall back to the hand-tuned heuristic.
        AbilityProcessor ap(gs);
        AiPlayer ai(pid, gs, ap);
        value = std::clamp(ai.staticEval() / 10000.f, -1.f, 1.f);
    }
    s_tt.store(key, value);
    return value;
}

// Returns true if `blocker` can legally block `attacker` (flying/reach/shadow only).
static bool canBlock(const Card& blocker, const Card& attacker) {
    if (attacker.unblockable) return false;
    if (attacker.hasKeyword(KeywordAbility::Flying)) {
        if (!blocker.hasKeyword(KeywordAbility::Flying) &&
            !blocker.hasKeyword(KeywordAbility::Reach))
            return false;
    }
    if (attacker.hasKeyword(KeywordAbility::Shadow) !=
        blocker.hasKeyword(KeywordAbility::Shadow))
        return false;
    return true;
}

// Simplified target picker for MCTS action enumeration.
// Returns empty vector when no valid target exists for a required-target spell.
static std::vector<Target> pickMctsTargets(const CardRules& rules,
                                            const GameState& gs,
                                            uint8_t playerId) {
    bool isAura = rules.type.isEnchantment() && rules.type.hasSubtype("Aura");

    for (const auto& rawLine : rules.abilityLines) {
        auto script = parseScriptLine(rawLine);
        if (script.abilityType != "SP" && script.abilityType != "AB") continue;

        std::string vt = std::string(script.get("ValidTgts"));
        if (vt.empty()) return {};  // no target required

        // Aura: target own best creature
        if (isAura && (vt.find("Creature") != std::string::npos ||
                       vt.find("Permanent") != std::string::npos)) {
            Card* best = nullptr; int bestScore = -1;
            for (Card* c : gs.battlefield().cards()) {
                if (c->controllerId != playerId || !c->isCreature()) continue;
                if (c->cantBeTargeted || c->hasKeyword(KeywordAbility::Shroud)) continue;
                int s = effectivePower(*c) + effectiveToughness(*c);
                if (s > bestScore) { bestScore = s; best = c; }
            }
            return best ? std::vector<Target>{Target::forCard(best->id)}
                        : std::vector<Target>{};
        }

        // Counterspells / instants on stack
        if (vt.find("Spell") != std::string::npos ||
            vt.find("Instant") != std::string::npos) {
            const auto& sv = gs.stack().cards();
            for (int i = (int)sv.size() - 1; i >= 0; --i)
                if (sv[i] && sv[i]->controllerId != playerId)
                    return { Target::forCard(sv[i]->id) };
            return {};
        }

        // Player target
        if (vt.find("Player") != std::string::npos &&
            vt.find("Creature") == std::string::npos)
            return { Target::forPlayer(static_cast<uint8_t>(playerId ^ 1)) };

        // "Any": prefer best opponent creature, fall back to face
        if (vt == "Any") {
            Card* best = nullptr; int bestPow = -1;
            for (Card* c : gs.battlefield().cards()) {
                if (c->controllerId == playerId || !c->isCreature()) continue;
                if (c->cantBeTargeted || c->hasKeyword(KeywordAbility::Shroud)) continue;
                int p = effectivePower(*c);
                if (p > bestPow) { bestPow = p; best = c; }
            }
            if (best) return { Target::forCard(best->id) };
            return { Target::forPlayer(static_cast<uint8_t>(playerId ^ 1)) };
        }

        // Creature / Permanent
        if (vt.find("Creature") != std::string::npos ||
            vt.find("Permanent") != std::string::npos) {
            bool wantOwn = vt.find("MyCtrl") != std::string::npos;
            Card* best = nullptr; int bestScore = -1;
            for (Card* c : gs.battlefield().cards()) {
                if (wantOwn  && c->controllerId != playerId) continue;
                if (!wantOwn && c->controllerId == playerId) continue;
                if (!c->isCreature() && vt.find("Permanent") == std::string::npos) continue;
                if (c->cantBeTargeted) continue;
                if (c->hasKeyword(KeywordAbility::Shroud)) continue;
                if (!wantOwn && c->hasKeyword(KeywordAbility::Hexproof)) continue;
                int s = c->isCreature() ? effectivePower(*c) + effectiveToughness(*c) : 1;
                if (s > bestScore) { bestScore = s; best = c; }
            }
            return best ? std::vector<Target>{Target::forCard(best->id)}
                        : std::vector<Target>{};
        }
        break;
    }
    return {};
}

// ── Action enumerators ─────────────────────────────────────────────────────────

std::vector<MctsAction> MctsSearch::legalSpellActions(const GameState& gs, uint8_t pid) {
    std::vector<MctsAction> actions;
    MctsAction pass; pass.kind = MctsAction::Kind::Pass;
    actions.push_back(pass);

    const Player& me = gs.player(pid);

    // Available mana: pool + untapped lands
    int avail = me.manaPool().total();
    for (const Card* c : gs.battlefield().cards()) {
        if (c->controllerId == pid && !c->tapped && c->rules->type.isLand())
            ++avail;
    }

    auto requiresTarget = [](const CardRules& r) {
        for (const auto& raw : r.abilityLines) {
            auto s = parseScriptLine(raw);
            if (!s.get("ValidTgts").empty()) return true;
        }
        return false;
    };

    for (const Card* c : me.hand().cards()) {
        if (c->rules->type.isLand()) continue;
        if (avail < c->rules->manaCost.cmc()) continue;

        auto tgts = pickMctsTargets(*c->rules, gs, pid);
        if (requiresTarget(*c->rules) && tgts.empty()) continue;

        MctsAction a;
        a.kind        = MctsAction::Kind::CastSpell;
        a.spellId     = c->id;
        a.spellTargets= std::move(tgts);
        a.spellCost   = c->rules->manaCost;
        a.spellTapCmc = c->rules->manaCost.cmc();
        actions.push_back(std::move(a));
    }
    return actions;
}

std::vector<MctsAction> MctsSearch::legalAttackerActions(const GameState& gs, uint8_t pid) {
    std::vector<ObjectId> optional;
    std::vector<ObjectId> forced;  // mustAttack

    for (const Card* c : gs.battlefield().cards()) {
        if (c->controllerId != pid || !c->isCreature()) continue;
        if (c->tapped) continue;
        if (c->summoningSickness && !c->hasKeyword(KeywordAbility::Haste)) continue;
        if (c->cantAttack) continue;
        if (c->mustAttack) forced.push_back(c->id);
        else               optional.push_back(c->id);
    }

    std::vector<MctsAction> actions;

    int n = (int)optional.size();
    if (n <= 8) {
        // Full enumeration of all 2^n subsets (max 256)
        for (int mask = 0; mask < (1 << n); ++mask) {
            MctsAction a; a.kind = MctsAction::Kind::DeclareAttackers;
            a.attackerIds = forced;
            for (int i = 0; i < n; ++i)
                if (mask & (1 << i))
                    a.attackerIds.push_back(optional[i]);
            actions.push_back(std::move(a));
        }
    } else {
        // Always include all-in and none
        {
            MctsAction a; a.kind = MctsAction::Kind::DeclareAttackers;
            a.attackerIds = forced; actions.push_back(a);
        }
        {
            MctsAction a; a.kind = MctsAction::Kind::DeclareAttackers;
            a.attackerIds = forced;
            for (auto id : optional) a.attackerIds.push_back(id);
            actions.push_back(std::move(a));
        }
        // Sample up to 198 random subsets
        thread_local std::mt19937 rng(std::random_device{}());
        for (int s = 0; s < 198; ++s) {
            MctsAction a; a.kind = MctsAction::Kind::DeclareAttackers;
            a.attackerIds = forced;
            for (auto id : optional)
                if (rng() & 1) a.attackerIds.push_back(id);
            actions.push_back(std::move(a));
        }
    }
    return actions;
}

std::vector<MctsAction> MctsSearch::legalBlockerActions(const GameState& gs,
                                                         const CombatState& combat,
                                                         uint8_t pid) {
    // Collect attackers and eligible blockers
    std::vector<const Card*> attackers;
    for (const auto& atk : combat.attacks) {
        const Card* c = gs.findCard(atk.attackerId);
        if (c) attackers.push_back(c);
    }

    std::vector<const Card*> blockers;
    for (const Card* c : gs.battlefield().cards()) {
        if (c->controllerId != pid || !c->isCreature() || c->tapped) continue;
        if (combat.isBlocking(c->id) || combat.isAttacking(c->id)) continue;
        blockers.push_back(c);
    }

    // Base case: nothing to block or no blockers
    MctsAction noBlock; noBlock.kind = MctsAction::Kind::AssignBlockers;
    if (attackers.empty() || blockers.empty()) return {noBlock};

    int m = (int)blockers.size();
    int k = (int)attackers.size();

    // Compute total combinations = (k+1)^m; cap enumeration
    long long total = 1;
    for (int i = 0; i < m && total <= 256; ++i) total *= (k + 1);

    std::vector<MctsAction> actions;

    if (total <= 128) {
        // Enumerate all assignments using a digit counter
        std::vector<int> assign(m, k);  // k = not blocking

        auto makeAction = [&]() {
            MctsAction a; a.kind = MctsAction::Kind::AssignBlockers;
            for (int i = 0; i < m; ++i)
                if (assign[i] < k && canBlock(*blockers[i], *attackers[assign[i]]))
                    a.blockerAssignments.push_back({blockers[i]->id, attackers[assign[i]]->id});
            return a;
        };

        actions.push_back(makeAction());  // initial: all not blocking

        while (true) {
            int pos = 0;
            while (pos < m) {
                ++assign[pos];
                if (assign[pos] <= k) break;
                assign[pos] = 0;
                ++pos;
            }
            if (pos >= m) break;
            actions.push_back(makeAction());
        }
    } else {
        // Sample-based: always add no-block and max-block, then random
        actions.push_back(noBlock);

        {
            MctsAction maxBlock; maxBlock.kind = MctsAction::Kind::AssignBlockers;
            for (int i = 0; i < m; ++i) {
                int best = -1; int bestPow = -1;
                for (int j = 0; j < k; ++j) {
                    if (!canBlock(*blockers[i], *attackers[j])) continue;
                    int p = effectivePower(*attackers[j]);
                    if (p > bestPow) { bestPow = p; best = j; }
                }
                if (best >= 0)
                    maxBlock.blockerAssignments.push_back({blockers[i]->id, attackers[best]->id});
            }
            actions.push_back(std::move(maxBlock));
        }

        thread_local std::mt19937 rng(std::random_device{}());
        std::uniform_int_distribution<int> dist(0, k);  // k = not blocking
        for (int s = 0; s < 198; ++s) {
            MctsAction a; a.kind = MctsAction::Kind::AssignBlockers;
            for (int i = 0; i < m; ++i) {
                int choice = dist(rng);
                if (choice < k && canBlock(*blockers[i], *attackers[choice]))
                    a.blockerAssignments.push_back({blockers[i]->id, attackers[choice]->id});
            }
            actions.push_back(std::move(a));
        }
    }
    return actions;
}

// ── Simulation functions ───────────────────────────────────────────────────────

float MctsSearch::simulateSpell(const GameState& gs, uint8_t pid,
                                 const MctsAction& a, const ValueFn& fn) {
    auto simPtr = std::make_unique<GameState>(gs);  // heap: keeps ~4KB off the stack
    GameState& sim = *simPtr;
    AbilityProcessor ap(sim);
    AiPlayer simAi(pid, sim, ap);

    if (a.kind == MctsAction::Kind::Pass)
        return evalState(sim, pid, fn);

    simAi.tapAllMana();
    if (!ap.castSpell(a.spellId, pid, a.spellTargets))
        return evalState(sim, pid, fn);
    ap.resolveTop();
    while (StateBasedActions::run(sim)) {}
    ap.drainPendingTriggers();
    return evalState(sim, pid, fn);
}

float MctsSearch::simulateAttackers(const GameState& gs, uint8_t pid,
                                     const MctsAction& a, const ValueFn& fn) {
    auto simPtr = std::make_unique<GameState>(gs);
    GameState& sim = *simPtr;
    AbilityProcessor ap(sim);
    TurnManager tm(sim);
    uint8_t oppId = pid ^ 1;

    for (ObjectId id : a.attackerIds)
        tm.declareAttacker(id, oppId);
    ap.drainPendingTriggers();

    if (!tm.combatState().empty()) {
        // Simulate opponent's blocking using their heuristic
        AiPlayer defender(oppId, sim, ap);
        defender.declareBlockers(tm);

        if (tm.hasFirstStrikers()) {
            tm.dealCombatDamage(true);
            while (StateBasedActions::run(sim)) {}
            ap.drainPendingTriggers();
        }

        tm.dealCombatDamage(false);
        while (StateBasedActions::run(sim)) {}
        ap.drainPendingTriggers();
    }
    return evalState(sim, pid, fn);
}

float MctsSearch::simulateBlockers(const GameState& gs, const CombatState& combat,
                                    uint8_t pid, const MctsAction& a, const ValueFn& fn) {
    auto simPtr = std::make_unique<GameState>(gs);
    GameState& sim = *simPtr;
    AbilityProcessor ap(sim);
    TurnManager tm(sim);

    // Copy the existing combat state (attackers already declared externally)
    tm.mutableCombatState() = combat;

    for (const auto& [blockerId, attackerId] : a.blockerAssignments)
        tm.declareBlocker(blockerId, attackerId);

    if (tm.hasFirstStrikers()) {
        tm.dealCombatDamage(true);
        while (StateBasedActions::run(sim)) {}
        ap.drainPendingTriggers();
    }

    tm.dealCombatDamage(false);
    while (StateBasedActions::run(sim)) {}
    ap.drainPendingTriggers();
    return evalState(sim, pid, fn);
}

// ── Prior initialisation ──────────────────────────────────────────────────────

void MctsSearch::applyPriors(std::vector<ActionStats>& stats,
                               const GameState& gs, uint8_t pid,
                               const PolicyFn& policyFn) {
    int n = (int)stats.size();
    if (!policyFn || n == 0) {
        // Uniform prior: 1 / n (keeps PUCT formula dimensionally correct)
        float p = 1.0f / n;
        for (auto& s : stats) s.prior = p;
        return;
    }

    std::vector<float> priors = policyFn(gs, pid, n);
    if ((int)priors.size() != n) {
        // Mismatch — fall back to uniform
        float p = 1.0f / n;
        for (auto& s : stats) s.prior = p;
        return;
    }
    for (int i = 0; i < n; ++i)
        stats[i].prior = priors[i];
}

// ── PUCT loop ─────────────────────────────────────────────────────────────────

void MctsSearch::runFlatPuct(const std::vector<MctsAction>& actions,
                              std::vector<ActionStats>& stats,
                              int totalIterations, float c_puct,
                              const std::function<float(const MctsAction&)>& simFn) {
    int n = (int)actions.size();
    if (n == 0) return;

    // Adaptive budget: allocate at least 10 iters and at most the configured max.
    // With many actions we need proportionally more budget to visit all of them;
    // with few actions we don't need to waste iterations on re-visiting.
    int effectiveIter = std::min(totalIterations, std::max(10, 15 * n));

    for (int it = 0; it < effectiveIter; ++it) {
        // Find first unvisited action; if all visited, apply PUCT
        int chosenIdx = -1;
        for (int i = 0; i < n; ++i) {
            if (stats[i].visits == 0) { chosenIdx = i; break; }
        }

        if (chosenIdx < 0) {
            // All visited: PUCT with policy priors P(i)
            // PUCT(i) = Q(i) + c_puct * P(i) * sqrt(N) / (1 + n_i)
            int N = 0;
            for (const auto& s : stats) N += s.visits;
            float sqrtN = std::sqrt((float)N);
            float bestPuct = -1e9f;
            for (int i = 0; i < n; ++i) {
                float q  = stats[i].totalValue / stats[i].visits;
                float pu = q + c_puct * stats[i].prior * sqrtN / (1 + stats[i].visits);
                if (pu > bestPuct) { bestPuct = pu; chosenIdx = i; }
            }
        }

        float v = simFn(actions[chosenIdx]);
        stats[chosenIdx].visits++;
        stats[chosenIdx].totalValue += v;
    }
}

// ── Action selection ───────────────────────────────────────────────────────────

MctsAction MctsSearch::pickAction(const std::vector<MctsAction>& actions,
                                   const std::vector<ActionStats>& stats,
                                   float temperature, int turnNumber, int tempCutoffTurn,
                                   std::vector<float>* visitDist) {
    if (actions.empty()) {
        MctsAction pass; pass.kind = MctsAction::Kind::Pass;
        return pass;
    }

    if (visitDist) {
        visitDist->resize(actions.size());
        float total = 0.f;
        for (const auto& s : stats) total += s.visits;
        for (int i = 0; i < (int)actions.size(); ++i)
            (*visitDist)[i] = total > 0.f ? stats[i].visits / total : 1.f / actions.size();
    }

    // Argmax after temperature cutoff
    if (turnNumber > tempCutoffTurn || temperature < 0.01f) {
        int best = 0;
        for (int i = 1; i < (int)stats.size(); ++i)
            if (stats[i].visits > stats[best].visits) best = i;
        return actions[best];
    }

    // Temperature sampling: weight by visits^(1/τ)
    std::vector<float> weights(actions.size());
    float inv_temp = 1.f / temperature;
    for (int i = 0; i < (int)actions.size(); ++i)
        weights[i] = std::pow((float)std::max(stats[i].visits, 1), inv_temp);

    thread_local std::mt19937 rng(std::random_device{}());
    std::discrete_distribution<int> dist(weights.begin(), weights.end());
    return actions[dist(rng)];
}

// ── Tree MCTS helpers (for searchMainPhase) ───────────────────────────────────

MctsNode* MctsSearch::selectLeaf(MctsNode* root, float c_puct) {
    MctsNode* node = root;
    while (node->expanded && !node->children.empty()) {
        int N = node->visits;
        float sqrtN = std::sqrt((float)N);
        float best = -1e9f;
        MctsNode* chosen = nullptr;
        for (auto& child : node->children) {
            float q  = child->q();
            float pu = q + c_puct * child->prior * sqrtN / (1 + child->visits);
            if (pu > best) { best = pu; chosen = child.get(); }
        }
        if (!chosen) break;
        node = chosen;
    }
    return node;
}

void MctsSearch::expandNode(MctsNode* node, uint8_t pid, const PolicyFn& policyFn) {
    if (node->expanded) return;
    node->expanded = true;

    auto actions = legalSpellActions(node->state, pid);
    int n = (int)actions.size();
    if (n == 0) return;

    std::vector<float> priors(n, 1.0f / n);
    if (policyFn) {
        auto p = policyFn(node->state, pid, n);
        if ((int)p.size() == n) priors = std::move(p);
    }

    node->children.reserve(n);
    for (int i = 0; i < n; ++i) {
        auto child = std::make_unique<MctsNode>();
        child->action = actions[i];
        child->prior  = priors[i];
        child->parent = node;
        // Apply the action to get the child state
        child->state = node->state;
        if (actions[i].kind != MctsAction::Kind::Pass) {
            AbilityProcessor ap(child->state);
            AiPlayer simAi(pid, child->state, ap);
            simAi.tapAllMana();
            ap.castSpell(actions[i].spellId, pid, actions[i].spellTargets);
            ap.resolveTop();
            while (StateBasedActions::run(child->state)) {}
            ap.drainPendingTriggers();
        }
        node->children.push_back(std::move(child));
    }
}

float MctsSearch::rolloutSpell(MctsNode* node, uint8_t pid, const ValueFn& fn) {
    auto simPtr = std::make_unique<GameState>(node->state);
    return evalState(*simPtr, pid, fn);
}

void MctsSearch::backpropagate(MctsNode* node, float value) {
    while (node) {
        node->visits++;
        node->totalValue += value;
        node = node->parent;
        value = -value;  // negate for parent's perspective
    }
}

// ── Public search entry points ─────────────────────────────────────────────────

MctsAction MctsSearch::searchMainPhase(
    const GameState& gs, const AbilityProcessor& /*srcAp*/, uint8_t pid,
    const MctsConfig& cfg, std::vector<float>* visitDist,
    const ValueFn& fn, const PolicyFn& policyFn,
    std::unique_ptr<MctsNode>* persistRoot,
    const BatchValueFn& batchFn) {

    // Check if there are any spells to cast (beyond Pass)
    auto rootActions = legalSpellActions(gs, pid);
    if (rootActions.size() <= 1) {
        if (visitDist) { visitDist->clear(); visitDist->push_back(1.f); }
        if (persistRoot) persistRoot->reset();
        MctsAction pass; pass.kind = MctsAction::Kind::Pass;
        return pass;
    }

    // ── Tree reuse: try to inherit the chosen child from the previous call ────
    std::unique_ptr<MctsNode> root;
    if (persistRoot && *persistRoot) {
        // Check if any child of the persisted root matches the current game state.
        // We compare turn number and a lightweight hash; a mismatch just means
        // we rebuild from scratch (safe fallback).
        auto& prev = *persistRoot;
        for (auto& child : prev->children) {
            if (child->state.turnNumber() == gs.turnNumber() &&
                child->state.player(pid).hand().size() == gs.player(pid).hand().size() &&
                child->state.battlefield().size() == gs.battlefield().size()) {
                // Promote this child as the new root
                child->parent = nullptr;
                root = std::move(child);
                break;
            }
        }
    }

    if (!root) {
        // Build fresh root
        root = std::make_unique<MctsNode>();
        root->state = gs;
        root->visits = 1;
        expandNode(root.get(), pid, policyFn);
    }

    // Mix Dirichlet(α=0.3) noise into root priors to ensure exploration
    // even when the policy head is highly confident.  ε=0.25 matches AlphaZero.
    if (!root->children.empty()) {
        thread_local std::mt19937 noiseRng{std::random_device{}()};
        std::gamma_distribution<float> gamma(0.3f, 1.0f);
        size_t nc = root->children.size();
        std::vector<float> noise(nc);
        float noiseSum = 0.0f;
        for (auto& x : noise) { x = gamma(noiseRng); noiseSum += x; }
        if (noiseSum > 1e-8f) {
            constexpr float eps = 0.25f;
            for (size_t i = 0; i < nc; ++i) {
                root->children[i]->prior =
                    (1.f - eps) * root->children[i]->prior
                    + eps * (noise[i] / noiseSum);
            }
        }
    }

    int effectiveIter = std::min(cfg.iterations,
                                  std::max(10, 15 * (int)root->children.size()));

    // Batch size: group leaf evaluations when a BatchValueFn is available.
    // Batching 8 leaves per GPU call gives ~5-8× throughput vs one-by-one.
    constexpr int kBatch = 8;

    if (batchFn) {
        // ── Batched evaluation path ────────────────────────────────────────────
        for (int it = 0; it < effectiveIter; it += kBatch) {
            int thisBatch = std::min(kBatch, effectiveIter - it);
            std::vector<MctsNode*> leaves;
            leaves.reserve(static_cast<size_t>(thisBatch));

            for (int b = 0; b < thisBatch; ++b) {
                MctsNode* leaf = selectLeaf(root.get(), cfg.c_puct);
                // Apply virtual loss to discourage other threads from picking the
                // same leaf while we're evaluating (standard MCTS parallelism trick)
                leaf->visits++;
                leaf->totalValue -= 1.f;

                if (leaf->visits > 1 && !leaf->expanded
                        && leaf->action.kind != MctsAction::Kind::Pass) {
                    expandNode(leaf, pid, policyFn);
                    if (!leaf->children.empty())
                        leaf = leaf->children.front().get();
                }
                leaves.push_back(leaf);
            }

            // Build batch query and evaluate
            std::vector<std::pair<const GameState*, uint8_t>> batch;
            batch.reserve(leaves.size());
            for (auto* l : leaves) batch.emplace_back(&l->state, pid);

            std::vector<float> values = batchFn(batch);

            // Back-prop and undo virtual loss
            for (int b = 0; b < (int)leaves.size(); ++b) {
                float v = (b < (int)values.size()) ? values[static_cast<size_t>(b)] : 0.f;
                leaves[static_cast<size_t>(b)]->visits--;
                leaves[static_cast<size_t>(b)]->totalValue += 1.f;
                backpropagate(leaves[static_cast<size_t>(b)], v);
            }
        }
    } else {
        // ── Parallel evaluation path ──────────────────────────────────────────
        // Divide iterations across up to 4 worker threads. Each thread runs a
        // separate set of leaf selections and rollouts, then back-propagates with
        // a mutex to protect the tree. This doubles AI quality for the same time.
        constexpr int kParallelWorkers = 4;
        int perWorker = effectiveIter / kParallelWorkers;
        int remainder = effectiveIter % kParallelWorkers;
        std::mutex treeMu;

        auto worker = [&](int count) {
            for (int it = 0; it < count; ++it) {
                MctsNode* leaf = nullptr;
                {
                    std::lock_guard<std::mutex> lk(treeMu);
                    leaf = selectLeaf(root.get(), cfg.c_puct);
                    if (leaf->visits > 0 && !leaf->expanded
                            && leaf->action.kind != MctsAction::Kind::Pass) {
                        expandNode(leaf, pid, policyFn);
                        if (!leaf->children.empty())
                            leaf = leaf->children.front().get();
                    }
                    leaf->visits++;
                    leaf->totalValue -= 0.5f;  // virtual loss
                }
                float v = rolloutSpell(leaf, pid, fn);
                {
                    std::lock_guard<std::mutex> lk(treeMu);
                    leaf->visits--;
                    leaf->totalValue += 0.5f;
                    backpropagate(leaf, v);
                }
            }
        };

        std::vector<std::future<void>> futures;
        for (int w = 0; w < kParallelWorkers; ++w) {
            int cnt = perWorker + (w < remainder ? 1 : 0);
            if (cnt > 0) futures.push_back(std::async(std::launch::async, worker, cnt));
        }
        for (auto& f : futures) f.get();
    }

    // Build visit distribution from root's direct children
    if (!root->children.empty()) {
        if (visitDist) {
            visitDist->resize(root->children.size());
            float total = 0.f;
            for (auto& c : root->children) total += (float)c->visits;
            for (int i = 0; i < (int)root->children.size(); ++i)
                (*visitDist)[i] = total > 0.f ? (float)root->children[i]->visits / total
                                              : 1.f / root->children.size();
        }

        // Select best child by visits (argmax / temperature)
        std::vector<ActionStats> stats;
        stats.reserve(root->children.size());
        for (auto& c : root->children)
            stats.push_back({c->visits, c->totalValue, c->prior});
        std::vector<MctsAction> actions;
        actions.reserve(root->children.size());
        for (auto& c : root->children)
            actions.push_back(c->action);

        MctsAction chosen = pickAction(actions, stats, cfg.temperature, gs.turnNumber(),
                                        cfg.tempCutoffTurn, nullptr);

        // Persist chosen child subtree for the next call (tree reuse)
        if (persistRoot) {
            persistRoot->reset();
            for (auto& c : root->children) {
                if (c->action.kind      == chosen.kind &&
                    c->action.spellId   == chosen.spellId &&
                    c->action.attackerIds == chosen.attackerIds) {
                    c->parent = nullptr;
                    *persistRoot = std::move(c);
                    break;
                }
            }
        }
        return chosen;
    }

    if (persistRoot) persistRoot->reset();
    MctsAction pass; pass.kind = MctsAction::Kind::Pass;
    return pass;
}

MctsAction MctsSearch::searchAttackers(
    const GameState& gs, const AbilityProcessor& /*srcAp*/, uint8_t pid,
    const MctsConfig& cfg, std::vector<float>* visitDist,
    const ValueFn& fn, const PolicyFn& policyFn) {

    auto actions = legalAttackerActions(gs, pid);
    std::vector<ActionStats> stats(actions.size());
    applyPriors(stats, gs, pid, policyFn);

    runFlatPuct(actions, stats, cfg.iterations, cfg.c_puct,
        [&](const MctsAction& a) { return simulateAttackers(gs, pid, a, fn); });

    return pickAction(actions, stats, cfg.temperature, gs.turnNumber(),
                       cfg.tempCutoffTurn, visitDist);
}

MctsAction MctsSearch::searchBlockers(
    const GameState& gs, const AbilityProcessor& /*srcAp*/, TurnManager& tm,
    uint8_t pid, const MctsConfig& cfg,
    std::vector<float>* visitDist, const ValueFn& fn, const PolicyFn& policyFn) {

    const CombatState& combat = tm.combatState();
    auto actions = legalBlockerActions(gs, combat, pid);
    std::vector<ActionStats> stats(actions.size());
    applyPriors(stats, gs, pid, policyFn);

    runFlatPuct(actions, stats, cfg.iterations, cfg.c_puct,
        [&](const MctsAction& a) { return simulateBlockers(gs, combat, pid, a, fn); });

    return pickAction(actions, stats, cfg.temperature, gs.turnNumber(),
                       cfg.tempCutoffTurn, visitDist);
}

} // namespace mtg
