#pragma once
#include <SFML/Graphics.hpp>
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace mtg { class CardDb; }

namespace ui {

// Two-column deck selection screen.
//   Left  — player deck list
//   Right — AI deck list
class MatchSetupScreen {
public:
    MatchSetupScreen(const sf::Font& font,
                     const std::filesystem::path& decksRoot,
                     const mtg::CardDb* db = nullptr);

    void refresh();  // rescan decks (call after returning from deck builder)

    enum class Action { None, StartGame, Back };
    Action onEvent(const sf::Event& ev);
    void   draw(sf::RenderWindow& w) const;

    std::filesystem::path playerDeckPath() const;
    std::filesystem::path aiDeckPath()     const;

private:
    const sf::Font&        m_font;
    std::filesystem::path  m_decksRoot;
    const mtg::CardDb*     m_db = nullptr;   // for computing deck colour identity

    std::vector<std::filesystem::path> m_decks;
    std::vector<std::string>           m_deckNames;
    // Per-deck metadata used only by the filter (text search + colour pips).
    // These are populated LAZILY — scanDecks() only enumerates paths/names and
    // reads no file contents, so opening the screen with thousands of decks is
    // instant. The first filter that actually needs a deck's body/colour reads
    // that file exactly once (deriving both at once) and caches the result via
    // m_deckMetaLoaded. Mutable so the lazy fill can run from const callers.
    mutable std::vector<std::string>   m_deckBodyLower;  // lower-case file text + filename
    mutable std::vector<uint8_t>       m_deckCI;         // WUBRG bits from commander(s)
    mutable std::vector<char>          m_deckCIKnown;    // 1 if a commander resolved
    mutable std::vector<char>          m_deckMetaLoaded; // 1 once the above are filled

    // Read deck i's file once (if not already) and fill m_deckBodyLower /
    // m_deckCI / m_deckCIKnown. No-op when already loaded.
    void ensureMeta(int i) const;

    // Colour-identity filter pips: W U B R G + colourless. Empty = no filter.
    std::array<bool, 6> m_colorFilter{};

    int m_playerSel    = -1;
    int m_aiSel        = -1;
    int m_activeCol    =  0;   // 0 = player list, 1 = AI list (keyboard focus)
    int m_playerScroll =  0;
    int m_aiScroll     =  0;
    int m_playerHover  = -1;
    int m_aiHover      = -1;

    // Click-and-drag scrollbar state.
    enum class DragWhich { None, Player, AI };
    DragWhich m_dragWhich = DragWhich::None;
    float     m_dragGrabY = 0.f;

    // Live deck-name filter — one per side (0 = player/left, 1 = AI/right).
    std::string      m_filter[2];
    int              m_filterActiveSide = -1;   // -1 none, else which box is typing
    std::vector<int> m_visible[2];              // indices into m_decks per side
    void rebuildFilter();
    static int sideOf(float panelX) noexcept { return panelX >= RIGHT_X - 1.f ? 1 : 0; }

    // AI difficulty: maps slider position to MCTS iterations
    int m_aiDifficulty = 2;  // 0=Easy(20), 1=Medium(100), 2=Hard(200), 3=Expert(500)
    static constexpr int kDifficultyIter[] = {20, 100, 200, 500};

    // Starting life total selector
    int m_startingLife = 40;  // default Commander (40); can be set to 20 or custom
    static constexpr int kLifeOptions[] = {20, 30, 40};

    // Editable player name (defaults to "Player"). Opponent name is auto-set
    // to the AI deck's commander name in startGameWithDecks.
    std::string m_playerName     = "Player";
    bool        m_nameEditActive = false;

    int m_numPlayers = 2;

public:
    int  aiMctsIterations() const noexcept { return kDifficultyIter[m_aiDifficulty]; }
    int  startingLife()     const noexcept { return m_startingLife; }
    int  numPlayers()       const noexcept { return m_numPlayers; }
    const std::string& playerName() const noexcept { return m_playerName; }

    // Pull the commander card name out of a .dck file. Returns "AI" if no
    // [Commander] block found. Lightweight enough for repeated calls.
    static std::string commanderNameOf(const std::filesystem::path& deck);

    // Random AI deck path from the pool, excluding any already chosen for
    // this match. Used to fill seats 1..N-1 in 3/4 player mode.
    std::filesystem::path pickRandomAiDeck(
        const std::vector<std::filesystem::path>& exclude) const;

    // ── Layout 
    static constexpr float WIN_W   = 1560.f;
    static constexpr float WIN_H   = 800.f;
    static constexpr float PANEL_W = 710.f;
    // Panel height is sized so that:
    //   panel bottom = PANEL_Y + PANEL_H
    //   + 8 px gap + preview block (~95 px = 16 header + 5*13 + slack)
    //   stays ABOVE the Starting Life row at WIN_H - 150.
    //   With WIN_H 800 and PANEL_Y 90, that gives PANEL_H ≤ 447.
    static constexpr float PANEL_H = 440.f;
    static constexpr float LEFT_X  = 60.f;
    static constexpr float RIGHT_X = LEFT_X + PANEL_W + 30.f;  // 800
    static constexpr float PANEL_Y = 90.f;
    static constexpr float ITEM_H  = 28.f;
    static constexpr int   VISIBLE = static_cast<int>((PANEL_H - 30.f) / ITEM_H);

    void scanDecks();

    int  hitList    (float px, float py, float panelX, int scroll) const;
    bool hitStart   (float px, float py) const;
    bool hitBack    (float px, float py) const;
    int  hitFilter  (float px, float py) const;   // which search box: 0/1, or -1
    int  hitColorPip(float px, float py) const;            // 0..5, or -1
    bool hitRandom  (float px, float py, float panelX) const;
    bool hitPlayerName(float px, float py) const;
    // Combined colour identity (WUBRG bits) of a deck's commander(s); sets
    // `known` false if no commander could be resolved against the CardDb.
    uint8_t deckColorIdentity(const std::filesystem::path& deck, bool& known) const;
    // Same, but from already-read .dck text — avoids a second file open when the
    // caller has the body in hand (see ensureMeta).
    uint8_t deckColorIdentityFromText(std::string_view text, bool& known) const;
    int  hitPlayerCount(float px, float py) const;  // 0=none, 1=2P, 2=3P, 3=4P

    void drawPanel (sf::RenderTarget&, const std::string& title,
                    float x, float y, int sel, int hover, int scroll) const;
    void drawButton(sf::RenderTarget&, const std::string& label,
                    float x, float y, float w, float h,
                    sf::Color fill, sf::Color textCol) const;
    void drawText  (sf::RenderTarget&, const std::string& s,
                    float x, float y, unsigned sz,
                    sf::Color col, bool bold = false) const;
};

} // namespace ui
