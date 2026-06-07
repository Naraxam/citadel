#pragma once
#include <cstdint>
#include <string_view>

namespace mtg {

struct CardRules;

// Combat and game-rules keywords stored as a bitmask on each Card.
// Each value is a single bit so they can be OR-ed into a uint32_t mask.
// Phase 5+ will handle ability-scripted keywords (Equip, Cycling, Kicker…).
enum class KeywordAbility : uint32_t {
    None          = 0,
    Flying        = 1u << 0,
    Reach         = 1u << 1,
    Vigilance     = 1u << 2,
    Haste         = 1u << 3,
    FirstStrike   = 1u << 4,
    DoubleStrike  = 1u << 5,
    Trample       = 1u << 6,
    Deathtouch    = 1u << 7,
    Lifelink      = 1u << 8,
    Menace        = 1u << 9,
    Hexproof      = 1u << 10,
    Shroud        = 1u << 11,
    Indestructible= 1u << 12,
    Flash         = 1u << 13,
    Infect        = 1u << 14,  // damage to creatures as -1/-1 counters; to players as poison
    Wither        = 1u << 15,  // damage to creatures as -1/-1 counters (not poison to players)
    Undying       = 1u << 16,  // when dies with no +1/+1 counter, return with +1/+1
    Persist        = 1u << 17,  // when dies with no -1/-1 counter, return with -1/-1
    Ward             = 1u << 18,
    ProtectionWhite  = 1u << 19,  // can't be targeted/blocked/damaged by white sources
    ProtectionBlue   = 1u << 20,
    ProtectionBlack  = 1u << 21,
    ProtectionRed    = 1u << 22,
    ProtectionGreen  = 1u << 23,
    ProtectionAll    = 1u << 24,
    // Landwalk — can't be blocked if defending player controls the matching land type
    Swampwalk     = 1u << 25,
    Islandwalk    = 1u << 26,
    Mountainwalk  = 1u << 27,
    Forestwalk    = 1u << 28,
    Plainswalk    = 1u << 29,
    // Other evasion
    Fear          = 1u << 30,  // blocked only by black and/or artifact creatures
    Shadow        = 1u << 31,  // can only block/be blocked by shadow creatures
};

// Parse one keyword string from a Forge K: line.
// Uses prefix matching so "Hexproof from [color]" still returns Hexproof.
// Returns KeywordAbility::None if unrecognised.
KeywordAbility parseKeyword(std::string_view s) noexcept;

// Build a keyword bitmask from all K: lines in a CardRules.
uint32_t buildKeywordMask(const CardRules& rules) noexcept;

// Convenience: test a single bit in a mask
inline bool maskHas(uint32_t mask, KeywordAbility kw) noexcept {
    return (mask & static_cast<uint32_t>(kw)) != 0;
}

// Given a source's ManaAtom color bitmask, return the set of ProtectionX bits
// that would be triggered on the target. Used to enforce protection rules.
// colorMask uses the same bit layout as ManaAtom (W=1,U=2,B=4,R=8,G=16).
inline uint32_t protectionBitsForColor(uint8_t colorMask) noexcept {
    uint32_t bits = 0;
    if (colorMask & 0x01) bits |= static_cast<uint32_t>(KeywordAbility::ProtectionWhite);
    if (colorMask & 0x02) bits |= static_cast<uint32_t>(KeywordAbility::ProtectionBlue);
    if (colorMask & 0x04) bits |= static_cast<uint32_t>(KeywordAbility::ProtectionBlack);
    if (colorMask & 0x08) bits |= static_cast<uint32_t>(KeywordAbility::ProtectionRed);
    if (colorMask & 0x10) bits |= static_cast<uint32_t>(KeywordAbility::ProtectionGreen);
    return bits;
}

// Returns true if the source's color is blocked by the target's protection.
inline bool hasProtectionFrom(uint32_t targetKeywordMask, uint8_t sourceColorMask) noexcept {
    if (maskHas(targetKeywordMask, KeywordAbility::ProtectionAll)) return true;
    return (targetKeywordMask & protectionBitsForColor(sourceColorMask)) != 0;
}

// ── Type-based protection and colour-specific hexproof ────────────────────────

// Type-based protection bitmask stored separately from keywordMask.
// Checked during targeting and blocking in addition to colour protection.
enum class ProtectionType : uint8_t {
    None         = 0,
    Artifacts    = 1u << 0,
    Enchantments = 1u << 1,
    Creatures    = 1u << 2,
    Instants     = 1u << 3,
    Sorceries    = 1u << 4,
    Monocolored  = 1u << 5,
    Everything   = 0xFF,
};

// Colour-specific hexproof: WUBRG bitmask; 0 = none, 0xFF = all colours.
// Returns true if the source's colour identity overlaps the hexproof-from mask.
inline bool hasHexproofFrom(uint8_t hexproofMask, uint8_t sourceColorMask) noexcept {
    if (hexproofMask == 0) return false;
    if (hexproofMask == 0xFF) return true;
    return (hexproofMask & sourceColorMask) != 0;
}

} // namespace mtg
