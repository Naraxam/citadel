#pragma once
#include "ManaAtom.h"
#include <string>
#include <string_view>
#include <cstdint>

namespace mtg {

// A single mana symbol within a cost: {W}, {2/U}, {G/P}, {X}, etc.
// Stored as a bitmask of ManaAtom flags plus a display symbol string.
struct ManaCostShard {
    uint32_t    atoms;   // ManaAtom bitmask
    const char* symbol;  // e.g. "W", "2/U", "G/P", "X" (no braces)

    constexpr int cmc() const noexcept {
        if (atoms & ManaAtom::IS_X)         return 0;
        if (atoms & ManaAtom::OR_2_GENERIC) return 2;
        return 1;
    }

    constexpr bool isColored()    const noexcept { return (atoms & ManaAtom::COLORS_MASK) != 0; }
    constexpr bool isPhyrexian()  const noexcept { return (atoms & ManaAtom::OR_2_LIFE) != 0; }
    constexpr bool isOr2Generic() const noexcept { return (atoms & ManaAtom::OR_2_GENERIC) != 0; }
    constexpr bool isX()          const noexcept { return (atoms & ManaAtom::IS_X) != 0; }
    constexpr bool isSnow()       const noexcept { return (atoms & ManaAtom::IS_SNOW) != 0; }
    constexpr bool isColorless()  const noexcept { return (atoms & ManaAtom::COLORLESS) != 0; }
    constexpr uint8_t colorMask() const noexcept {
        return static_cast<uint8_t>(atoms & ManaAtom::COLORS_MASK);
    }

    // Returns "{W}", "{2/U}", etc.
    std::string display() const { return '{' + std::string(symbol) + '}'; }

    // Finds the named shard whose atoms match exactly, or a fallback.
    static ManaCostShard fromAtoms(uint32_t atoms) noexcept;

    // Parses a single non-generic token from a Forge mana cost string
    // (e.g. "W", "U/B", "2/W", "W/P", "X", "S", "C").
    static ManaCostShard parseNonGeneric(std::string_view token) noexcept;

    // ── Named constants (every possible shard type) ────────────────────────
    // Declared here; defined in ManaCostShard.cpp after the type is complete.
    // MSVC requires the type to be complete before static members of the same
    // type can be initialized, so we cannot use = {...} inside the class body.

    static const ManaCostShard WHITE, BLUE, BLACK, RED, GREEN, COLORLESS;
    static const ManaCostShard WU, WB, UB, UR, BR, BG, RW, RG, GW, GU;
    static const ManaCostShard W2, U2, B2, R2, G2;
    static const ManaCostShard CW, CU, CB, CR, CG;
    static const ManaCostShard WP, UP, BP, RP, GP;
    static const ManaCostShard BGP, BRP, GUP, GWP, RGP, RWP, UBP, URP, WBP, WUP;
    static const ManaCostShard X, SNOW;
};

constexpr bool operator==(ManaCostShard a, ManaCostShard b) noexcept { return a.atoms == b.atoms; }
constexpr bool operator!=(ManaCostShard a, ManaCostShard b) noexcept { return a.atoms != b.atoms; }

} // namespace mtg
