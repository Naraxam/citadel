#pragma once
#include "BoardRenderer.h"
#include "SoundManager.h"
#include "HumanController.h"
#include "MainMenuScreen.h"
#include "MatchSetupScreen.h"
#include "DeckEditorScreen.h"
#include "CardDownloadScreen.h"
#include "../core/db/CardDb.h"
#include "../core/db/EditionDb.h"
#include "../game/GameState.h"
#include "../game/TurnManager.h"
#include "../game/DeckLoader.h"
#include "../game/ability/AbilityProcessor.h"
#include "../game/ai/AiPlayer.h"
#include <SFML/Graphics.hpp>
#include <deque>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace ui {

// Top-level SFML window that drives a Human (Alice, player 0) vs AI (Bob, player 1) game.
//
// Input summary:
//   Mouse click      — routed to HumanController / dock buttons / prompt buttons
//   Space / Enter    — pass priority / end phase
//   Escape           — quit
//
// Screens:
//   DeckEditor  — build / select decks before the game starts
//   Playing     — the match itself
class GameWindow {
public:
    // Open the main window immediately, then load the card database, AI data,
    // and oracle catalog in the background while drawing a stage-driven
    // progress bar on the same window. Returns once all assets are loaded.
    GameWindow(const std::filesystem::path& cardFolder,
               const std::filesystem::path& dataDir);
    ~GameWindow();
    void run();

private:
    mtg::CardDb        m_db;
    mtg::EditionDb     m_editions;   // name → sets index, parsed from Forge editions/*.txt
    sf::RenderWindow   m_window;
    sf::Font           m_font;
    std::string        m_picsDir;
    std::string        m_skinDir;
    std::filesystem::path m_dataDir;   // remembered from ctor for on-demand data downloads

    // Game objects
    mtg::GameState          m_game;
    mtg::AbilityProcessor   m_abilities{m_game};
    mtg::TurnManager        m_tm{m_game};
    mtg::AiPlayer           m_bobAi{1, m_game, m_abilities};
    HumanController         m_human{m_game, m_tm, m_abilities, m_bobAi};
    BoardRenderer           m_renderer;

    enum class AppState { MainMenu, DeckEditor, MatchSetup, Mulligan, Playing, Spectate, DownloadArt };
    int m_numPlayers = 2;   // 2 or 4 — set before startGame

    // Tournament mode (best-of-3): 0 = off, 1+ = tracking match wins
    bool m_tournamentMode     = false;
    int  m_matchWins[2]       = {0, 0};
    int  m_gamesPlayed        = 0;
    void checkTournamentEnd();  // called after each game result
    AppState  m_appState  = AppState::MainMenu;

    // ── Mulligan state ────────────────────────────────────────────────────
    struct MulliganState {
        int  humanMulls    = 0;
        int  aiMulls       = 0;
        bool humanKept     = false;
        bool aiKept        = false;
        int  toBottomCount = 0;   // how many more cards human must put to bottom
        std::vector<mtg::ObjectId> toBottomSelected;
    };
    MulliganState m_mull;
    bool          m_aliceGoesFirst = true;

    enum class WhosTurn { Human, AI };
    WhosTurn  m_turn      = WhosTurn::Human;
    bool      m_aiRunning  = false;
    std::string m_aiPhase;

    // Key bindings: action name -> sf::Keyboard::Key
    // Default bindings are set in the constructor; saved to settings.
    std::unordered_map<std::string, sf::Keyboard::Key> m_keyBindings;
    void initKeyBindings();
    void saveKeyBindings();
    void loadKeyBindings();
    bool m_showKeyBindings = false;
    int  m_editingBinding  = -1;  // index in binding list being remapped

    // FPS / profiler overlay (toggled with F3)
    bool  m_showFpsOverlay  = false;
    int   m_autoSaveInterval = 5;   // save every N turns (0 = disabled)
    int   m_turnAtLastAutoSave = 0;
    bool  m_pendingCrashRecovery = false;
    bool  m_puzzleMode       = false;   // win-this-turn challenge
    std::string m_puzzleTitle;
    void  startPuzzleMode(const std::filesystem::path& puzzleFile);  // set if autosave from crashed session found
    bool  m_replayMode    = false;         // true when in replay-viewer mode
    bool  m_goldfishMode  = false;
    // Life total history: [player][turn] = life at end of that turn
    std::vector<std::array<int,2>> m_lifeHistory;
    bool m_showLifeChart = false;
    void renderLifeChart();

