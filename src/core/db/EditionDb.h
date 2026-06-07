#pragma once
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace mtg {

// EditionDb — parses Forge's res/editions/*.txt to learn which sets each card
// appears in. We don't need printing-level art yet (we still pull a single
// canonical image via Scryfall's oracle bulk data), but we DO need a fast
// name→sets lookup for the deck builder's Set filter dropdown.
//
// Format example (Foundations.txt):
//   [metadata]
//   Code=FDN
//   Date=2024-11-15
//   Name=Foundations
//   Type=Core
//
//   [cards]
//   1 M Sire of Seven Deaths @Artist
//   2 R Arahbo, the First Fang @Artist
//   ...
//
// Card names are case-preserved; we lower-case them in the index for
// case-insensitive matching against user-typed search text.
class EditionDb {
public:
    struct SetInfo {
        std::string code;   // "FDN"
        std::string name;   // "Foundations"
        std::string date;   // "2024-11-15"  (lex-sortable)
        std::string type;   // "Core", "Expansion", "Commander", "Promo", ...
    };

    // Load every edition file under `editionsDir`. Returns the number of sets
    // parsed; safe to re-call.
    int loadFromDirectory(const std::filesystem::path& editionsDir);

    // All known sets, sorted newest-first by date for the UI dropdown.
    const std::vector<SetInfo>& sets() const noexcept { return m_sets; }

    // Set codes a card appears in (empty if unknown).  Card name is matched
    // case-insensitively but exact otherwise — Forge editions list "Sol Ring"
    // not "sol ring".
    const std::vector<std::string>& setsForCard(const std::string& name) const;

    // True if this card has at least one printing in the named set code.
    bool cardInSet(const std::string& name, const std::string& code) const;

    // Display name for a set code, "" if unknown.
    std::string setName(const std::string& code) const;

private:
    std::vector<SetInfo>                         m_sets;
    std::unordered_map<std::string, std::vector<std::string>> m_nameToSets;
    std::unordered_map<std::string, std::string> m_codeToName;

    static const std::vector<std::string>        s_empty;
};

} // namespace mtg
