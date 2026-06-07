#include "CardType.h"
#include <algorithm>
#include <sstream>
#include <unordered_map>

namespace mtg {

namespace {

const std::unordered_map<std::string_view, CardSuperType> kSuperTypeNames = {
    {"Basic",     CardSuperType::Basic},
    {"Legendary", CardSuperType::Legendary},
    {"Snow",      CardSuperType::Snow},
    {"World",     CardSuperType::World},
    {"Elite",     CardSuperType::Elite},
};

const std::unordered_map<std::string_view, CardMainType> kMainTypeNames = {
    {"Creature",     CardMainType::Creature},
    {"Instant",      CardMainType::Instant},
    {"Sorcery",      CardMainType::Sorcery},
    {"Artifact",     CardMainType::Artifact},
    {"Enchantment",  CardMainType::Enchantment},
    {"Land",         CardMainType::Land},
    {"Planeswalker", CardMainType::Planeswalker},
    {"Battle",       CardMainType::Battle},
    {"Dungeon",      CardMainType::Dungeon},
    {"Tribal",       CardMainType::Tribal},
    {"Kindred",      CardMainType::Kindred},
};

} // namespace

CardType CardType::parse(std::string_view text) {
    CardType ct;
    bool seenMainType = false;

    std::string_view remaining = text;
    while (!remaining.empty()) {
        auto space = remaining.find(' ');
        std::string_view token = (space == std::string_view::npos)
                                 ? remaining
                                 : remaining.substr(0, space);
        remaining = (space == std::string_view::npos)
                    ? std::string_view{}
                    : remaining.substr(space + 1);

        if (token.empty()) continue;

        if (!seenMainType) {
            if (auto it = kSuperTypeNames.find(token); it != kSuperTypeNames.end()) {
                ct.supertypes.push_back(it->second);
                continue;
            }
            if (auto it = kMainTypeNames.find(token); it != kMainTypeNames.end()) {
                ct.types.push_back(it->second);
                seenMainType = true;
                continue;
            }
            // Unknown token before any main type — treat as main type best-effort
            if (auto it = kMainTypeNames.find(token); it != kMainTypeNames.end()) {
                ct.types.push_back(it->second);
                seenMainType = true;
            }
        } else {
            // After the first main type, everything else is either another main type
            // or a subtype.
            if (auto it = kMainTypeNames.find(token); it != kMainTypeNames.end()) {
                ct.types.push_back(it->second);
            } else {
                ct.subtypes.emplace_back(token);
            }
        }
    }
    return ct;
}

bool CardType::has(CardMainType t) const noexcept {
    return std::find(types.begin(), types.end(), t) != types.end();
}

bool CardType::has(CardSuperType t) const noexcept {
    return std::find(supertypes.begin(), supertypes.end(), t) != supertypes.end();
}

bool CardType::hasSubtype(std::string_view sub) const noexcept {
    for (const auto& s : subtypes)
        if (s == sub) return true;
    return false;
}

bool CardType::isPermanent() const noexcept {
    for (auto t : types) {
        switch (t) {
            case CardMainType::Creature:
            case CardMainType::Artifact:
            case CardMainType::Enchantment:
            case CardMainType::Land:
            case CardMainType::Planeswalker:
            case CardMainType::Battle:
                return true;
            default: break;
        }
    }
    return false;
}

std::string_view CardType::superTypeName(CardSuperType t) noexcept {
    switch (t) {
        case CardSuperType::Basic:     return "Basic";
        case CardSuperType::Legendary: return "Legendary";
        case CardSuperType::Snow:      return "Snow";
        case CardSuperType::World:     return "World";
        case CardSuperType::Elite:     return "Elite";
    }
    return "";
}

std::string_view CardType::mainTypeName(CardMainType t) noexcept {
    switch (t) {
        case CardMainType::Creature:     return "Creature";
        case CardMainType::Instant:      return "Instant";
        case CardMainType::Sorcery:      return "Sorcery";
        case CardMainType::Artifact:     return "Artifact";
        case CardMainType::Enchantment:  return "Enchantment";
        case CardMainType::Land:         return "Land";
        case CardMainType::Planeswalker: return "Planeswalker";
        case CardMainType::Battle:       return "Battle";
        case CardMainType::Dungeon:      return "Dungeon";
        case CardMainType::Tribal:       return "Tribal";
        case CardMainType::Kindred:      return "Kindred";
    }
    return "";
}

std::string CardType::toString() const {
    std::string result;
    for (auto st : supertypes) {
        if (!result.empty()) result += ' ';
        result += superTypeName(st);
    }
    for (auto mt : types) {
        if (!result.empty()) result += ' ';
        result += mainTypeName(mt);
    }
    if (!subtypes.empty()) {
        result += " \xe2\x80\x94 "; // UTF-8 em dash
        bool first = true;
        for (const auto& sub : subtypes) {
            if (!first) result += ' ';
            result += sub;
            first = false;
        }
    }
    return result;
}

} // namespace mtg