    // Turn history: brief summary of what happened each turn
    struct TurnSummary {
        int  turnNum = 0;
        std::string activePlayer;
        int  spellsCast = 0;
        int  damageDealt = 0;
        std::vector<std::string> notableCards;  // cards cast this turn
    };
    std::vector<TurnSummary> m_turnHistory;
    bool m_showTurnHistory = false;
    int  m_currentTurnSpellsCast = 0;
    void renderTurnHistory();         // practice draw-hand without opponent
    int   m_goldfishDraws = 0;             // number of 7-card hands drawn so far
    void  startGoldfishMode();
    void  renderGoldfishOverlay();
    std::vector<std::filesystem::path> m_replaySlots;  // saved states for step-through
    int   m_replayIdx  = 0;
    sf::Clock m_fpsClock;
    int   m_frameCount      = 0;
    float m_fpsDisplay      = 0.f;

    // Spectate mode controls
    bool  m_spectatePaused  = false;   // Space to pause/unpause
    float m_spectateSpeed   = 1.0f;   // +/- to change speed (0.25–4×)
    bool  m_spectateStep    = false;   // N to step one turn  // current AI phase label for "Bob is thinking..." display
    mtg::ObjectId m_zoomCardId    = mtg::kInvalidId;  // card zoom with oracle text
    mtg::ObjectId m_artZoomCardId = mtg::kInvalidId;  // art-only zoom (Ctrl+right-click)
    std::vector<std::string> m_cachedRulings;       // last-fetched Scryfall rulings
    std::string              m_rulingsFetchedFor;   // card name last fetched for
    bool                     m_showRulings = false; // show rulings panel in zoom overlay

    // Game log filter (/ key to activate, Escape to clear)
    std::string m_logFilter;
    bool        m_logFilterActive = false;

    // Mode-options popup at the screen-bottom whenever m_human has a pending
    // spell waiting on a decision. The popup lists one row per available cast
    // mode (Cast / Foretell / …) plus a Cancel button. Rects are stamped each
    // frame by render() and read back in handleEvents() to dispatch clicks.
    // Empty when no popup is showing.
    struct CastPopupHit {
        sf::FloatRect                          rect{};
        HumanController::CastModeKind          kind = HumanController::CastModeKind::Cast;
    };
    std::vector<CastPopupHit> m_castPopupHits;
    sf::FloatRect             m_castPopupCancelRect{};

    // Combo mana-choice button rects (Temple-style "Add {B} or {G}"). Each
    // entry carries the colour char so the click handler can call
    // completeManaChoice(char).
    struct ManaChoiceHit { sf::FloatRect rect; char color; };
    std::vector<ManaChoiceHit> m_manaChoiceHits;

    // Multi-mana-ability picker (Shivan Reef, Temple of Malady…) button rects.
    // Each entry carries the AB$ Mana line index it represents.
    struct ManaAbilityHit { sf::FloatRect rect; int abilityIndex; };
    std::vector<ManaAbilityHit> m_manaAbilityHits;
    sf::FloatRect               m_manaAbilityCancelRect{};

    // Scry overlay buttons.
    sf::FloatRect m_scryKeepRect{};
    sf::FloatRect m_scryBottomRect{};

    void completeManaChoice(char color);
    void completeScryChoice(bool keepTop);

    // ESC pause menu (in-game only) — replaces the old bottom-dock buttons.
    // Has Resume / Undo / Concede / Quit-to-Menu entries.
    bool        m_pauseMenuOpen   = false;
    int         m_pauseMenuHover  = -1;   // 0..3 = which row the mouse is over
    void drawPauseMenu();
    // Self-contained modal pause loop for nested loops (humanPriorityWindow)
    // where the main run() loop isn't pumping events. Returns true if the match
    // should end/unwind (Concede or Quit), false if resumed.
    bool pauseMenuModal();
    enum class PauseAction { None, Resume, Undo, Concede, Quit };
    PauseAction hitPauseMenu(float px, float py) const;

    // Animation speed multiplier (1.0 = normal, 2.0 = double, 0.5 = half)
    // Adjusted with + and - keys in the options panel
    float m_animSpeed = 1.0f;

    // Toast notification: brief banner shown when the AI does something impactful
    struct Toast {
        std::string msg;
        float       ttl = 0.f;   // seconds remaining (counts down)
    };
    Toast m_toast;
    void showToast(const std::string& msg, float duration = 2.5f);
    void renderToast();
    bool      m_fullscreen = false;

