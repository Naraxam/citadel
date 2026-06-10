#pragma once
#include "CardRenderer.h"
#include "../game/GameState.h"
#include "../game/TurnManager.h"

namespace mtg { class AbilityProcessor; }
#include <SFML/Graphics.hpp>
#include <map>
#include <string>
#include <set>
#include <unordered_map>
#include <vector>

namespace ui {

// Which hidden/visible zone the zone-browser overlay is showing.
enum class BrowseZone : uint8_t { Graveyard = 0, Exile = 1, Library = 2 };

// Describes the current interactive state so BoardRenderer can apply
// highlights and messages without knowing game logic.
struct RenderHints {
    std::set<mtg::ObjectId> selectedCards;
    std::set<mtg::ObjectId> attackers;
    std::set<mtg::ObjectId> blockers;
    mtg::ObjectId           pendingSpell  = mtg::kInvalidId;
    std::string             instruction;         // prompt panel message
    std::string             phase;               // current phase name
    sf::Vector2f            mousePos      = {-1.f, -1.f};
    mtg::ObjectId           previewCardId = mtg::kInvalidId;
    std::vector<std::string> logLines;           // recent game log (newest last)
    bool                    showOkButton  = false;
    bool                    showCancelBtn = false;

    // Attacker declaration (pre-confirmation): creatures the human has selected
    // to attack, and the defending player (255 = none). The renderer cosmetically
    // taps these and draws amber pending-attack arrows; no real tap or fly
    // animation happens until the attack is actually confirmed.
    std::set<mtg::ObjectId> pendingAttackers;
    uint8_t                 pendingAttackDefender = 255;

    // Library search overlay (tutor)
    bool                       showLibrarySearch = false;
    std::vector<mtg::ObjectId> searchChoices;     // pre-filtered matching card IDs
    std::string                searchInstruction; // e.g. "Search for a basic land"
    bool                       searchShowDone = false; // "up to N" — offer a Done button

    // Blocker ordering: maps blocker ID → 1-based position in damage order.
    // Populated only during HumanState::OrderBlockers.
    std::map<mtg::ObjectId, int> blockerOrder;

    // Phase-stop toggle state — one bool per step in kPhases[] order.
    // true = game pauses here and waits for player to click End Phase.
    std::array<bool, 13> stepStops = {};

    // Card collection counts (name → times seen) — supplied by GameWindow
    const std::unordered_map<std::string, int>* cardCollection = nullptr;

    // Zone browser overlay — show all cards in a player's GY, Exile or Library
    bool       showZoneBrowse   = false;
    uint8_t    zoneBrowsePlayer = 0;    // 0 = Alice, 1 = Bob
    BrowseZone zoneBrowseZone   = BrowseZone::Graveyard;

    // When true: replace all color-coded tints with shape/pattern overlays
    // so the display is accessible to colour-blind players.
    bool    colorBlindMode      = false;

    // Log filter: only show lines containing this string (empty = show all)
    std::string logFilter;

    // Legal targets for the current spell/ability being cast (highlighted with teal glow).
    // Only populated during HumanState::TargetSelect.
    std::set<mtg::ObjectId> validTargets;

    // Combat damage preview: total potential damage to opponent from currently declared attackers.
    // -1 = not in attack declaration (don't show).
    int combatDamagePreview = -1;

    // Pending human trigger names for the reorder overlay (empty = none)
    std::vector<std::string> pendingTriggerNames;

    // Card name hovered in the game log — show a small preview popup if non-empty
    std::string logHoverCardName;

    // Contextual help tooltip: shown in a small box near the element being hovered.
    // Set by GameWindow when mouse is over a named UI region.
    std::string uiTooltip;
    sf::Vector2f uiTooltipPos;

    // Card comparison: when non-invalid and Alt is held, show this card side-by-side
    // with the hovered card in the preview panel.
    mtg::ObjectId compareCardId = mtg::kInvalidId;
};

class BoardRenderer {
public:
    BoardRenderer() = default;
    BoardRenderer(const sf::Font& font,
                  const mtg::GameState& game,
                  const mtg::TurnManager& tm,
                  const std::string& picsDir = "");

    void init(const sf::Font& font, const mtg::GameState& game,
              const mtg::TurnManager& tm, const std::string& picsDir = "");

    // The renderer reads the ability stack from here so activated/triggered
    // abilities (fetch lands, etc.) appear on the visible stack alongside spells.
    void setStackSource(const mtg::AbilityProcessor* ap) noexcept { m_abilities = ap; }

    void draw(sf::RenderTarget& window, const RenderHints& hints) const;

    // ── Per-frame animation update ─────────────────────────────────────────────
    // Advance all animations and detect new BF changes (card deaths, new attackers).
    // Must be called once per frame from GameWindow::update() with elapsed seconds.
    void update(float dt);

