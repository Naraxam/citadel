#pragma once
#include "../game/DeckLoader.h"
#include "../core/db/CardDb.h"
#include "../core/db/EditionDb.h"
#include "../core/card/CardRules.h"
#include <SFML/Graphics.hpp>
#include <filesystem>
#include <string>
#include <vector>
#include <atomic>
#include <mutex>

namespace ui {

// Three-panel deck builder:
//   Left  (0..CAT_W=560):        Card catalog — virtual-scroll table with search + filters.
//   Middle (MID_X..MID_X+640):   Current deck grouped by type, mana curve, controls.
//   Right  (DETAIL_X..WIN_W):    Card detail — large image + stats for hovered card.
//
// Left-click catalog row  → add 1 copy (respects format max).
// Right-click catalog row → add max copies at once.
// Left-click deck row     → remove 1 copy.
// "Start Game"            → onEvent returns StartGame (only enabled when deck is valid).
class DeckEditorScreen {
public:
    DeckEditorScreen(const sf::Font& font,
                     const mtg::CardDb& db,
                     const std::filesystem::path& decksRoot,
                     std::string picsDir = "",
                     const mtg::EditionDb* editions = nullptr);

    enum class Result { None, StartGame, Back };
    Result onEvent(const sf::Event& ev);

    void draw(sf::RenderWindow& window) const;

    // Rebuild the card catalog from the CardDb (after a custom-card hot reload).
    void refreshCatalog();

    // Player deck built from current editor state (valid after onEvent returns StartGame).
    mtg::DeckLoader::Deck playerDeck() const;

    // AI deck file selected by the user; empty path = use built-in fallback.
    const std::filesystem::path& aiDeckPath() const { return m_aiDeckPath; }

private:
    const sf::Font&    m_font;
    const mtg::CardDb& m_db;
    const mtg::EditionDb* m_editions = nullptr;   // optional; nullptr disables set filter
    std::filesystem::path m_decksRoot;
    std::string           m_picsDir;
    std::filesystem::path m_loadedPath;

    // Set filter — empty = "All sets". Stored as the set code (e.g. "FDN").
    std::string m_setFilter;
    // When the Set dropdown is open: a paginated overlay list of all sets.
    bool m_setMenuOpen = false;
    int  m_setMenuScroll = 0;

    // Hide Alchemy "A-*" reprints from the catalog (user-toggleable).
    bool m_hideAlchemy = true;

    // ── Layout ───────────────────────────────────────────────────────────────
    static constexpr float WIN_W    = 1560.f;
    static constexpr float WIN_H    = 800.f;
    static constexpr float CAT_W    = 560.f;           // catalog panel width
    static constexpr float MID_X    = CAT_W;           // 560
    static constexpr float MID_W    = 640.f;           // deck panel width
    static constexpr float DETAIL_X = MID_X + MID_W;  // 1200
    static constexpr float DETAIL_W = WIN_W - DETAIL_X; // 360

    // Catalog filter bar: 3 rows (search / colors / type chips)
    static constexpr float CAT_FILTER_H = 106.f;
    static constexpr float CAT_COLHDR_H =  22.f;
    static constexpr float CAT_ROW_H    =  21.f;
    static constexpr float CAT_ROWS_Y   = CAT_FILTER_H + CAT_COLHDR_H; // 128
    static constexpr int   CAT_VISIBLE  = 32;  // (800-128)/21

    // Catalog column x-positions (within 560px panel)
    static constexpr float CC_NAME_X = 4.f;
    static constexpr float CC_NAME_W = 190.f;
    static constexpr float CC_COST_X = CC_NAME_X + CC_NAME_W;  // 194
    static constexpr float CC_COST_W =  78.f;
    static constexpr float CC_TYPE_X = CC_COST_X + CC_COST_W;  // 272
    static constexpr float CC_TYPE_W = 152.f;
    static constexpr float CC_PT_X   = CC_TYPE_X + CC_TYPE_W;  // 424
    static constexpr float CC_PT_W   =  50.f;
    static constexpr float CC_CMC_X  = CC_PT_X + CC_PT_W;      // 474
    static constexpr float CC_CMC_W  =  32.f;
    static constexpr float CC_QTY_X  = CC_CMC_X + CC_CMC_W;   // 506
    static constexpr float CC_QTY_W  =  28.f;

