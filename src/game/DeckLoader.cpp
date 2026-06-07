#include "DeckLoader.h"
#include "Card.h"
#include <fstream>
#include <iostream>
#include <sstream>
#include <charconv>

namespace mtg {

namespace {

// Strip leading/trailing whitespace
std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r'))
        s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
        s.remove_suffix(1);
    return s;
}

// Parse "N CardName" or "N CardName|SetCode|Num" — returns {name, count} or {name,0} on failure
std::pair<std::string, int> parseCardLine(std::string_view line) {
    line = trim(line);
    if (line.empty() || line[0] == '#') return {"", 0};

    // Strip optional set-code suffix: "CardName|SET|123" → "CardName"
    auto pipe = line.find('|');
    if (pipe != std::string_view::npos)
        line = line.substr(0, pipe);
    line = trim(line);

    // First token is count if it starts with a digit
    auto space = line.find(' ');
    if (space == std::string_view::npos) {
        // No count — treat as single card
        return {std::string(line), 1};
    }

    int count = 1;
    std::from_chars(line.data(), line.data() + space, count);
    if (count <= 0) count = 1;

    std::string name(trim(line.substr(space + 1)));
    return {name, count};
}

} // namespace

DeckLoader::Deck DeckLoader::loadFromString(std::string_view text) {
    Deck deck;
    // Sections we care about
    enum class Section { None, Main, Sideboard, Commander, Companion, Metadata } section = Section::None;

    auto process = [&](std::string_view line) {
        line = trim(line);
        if (line.empty()) return;
        // Strip UTF-8 BOM (EF BB BF) if present at start of line.
        // Some deck editors write BOM at the start of the file; it ends up on
        // the first line if the file is read line-by-line as UTF-8 bytes.
        if (line.size() >= 3 &&
            static_cast<unsigned char>(line[0]) == 0xEF &&
            static_cast<unsigned char>(line[1]) == 0xBB &&
            static_cast<unsigned char>(line[2]) == 0xBF)
            line = line.substr(3);
        if (line.empty()) return;

        // Section header detection
        if (line.front() == '[') {
            auto close = line.find(']');
            std::string_view hdr = (close != std::string_view::npos)
                                   ? line.substr(1, close - 1) : line.substr(1);
            // Case-insensitive comparison for common headers
            if      (hdr == "Main"     || hdr == "main")     section = Section::Main;
            else if (hdr == "Sideboard"|| hdr == "sideboard") section = Section::Sideboard;
            else if (hdr == "Commander"|| hdr == "commander") section = Section::Commander;
            else if (hdr == "Companion"|| hdr == "companion") section = Section::Companion;
            else if (hdr == "metadata" || hdr == "Metadata")  section = Section::Metadata;
            else section = Section::None; // [Planes], [Schemes], etc. — skip
            return;
        }

        // Metadata key=value
        if (section == Section::Metadata) {
            auto eq = line.find('=');
            if (eq != std::string_view::npos) {
                auto key = trim(line.substr(0, eq));
                auto val = trim(line.substr(eq + 1));
                if      (key == "Name")        deck.name        = std::string(val);
                else if (key == "Personality") deck.personality = std::string(val);
            }
            return;
        }

        // Card line in one of the card sections
        auto [name, count] = parseCardLine(line);
        if (name.empty() || count <= 0) return;

        CardEntry entry{name, count};
        if      (section == Section::Main      || section == Section::None)
            deck.mainboard.push_back(entry);
        else if (section == Section::Sideboard)
            deck.sideboard.push_back(entry);
        else if (section == Section::Commander)
            deck.commander.push_back(entry);
        else if (section == Section::Companion)
            deck.companion.push_back(entry);
    };

    // Process line by line
    std::string_view remaining = text;
    while (!remaining.empty()) {
        auto nl = remaining.find('\n');
        std::string_view line = (nl == std::string_view::npos)
                                ? remaining : remaining.substr(0, nl);
        remaining = (nl == std::string_view::npos) ? "" : remaining.substr(nl + 1);
        process(line);
    }
    return deck;
}

std::optional<DeckLoader::Deck> DeckLoader::loadFromFile(const std::filesystem::path& path) {
    std::ifstream f(path);
    if (!f) return std::nullopt;
    std::ostringstream ss;
    ss << f.rdbuf();
    return loadFromString(ss.str());
}

int DeckLoader::buildDeck(const Deck& deck, const CardDb& db,
                           GameState& game, uint8_t playerId) {
    int created = 0;

    int skipped = 0;
    auto addCards = [&](const std::vector<CardEntry>& list) {
        for (const auto& entry : list) {
            const CardRules* rules = db.find(entry.name);
            if (!rules) {
                std::cerr << "[DeckLoader] Card not found in DB (skipped): \""
                          << entry.name << "\"\n";
                ++skipped;
                continue;
            }
            for (int i = 0; i < entry.count; ++i) {
                game.createCard(rules, playerId);
                ++created;
            }
        }
    };

    // Commanders go to the command zone, not the library.
    // Collect commander names for Partner With validation.
    std::vector<std::string> commanderNames;
    for (const auto& entry : deck.commander)
        commanderNames.push_back(entry.name);

    for (const auto& entry : deck.commander) {
        const CardRules* rules = db.find(entry.name);
        if (!rules) continue;

        // Partner With: only valid if both named partners are listed as commanders.
        // If only one of the pair is present, skip loading the second silently.
        if (rules->hasPartnerWith && !rules->partnerWithName.empty()) {
            bool partnerPresent = false;
            for (const auto& n : commanderNames)
                if (n == rules->partnerWithName) { partnerPresent = true; break; }
            if (!partnerPresent && commanderNames.size() > 1) {
                std::cerr << "[DeckLoader] Partner With '" << entry.name
                          << "' requires '" << rules->partnerWithName << "' as co-commander.\n";
            }
        }

        for (int i = 0; i < entry.count; ++i) {
            Card* card = game.createCard(rules, playerId); // starts in Library
            Card* inCmd = game.moveToZone(card->id, ZoneType::Command, playerId);
            if (inCmd) inCmd->isCommander = true;
            ++created;
        }
    }

    // Companion: set aside. We keep it in the command zone for visibility (it is
    // NOT a commander); the once-per-game {3} special action moves it to hand.
    if (playerId < 4) {
        for (const auto& entry : deck.companion) {
            const CardRules* rules = db.find(entry.name);
            if (!rules) continue;
            Card* card  = game.createCard(rules, playerId);   // starts in Library
            Card* inCmd = game.moveToZone(card->id, ZoneType::Command, playerId);
            if (inCmd) {
                game.companionId[playerId]   = inCmd->id;
                game.companionUsed[playerId] = false;
                ++created;
            }
            break;   // a deck has at most one companion
        }
    }

    addCards(deck.mainboard);
    // Sideboard not added to the game deck (used for construction only)
    if (skipped > 0)
        std::cerr << "[DeckLoader] " << skipped << " card(s) missing from DB for player "
                  << (int)playerId << " — check card scripts are loaded.\n";
    return created;
}

int DeckLoader::loadAndBuild(const std::filesystem::path& path,
                              const CardDb& db, GameState& game,
                              uint8_t playerId, bool shuffle) {
    auto deck = loadFromFile(path);
    if (!deck) return -1;
    int n = buildDeck(*deck, db, game, playerId);
    if (shuffle)
        game.player(playerId).library().shuffle(game.rng());
    return n;
}

} // namespace mtg
