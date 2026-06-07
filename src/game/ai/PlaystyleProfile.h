#pragma once
#include <string>
#include <vector>

namespace mtg {

// Parameterises how an AiPlayer makes decisions.  Every field has a sane default
// (the existing baseline heuristic).  Preset factory functions return the 10
// Commander archetype profiles.
struct PlaystyleProfile {
    std::string name;
    std::string colorIdentity;  // e.g. "GW", "WU", "UB"
    std::string archetype;      // e.g. "enchantress", "mill"

    // ── Casting priority weights ─────────────────────────────────────────────
    // Multiplied into the final rateSpell() score for each permanent type.
    float creatureWeight     = 1.0f;
    float spellWeight        = 1.0f;   // instants & sorceries
    float enchantmentWeight  = 1.0f;
    float artifactWeight     = 1.0f;
    float planeswalkerWeight = 1.0f;

    // ── Combat ───────────────────────────────────────────────────────────────
    // 0.0 = only attack when guaranteed safe; 1.0 = always swing even suicidally.
    float aggressionBias = 0.5f;

    // ── Keyword bonuses ──────────────────────────────────────────────────────
    // Additive score bonus per creature that has this keyword.
    int flyingBonus   = 0;
    int hasteBonus    = 0;
    int lifegainBonus = 0;  // on cards with Lifelink OR GainLife effects

    // ── Threat removal / salt ────────────────────────────────────────────────
    // saltMultiplier scales the salt score when computing threat priority in
    // staticEval(), pickDestroyTarget(), and pickBestTarget().
    float saltMultiplier = 1.0f;

    // ── Strategy hints ───────────────────────────────────────────────────────
    bool prefersGraveyardPlay    = false;  // boosts ChangeZone→BF effects
    bool holdInstantsForResponse = false;  // amplifies the "hold instant" penalty
    bool prefersWide             = false;  // token/go-wide: boosts creature count value

    // ── Named presets ────────────────────────────────────────────────────────
    static PlaystyleProfile Default();

    static PlaystyleProfile SelesnyaEnchantress();   // GW  enchantress
    static PlaystyleProfile AzoriusMill();            // WU  mill
    static PlaystyleProfile DimirNinjutsu();          // UB  ninjutsu
    static PlaystyleProfile RakdosBurn();             // BR  burn
    static PlaystyleProfile GruulAggro();             // RG  aggro
    static PlaystyleProfile OrzhovAristocrats();      // WB  aristocrats
    static PlaystyleProfile IzzetSpellslinger();      // UR  spellslinger
    static PlaystyleProfile GolgariReanimator();      // BG  reanimator
    static PlaystyleProfile BorosAngels();            // RW  angels
    static PlaystyleProfile SimicCounters();          // GU  +1/+1 counters

    // Returns all 10 commander presets in UI display order.
    static std::vector<PlaystyleProfile> allPresets();
};

} // namespace mtg
