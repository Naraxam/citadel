#pragma once
#include "../core/mana/ManaCost.h"
#include "../core/mana/ManaCostShard.h"
#include <array>
#include <string>
#include <unordered_map>

namespace mtg {

// Tracks the floating mana available to a player during a phase.
// Mana is stored as a map from atom bitmask to amount.
// At end of each step/phase the pool empties (burn if applicable).
class ManaPool {
public:
    // Add one or more mana of the given shard type.
    void add(const ManaCostShard& shard, int amount = 1);

    // Add pure generic/colorless floating mana (produced by Sol Ring etc.).
    void addGeneric(int amount);

    // How much of a specific shard is available.
    int available(const ManaCostShard& shard) const noexcept;

    // Total mana symbols in the pool (generic counted as units).
    int total() const noexcept;

    bool isEmpty() const noexcept;

    // Returns true if the pool contains enough mana to pay the given cost,
    // respecting color requirements (not just total amount).
    bool canPay(const ManaCost& cost) const noexcept;

    // Deduct the given cost from the pool, satisfying colored shards first,
    // then consuming remaining pool for generic. Caller must ensure canPay() first.
    void pay(const ManaCost& cost) noexcept;

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
};

} // namespace mtg
