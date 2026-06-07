#pragma once
#include <string>
#include <unordered_map>
#include <fstream>
#include <filesystem>

namespace ui {

class Localization {
public:
    static Localization& instance() {
        static Localization inst;
        return inst;
    }

    void load(const std::string& langCode) {
        m_strings.clear();
        m_langCode = langCode;
        if (langCode == "en") return;  // English is default: keys are already English

        namespace fs = std::filesystem;
        std::string path;
        if (const char* ap = std::getenv("APPDATA"))
            path = std::string(ap) + "\\CitadelMTG\\lang_" + langCode + ".txt";
        if (path.empty() || !fs::exists(path)) return;

        std::ifstream f(path);
        std::string line;
        while (std::getline(f, line)) {
            auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            m_strings[line.substr(0, eq)] = line.substr(eq + 1);
        }
    }

    // Look up a localized string; returns key if not found.
    const std::string& get(const std::string& key) const {
        auto it = m_strings.find(key);
        return it != m_strings.end() ? it->second : key;
    }

    const std::string& langCode() const noexcept { return m_langCode; }

private:
    std::unordered_map<std::string, std::string> m_strings;
    std::string m_langCode = "en";
};

// Convenience shorthand: L("key") returns the localized string.
inline const std::string& L(const std::string& key) {
    return Localization::instance().get(key);
}

} // namespace ui
