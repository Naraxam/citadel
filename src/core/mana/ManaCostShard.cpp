#include "ManaCostShard.h"
#include <array>

namespace mtg {

// ── Named constant definitions ────────────────────────────────────────────────
// Defined here (outside the class) so ManaCostShard is complete at init time.

const ManaCostShard ManaCostShard::WHITE     = {ManaAtom::WHITE,    "W"};
const ManaCostShard ManaCostShard::BLUE      = {ManaAtom::BLUE,     "U"};
const ManaCostShard ManaCostShard::BLACK     = {ManaAtom::BLACK,    "B"};
const ManaCostShard ManaCostShard::RED       = {ManaAtom::RED,      "R"};
const ManaCostShard ManaCostShard::GREEN     = {ManaAtom::GREEN,    "G"};
const ManaCostShard ManaCostShard::COLORLESS = {ManaAtom::COLORLESS,"C"};

const ManaCostShard ManaCostShard::WU = {ManaAtom::WHITE | ManaAtom::BLUE,  "W/U"};
const ManaCostShard ManaCostShard::WB = {ManaAtom::WHITE | ManaAtom::BLACK, "W/B"};
const ManaCostShard ManaCostShard::UB = {ManaAtom::BLUE  | ManaAtom::BLACK, "U/B"};
const ManaCostShard ManaCostShard::UR = {ManaAtom::BLUE  | ManaAtom::RED,   "U/R"};
const ManaCostShard ManaCostShard::BR = {ManaAtom::BLACK | ManaAtom::RED,   "B/R"};
const ManaCostShard ManaCostShard::BG = {ManaAtom::BLACK | ManaAtom::GREEN, "B/G"};
const ManaCostShard ManaCostShard::RW = {ManaAtom::RED   | ManaAtom::WHITE, "R/W"};
const ManaCostShard ManaCostShard::RG = {ManaAtom::RED   | ManaAtom::GREEN, "R/G"};
const ManaCostShard ManaCostShard::GW = {ManaAtom::GREEN | ManaAtom::WHITE, "G/W"};
const ManaCostShard ManaCostShard::GU = {ManaAtom::GREEN | ManaAtom::BLUE,  "G/U"};

const ManaCostShard ManaCostShard::W2 = {ManaAtom::WHITE | ManaAtom::OR_2_GENERIC, "2/W"};
const ManaCostShard ManaCostShard::U2 = {ManaAtom::BLUE  | ManaAtom::OR_2_GENERIC, "2/U"};
const ManaCostShard ManaCostShard::B2 = {ManaAtom::BLACK | ManaAtom::OR_2_GENERIC, "2/B"};
const ManaCostShard ManaCostShard::R2 = {ManaAtom::RED   | ManaAtom::OR_2_GENERIC, "2/R"};
const ManaCostShard ManaCostShard::G2 = {ManaAtom::GREEN | ManaAtom::OR_2_GENERIC, "2/G"};

const ManaCostShard ManaCostShard::CW = {ManaAtom::WHITE | ManaAtom::COLORLESS, "C/W"};
const ManaCostShard ManaCostShard::CU = {ManaAtom::BLUE  | ManaAtom::COLORLESS, "C/U"};
const ManaCostShard ManaCostShard::CB = {ManaAtom::BLACK | ManaAtom::COLORLESS, "C/B"};
const ManaCostShard ManaCostShard::CR = {ManaAtom::RED   | ManaAtom::COLORLESS, "C/R"};
const ManaCostShard ManaCostShard::CG = {ManaAtom::GREEN | ManaAtom::COLORLESS, "C/G"};

const ManaCostShard ManaCostShard::WP = {ManaAtom::WHITE | ManaAtom::OR_2_LIFE, "W/P"};
const ManaCostShard ManaCostShard::UP = {ManaAtom::BLUE  | ManaAtom::OR_2_LIFE, "U/P"};
const ManaCostShard ManaCostShard::BP = {ManaAtom::BLACK | ManaAtom::OR_2_LIFE, "B/P"};
const ManaCostShard ManaCostShard::RP = {ManaAtom::RED   | ManaAtom::OR_2_LIFE, "R/P"};
const ManaCostShard ManaCostShard::GP = {ManaAtom::GREEN | ManaAtom::OR_2_LIFE, "G/P"};

const ManaCostShard ManaCostShard::BGP = {ManaAtom::BLACK | ManaAtom::GREEN | ManaAtom::OR_2_LIFE, "B/G/P"};
const ManaCostShard ManaCostShard::BRP = {ManaAtom::BLACK | ManaAtom::RED   | ManaAtom::OR_2_LIFE, "B/R/P"};
const ManaCostShard ManaCostShard::GUP = {ManaAtom::GREEN | ManaAtom::BLUE  | ManaAtom::OR_2_LIFE, "G/U/P"};
const ManaCostShard ManaCostShard::GWP = {ManaAtom::GREEN | ManaAtom::WHITE | ManaAtom::OR_2_LIFE, "G/W/P"};
const ManaCostShard ManaCostShard::RGP = {ManaAtom::RED   | ManaAtom::GREEN | ManaAtom::OR_2_LIFE, "R/G/P"};
const ManaCostShard ManaCostShard::RWP = {ManaAtom::RED   | ManaAtom::WHITE | ManaAtom::OR_2_LIFE, "R/W/P"};
const ManaCostShard ManaCostShard::UBP = {ManaAtom::BLUE  | ManaAtom::BLACK | ManaAtom::OR_2_LIFE, "U/B/P"};
const ManaCostShard ManaCostShard::URP = {ManaAtom::BLUE  | ManaAtom::RED   | ManaAtom::OR_2_LIFE, "U/R/P"};
const ManaCostShard ManaCostShard::WBP = {ManaAtom::WHITE | ManaAtom::BLACK | ManaAtom::OR_2_LIFE, "W/B/P"};
const ManaCostShard ManaCostShard::WUP = {ManaAtom::WHITE | ManaAtom::BLUE  | ManaAtom::OR_2_LIFE, "W/U/P"};

const ManaCostShard ManaCostShard::X    = {ManaAtom::IS_X,    "X"};
const ManaCostShard ManaCostShard::SNOW = {ManaAtom::IS_SNOW, "S"};

// ── Lookup table (all shards in one place for fromAtoms search) ───────────────
// Must come AFTER the definitions above so the named constants are initialised.

static const ManaCostShard* const kAllShards[] = {
    &ManaCostShard::WHITE, &ManaCostShard::BLUE, &ManaCostShard::BLACK,
    &ManaCostShard::RED,   &ManaCostShard::GREEN,&ManaCostShard::COLORLESS,
    &ManaCostShard::WU, &ManaCostShard::WB, &ManaCostShard::UB, &ManaCostShard::UR,
    &ManaCostShard::BR, &ManaCostShard::BG, &ManaCostShard::RW, &ManaCostShard::RG,
    &ManaCostShard::GW, &ManaCostShard::GU,
    &ManaCostShard::W2, &ManaCostShard::U2, &ManaCostShard::B2, &ManaCostShard::R2, &ManaCostShard::G2,
    &ManaCostShard::CW, &ManaCostShard::CU, &ManaCostShard::CB, &ManaCostShard::CR, &ManaCostShard::CG,
    &ManaCostShard::WP, &ManaCostShard::UP, &ManaCostShard::BP, &ManaCostShard::RP, &ManaCostShard::GP,
    &ManaCostShard::BGP, &ManaCostShard::BRP, &ManaCostShard::GUP, &ManaCostShard::GWP,
    &ManaCostShard::RGP, &ManaCostShard::RWP,
};

ManaCostShard ManaCostShard::fromAtoms(uint32_t atoms) noexcept {
    for (const auto* s : kAllShards) {
        if (s->atoms == atoms) return *s;
    }
    if (atoms & ManaAtom::IS_X)    return ManaCostShard::X;
    if (atoms & ManaAtom::IS_SNOW) return ManaCostShard::SNOW;
    return {ManaAtom::GENERIC, "1"};
}

ManaCostShard ManaCostShard::parseNonGeneric(std::string_view token) noexcept {
    // Build an atom bitmask by scanning each character.
    // '/' is a separator between hybrid parts — skip it.
    uint32_t atoms = 0;
    for (char c : token) {
        switch (c) {
            case 'W': atoms |= ManaAtom::WHITE;        break;
            case 'U': atoms |= ManaAtom::BLUE;         break;
            case 'B': atoms |= ManaAtom::BLACK;        break;
            case 'R': atoms |= ManaAtom::RED;          break;
            case 'G': atoms |= ManaAtom::GREEN;        break;
            case 'C': atoms |= ManaAtom::COLORLESS;    break;
            case 'P': atoms |= ManaAtom::OR_2_LIFE;    break;
            case 'S': atoms |= ManaAtom::IS_SNOW;      break;
            case 'X': atoms |= ManaAtom::IS_X;         break;
            case '2': atoms |= ManaAtom::OR_2_GENERIC; break;
            case '/': break;
            default:  break;
        }
    }
    // "2" alone as a non-generic token → treat as generic (mirrors Forge edge case)
    if (atoms == ManaAtom::OR_2_GENERIC)
        return {ManaAtom::GENERIC, "1"};

    return fromAtoms(atoms);
}

} // namespace mtg
