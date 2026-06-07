#pragma once
#include <string>
#include <string_view>
#include <unordered_map>

namespace mtg {

// A single parsed Forge ability script entry.
//
// Raw format (from A:, T:, or R: lines in card .txt files):
//   SP$ DealDamage | ValidTgts$ Any | NumDmg$ 3 | SpellDescription$ ...
//
// Parsed result:
//   abilityType = "SP"           (SP/AB/DB/Mode/Event)
//   effectType  = "DealDamage"   (the operation to execute)
//   params      = { "ValidTgts":"Any", "NumDmg":"3", ... }
//
// Ability type meanings:
//   SP  — spell effect (cast from hand, goes on stack)
//   AB  — activated ability (can be activated from zone)
//   DB  — chained sub-ability (follow-up effect)
//   Mode (T: lines) — trigger event type
//   Event (R: lines) — replacement event type
struct ScriptLine {
    std::string abilityType; // "SP", "AB", "DB", "Mode", "Event"
    std::string effectType;  // "DealDamage", "Mana", "ChangeZone", "Counter", ...

    std::unordered_map<std::string, std::string> params;

    // Look up a parameter value; returns defaultVal if not present
    std::string_view get(std::string_view key, std::string_view defaultVal = "") const;

    // Parse a parameter as int; returns defaultVal on missing or malformed
    int getInt(std::string_view key, int defaultVal = 0) const;

    // Like getInt, but if the value is the literal string "X" returns xValue instead.
    int getIntOrX(std::string_view key, int xValue, int defaultVal = 0) const;

    bool empty() const { return abilityType.empty(); }
};

// Parse a single Forge script string into a ScriptLine.
// Does not include the leading "A:", "T:", or "R:" prefix.
ScriptLine parseScriptLine(std::string_view raw);

} // namespace mtg
