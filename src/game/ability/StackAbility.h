#pragma once
#include "ScriptLine.h"
#include "Target.h"
#include "../ObjectId.h"
#include <cstdint>
#include <vector>

namespace mtg {

// Represents one entry on the stack: a spell being cast, or an activated/
// triggered ability waiting to resolve.
//
// For spells, sourceCardId points to the card that was moved into the Stack
// zone. For abilities without an associated card, it is kInvalidId.
struct StackAbility {
    ObjectId           sourceCardId       = kInvalidId;
    uint8_t            controllerId       = 0;
    ScriptLine         script;            // the effect to execute on resolution
    std::vector<Target> targets;          // chosen targets

    // If true, the source card stays in its current zone after resolution
    // (activated abilities, triggered abilities) rather than moving to graveyard/battlefield.
    bool isActivatedAbility = false;

    // Value of X chosen when casting (0 for non-X spells).
    int xValue = 0;

    // True if this spell was cast via Flashback (exile instead of GY after resolving).
    bool flashback = false;
    // True if the optional Kicker cost was paid at cast time.
    bool kicked = false;
    // True if Buyback was paid (return to hand after resolving).
    bool buyback = false;
    // True if the optional Bargain cost was paid (sacrificed art/ench/token for a bonus).
    bool bargained = false;
    // True if cast via the Evoke alternative cost (sacrifice creature after ETB resolves).
    bool evoke = false;
    // True if cast via the Dash alternative cost (return creature to hand at next EOT).
    bool dashed = false;
    // True if cast via Unearth from graveyard (exile creature at next EOT).
    bool unearthed = false;
    // True if cast via Blitz (sacrifice creature at next EOT; draw a card on death).
    bool blitzed = false;
    // True if cast for its Overload cost (applies the effect to ALL valid targets).
    bool overloaded = false;
    // True if cast from Exile via Foretell.
    bool castViForetell = false;
    // True if this spell has Rebound and was cast from hand (exile instead of GY after resolving).
    // NOT set when the spell is being cast via Rebound from exile (second cast goes to GY).
    bool rebound = false;

    // True if cast via Escape from graveyard (exile after resolving, like Flashback).
    bool escape = false;
    // True if cast via Jump-start from graveyard (exile after resolving).
    bool jumpStart = false;
    // True if cast via Aftermath (right half of a split card from GY; exile after resolving).
    bool aftermath = false;

    // True if cast face-down via the Morph or Megamorph alternative cost {3}.
    // The permanent enters the battlefield with isFaceDown = true.
    bool morphed = false;

    // Replicate: the spell was replicated once; create a copy when it resolves.
    bool replicated = false;
    // Conspire: the spell was conspired; create a copy when it resolves.
    bool conspired  = false;
    // Bestow: this card was cast as an Aura; attach to the first target on ETB.
    bool bestowed   = false;

    // True if cast using the Prototype alternate cost (reduced cost, reduced P/T).
    bool prototyped = false;

    // True if this creature was cast via Suspend (gets Haste until it leaves BF).
    bool isSuspendHaste = false;

    // True if this is a storm/replicate/conspire copy (never has a physical card source).
    // Copies fizzle silently if no legal targets exist; they don't move a source card.
    bool isCopy = false;

    // Optional chained sub-abilities (DB$ effects executed in order after main)
    std::vector<ScriptLine> subEffects;
};

} // namespace mtg