    sf::Vector2f  m_mousePos    = {-1.f, -1.f};
    sf::View      m_gameView;
    sf::Clock     m_animClock;   // wall-clock for animation delta time
    SoundManager  m_sound;       // procedural audio
    HumanController::PhaseStops m_stops;  // which steps pause for player input
    mtg::ObjectId m_browseSelect = mtg::kInvalidId;

    // Drag-to-play: visual card-follows-cursor after clicking a hand card
    struct DragState {
        mtg::ObjectId cardId = mtg::kInvalidId;
        bool          active = false;
    };
    DragState m_drag;

    // In-game options panel (O key)
    bool    m_showOptions       = false;
    bool    m_showHelp          = false;   // ? key toggles keybinding reference
    bool    m_colorBlindMode    = false;   // use pattern-based card identification
    bool    m_priorityChime     = true;    // soft chime when the player gains priority
    // Card collection: count of times each card has been seen in any game
    std::unordered_map<std::string, int> m_cardSeen;
    void loadCardCollection();
    void saveCardCollection() const;
    void recordGameCards();    // call at game end to track cards seen
    bool    m_showCardSearch    = false;   // Ctrl+F card search overlay

    // Split card choice: player must choose which half to cast
    struct PendingSplitChoice {
        bool          active    = false;
        mtg::ObjectId cardId    = mtg::kInvalidId;
        int           selection = 0;   // 0 = left half, 1 = right half (for keyboard nav)
    };
    PendingSplitChoice m_splitChoice;
    std::string m_cardSearchQuery;
    void renderCardSearch();               // draw search overlay

    // Per-game statistics tracked during play for the post-game overlay
    struct GameStats {
        int turnsPlayed  = 0;
        int damageDealt[2] = {0, 0};  // total life lost by each player
        int cardsDrawn[2]  = {0, 0};  // total cards drawn by each player
        int spellsCast[2]  = {0, 0};  // total spells cast by each player
    };
    GameStats m_stats;

    // Card tags: card name -> user-assigned tag (removal/ramp/draw/combo/etc.)
    std::unordered_map<std::string, std::string> m_cardTags;
    void loadCardTags();
    void saveCardTags() const;

    // Per-deck win/loss tracking persisted to %APPDATA%\CitadelMTG\game_stats.json
    struct DeckStats {
        int wins = 0, losses = 0, draws = 0;
        std::string commanderName;  // commander used in this matchup
    };
    std::unordered_map<std::string, DeckStats> m_deckStats;
    void loadDeckStats();
    void saveDeckStats() const;
    void recordGameResult(int winnerPlayerId);   // call at game end
    void renderStatsOverlay();                   // full session stats overlay

    // Achievements
    struct Achievement {
        std::string id;
        std::string name;
        std::string desc;
        bool        unlocked = false;
    };
    std::vector<Achievement> m_achievements;
    void initAchievements();
    void checkAchievements();
    void renderAchievementsOverlay();
    bool m_showStats        = false;
    bool m_showAchievements = false;
    bool m_showCollection   = false;
    void renderCollectionStats();

    // Zone browser overlay state
    bool    m_zoneBrowseActive  = false;
    uint8_t m_zoneBrowsePlayer  = 0;
    bool    m_zoneBrowseIsExile = false;

    // Undo: ring buffer of up to 5 game state snapshots (Ctrl+Z to restore)
    static constexpr int kUndoDepth = 5;
    struct UndoSnapshot {
        mtg::GameState               game;
        mtg::TurnManager::Snapshot   tm;
        bool                         valid = false;
    };
    std::array<UndoSnapshot, kUndoDepth> m_undoStack;
    int m_undoHead = 0;   // index of the next slot to write
    int m_undoSize = 0;   // number of valid entries (0..kUndoDepth)

    // Game log — newest entry last, capped at 30 lines, shown in sidebar
    std::deque<std::string>           m_gameLog;
    std::unique_ptr<MainMenuScreen>      m_mainMenu;
    std::unique_ptr<MatchSetupScreen>    m_matchSetup;
    std::unique_ptr<DeckEditorScreen>    m_deckEditor;
    std::unique_ptr<CardDownloadScreen>  m_downloadScreen;

    // Pre-filtered card IDs for the current library search (rebuilt each frame)
    std::vector<mtg::ObjectId>        m_searchChoices;