    // Deck panel Y-positions (window absolute)
    static constexpr float DP_CTRL_Y  =   0.f;
    static constexpr float DP_CTRL_H  =  44.f;
    static constexpr float DP_LIST_Y  =  DP_CTRL_Y + DP_CTRL_H;   //  44
    static constexpr float DP_LIST_H  = 522.f;
    static constexpr float DP_CURVE_Y =  DP_LIST_Y + DP_LIST_H;   // 566
    static constexpr float DP_CURVE_H = 120.f;
    static constexpr float DP_STATS_Y =  DP_CURVE_Y + DP_CURVE_H; // 686
    static constexpr float DP_STATS_H =  28.f;
    static constexpr float DP_AI_Y    =  DP_STATS_Y + DP_STATS_H; // 714
    static constexpr float DP_AI_H    =  36.f;
    static constexpr float DP_START_Y =  DP_AI_Y + DP_AI_H;       // 750
    static constexpr float DP_START_H =  50.f;
    // 44+522+120+28+36+50 = 800 ✓

    static constexpr float DP_ITEM_H  = 18.f;
    static constexpr int   DP_VISIBLE = 29;

    // ── Format ───────────────────────────────────────────────────────────────
    enum class Format { Standard, Commander };

    // ── Catalog data ─────────────────────────────────────────────────────────
    std::vector<const mtg::CardRules*> m_allCards;
    std::vector<int>                   m_filtered;

    // ── Filter state ─────────────────────────────────────────────────────────
    std::string m_searchText;
    bool        m_searchActive   = false;
    bool        m_colorFilter[6] = {};  // W U B R G Colorless
    int         m_typeFilterIdx  = 0;   // 0=All … 7=Planeswalker

    // ── Owned-only filter ─────────────────────────────────────────────────────
    bool m_ownedOnly = false;
    const std::unordered_map<std::string, int>* m_collection = nullptr;
public:
    void setCollection(const std::unordered_map<std::string, int>* coll) noexcept {
        m_collection = coll;
    }
private:

    // ── Sort state ────────────────────────────────────────────────────────────
    enum class SortBy { Name, Cmc, Color, Type };
    SortBy m_sortBy  = SortBy::Name;
    bool   m_sortAsc = true;   // true = ascending

    // Mana curve filter: -1 = show all; 0-7 = show only cards of that CMC (click bar to set)
    int m_cmcFilter  = -1;

    // ── Catalog scroll/hover ─────────────────────────────────────────────────
    int m_catScroll = 0;
    int m_catHover  = -1;

    // Click-and-drag scrollbar state. Which scrollable region is currently
    // being dragged (None = no drag in progress) and the offset from the top
    // of the thumb to the press point so dragging tracks smoothly without
    // jumping the thumb under the cursor on first move.
    enum class ScrollTarget { None, Catalog, Deck, LoadMenu };
    ScrollTarget m_dragScroll      = ScrollTarget::None;
    float        m_dragScrollGrabY = 0.f;

    // ── Deck state ───────────────────────────────────────────────────────────
    std::string m_deckName    = "New Deck";
    bool        m_nameEditing = false;
    Format      m_format      = Format::Commander;

    // Commander designation (points into m_deck; null = none selected).
    // m_partnerRules supports Partner commanders (second commander slot).
    const mtg::CardRules* m_commanderRules  = nullptr;
    const mtg::CardRules* m_partnerRules    = nullptr;
    // Companion: one companion card outside the 100 (right-click a Companion card to designate)
    const mtg::CardRules* m_companionRules  = nullptr;

    // Paste-import overlay (Moxfield / MTGO / Arena format)
    bool        m_showPasteImport  = false;
    std::string m_pasteBuffer;          // text the user is typing
    void importFromPastedList(const std::string& text);  // parse and load

