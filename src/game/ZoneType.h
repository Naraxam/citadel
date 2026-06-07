#pragma once
#include <string_view>

namespace mtg {

enum class ZoneType {
    Library,
    Hand,
    Battlefield,
    Graveyard,
    Exile,
    Stack,
    Command,
};

inline std::string_view zoneName(ZoneType z) noexcept {
    switch (z) {
        case ZoneType::Library:     return "Library";
        case ZoneType::Hand:        return "Hand";
        case ZoneType::Battlefield: return "Battlefield";
        case ZoneType::Graveyard:   return "Graveyard";
        case ZoneType::Exile:       return "Exile";
        case ZoneType::Stack:       return "Stack";
        case ZoneType::Command:     return "Command";
    }
    return "Unknown";
}

} // namespace mtg
