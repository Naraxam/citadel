#pragma once
#include "ManaCostShard.h"
#include <vector>
#include <string>
#include <string_view>
#include <cstdint>

namespace mtg {

// The full mana cost of a card or ability.
// Stores the generic amount separately from the colored/hybrid shards,
// matching Forge's ManaCost representation.
class ManaCost {
public:
    // Parse from the Forge script format (space-separated tokens).
    // Examples: "R", "3 W W", "U U", "0", "no cost", "X R", "2/B 2/R 2/G"
    static ManaCost parse(std::string_view text);

    // Converted mana cost (X counts as 0)
    int cmc() const noexcept;

    int  genericAmount()                         const noexcept { return m_generic; }
    const std::vector<ManaCostShard>& shards()   const noexcept { return m_shards; }
    bool isNoCost()                              const noexcept { return m_noCost; }
    bool hasX()                                  const noexcept { return m_hasX; }

    // Bitmask of colors present in the cost (ManaAtom::COLORS_MASK bits)
    uint8_t colorIdentity() const noexcept;

    // Returns "{3}{W}{W}", "{X}{R}", "{0}", or "" for no-cost
    std::string toString() const;

private:
    std::vector<ManaCostShard> m_shards;
    int  m_generic = 0;
    bool m_noCost  = false;
    bool m_hasX    = false; // true if the cost contains {X}
};

} // namespace mtg