    struct Entry { const mtg::CardRules* rules; int count; };
    std::vector<Entry> m_deck;

    int m_deckScroll = 0;
    int m_deckHover  = -1;

    // ── Detail panel ─────────────────────────────────────────────────────────
    mutable const mtg::CardRules* m_detailCard = nullptr;

    // ── AI deck list ─────────────────────────────────────────────────────────
    std::vector<std::filesystem::path> m_aiDecks;
    std::vector<std::string>           m_aiDeckNames;
    int                                m_aiDeckIdx = -1;
    std::filesystem::path              m_aiDeckPath;

    // ── Save/load overlay ─────────────────────────────────────────────────────
    std::vector<std::filesystem::path> m_savedDecks;
    std::vector<std::string>           m_savedNames;
    std::vector<uint8_t>               m_savedCI;       // commander colour identity
    std::vector<char>                  m_savedCIKnown;
    std::string                        m_loadFilter;    // live filter in the Load menu
    std::vector<int>                   m_loadVisible;   // indices into m_savedDecks
    bool                               m_showLoadMenu   = false;
    int                                m_loadMenuScroll = 0;
    void                               rebuildLoadFilter();
    uint8_t deckColorIdentity(const std::filesystem::path& deck, bool& known) const;

    // ── EDHREC tag picker overlay ─────────────────────────────────────────────
    // m_edhrecOpen latches when the user clicks the EDH button. The fetch runs
    // on a worker thread; m_edhrecBusy gates re-entrancy. m_edhrecTags is the
    // result; m_edhrecStatus shows progress/error text in the panel.
    bool                              m_edhrecOpen     = false;
    std::atomic<bool>                 m_edhrecBusy{false};
    int                               m_edhrecScroll   = 0;
    int                               m_edhrecHover    = -1;
    std::string                       m_edhrecCommander;   // commander name at time of fetch
    std::string                       m_edhrecStatus;      // shown when no rows yet
    struct EdhrecTagRow {
        std::string name;     // human label
        std::string slug;     // url slug
        int         count;    // deck inclusion count
    };
    std::vector<EdhrecTagRow>         m_edhrecTags;
    mutable std::mutex                m_edhrecMutex;       // guards m_edhrecTags/Status

    // ── Grouped deck-list rows (lazy cache) ──────────────────────────────────
    struct DeckRow {
        bool                  isHeader;
        std::string           header;
        const mtg::CardRules* rules;
        int                   count;
    };
    mutable std::vector<DeckRow> m_deckRows;
    mutable bool                 m_deckRowsDirty = true;

    // ── Helpers ───────────────────────────────────────────────────────────────
    void buildCatalog    ();
    void applyFilters    ();
    void rebuildDeckRows () const;
    void scanSavedDecks  ();
    void scanAiDecks     ();

    void addCard   (const mtg::CardRules* r, int n = 1);
    void removeCard(const mtg::CardRules* r, int n = 1);
    void saveDeck  ();
    void deleteDeck();
    void loadDeckFromFile(const std::filesystem::path& p);

    // Paste a decklist from the clipboard into the current deck (merges).
    // Accepts Forge .dck sections, MTGO "1 Card Name", Arena "1 Name (SET) NUM",
    // or bare card names. Unknown card names are logged and skipped.
    void importDeckFromClipboard();
    // Copy the current deck to the clipboard as a Forge .dck-style listing.
    void exportDeckToClipboard();

    // EDHREC integration ----------------------------------------------------
    // Open the tag picker for the currently-designated commander. Spawns a
    // worker thread that fetches /pages/commanders/<slug>.json from EDHREC
    // and populates m_edhrecTags; on completion the overlay shows the list.
    // No-op if no commander is set or a fetch is already in flight.
    void openEdhrecPicker();
    // Close the picker without importing anything.
    void closeEdhrecPicker() noexcept;
    // Pick a tag — kicks off a background fetch of the per-tag average deck
    // and merges it into the current deck on completion.
    void importEdhrecDeckForTag(const std::string& tagSlug,
                                  const std::string& tagLabel);
    bool edhrecPickerOpen() const noexcept { return m_edhrecOpen; }