    struct CardAnim {
        enum class Kind { FlyAttack, DeathFade, DrawSlide } kind;
        mtg::ObjectId    id      = mtg::kInvalidId;
        sf::Vector2f     from;
        sf::Vector2f     to;
        float            t       = 0.f;  // [0, 1] normalised progress
        float            dur     = 0.5f; // duration in seconds
        const mtg::Card* cardRef = nullptr; // safe while card is alive in any zone
    };

    // Brief "mana stream": colored particle that travels from a tapped mana
    // source to the controller's mana-pool display. Spawned by detectChanges()
    // when a permanent transitions tapped + had an AB$ Mana ability.
    struct ManaStream {
        sf::Vector2f from;
        sf::Vector2f to;
        sf::Color    color = sf::Color(255, 220, 80);
        float        t   = 0.f;
        float        dur = 0.45f;
    };

    // A floating "-N" combat-damage number that rises and fades. Spawned via
    // spawnCombatDamageText() when the GameWindow drains the game's damage FX.
    struct FloatText {
        std::string  text;
        sf::Vector2f pos;
        sf::Color    color = sf::Color(235, 50, 40);
        float        t   = 0.f;
        float        dur = 1.2f;
    };
    // Spawn a red damage number over a creature (targetCard) or a player's info
    // bar (targetCard == kInvalidId). No-op for amount <= 0.
    void spawnCombatDamageText(mtg::ObjectId targetCard, uint8_t targetPlayer, int amount);

    struct HitResult {
        mtg::ObjectId id     = mtg::kInvalidId;
        mtg::ZoneType zone   {};
        uint8_t       player = 0;
    };
    HitResult hitTest(float px, float py) const;

    // Hit-test specific dock buttons (bottom of play area)
    enum class DockBtn { None, EndPhase, PassPriority, AlphaStrike, Concede };
    DockBtn hitDock(float px, float py) const;

    // Hit-test prompt OK / Cancel buttons in the sidebar
    enum class PromptBtn { None, Ok, Cancel };
    PromptBtn hitPrompt(float px, float py) const;

    // Hit-test the info-bar zone icons (gravestone for GY, gate for exile).
    // Returns the (player, isExile) pair to open in the zone browser, or
    // (-1, false) on miss. Hit rects are captured during drawInfoBar so
    // callers get pixel-accurate hits matching the rendered icons.
    struct ZoneIconHit { int player; BrowseZone zone; bool valid = true; };
    ZoneIconHit hitInfoBarZone(float px, float py) const noexcept;
    // Player info-bar click (life/name area, excluding the GY/Exile zone icons).
    // Returns 0 (you) / 1 (opponent), or -1. Used to target players with spells.
    int hitPlayerArea(float px, float py) const noexcept;

    // Returns the card ID the click lands on in the library-search overlay,
    // or kInvalidId if the click misses all items.
    mtg::ObjectId hitSearchChoice(float px, float py, const RenderHints& hints) const;
    bool          hitSearchDone(float px, float py, const RenderHints& hints) const;

private:
    const sf::Font*             m_font      = nullptr;
    const mtg::GameState*       m_game      = nullptr;
    const mtg::TurnManager*     m_tm        = nullptr;
    const mtg::AbilityProcessor* m_abilities = nullptr;
    std::string                 m_picsDir;

    // Procedurally-generated dark grunge match background, built once on first
    // draw and cached (no external image file required).
    mutable sf::Texture m_bgTexture;
    mutable bool        m_bgBuilt = false;
    void buildProceduralBackground() const;

    // ── Draw sections ─────────────────────────────────────────────────────────
    void drawBackground   (sf::RenderTarget&) const;
    void drawInfoBar      (sf::RenderTarget&, uint8_t pid) const;
    void drawHand         (sf::RenderTarget&, uint8_t pid, const RenderHints&) const;
    void drawBattlefield  (sf::RenderTarget&, uint8_t pid, const RenderHints&) const;
    void drawPhaseTracker (sf::RenderTarget&, const RenderHints&) const;  // vertical phase tracker in sidebar
    void drawDock         (sf::RenderTarget&, const RenderHints&) const;
    void drawSidebar      (sf::RenderTarget&, const RenderHints&) const;
    void drawPlayerStatus (sf::RenderTarget&, uint8_t pid,
                           float y, float h) const;
    void drawStackSection (sf::RenderTarget&, float& ty) const;
    void drawPromptSection(sf::RenderTarget&, const RenderHints&) const;
    void drawLogSection   (sf::RenderTarget&, const RenderHints&) const;
    void drawPreviewPanel (sf::RenderTarget&, const RenderHints&) const;
    void drawTooltip      (sf::RenderTarget&, const RenderHints&) const;
    void drawCombatArrows      (sf::RenderTarget&, const RenderHints&) const;
    void drawLibrarySearchOverlay(sf::RenderTarget&, const RenderHints&) const;
    void drawAnims             (sf::RenderTarget&) const;
    void drawZoneBrowserOverlay(sf::RenderTarget&, const RenderHints&) const;
    void drawDayNightIndicator    (sf::RenderTarget&) const;
    void drawDungeonProgress      (sf::RenderTarget&) const;
    void drawCommandZoneThumbnail (sf::RenderTarget&) const;

