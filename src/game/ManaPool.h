#pragma once
#include "../core/mana/ManaCost.h"
#include "../core/mana/ManaCostShard.h"
#include "ObjectId.h"
#include <array>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace mtg {

// A unit of "restricted" floating mana — produced by a mana ability carrying a
// RestrictValid$ clause (Secluded Courtyard, Cavern of Souls, Pillar of Origins…).
// It may only be spent on a payment that satisfies `restriction`; the decision is
// made by a caller-supplied predicate (which has GameState/CardFilter access),
// keeping ManaPool decoupled from the rules engine.
struct RestrictedMana {
    uint32_t    atoms       = 0;   // colour/atom mask of this mana
    int         amount      = 0;
    std::string restriction;       // the RestrictValid spec, verbatim
    ObjectId    producerId  = kInvalidId;  // producing permanent (for ChosenType resolution)
};

// Predicate: may this restricted unit be spent on the payment currently underway?
using ManaUsePredicate = std::function<bool(const RestrictedMana&)>;

// Tracks the floating mana available to a player during a phase.
// Mana is stored as a map from atom bitmask to amount.
// At end of each step/phase the pool empties (burn if applicable).
class ManaPool {
public:
    // Add one or more mana of the given shard type.
    void add(const ManaCostShard& shard, int amount = 1);

    // Add pure generic/colorless floating mana (produced by Sol Ring etc.).
    void addGeneric(int amount);

    // Add restricted mana (only spendable on payments matching `restriction`).
    void addRestricted(const ManaCostShard& shard, int amount,
                       std::string restriction, ObjectId producer);
    bool hasRestricted() const noexcept { return !m_restricted.empty(); }
    const std::vector<RestrictedMana>& restricted() const noexcept { return m_restricted; }

    // How much of a specific shard is available.
    int available(const ManaCostShard& shard) const noexcept;

    // Total mana symbols in the pool (generic counted as units).
    int total() const noexcept;

    bool isEmpty() const noexcept;

    // Returns true if the pool contains enough mana to pay the given cost,
    // respecting color requirements (not just total amount).
    // The plain overloads consider ONLY unrestricted mana — restricted mana is
    // invisible to them, so it can never be misspent on an arbitrary cost.
    bool canPay(const ManaCost& cost) const noexcept;

    // Deduct the given cost from the pool, satisfying colored shards first,
    // then consuming remaining pool for generic. Caller must ensure canPay() first.
    void pay(const ManaCost& cost) noexcept;

    // Context-aware overloads: restricted mana whose `canUseRestricted` predicate
    // returns true is also spendable (and is consumed first, use-it-or-lose-it).
    bool canPay(const ManaCost& cost, const ManaUsePredicate& canUseRestricted) const noexcept;
    void pay(const ManaCost& cost, const ManaUsePredicate& canUseRestricted) noexcept;

    // Remove all mana (called at end of each phase).
    void empty() noexcept;

    // Human-readable: "{W}{W}{U}(2)" where (2) is floating generic.
    std::string toString() const;

    // Per-category counts for UI pips. Index:
    //   0=W 1=U 2=B 3=R 4=G 5=C(colourless) 6=any(flexible) 7=generic
    std::array<int, 8> colorCounts() const noexcept;

private:
    std::unordered_map<uint32_t, int> m_pool; // atom bitmask → count
    int m_generic = 0;                         // pure generic floating mana
    std::vector<RestrictedMana> m_restricted;  // mana with a spend restriction
};

} // namespace mtg
