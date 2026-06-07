#pragma once
#include "../card/CardRules.h"
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>

namespace mtg {

// Stores all loaded card definitions, indexed by canonical name.
// Load from Forge's cardsfolder (recursively scans subdirectories for .txt files).
class CardDb {
public:
    // Recursively load all .txt files under dir.
    // Skips files that fail to parse (logs nothing — silent drop).
    void loadFromDirectory(const std::filesystem::path& dir);

    // Load cards from a Forge cardsfolder.zip archive.
    // Each .txt entry inside the ZIP is parsed as a card script.
    void loadFromZip(const std::filesystem::path& zipPath);

    // Resolve the optional user "custom cards" overlay directory:
    //   $CITADEL_CUSTOM_CARDS if set, else %APPDATA%/CitadelMTG/customcards.
    // This is where training/scryfall_to_cards.py writes generated scripts.
    // Returns an empty path if it cannot be resolved.
    static std::filesystem::path customCardsDir();

    // Load the custom-cards overlay (if the directory exists) on top of the
    // current set. Cards here OVERRIDE base cards of the same name, so this
    // also lets you patch/replace existing scripts. No-op if absent.
    // Call before wireBackFaces() so overlaid DFCs get linked too.
    void loadCustomCards();

    const CardRules* find(std::string_view name) const noexcept;

    // Second-pass: wire each card's backFace pointer to the card named by its altName.
    // Call once after all cards are loaded. Safe to call multiple times (idempotent).
    void wireBackFaces();

    size_t size() const noexcept { return m_cards.size(); }
    bool   empty() const noexcept { return m_cards.empty(); }

    // Range-based for support
    auto begin() const noexcept { return m_cards.begin(); }
    auto end()   const noexcept { return m_cards.end(); }

    // Hot-reload: re-parse a single card's .txt file and replace its rules entry.
    // Useful for iterating on card scripting without restarting the app.
    // Returns true if the card was found and successfully re-parsed.
    bool reloadCard(const std::filesystem::path& cardsDir, std::string_view cardName);

private:
    std::unordered_map<std::string, CardRules> m_cards;
};

} // namespace mtg
