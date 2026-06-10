#include "CardRules.h"
#include <algorithm>

namespace mtg {

bool CardRules::hasKeyword(std::string_view kw) const noexcept {
    for (const auto& k : keywords)
        if (k == kw) return true;
    return false;
}

uint8_t CardRules::commanderColorIdentity() const {
    uint8_t mask = manaCost.colorIdentity();

    // Add any coloured mana symbol found inside braces anywhere in the card's
    // rules. Handles plain ({W}), hybrid ({W/U}), and Phyrexian ({W/P}) pips.
    auto scan = [&](const std::string& s) {
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] != '{') continue;
            size_t j = s.find('}', i);
            if (j == std::string::npos) break;
            for (size_t k = i + 1; k < j; ++k) {
                switch (s[k]) {
                    case 'W': mask |= 0x01; break;
                    case 'U': mask |= 0x02; break;
                    case 'B': mask |= 0x04; break;
                    case 'R': mask |= 0x08; break;
                    case 'G': mask |= 0x10; break;
                    default: break;
                }
            }
            i = j;
        }
    };

    scan(oracleText);
    for (const auto& l : abilityLines)       scan(l);
    for (const auto& l : triggerLines)       scan(l);
    for (const auto& l : replacementLines)   scan(l);
    for (const auto& l : staticAbilityLines) scan(l);
    for (const auto& kv : svars)             scan(kv.second);
    return mask;
}

} // namespace mtg
