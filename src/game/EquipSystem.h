#pragma once
#include "Card.h"
#include "GameState.h"
#include "KeywordAbility.h"
#include "ability/ScriptLine.h"
#include <algorithm>
#include <charconv>
#include <string>

namespace mtg {

// Parsed bonus from an equipment's S:Mode$ Continuous | Affected$ Creature.EquippedBy line.
struct EquipBonus {
    int  addPower     = 0;
    int  addToughness = 0;
    uint32_t addKeywords = 0; // KeywordAbility bitmask
};

// Parse the equipment bonus from a CardRules' staticAbilityLines.
// Returns the total bonus granted to the equipped creature.
inline EquipBonus parseEquipBonus(const CardRules& rules) {
    EquipBonus bonus;
    for (const auto& line : rules.staticAbilityLines) {
        auto s = parseScriptLine(line);
        if (s.effectType != "Continuous") continue;
        auto affected = s.get("Affected", "");
        if (affected.find("EquippedBy") == std::string_view::npos) continue;

        bonus.addPower     += s.getInt("AddPower",     0);
        bonus.addToughness += s.getInt("AddToughness", 0);

        // Parse AddKeyword$ (& or space separated, e.g. "First Strike & Vigilance")
        auto kwStr = s.get("AddKeyword", "");
        std::string cur;
        for (char c : std::string(kwStr)) {
            if (c == '&') {
                if (!cur.empty()) {
                    auto kw = parseKeyword(cur);
                    bonus.addKeywords |= static_cast<uint32_t>(kw);
                    cur.clear();
                }
            } else if (c != ' ' || !cur.empty()) {
                cur += c;
            }
        }
        if (!cur.empty()) {
            auto kw = parseKeyword(cur);
            bonus.addKeywords |= static_cast<uint32_t>(kw);
        }
    }
    return bonus;
}

// Parse equip cost from "K:Equip:N" keyword string. Returns -1 if not an equip keyword.
inline int parseEquipCost(std::string_view keyword) {
    if (keyword.substr(0, 6) != "Equip:") {
        if (keyword.substr(0, 5) == "Equip") return 0; // "Equip" alone = 0
        return -1;
    }
    int n = 0;
    auto rest = keyword.substr(6);
    std::from_chars(rest.data(), rest.data() + rest.size(), n);
    return n;
}

// Apply an equipment's bonus to a creature target.
inline void applyEquipBonus(Card& equipment, Card& target) {
    auto bonus = parseEquipBonus(*equipment.rules);
    target.bonusPower     += bonus.addPower;
    target.bonusToughness += bonus.addToughness;
    target.bonusKeywords  |= bonus.addKeywords;
    // Propagate keywords to the card's active keyword mask
    target.keywordMask |= bonus.addKeywords;
}

// Remove an equipment's bonus from a creature.
inline void removeEquipBonus(Card& equipment, Card& target) {
    auto bonus = parseEquipBonus(*equipment.rules);
    target.bonusPower     -= bonus.addPower;
    target.bonusToughness -= bonus.addToughness;
    target.bonusKeywords  &= ~bonus.addKeywords;
    target.keywordMask    &= ~bonus.addKeywords;
}

// Attach equipment to target creature (detaches from old target first).
inline void attachEquipment(Card& equipment, Card& target, GameState& game) {
    // Detach from previous target if any
    if (equipment.attachedTo != kInvalidId) {
        Card* old = game.findCard(equipment.attachedTo);
        if (old) {
            removeEquipBonus(equipment, *old);
            auto& atts = old->attachments;
            atts.erase(std::remove(atts.begin(), atts.end(), equipment.id), atts.end());
        }
    }
    // Attach to new target
    equipment.attachedTo = target.id;
    target.attachments.push_back(equipment.id);
    applyEquipBonus(equipment, target);
}

// Detach equipment (called when equipped creature dies or equipment leaves play).
inline void detachEquipment(Card& equipment, GameState& game) {
    if (equipment.attachedTo == kInvalidId) return;
    Card* target = game.findCard(equipment.attachedTo);
    if (target) removeEquipBonus(equipment, *target);
    equipment.attachedTo = kInvalidId;
}

} // namespace mtg
