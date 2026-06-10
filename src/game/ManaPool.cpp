#include "ManaPool.h"
#include <algorithm>

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
    // Restricted mana is shown too, so the player can see what tapping (e.g.)
    // Secluded Courtyard produced — it just can't be spent on everything.
    for (const auto& r : m_restricted) {
        if (r.amount <= 0) continue;
        uint8_t col = static_cast<uint8_t>(r.atoms & ManaAtom::COLORS_MASK);
        int bits = 0; for (uint8_t m = col; m; m &= static_cast<uint8_t>(m - 1)) ++bits;
        if (bits >= 2)                       out[6] += r.amount;
        else if (col & ManaAtom::WHITE)      out[0] += r.amount;
        else if (col & ManaAtom::BLUE)       out[1] += r.amount;
        else if (col & ManaAtom::BLACK)      out[2] += r.amount;
        else if (col & ManaAtom::RED)        out[3] += r.amount;
        else if (col & ManaAtom::GREEN)      out[4] += r.amount;
        else                                 out[5] += r.amount;
    }
    out[7] = m_generic;
    return out;
}

void ManaPool::addGeneric(int amount) {
    m_generic += amount;
}

void ManaPool::addRestricted(const ManaCostShard& shard, int amount,
                             std::string restriction, ObjectId producer) {
    if (amount <= 0) return;
    // Merge into an existing entry with the same atoms + restriction so the pool
    // doesn't accumulate a separate record per tap.
    for (auto& r : m_restricted) {
        if (r.atoms == shard.atoms && r.restriction == restriction &&
            r.producerId == producer) {
            r.amount += amount;
            return;
        }
    }
    m_restricted.push_back({shard.atoms, amount, std::move(restriction), producer});
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
    m_restricted.clear();
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

// ── Context-aware payment (restricted mana) ───────────────────────────────────
namespace {
// A spendable bucket during a context-aware payment. `order` 0 = restricted
// (spend first, use-it-or-lose-it), 1 = unrestricted. For write-back: kind 0 →
// m_restricted[idx], kind 1 → m_pool[atoms].
struct Src { uint32_t atoms; int avail; int order; int kind; size_t idx; };

// Consume `amount` mana matching colorMask (0 = generic/any) from srcs, preferring
// lower order, then fewer colour bits so flexible mana isn't wasted. False if impossible.
bool consumeColor(std::vector<Src>& srcs, uint8_t colorMask, int amount) {
    while (amount > 0) {
        int best = -1, bestOrder = 99, bestBits = 99;
        for (int i = 0; i < (int)srcs.size(); ++i) {
            if (srcs[i].avail <= 0) continue;
            uint8_t ac = static_cast<uint8_t>(srcs[i].atoms & ManaAtom::COLORS_MASK);
            if (colorMask && !(ac & colorMask)) continue;   // must produce the needed colour
            int bits = 0; for (uint8_t m = ac; m; m &= static_cast<uint8_t>(m - 1)) ++bits;
            if (srcs[i].order < bestOrder ||
                (srcs[i].order == bestOrder && bits < bestBits)) {
                best = i; bestOrder = srcs[i].order; bestBits = bits;
            }
        }
        if (best < 0) return false;
        --srcs[best].avail; --amount;
    }
    return true;
}

// Run the three payment phases (monocolor, hybrid, generic) over srcs + a pure
// generic reserve. Mutates srcs/genericAvail. Returns false if unaffordable.
bool runPhases(std::vector<Src>& srcs, int& genericAvail, const ManaCost& cost) {
    for (int phase = 0; phase < 2; ++phase) {           // 0 = monocolor, 1 = hybrid
        for (const ManaCostShard& s : cost.shards()) {
            if (s.isX()) continue;
            uint8_t cm = s.colorMask();
            if (cm == 0) continue;
            int bits = 0; for (uint8_t m = cm; m; m >>= 1) bits += (m & 1);
            bool mono = (bits == 1);
            if ((phase == 0) != mono) continue;
            if (!consumeColor(srcs, cm, 1)) return false;
        }
    }
    int need = cost.genericAmount();
    if (need < 0) need = 0;
    // Generic: restricted first, then unrestricted buckets, then pure generic.
    for (int ord = 0; ord <= 1 && need > 0; ++ord)
        for (auto& s : srcs)
            while (s.order == ord && s.avail > 0 && need > 0) { --s.avail; --need; }
    if (need > 0) { int take = std::min(need, genericAvail); genericAvail -= take; need -= take; }
    return need == 0;
}
} // namespace

bool ManaPool::canPay(const ManaCost& cost,
                      const ManaUsePredicate& canUseRestricted) const noexcept {
    if (cost.isNoCost()) return true;
    std::vector<Src> srcs;
    for (size_t i = 0; i < m_restricted.size(); ++i) {
        if (m_restricted[i].amount <= 0) continue;
        if (canUseRestricted && canUseRestricted(m_restricted[i]))
            srcs.push_back({m_restricted[i].atoms, m_restricted[i].amount, 0, 0, i});
    }
    for (const auto& [atoms, cnt] : m_pool)
        if (cnt > 0) srcs.push_back({atoms, cnt, 1, 1, 0});
    int genericAvail = m_generic;
    return runPhases(srcs, genericAvail, cost);
}

void ManaPool::pay(const ManaCost& cost,
                   const ManaUsePredicate& canUseRestricted) noexcept {
    if (cost.isNoCost()) return;
    std::vector<Src> srcs;
    for (size_t i = 0; i < m_restricted.size(); ++i) {
        if (m_restricted[i].amount <= 0) continue;
        if (canUseRestricted && canUseRestricted(m_restricted[i]))
            srcs.push_back({m_restricted[i].atoms, m_restricted[i].amount, 0, 0, i});
    }
    for (const auto& [atoms, cnt] : m_pool)
        if (cnt > 0) srcs.push_back({atoms, cnt, 1, 1, 0});
    int genericAvail = m_generic;

    runPhases(srcs, genericAvail, cost);   // caller guarantees affordability

    // Write the post-payment balances back to the real storage.
    for (const auto& s : srcs) {
        if (s.kind == 0) m_restricted[s.idx].amount = s.avail;
        else             m_pool[s.atoms]            = s.avail;
    }
    m_generic = genericAvail;
    m_restricted.erase(std::remove_if(m_restricted.begin(), m_restricted.end(),
        [](const RestrictedMana& r){ return r.amount <= 0; }), m_restricted.end());
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