    int  deckCount     (const mtg::CardRules* r) const;
    int  totalDeckCards() const;
    int  maxCopies     (const mtg::CardRules* r) const;
    bool isValidDeck   () const;
    std::string validationMessage() const;

    mtg::DeckLoader::Deck buildDeck() const;

    // ── Draw ──────────────────────────────────────────────────────────────────
    bool hitOwnedToggle(float px, float py) const;
    // Import a deck from a Moxfield/Archidekt URL (fetches via HTTP, then parses as text)
    void importFromUrl(const std::string& url);
    bool m_showUrlImport = false;
    std::string m_urlBuffer;   // typed URL
    // Returns a list of staple card names recommended for the current commander's color identity
    std::vector<std::string> getStapleRecommendations() const;
    bool m_showRecommendations = false;
    void drawCatalogPanel  (sf::RenderTarget& t) const;
    void drawFilterBar     (sf::RenderTarget& t) const;
    void drawCatalogHeader (sf::RenderTarget& t) const;
    void drawCatalogRows   (sf::RenderTarget& t) const;
    void drawDeckPanel     (sf::RenderTarget& t) const;
    void drawDeckControls  (sf::RenderTarget& t) const;
    void drawDeckList      (sf::RenderTarget& t) const;
    void drawManaChart     (sf::RenderTarget& t) const;
    void drawDeckStats     (sf::RenderTarget& t) const;
    void drawAiPicker      (sf::RenderTarget& t) const;
    void drawStartButton   (sf::RenderTarget& t) const;
    void drawLoadMenu         (sf::RenderTarget& t) const;
    void drawDetailPanel      (sf::RenderTarget& t) const;
    void drawPasteImportOverlay(sf::RenderTarget& t) const;

    void drawText  (sf::RenderTarget& t, const std::string& s,
                    float x, float y, unsigned sz,
                    sf::Color col = sf::Color(220,220,220),
                    bool bold = false) const;
    void drawRect  (sf::RenderTarget& t,
                    float x, float y, float w, float h,
                    sf::Color fill,
                    sf::Color outline = sf::Color::Transparent,
                    float thick = 1.f) const;
    void drawButton(sf::RenderTarget& t, const std::string& label,
                    float x, float y, float w, float h,
                    sf::Color fill = sf::Color(60,80,100),
                    sf::Color textCol = sf::Color(220,220,220)) const;

    // ── Hit-test ──────────────────────────────────────────────────────────────
    int  hitCatalogRow (float px, float py) const;
    int  hitDeckRow    (float px, float py) const;
    bool hitSearch     (float px, float py) const;
    int  hitColorBtn   (float px, float py) const;
    int  hitTypeFilter (float px, float py) const;
    bool hitStart      (float px, float py) const;
    bool hitSave       (float px, float py) const;
    bool hitLoad       (float px, float py) const;
    bool hitNew        (float px, float py) const;
    bool hitClear      (float px, float py) const;
    bool hitDelete     (float px, float py) const;
    bool hitImport     (float px, float py) const;
    bool hitExport     (float px, float py) const;
    bool hitEdhrec     (float px, float py) const;
    bool hitBackBtn    (float px, float py) const;
    bool hitDeckName   (float px, float py) const;
    int  hitAiArrow    (float px, float py) const;
    int  hitFormatBtn  (float px, float py) const;  // 0=none 1=Standard 2=Commander

    // ── Static helpers ────────────────────────────────────────────────────────
    static std::string manaCostStr (const mtg::CardRules* r);
    static std::string typeStr     (const mtg::CardRules* r);
    static std::string ptStr       (const mtg::CardRules* r);
    static sf::Color   colorForCard(const mtg::CardRules* r);
    static int         typeGroup   (const mtg::CardRules* r);
    static const char* typeGroupName(int g);
};

} // namespace ui
