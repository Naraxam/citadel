#pragma once
#include "../game/DeckLoader.h"
#include "../core/db/CardDb.h"
#include <SFML/Graphics.hpp>
#include <filesystem>
#include <string>
#include <vector>

namespace ui {

// Full-screen deck selection shown before a game starts.
// Two scrollable panels (Player 1 / Player 2) each show available .dck files.
// Click an entry to select it, scroll wheel or arrow keys to navigate.
// "Start Game" becomes enabled once both players have a deck.
class DeckBrowserScreen {
public:
    DeckBrowserScreen(const sf::Font& font,
                      const mtg::CardDb& db,
                      const std::filesystem::path& searchRoot);

    // Process one SFML event. Returns true when the user clicked "Start Game".
    bool onEvent(const sf::Event& event);

    // Render to the given window.
    void draw(sf::RenderWindow& window) const;

    // Paths selected by the user (valid after onEvent returns true)
    const std::filesystem::path& deckPath(uint8_t player) const { return m_selected[player]; }
    const std::string&           deckName(uint8_t player) const { return m_names[player]; }

private:
    const sf::Font&    m_font;
    const mtg::CardDb& m_db;

    // All discovered deck files
    std::vector<std::filesystem::path> m_decks;
    std::vector<std::string>           m_deckNames; // display names (from metadata or filename)

    // Per-player selection state
    int m_selectedIdx[2]  = {0, 0};  // index into m_decks
    int m_scrollOffset[2] = {0, 0};  // first visible row
    int m_activePanel     = 0;       // 0 = P1, 1 = P2

    std::filesystem::path m_selected[2];
    std::string           m_names[2]{"(default)", "(default)"};

    // Layout constants
    static constexpr float WIN_W      = 1280.f;
    static constexpr float WIN_H      = 800.f;
    static constexpr float PANEL_W    = 580.f;
    static constexpr float LIST_Y     = 90.f;
    static constexpr float LIST_H     = 590.f;
    static constexpr float ITEM_H     = 26.f;
    static constexpr int   VISIBLE    = static_cast<int>(LIST_H / ITEM_H);

    static constexpr float P0_X       = 20.f;
    static constexpr float P1_X       = 680.f;

    void scanDecks(const std::filesystem::path& root);
    void drawPanel(sf::RenderTarget& t, int player) const;
    void drawButton(sf::RenderTarget& t, const std::string& label,
                    float x, float y, float w, float h, bool enabled) const;

    // Returns {panel, row_index} if the point is inside a list panel, or {-1,-1}
    std::pair<int,int> hitTestList(float px, float py) const;

    // Is the "Start Game" button at this position?
    bool hitStartButton(float px, float py) const;
    bool hitRandomButton(int player, float px, float py) const;

    void clampScroll(int player);
    void selectRandom(int player);
};

} // namespace ui
