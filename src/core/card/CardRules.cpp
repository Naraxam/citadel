#include "CardRules.h"
#include <algorithm>

namespace mtg {

bool CardRules::hasKeyword(std::string_view kw) const noexcept {
    for (const auto& k : keywords)
        if (k == kw) return true;
    return false;
}

} // namespace mtg
