#pragma once
#include "ObjectId.h"
#include <cstdint>
#include <string>
#include <vector>

namespace mtg {

// MTG Rule 613 layer numbers.
// Only layers that are implemented are listed; the values encode ordering.
// Sub-layers 7a/b/c/e are encoded as 71/72/73/75 so they sort after 7 < each.
enum class Layer : uint8_t {
    Copy          = 1,   // Characteristic-copy effects (Clone, Metamorph)
    Control       = 2,   // Control-changing effects (Act of Treason)
    TextChanging  = 3,   // Text-change effects (Magical Hack) — stub
    TypeChanging  = 4,   // Type/subtype-changing (Conspiracy, lord-granted subtypes)
    ColorChanging = 5,   // Color-changing effects (Painter's Servant, etc.)
    AbilityAddRem = 6,   // Ability add/remove (Humility, Glorious Anthem keywords)
    SetPTFromChar = 71,  // 7a: P/T set by characteristic-defining ability (*/* creatures)
    SetPT         = 72,  // 7b: P/T set by effect ("becomes 1/1" — Humility)
    ModifyPT      = 73,  // 7c: P/T modified additively (+N/+N — Glorious Anthem, pump)
    SwitchPT      = 75,  // 7e: P/T switched (Crypsis, Inside Out)
};

// A single ongoing continuous effect active in the current game state.
// Produced by LayerEngine from static ability lines on permanents and by
// Effects.cpp for "until end of turn" / "this turn" dynamic effects.
struct ContinuousEffect {
    Layer    layer;
    ObjectId sourceId          = kInvalidId;
    uint8_t  sourceController  = 0;
    uint32_t timestamp         = 0;   // insertion order — determines 7c dependency ordering

    // Affected targets: pre-resolved card IDs (empty → use affectedFilter at apply time).
    std::vector<ObjectId> affectedIds;
    std::string           affectedFilter;  // Forge ValidCards$ string, e.g. "Creature.YouControl"

    // ── Layer 1: Copy ────────────────────────────────────────────────────────
    ObjectId copySourceId = kInvalidId;  // card whose characteristics to copy

    // ── Layer 2: Control ─────────────────────────────────────────────────────
    uint8_t newController = 255;         // 255 = not a control effect

    // ── Layer 3: Text (stub — stored but not applied) ────────────────────────
    std::string textFind;
    std::string textReplace;

    // ── Layer 4: Type / subtype ──────────────────────────────────────────────
    std::vector<std::string> addSubtypes;
    std::vector<std::string> removeSubtypes;
    bool removeAllCreatureTypes = false;

    // ── Layer 5: Color ───────────────────────────────────────────────────────
    // colorIdOverride bits: W=0x01 U=0x02 B=0x04 R=0x08 G=0x10 (Forge color atoms)
    uint8_t setColorMask    = 0;    // 0 = no set-color effect
    uint8_t addColorMask    = 0;    // bits to OR into color
    bool    removeAllColors = false;

    // ── Layer 6: Ability add / remove ────────────────────────────────────────
    uint32_t addKeywords        = 0;   // bitmask (KeywordAbility enum)
    uint32_t removeKeywords     = 0;
    bool     removeAllAbilities = false;
    bool     cantAttack         = false;
    bool     cantBlock          = false;
    bool     cantBeTargeted     = false;
    bool     unblockable        = false;
    bool     dealsDmgByToughness = false;
    bool     activateAsIfHaste  = false;

    // ── Layer 7a: Set P/T from CDA ───────────────────────────────────────────
    int varPower     = -1;   // -1 = not a 7a effect
    int varToughness = -1;

    // ── Layer 7b: Set P/T ────────────────────────────────────────────────────
    int setPower     = -1;   // -1 = not a 7b effect
    int setToughness = -1;

    // ── Layer 7c: Modify P/T ─────────────────────────────────────────────────
    int addPower     = 0;
    int addToughness = 0;

    // ── Layer 7e: Switch P/T ─────────────────────────────────────────────────
    bool switchPT = false;

    // ── Duration ─────────────────────────────────────────────────────────────
    enum class Duration : uint8_t {
        Static,     // Persists while source is on battlefield (rebuilt each recompute)
        UntilEOT,   // Cleared at Cleanup step
        Permanent,  // Equipment / Aura attachment — cleared when detached
    };
    Duration duration = Duration::Static;
};

} // namespace mtg
