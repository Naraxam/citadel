#pragma once
#include "GameState.h"
#include "../core/db/CardDb.h"
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace mtg {

// Loads Forge-format deck files (.dck) and builds them in a GameState.
//
// Supported Forge .dck format:
//   [metadata]
//   Name=My Deck
//
//   [Main]
//   4 Lightning Bolt
//   4 Lightning Bolt|3ED|50       <- optional |SetCode|CollNum suffix (ignored for lookup)
//   1 Snapcaster Mage|INN
//
//   [Sideboard]
//   2 Surgical Extraction
//
// Also accepts plain "N CardName" without headers.
class DeckLoader {
public:
    struct CardEntry {
        std::string name;
        int         count = 1;
    };

    struct Deck {
        std::string            name;
        std::string            personality; // optional: locked AI personality name (from Personality= in [metadata])
        std::vector<CardEntry> mainboard;
        std::vector<CardEntry> sideboard;
        std::vector<CardEntry> commander;
        std::vector<CardEntry> companion;   // [Companion] — set aside, {3} to hand
    };

    // Parse a .dck file. Returns nullopt on read error.
    static std::optional<Deck> loadFromFile(const std::filesystem::path& path);

    // Parse deck from a string (useful for tests or inline decks).
    static Deck loadFromString(std::string_view text);

    // Create cards from a Deck in the given GameState (adds to library).
    // Missing cards are silently skipped; returns the number of cards created.
    static int buildDeck(const Deck& deck, const CardDb& db,
                          GameState& game, uint8_t playerId);

    // Convenience: load file + build deck, shuffle the library.
    // Returns card count on success, -1 if the file could not be opened.
    static int loadAndBuild(const std::filesystem::path& path,
                             const CardDb& db,
                             GameState& game,
                             uint8_t playerId,
                             bool shuffle = true);
};

} // namespace mtg
