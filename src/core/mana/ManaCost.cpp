#include "ManaCost.h"
#include <charconv>
#include <sstream>

namespace mtg {

ManaCost ManaCost::parse(std::string_view text) {
    ManaCost result;

    if (text == "no cost") {
        result.m_noCost = true;
        return result;
    }

    // Split on spaces; each token is further analysed below.
    // Handles:
    //   Pure integer  "3"      → 3 generic
    //   Single letter "R"      → RED shard
    //   Hybrid        "2/W"    → {2/W} shard (contains '/')
    //   Concatenated  "4R"     → 4 generic + RED shard  (Forge compact format)
    //   Multi-colored "GG"     → GREEN + GREEN shards
    std::string_view remaining = text;
    while (!remaining.empty()) {
        auto space = remaining.find(' ');
        std::string_view token = (space == std::string_view::npos)
                                 ? remaining : remaining.substr(0, space);
        remaining = (space == std::string_view::npos)
                    ? std::string_view{} : remaining.substr(space + 1);

        if (token.empty()) continue;

        // X mana symbol
        if (token == "X" || token == "X X") { result.m_hasX = true; continue; }

        // Pure integer → generic
        int value = 0;
        auto [ptr, ec] = std::from_chars(token.data(), token.data() + token.size(), value);
        if (ec == std::errc{} && ptr == token.data() + token.size()) {
            result.m_generic += value;
            continue;
        }

        // Hybrid shard (contains '/') — pass whole token to parseNonGeneric
        if (token.find('/') != std::string_view::npos) {
            result.m_shards.push_back(ManaCostShard::parseNonGeneric(token));
            continue;
        }

        // Forge compact format: leading digit(s) followed by colored letters, e.g. "4R", "1GG"
        size_t digitEnd = 0;
        while (digitEnd < token.size() &&
               token[digitEnd] >= '0' && token[digitEnd] <= '9') ++digitEnd;
        if (digitEnd > 0 && digitEnd < token.size()) {
            int gen = 0;
            std::from_chars(token.data(), token.data() + digitEnd, gen);
            result.m_generic += gen;
            for (size_t i = digitEnd; i < token.size(); ++i) {
                if (token[i] == 'X') { result.m_hasX = true; continue; }
                result.m_shards.push_back(ManaCostShard::parseNonGeneric({token.data() + i, 1}));
            }
            continue;
        }

        // Multi-char colored string without digits ("GG", "WW", "UB") — split per char
        if (token.size() > 1) {
            for (char ch : token) {
                if (ch == 'X') { result.m_hasX = true; continue; }
                result.m_shards.push_back(ManaCostShard::parseNonGeneric({&ch, 1}));
            }
            continue;
        }

        // Single-char symbol
        result.m_shards.push_back(ManaCostShard::parseNonGeneric(token));
    }

    return result;
}

int ManaCost::cmc() const noexcept {
    if (m_noCost) return 0;
    int total = m_generic;
    for (const auto& s : m_shards)
        total += s.cmc();
    return total;
}

ManaCost ManaCost::reduceGeneric(int n) const noexcept {
    ManaCost out = *this;
    if (out.m_noCost) return out;
    out.m_generic = out.m_generic - n;
    if (out.m_generic < 0) out.m_generic = 0;
    return out;
}

uint8_t ManaCost::colorIdentity() const noexcept {
    uint8_t mask = 0;
    for (const auto& s : m_shards)
        mask |= s.colorMask();
    return mask;
}

std::string ManaCost::toString() const {
    if (m_noCost) return "";

    std::string result;
    if (m_hasX) result += "{X}";
    if (m_shards.empty() && m_generic == 0 && !m_hasX) {
        result = "{0}";
    } else {
        if (m_generic > 0)
            result += '{' + std::to_string(m_generic) + '}';
        for (const auto& s : m_shards)
            result += s.display();
    }
    return result;
}

} // namespace mtg