    // Right-preview-panel commander thumbnail rectangle for player `pid`.
    // pid 1 (opponent) at top, pid 0 (you) at bottom.
    static sf::FloatRect commanderThumbRect(uint8_t pid);

    // Returns the commander Card* whose right-panel thumbnail the cursor is
    // hovering, or nullptr if the cursor isn't over a thumbnail.
    const mtg::Card* commanderUnderCursor(float px, float py) const;

    sf::Vector2f screenCenterOf(mtg::ObjectId) const;

    // ── Helpers ───────────────────────────────────────────────────────────────
    void drawSectionHeader(sf::RenderTarget&, const std::string& label,
                           float x, float y, float w, float h) const;
    void drawButton(sf::RenderTarget&, const std::string& label,
                    float x, float y, float w, float h,
                    sf::Color fill = sf::Color(60, 80, 100),
                    sf::Color text = sf::Color(220, 220, 220)) const;
    void drawZoneBg(sf::RenderTarget&, float x, float y,
                    float w, float h, const std::string& label = "") const;

    sf::Vector2f  cardScreenPos(int index, uint8_t pid, mtg::ZoneType zone) const;
    mtg::ObjectId cardAt(float px, float py, uint8_t pid, mtg::ZoneType zone) const;

public:
    // Returns the phase-row index (0–12) if (px,py) is inside a phase tracker row, else -1.
    // Coordinates are in the logical window space (0..WIN_W × 0..WIN_H).
    int hitPhaseRow(float px, float py) const noexcept;

    // Returns true if (px,py) hits the "Resolve All" button in the stack sidebar section.
    bool hitResolveAll(float px, float py) const noexcept;

    // Returns true if (px,py) hits the zone-browser overlay's close area (background).
    bool hitZoneBrowserClose(float px, float py, const RenderHints& hints) const noexcept;

    // Returns the ObjectId of the card row clicked in the zone browser, or kInvalidId.
    mtg::ObjectId hitZoneBrowserCard(float px, float py, const RenderHints& hints) const;

    void spawnDeathAnim(mtg::ObjectId id, sf::Vector2f topLeft, const mtg::Card* c);

    // Animation state — updated by update(), read by draw()
    std::vector<CardAnim>                     m_anims;
    std::set<mtg::ObjectId>                   m_lastBfSet;
    std::map<mtg::ObjectId, sf::Vector2f>     m_lastCardPos;
    std::set<mtg::ObjectId>                   m_lastAttackerSet;
    // Tracks hand card sets between frames for draw animation detection
    std::set<mtg::ObjectId>                   m_lastHandSet[2];

    // Tap rotation animation: cardId → progress [0,1]. Inserted on
    // transition untapped→tapped, removed when progress reaches 1 OR the
    // card becomes untapped. update() advances; drawBattlefield() reads via
    // rotationOverride on CardDrawOptions.
    mutable std::set<mtg::ObjectId>           m_lastTappedSet;
    std::map<mtg::ObjectId, float>            m_tapAnims;     // 0..1 progress
    std::vector<ManaStream>                   m_manaStreams;
    std::vector<FloatText>                    m_floatTexts;   // combat damage numbers
    // Attackers shown as cosmetically tapped during declaration (RenderHints
    // .pendingAttackers) last frame — so when the attack is confirmed and the
    // creature really taps, we skip the 0→90° re-rotation (it's already shown
    // rotated). Tracked across frames by update().
    mutable std::set<mtg::ObjectId>           m_lastPendingAttackers;

    // Hit-rects for the info-bar GY/Exile icons, refreshed each draw pass.
    // Indexed by player (0 = Alice, 1 = Bob).
    mutable sf::FloatRect m_gyIconRect[2];
    mutable sf::FloatRect m_exileIconRect[2];
    mutable sf::FloatRect m_libIconRect[2];

    // Zone browser scroll offset (number of rows scrolled down).
    // Mutable so hitTest and draw can use it while const.
    mutable int m_gyBrowserScroll = 0;

    // Zone browser card list cache — rebuilt only when player/zone/exile selection changes
    mutable std::vector<const mtg::Card*> m_gyBrowserCache;
    mutable bool    m_gyBrowserCacheDirty = true;
    mutable uint8_t    m_gyBrowserCachedPlayer = 255;
    mutable BrowseZone m_gyBrowserCachedZone   = BrowseZone::Graveyard;

    // Scroll the zone browser overlay (positive = down, negative = up).
    // Returns true if the event was consumed (browser is open).
    bool scrollZoneBrowser(int delta, const RenderHints& hints) noexcept;
    // Invalidate the card list cache (call when opening or the underlying zone changes).
    void invalidateZoneBrowserCache() noexcept;
};

} // namespace ui
