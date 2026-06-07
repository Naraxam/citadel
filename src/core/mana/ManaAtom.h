#pragma once
#include <cstdint>

// Bit-flag constants for mana properties, mirroring Forge's ManaAtom.java.
// Each ManaCostShard stores a bitmask of these flags.
namespace mtg::ManaAtom {

inline constexpr uint32_t WHITE        = 1u << 0;
inline constexpr uint32_t BLUE         = 1u << 1;
inline constexpr uint32_t BLACK        = 1u << 2;
inline constexpr uint32_t RED          = 1u << 3;
inline constexpr uint32_t GREEN        = 1u << 4;
inline constexpr uint32_t COLORLESS    = 1u << 5;  // {C} — pure colorless, not generic
inline constexpr uint32_t OR_2_GENERIC = 1u << 6;  // hybrid "pay 2 generic" alternative
inline constexpr uint32_t OR_2_LIFE    = 1u << 7;  // Phyrexian "pay 2 life" alternative
inline constexpr uint32_t IS_X         = 1u << 8;
inline constexpr uint32_t IS_SNOW      = 1u << 9;
inline constexpr uint32_t GENERIC      = 1u << 10; // generic, counted separately in ManaCost

inline constexpr uint32_t COLORS_MASK = WHITE | BLUE | BLACK | RED | GREEN;

} // namespace mtg::ManaAtom
