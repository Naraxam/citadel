#include "EditionDb.h"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <iostream>
#include <sstream>

namespace mtg {

const std::vector<std::string> EditionDb::s_empty;

namespace {

std::string toLower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower((unsigned char)c));
    return s;
}

// Trim leading/trailing ASCII whitespace + '\r'.
std::string trim(std::string s) {
    auto issp = [](unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    size_t a = 0, b = s.size();
    while (a < b && issp((unsigned char)s[a])) ++a;
    while (b > a && issp((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}

// Split a [cards] line "1 M Sire of Seven Deaths @Lius Lasahido" → "Sire of Seven Deaths".
// Strip leading "<num> <rarity>" tokens and trailing "@<artist>" if present.
std::string cardNameFromLine(const std::string& line) {
    std::string s = trim(line);
    if (s.empty()) return {};
    // Drop the artist suffix if present.
    auto at = s.find(" @");
    if (at != std::string::npos) s = s.substr(0, at);
    // Drop leading collector number.
    size_t i = 0;
    while (i < s.size() && (std::isdigit((unsigned char)s[i]) ||
                            s[i] == '★' || s[i] == '*' || s[i] == 'a' || s[i] == 'b'))
        ++i;
    // Drop number-suffix and the space.
    while (i < s.size() && s[i] == ' ') ++i;
    // Drop the rarity letter (C/U/R/M/L/S/T/P) and one trailing space.
    if (i + 1 < s.size() && s[i + 1] == ' ' &&
        (s[i] == 'C' || s[i] == 'U' || s[i] == 'R' || s[i] == 'M' ||
         s[i] == 'L' || s[i] == 'S' || s[i] == 'T' || s[i] == 'P')) {
        i += 2;
    }
    return trim(s.substr(i));
}

} // namespace

int EditionDb::loadFromDirectory(const std::filesystem::path& editionsDir) {
    namespace fs = std::filesystem;
    if (!fs::exists(editionsDir)) return 0;

    std::vector<SetInfo> setsLocal;
    std::unordered_map<std::string, std::vector<std::string>> nameToSets;
    std::unordered_map<std::string, std::string> codeToName;

    std::error_code ec;
    for (auto& e : fs::directory_iterator(editionsDir, ec)) {
        if (!e.is_regular_file()) continue;
        if (e.path().extension() != ".txt") continue;

        std::ifstream f(e.path());
        if (!f) continue;

        SetInfo info;
        bool inMetadata = false, inCards = false;
        std::string line;
        std::vector<std::string> cardsHere;

        while (std::getline(f, line)) {
            std::string t = trim(line);
            if (t.empty() || t[0] == '#') continue;
            if (t[0] == '[') {
                // Lower-cased section header (e.g. "[metadata]", "[cards]")
                std::string sec = toLower(t.substr(1, t.size() - 2));
                inMetadata = (sec == "metadata");
                inCards    = (sec == "cards" || sec == "tokens");
                continue;
            }
            if (inMetadata) {
                auto eq = t.find('=');
                if (eq == std::string::npos) continue;
                std::string key = toLower(trim(t.substr(0, eq)));
                std::string val = trim(t.substr(eq + 1));
                if      (key == "code")        info.code = val;
                else if (key == "name")        info.name = val;
                else if (key == "date")        info.date = val;
                else if (key == "type")        info.type = val;
            } else if (inCards) {
                std::string nm = cardNameFromLine(line);
                if (!nm.empty()) cardsHere.push_back(std::move(nm));
            }
        }

        if (info.code.empty() || info.name.empty()) continue;
        setsLocal.push_back(info);
        codeToName[info.code] = info.name;
        for (auto& nm : cardsHere) {
            auto& v = nameToSets[toLower(nm)];
            // Dedup: a card can appear multiple times in one edition (variant
            // borderless printings list it again).
            if (std::find(v.begin(), v.end(), info.code) == v.end())
                v.push_back(info.code);
        }
    }

    // Newest first by date (lex sort works on YYYY-MM-DD); blank dates sink.
    std::sort(setsLocal.begin(), setsLocal.end(),
              [](const SetInfo& a, const SetInfo& b) {
                  if (a.date.empty() != b.date.empty()) return !a.date.empty();
                  return a.date > b.date;
              });

    m_sets        = std::move(setsLocal);
    m_nameToSets  = std::move(nameToSets);
    m_codeToName  = std::move(codeToName);

    std::cout << "[EditionDb] Loaded " << m_sets.size() << " sets ("
              << m_nameToSets.size() << " cards indexed)\n";
    return static_cast<int>(m_sets.size());
}

const std::vector<std::string>& EditionDb::setsForCard(const std::string& name) const {
    auto it = m_nameToSets.find(toLower(name));
    return it == m_nameToSets.end() ? s_empty : it->second;
}

bool EditionDb::cardInSet(const std::string& name, const std::string& code) const {
    auto& v = setsForCard(name);
    return std::find(v.begin(), v.end(), code) != v.end();
}

std::string EditionDb::setName(const std::string& code) const {
    auto it = m_codeToName.find(code);
    return it == m_codeToName.end() ? std::string{} : it->second;
}

} // namespace mtg
