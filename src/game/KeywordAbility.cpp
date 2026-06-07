#include "KeywordAbility.h"
#include "../core/card/CardRules.h"

namespace mtg {

KeywordAbility parseKeyword(std::string_view s) noexcept {
    // Prefix match so "Hexproof from Blue" → Hexproof, etc.
    struct Entry { std::string_view name; KeywordAbility kw; };
    static constexpr Entry kTable[] = {
        // Two-word keywords first (longer prefix wins)
        {"Double Strike", KeywordAbility::DoubleStrike},
        {"First Strike",  KeywordAbility::FirstStrike},
        // Single-word keywords
        {"Flying",        KeywordAbility::Flying},
        {"Reach",         KeywordAbility::Reach},
        {"Vigilance",     KeywordAbility::Vigilance},
        {"Haste",         KeywordAbility::Haste},
        {"Trample",       KeywordAbility::Trample},
        {"Deathtouch",    KeywordAbility::Deathtouch},
        {"Lifelink",      KeywordAbility::Lifelink},
        {"Menace",        KeywordAbility::Menace},
        {"Hexproof",      KeywordAbility::Hexproof},
        {"Shroud",        KeywordAbility::Shroud},
        {"Indestructible",KeywordAbility::Indestructible},
        {"Flash",         KeywordAbility::Flash},
        {"Infect",        KeywordAbility::Infect},
        {"Wither",        KeywordAbility::Wither},
        {"Undying",       KeywordAbility::Undying},
        {"Persist",       KeywordAbility::Persist},
        {"Ward",                  KeywordAbility::Ward},
        // Protection — prefix-match: "Protection from Red" → ProtectionRed
        {"Protection from White", KeywordAbility::ProtectionWhite},
        {"Protection from Blue",  KeywordAbility::ProtectionBlue},
        {"Protection from Black", KeywordAbility::ProtectionBlack},
        {"Protection from Red",   KeywordAbility::ProtectionRed},
        {"Protection from Green", KeywordAbility::ProtectionGreen},
        {"Protection from Everything", KeywordAbility::ProtectionAll},
        // Forge sometimes uses "Protection:Color" style
        {"Protection:White",      KeywordAbility::ProtectionWhite},
        {"Protection:Blue",       KeywordAbility::ProtectionBlue},
        {"Protection:Black",      KeywordAbility::ProtectionBlack},
        {"Protection:Red",        KeywordAbility::ProtectionRed},
        {"Protection:Green",      KeywordAbility::ProtectionGreen},
        {"Protection:Everything", KeywordAbility::ProtectionAll},
        // Landwalk
        {"Swampwalk",    KeywordAbility::Swampwalk},
        {"Islandwalk",   KeywordAbility::Islandwalk},
        {"Mountainwalk", KeywordAbility::Mountainwalk},
        {"Forestwalk",   KeywordAbility::Forestwalk},
        {"Plainswalk",   KeywordAbility::Plainswalk},
        // Other evasion
        {"Fear",         KeywordAbility::Fear},
        {"Shadow",       KeywordAbility::Shadow},
    };
    for (const auto& e : kTable) {
        if (s.size() >= e.name.size() && s.substr(0, e.name.size()) == e.name)
            return e.kw;
    }
    return KeywordAbility::None;
}

uint32_t buildKeywordMask(const CardRules& rules) noexcept {
    uint32_t mask = 0;
    for (const auto& kw : rules.keywords) {
        auto parsed = parseKeyword(kw);
        mask |= static_cast<uint32_t>(parsed);
    }
    return mask;
}

} // namespace mtg
