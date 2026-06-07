#include "PlaystyleProfile.h"

namespace mtg {

PlaystyleProfile PlaystyleProfile::Default() {
    PlaystyleProfile p;
    p.name          = "Default";
    p.colorIdentity = "";
    p.archetype     = "balanced";
    return p;
}

// ── Selesnya Enchantress (GW) ─────────────────────────────────────────────────
// Builds up enchantments that draw cards and pump creatures, then swings wide.
PlaystyleProfile PlaystyleProfile::SelesnyaEnchantress() {
    PlaystyleProfile p;
    p.name              = "Selesnya Enchantress";
    p.colorIdentity     = "GW";
    p.archetype         = "enchantress";
    p.enchantmentWeight = 1.9f;  // enchantments are the deck's engine
    p.creatureWeight    = 1.2f;
    p.spellWeight       = 0.8f;
    p.aggressionBias    = 0.45f; // patient build-up before pushing
    p.flyingBonus       = 8;
    p.lifegainBonus     = 10;
    p.saltMultiplier    = 1.3f;
    return p;
}

// ── Azorius Mill (WU) ─────────────────────────────────────────────────────────
// Mills opponent's library; very controlling, almost never attacks.
PlaystyleProfile PlaystyleProfile::AzoriusMill() {
    PlaystyleProfile p;
    p.name                   = "Azorius Mill";
    p.colorIdentity          = "WU";
    p.archetype              = "mill";
    p.spellWeight            = 1.6f;  // mill spells score highest
    p.creatureWeight         = 0.6f;  // minimal creature investment
    p.enchantmentWeight      = 1.2f;
    p.aggressionBias         = 0.05f; // almost never attacks proactively
    p.holdInstantsForResponse= true;  // hold counterspells
    p.saltMultiplier         = 2.0f;  // aggressively removes high-salt threats
    p.lifegainBonus          = 12;
    return p;
}

// ── Dimir Ninjutsu (UB) ──────────────────────────────────────────────────────
// Sneaks unblocked attackers to trigger Ninjutsu; card-draw over removal.
PlaystyleProfile PlaystyleProfile::DimirNinjutsu() {
    PlaystyleProfile p;
    p.name           = "Dimir Ninjutsu";
    p.colorIdentity  = "UB";
    p.archetype      = "ninjutsu";
    p.creatureWeight = 1.3f;
    p.spellWeight    = 1.1f;
    p.aggressionBias = 0.65f; // attacks often to set up Ninjutsu
    p.flyingBonus    = 15;    // strong preference for evasive attackers
    p.saltMultiplier = 1.4f;
    return p;
}

// ── Rakdos Burn (BR) ─────────────────────────────────────────────────────────
// Goes to the face as fast as possible; treats every spell as reach damage.
PlaystyleProfile PlaystyleProfile::RakdosBurn() {
    PlaystyleProfile p;
    p.name           = "Rakdos Burn";
    p.colorIdentity  = "BR";
    p.archetype      = "burn";
    p.spellWeight    = 1.7f;  // damage spells are the plan
    p.creatureWeight = 0.9f;
    p.aggressionBias = 0.95f; // always attacking
    p.hasteBonus     = 18;    // haste creatures attack the turn they land
    p.saltMultiplier = 0.8f;  // doesn't care much about threats — just burns
    return p;
}

// ── Gruul Aggro (RG) ─────────────────────────────────────────────────────────
// Biggest creatures, trample everything, no patience for a long game.
PlaystyleProfile PlaystyleProfile::GruulAggro() {
    PlaystyleProfile p;
    p.name           = "Gruul Aggro";
    p.colorIdentity  = "RG";
    p.archetype      = "aggro";
    p.creatureWeight = 1.6f;
    p.spellWeight    = 0.9f;
    p.aggressionBias = 0.90f;
    p.hasteBonus     = 14;
    p.saltMultiplier = 0.7f;
    return p;
}

// ── Orzhov Aristocrats (WB) ──────────────────────────────────────────────────
// Floods the board with tokens; sacrifices them for value and life drain.
PlaystyleProfile PlaystyleProfile::OrzhovAristocrats() {
    PlaystyleProfile p;
    p.name           = "Orzhov Aristocrats";
    p.colorIdentity  = "WB";
    p.archetype      = "aristocrats";
    p.creatureWeight = 1.4f;
    p.enchantmentWeight = 1.2f;
    p.aggressionBias = 0.55f;
    p.prefersWide    = true;
    p.lifegainBonus  = 14;
    p.saltMultiplier = 1.5f;
    return p;
}

// ── Izzet Spellslinger (UR) ───────────────────────────────────────────────────
// Casts instants/sorceries for triggers; holds mana open every turn.
PlaystyleProfile PlaystyleProfile::IzzetSpellslinger() {
    PlaystyleProfile p;
    p.name                   = "Izzet Spellslinger";
    p.colorIdentity          = "UR";
    p.archetype              = "spellslinger";
    p.spellWeight            = 1.8f;
    p.creatureWeight         = 0.8f;
    p.aggressionBias         = 0.40f;
    p.holdInstantsForResponse= true;
    p.saltMultiplier         = 1.6f;
    return p;
}

// ── Golgari Reanimator (BG) ───────────────────────────────────────────────────
// Self-mills large creatures then reanimates them for massive value.
PlaystyleProfile PlaystyleProfile::GolgariReanimator() {
    PlaystyleProfile p;
    p.name                = "Golgari Reanimator";
    p.colorIdentity       = "BG";
    p.archetype           = "reanimator";
    p.creatureWeight      = 1.3f;
    p.spellWeight         = 1.1f;
    p.aggressionBias      = 0.50f;
    p.prefersGraveyardPlay= true;
    p.saltMultiplier      = 1.3f;
    return p;
}

// ── Boros Angels (RW) ─────────────────────────────────────────────────────────
// Ramps into large, flying, lifelinking angels and crashes in the air.
PlaystyleProfile PlaystyleProfile::BorosAngels() {
    PlaystyleProfile p;
    p.name           = "Boros Angels";
    p.colorIdentity  = "RW";
    p.archetype      = "angels";
    p.creatureWeight = 1.5f;
    p.aggressionBias = 0.70f;
    p.flyingBonus    = 20;   // angels fly — reward that heavily
    p.lifegainBonus  = 16;
    p.saltMultiplier = 1.2f;
    return p;
}

// ── Simic +1/+1 Counters (GU) ────────────────────────────────────────────────
// Ramps, draws, and grows creatures with +1/+1 counters to overwhelm.
PlaystyleProfile PlaystyleProfile::SimicCounters() {
    PlaystyleProfile p;
    p.name           = "Simic Counters";
    p.colorIdentity  = "GU";
    p.archetype      = "counters";
    p.creatureWeight = 1.4f;
    p.spellWeight    = 1.1f;
    p.enchantmentWeight = 1.1f;
    p.aggressionBias = 0.55f;
    p.saltMultiplier = 1.3f;
    return p;
}

// ── Roster ────────────────────────────────────────────────────────────────────

std::vector<PlaystyleProfile> PlaystyleProfile::allPresets() {
    return {
        SelesnyaEnchantress(),
        AzoriusMill(),
        DimirNinjutsu(),
        RakdosBurn(),
        GruulAggro(),
        OrzhovAristocrats(),
        IzzetSpellslinger(),
        GolgariReanimator(),
        BorosAngels(),
        SimicCounters(),
    };
}

} // namespace mtg
