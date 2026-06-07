#include "ScriptLine.h"
#include <charconv>

namespace mtg {

ScriptLine parseScriptLine(std::string_view raw) {
    ScriptLine result;

    // Split on " | " then each segment on "$ "
    bool first = true;
    while (!raw.empty()) {
        auto sep = raw.find(" | ");
        std::string_view segment = (sep == std::string_view::npos) ? raw : raw.substr(0, sep);
        raw = (sep == std::string_view::npos) ? std::string_view{} : raw.substr(sep + 3);

        if (segment.empty()) continue;

        // Find the "$ " separator in this segment
        auto dollar = segment.find("$ ");
        if (dollar == std::string_view::npos) {
            // Fallback: try bare "$" (no trailing space)
            dollar = segment.find('$');
            if (dollar == std::string_view::npos) continue;
            std::string key(segment.substr(0, dollar));
            std::string val(segment.substr(dollar + 1));
            if (first) {
                result.abilityType = std::move(key);
                result.effectType  = std::move(val);
                first = false;
            } else {
                result.params.emplace(std::move(key), std::move(val));
            }
        } else {
            std::string key(segment.substr(0, dollar));
            std::string val(segment.substr(dollar + 2));
            if (first) {
                result.abilityType = std::move(key);
                result.effectType  = std::move(val);
                first = false;
            } else {
                result.params.emplace(std::move(key), std::move(val));
            }
        }
    }

    return result;
}

std::string_view ScriptLine::get(std::string_view key, std::string_view defaultVal) const {
    auto it = params.find(std::string(key));
    return it != params.end() ? std::string_view(it->second) : defaultVal;
}

int ScriptLine::getInt(std::string_view key, int defaultVal) const {
    auto sv = get(key);
    if (sv.empty()) return defaultVal;
    int v = defaultVal;
    std::from_chars(sv.data(), sv.data() + sv.size(), v);
    return v;
}

int ScriptLine::getIntOrX(std::string_view key, int xValue, int defaultVal) const {
    auto sv = get(key);
    if (sv.empty()) return defaultVal;
    if (sv == "X") return xValue;
    int v = defaultVal;
    std::from_chars(sv.data(), sv.data() + sv.size(), v);
    return v;
}

} // namespace mtg
