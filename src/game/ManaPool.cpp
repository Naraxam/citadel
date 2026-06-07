#include "ManaPool.h"

namespace mtg {

void ManaPool::add(const ManaCostShard& shard, int amount) {
    m_pool[shard.atoms] += amount;
}

std::array<int, 8> ManaPool::colorCounts() const noexcept {
    std::array<int, 8> out{};   // W U B R G C any generic
    for (const auto& [atoms, cnt] : m_pool) {
        if (cnt <= 0) continue;
        uint8_t col = static_cast<uint8_t>(atoms & ManaAtom::COLORS_MASK);
        int bits = 0; for (uint8_t m = col; m; m &= static_cast<uint8_t>(m - 1)) ++bits;
        if (bits >= 2)                       out[6] += cnt;   // flexible "any"
        else if (col & ManaAtom::WHITE)      out[0] += cnt;
        else if (col & ManaAtom::BLUE)       out[1] += cnt;
        else if (col & ManaAtom::BLACK)      out[2] += cnt;
        else if (col & ManaAtom::RED)        out[3] += cnt;
        else if (col & ManaAtom::GREEN)      out[4] += cnt;
        else                                 out[5] += cnt;   // colourless {C}
    }
    out[7] = m_generic;
    return out;
}

void ManaPool::addGeneric(int amount) {
    m_generic += amount;
}

int ManaPool::available(const ManaCostShard& shard) const noexcept {
    auto it = m_pool.find(shard.atoms);
    return it != m_pool.end() ? it->second : 0;
}

int ManaPool::total() const noexcept {
    int sum = m_generic;
    for (const auto& [atoms, count] : m_pool)
        sum += count;
    return sum;
}

bool ManaPool::isEmpty() const noexcept {
    if (m_generic > 0) return false;
    for (const auto& [atoms, count] : m_pool)
        if (count > 0) return false;
    return true;
}

void ManaPool::empty() noexcept {
    m_pool.clear();
    m_generic = 0;
}

// ── Color-aware payment ───────────────────────────────────────────────────────

// Try to consume 'amount' mana that satisfies colorMask from the pool.
// colorMask = 0 means pure generic (any mana). Returns true on success.
static bool consumeColored(std::unordered_map<uint32_t,int>& pool,
                            int& generic, uint8_t colorMask, int amount) {
    (void)generic;   // coloured pips can ONLY be paid with coloured mana — never generic.
    while (amount > 0) {
        // Pay with the matching mana that has the FEWEST colour bits, so flexible
        // "any colour" mana isn't wasted when a single-colour source would do.
        auto best = pool.end();
        int  bestBits = 99;
        for (auto it = pool.begin(); it != pool.end(); ++it) {
            if (it->second <= 0) continue;
            uint8_t ac = static_cast<uint8_t>(it->first & ManaAtom::COLORS_MASK);
            if (!(ac & colorMask)) continue;          // must produce the needed colour
            int bits = 0; for (uint8_t m = ac; m; m &= static_cast<uint8_t>(m - 1)) ++bits;
            if (bits < bestBits) { bestBits = bits; best = it; }
        }
        if (best == pool.end()) return false;          // no coloured mana of that colour
        --best->second; --amount;
    }
    return true;
}

bool ManaPool::canPay(const ManaCost& cost) const noexcept {
    if (cost.isNoCost()) return true;
    // Work on a copy to simulate payment
    auto poolCopy   = m_pool;
    int  genericCopy = m_generic;

    // Pay colored shards in order of most restrictive (monocolor first, hybrid later)
    // Phase 1: strict monocolor shards
    for (const ManaCostShard& s : cost.shards()) {
        if (s.isX()) continue;
        uint8_t cm = s.colorMask();
        if (cm == 0) continue; // generic or colorless — handled in phase 3
        int bits = 0; for (uint8_t m = cm; m; m >>= 1) bits += (m & 1);
        if (bits == 1) {
            // Monocolor: must pay with that color (or generic)
            if (!consumeColored(poolCopy, genericCopy, cm, 1)) return false;
        }
    }
    // Phase 2: hybrid shards
    for (const ManaCostShard& s : cost.shards()) {
        if (s.isX()) continue;
        uint8_t cm = s.colorMask();
        if (cm == 0) continue;
        int bits = 0; for (uint8_t m = cm; m; m >>= 1) bits += (m & 1);
        if (bits > 1) {
            if (!consumeColored(poolCopy, genericCopy, cm, 1)) return false;
        }
    }
    // Phase 3: generic portion (any remaining mana)
    int remaining = cost.genericAmount();
    if (remaining < 0) remaining = 0;
    int rem = genericCopy;
    for (const auto& [atoms, cnt] : poolCopy) rem += cnt;
    if (rem < remaining) return false;
    return true;
}

void ManaPool::pay(const ManaCost& cost) noexcept {
    if (cost.isNoCost()) return;
    // Phase 1: monocolor
    for (const ManaCostShard& s : cost.shards()) {
        if (s.isX()) continue;
        uint8_t cm = s.colorMask();
        if (cm == 0) continue;
        int bits = 0; for (uint8_t m = cm; m; m >>= 1) bits += (m & 1);
        if (bits == 1) consumeColored(m_pool, m_generic, cm, 1);
    }
    // Phase 2: hybrid
    for (const ManaCostShard& s : cost.shards()) {
        if (s.isX()) continue;
        uint8_t cm = s.colorMask();
        if (cm == 0) continue;
        int bits = 0; for (uint8_t m = cm; m; m >>= 1) bits += (m & 1);
        if (bits > 1) consumeColored(m_pool, m_generic, cm, 1);
    }
    // Phase 3: generic — consume remaining in pool order
    int needed = cost.genericAmount();
    while (needed > 0) {
        bool used = false;
        for (auto& [atoms, cnt] : m_pool) {
            if (cnt > 0) { --cnt; --needed; used = true; break; }
        }
        if (!used) { if (m_generic > 0) { --m_generic; --needed; } else break; }
    }
}

std::string ManaPool::toString() const {
    std::string result;
    // Iterate in a stable order for display: W U B R G C then others
    static const ManaCostShard* const kOrder[] = {
        &ManaCostShard::WHITE, &ManaCostShard::BLUE, &ManaCostShard::BLACK,
        &ManaCostShard::RED,   &ManaCostShard::GREEN,&ManaCostShard::COLORLESS,
    };
    for (const auto* s : kOrder) {
        auto it = m_pool.find(s->atoms);
        if (it != m_pool.end() && it->second > 0) {
            for (int i = 0; i < it->second; ++i)
                result += s->display();
        }
    }
    // Remaining non-basic shards
    for (const auto& [atoms, count] : m_pool) {
        if (count <= 0) continue;
        bool isBasic = false;
        for (const auto* s : kOrder)
            if (s->atoms == atoms) { isBasic = true; break; }
        if (isBasic) continue;
        ManaCostShard s = ManaCostShard::fromAtoms(atoms);
        for (int i = 0; i < count; ++i)
            result += s.display();
    }
    if (m_generic > 0)
        result += '{' + std::to_string(m_generic) + '}';
    return result.empty() ? "(empty)" : result;
}

} // namespace mtg