    // ── Rematch / settings persistence ───────────────────────────────────
    // Saved after each game start so "Play Again" can replay with same decks.
    std::filesystem::path    m_rematchDeck0;         // used by startGameWithDecks path
    std::filesystem::path    m_rematchDeck1;
    mtg::DeckLoader::Deck    m_rematchEditorDeck;    // used by startGameWithDeck (editor)
    std::filesystem::path    m_rematchAiDeckPath;
    bool                     m_rematchUseEditor = false;

    void addLog(std::string msg);

    void setupGame();
    // Preload opening-hand card art (async texture loads) for the first numSeats players.
    void preloadHandArt(int numSeats);
    // Per-frame (throttled) prefetch of art for all on-board permanents + your
    // hand, so cards drawn/played mid-game don't pop in.
    void prefetchVisibleArt();
    // Hot-reload the custom-card overlay (after editing in the card creator) and
    // refresh the dependent screens — no restart needed.
    void reloadCustomCards();
    // Launch the standalone card-creator GUI (detached).
    void launchCardCreator();
    // Start from two .dck file paths (original flow, kept for env-var startup)
    void startGameWithDecks(const std::filesystem::path& deck0,
                             const std::filesystem::path& deck1);
    // Start from a pre-built player deck + AI deck path (deck editor flow)
    void startGameWithDeck(const mtg::DeckLoader::Deck& playerDeck,
                           const std::filesystem::path& aiDeckPath);

    bool         loadFont();
    void         handleEvents();
    void         update();
    void         render();
    void         toggleFullscreen();
    void         updateView();
    sf::Vector2f mapMousePos(int x, int y) const;
    sf::Event    remapEvent(const sf::Event& ev) const;

    void runAiTurn();
#ifdef _WIN32
    // SEH wrapper — no C++ destructors in frame, required for __try/__except.
    static int runAiTurnSafe(GameWindow* self);
#endif
    void humanPriorityWindow();
    // Before resolving a human Pass, give Bob a chance to cast instants/Flash.
    // If Bob casts something, opens humanPriorityWindow() for Alice to respond,
    // then lets the normal Pass flow continue.
    void doAiResponseThenPass();
    void doAlphaStrike();
    void doConcede();
    void doResolveAll();     // resolve the entire stack without stopping for response windows
    void doTapAllMana();     // T key: tap all untapped lands for mana
    void startSpectate();    // switch to AI-vs-AI spectate mode with current decks
    void snapshotUndo();     // call before any human action to enable Ctrl+Z
    void doUndo();           // Ctrl+Z: restore to last snapshot
    void renderOptions();    // draw in-game options overlay
    void renderSplitChoice(); // draw split card half-selector
    void renderHelp();       // draw ? keybinding reference overlay
    bool saveGame(const std::filesystem::path& path);       // Ctrl+S
    bool loadGame(const std::filesystem::path& path);       // Ctrl+L

    void renderGameOver();          // full-screen win/loss overlay
    void doRematch();               // restart with same decks
    void saveSettings() const;      // persist rematch decks + fullscreen to settings.json
    void loadSettings();            // restore settings on startup

    // ── Mulligan helpers ──────────────────────────────────────────────────
    void doMulliganForAi();               // AI makes all mulligan decisions
    void aiPutToBottom(int n);            // AI places n cards at bottom of library
    void handleMulliganClick(float px, float py);
    void renderMulligan();
    void startGameFromMulligan();

    // ── Library search helpers ────────────────────────────────────────────
    void rebuildSearchChoices();
    void completePendingSearch(mtg::ObjectId selectedId);

    // ── Riot / Fabricate / Charm / Madness choice helpers ────────────────
    void completeRiotChoice(bool giveHaste);
    void completePayLifeChoice(bool pay);   // shock lands: pay N life vs enter tapped
    void completeFabricateChoice(bool wantCounters);
    void completeCharmChoice(int modeIndex);
    void completeMadnessCast(bool wantCast);
    int  hitChoiceButton(float px, float py, int numButtons) const;

    // ── Forced discard helper ─────────────────────────────────────────────
    // Complete one card of the pending human discard. Decrements the count;
    // when done clears the pending state and drains triggers.
    void completeDiscardChoice(mtg::ObjectId cardId);
    // Mini event loop — spins until hasPendingDiscard() is false.
    // Called from runAiTurn when an AI spell forces the human to discard.
    void handlePendingDiscard();
};

} // namespace ui
