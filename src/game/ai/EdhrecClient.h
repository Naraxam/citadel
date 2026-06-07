#pragma once
#include <optional>
#include <string>
#include <vector>

namespace mtg {

// Thin WinHTTP client for json.edhrec.com used by the deck builder's
// "import from EDHREC" flow. Mirrors the layout the Python helper script in
// training/edhrec_import.py already uses:
//   /pages/commanders/<slug>.json       → panels.taglinks
//   /pages/average-decks/<cmd>/<tag>.json  (fallback /pages/average-decks/<cmd>.json)
//
// All calls are synchronous. Callers should run them on a worker thread; a
// single tag-list fetch is typically <500ms but the average-deck fetch can
// take a second or two over slow links.
namespace EdhrecClient {

struct TagInfo {
    std::string name;   // e.g. "Dragons"
    std::string slug;   // e.g. "dragons"
    int         count = 0;  // how many decks include the commander under this tag
};

struct DeckLine {
    int         count = 1;
    std::string name;   // card name as EDHREC lists it
};

// Convert a card name to EDHREC's URL slug ("The Ur-Dragon" → "the-ur-dragon").
std::string slugify(const std::string& name);

// Fetch the panel's tag list for the given commander, sorted by inclusion
// count descending. Returns std::nullopt on network failure or unexpected
// JSON format. An empty vector is returned when the commander page parses
// but has no taglinks panel.
std::optional<std::vector<TagInfo>>
    fetchCommanderTags(const std::string& commanderName);

// Fetch the tag-specific average deck (falling back to the generic one if
// the per-tag variant 404s). Returns std::nullopt on failure.
std::optional<std::vector<DeckLine>>
    fetchAverageDeck(const std::string& commanderName,
                     const std::string& tagSlug);

}  // namespace EdhrecClient
}  // namespace mtg
