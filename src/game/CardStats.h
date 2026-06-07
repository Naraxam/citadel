#pragma once
#include "Card.h"
#include <charconv>

namespace mtg {

// Parse a power/toughness string to int.
// Returns 0 for variable stats ("*", "*+1", etc.).
inline int parseStat(std::string_view s) noexcept {
    if (s.empty()) return 0;
    // Strip a leading '*' or handle pure variable stats
    if (s.front() == '*') return 0;
    int v = 0;
    std::from_chars(s.data(), s.data() + s.size(), v);
    return v;
}

namespace detail {
// Compute raw {power, toughness} before Layer 7e (switch) is applied.
// Layers 7b (set) → 7a (var/base) → 7d (counters) → 7c (additive bonuses).
inline std::pair<int,int> rawPT(const Card& c) noexcept {
    int base_p, base_t;
    if (c.isFaceDown) {
        base_p = 2; base_t = 2;
    } else if (c.setPower >= 0) {
        base_p = c.setPower;
        base_t = c.setToughness >= 0 ? c.setToughness : parseStat(c.rules->toughness);
    } else if (c.varPower >= 0) {
        base_p = c.varPower;
        base_t = c.varToughness >= 0 ? c.varToughness : parseStat(c.rules->toughness);
    } else if (c.basePowerOverride >= 0) {
        base_p = c.basePowerOverride;
        base_t = c.baseToughOverride >= 0 ? c.baseToughOverride : parseStat(c.rules->toughness);
    } else {
        base_p = parseStat(c.rules->power);
        base_t = parseStat(c.rules->toughness);
    }
    int counters = c.counterCount("+1/+1") - c.counterCount("-1/-1");
    int p = base_p + counters + c.bonusPower    + c.tempPower    + c.continuousPower    + c.enlistBonus;
    int t = base_t + counters + c.bonusToughness + c.tempToughness + c.continuousToughness;
    return {p, t};
}
} // namespace detail

// Effective power — Layer 7e (swapPT) applied last.
inline int effectivePower(const Card& c) noexcept {
    auto [p, t] = detail::rawPT(c);
    return c.swapPT ? t : p;
}

// Effective toughness — Layer 7e (swapPT) applied last.
inline int effectiveToughness(const Card& c) noexcept {
    auto [p, t] = detail::rawPT(c);
    return c.swapPT ? p : t;
}

// Net power/toughness as a display string, e.g. "4/4", "3/2"
inline std::string statLine(const Card& c) {
    return std::to_string(effectivePower(c)) + '/' + std::to_string(effectiveToughness(c));
}

} // namespace mtg
