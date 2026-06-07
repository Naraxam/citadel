#include "GameWindow.h"
#include "CardRenderer.h"
#include "Localization.h"
#include "../game/CardStats.h"
#include <sstream>
#include "CardImageDownloader.h"
#include "UiScale.h"
#include "../game/StateBasedActions.h"
#include "../game/CardFilter.h"
#include "../game/DeckLoader.h"
#include "../game/TriggerSystem.h"
#include "../game/ability/Effects.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <initializer_list>
#include <random>
#include <utility>
#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>
#include <malloc.h>
#endif

namespace { // local helper
    std::filesystem::path detectResRoot() {
        namespace fs = std::filesystem;
        if (const char* v = std::getenv("CITADEL_RES"); v && fs::exists(v)) return v;
        // 0. Bundled res shipped with the app (portable install) — next to the
        //    working dir or under %APPDATA%. Takes precedence over Forge paths.
        {
            std::vector<fs::path> bases = { fs::current_path() };
            if (const char* ap = std::getenv("APPDATA"); ap && *ap)
                bases.push_back(fs::path(ap) / "CitadelMTG");
            for (const auto& base : bases) {
                auto p = base / "res";
                if (fs::exists(p / "skins") || fs::exists(p / "editions") ||
                    fs::exists(p / "cardsfolder")) return p;
            }
        }
        // 1. CITADEL_CARDS env var (most reliable)
        if (const char* v = std::getenv("CITADEL_CARDS"); v) {
            auto p = fs::path(v).parent_path();
            if (fs::exists(p)) return p;
        }
        // 2. CITADEL_RES env var (explicit res path)
        if (const char* v = std::getenv("CITADEL_RES"); v) {
            if (fs::exists(v)) return fs::path(v);
        }
        // 3. citadel-path.txt config file next to the exe (set by installer or user)
        {
            auto exeDir = fs::current_path();
            auto cfgPath = exeDir / "citadel-path.txt";
            if (fs::exists(cfgPath)) {
                std::ifstream cfg(cfgPath);
                std::string line;
                if (std::getline(cfg, line) && fs::exists(line)) return fs::path(line);
            }
        }
        // 4. Common installation paths (fallback — may fail on non-dev machines)
        for (const char* c : {
                R"(Z:\cardforge\res)",
                R"(C:\Forge\res)",
                R"(C:\Program Files\Forge\res)",
                R"(C:\Users\Chris\source\repos\forge\forge-gui\res)"}) {
            if (fs::exists(c)) return c;
        }
        // 5. Print a diagnostic so users know how to fix it
        std::cerr << "[GameWindow] No Citadel MTG res directory found.\n"
                  << "  Set CITADEL_CARDS=<path>\\cardsfolder  OR\n"
                  << "  Set CITADEL_RES=<path>\\res  OR\n"
                  << "  Create citadel-path.txt next to the exe with one line: <path>\\res\n";
        return {};
    }
}

namespace ui {

using namespace mtg;
using namespace Layout;

// ── Log helper ────────────────────────────────────────────────────────────────

// Append one line to the persistent crash-recovery log.
// Written synchronously (open/write/close) so the file is valid even if the
// process dies immediately after — no need for an open file handle member.
static void persistLog(const std::string& line) {
#ifdef _WIN32
    if (const char* ap = std::getenv("APPDATA")) {
        std::ofstream f(std::string(ap) + "\\CitadelMTG\\game_log.txt",
                        std::ios::app | std::ios::binary);
        if (f) f << line << '\n';
    }
#endif
}

void GameWindow::addLog(std::string msg) {
    m_gameLog.push_back(msg);
    while (m_gameLog.size() > 30) m_gameLog.pop_front();
    persistLog(m_gameLog.back());

    // ── Trigger sound effects based on log content ────────────────────────────
    // These patterns match the log messages produced by AiPlayer and GameWindow.
    // Toast for notable AI actions (only show when it's Bob's turn so it doesn't spam)
    // Toast for notable AI actions during Bob's turn
    if (m_turn == WhosTurn::AI || m_aiRunning) {
        if (msg.find("Bob") != std::string::npos) {
            if      (msg.find("casts ") != std::string::npos)     showToast("Bob: " + msg);
            else if (msg.find("attacks") != std::string::npos)    showToast("Bob: " + msg);
            else if (msg.find("activates") != std::string::npos)  showToast("Bob: " + msg);
        }
    }
    // Toast for Alice's important events (trigger notifications)
    if (msg.find("Alice") != std::string::npos || msg.find("TOKEN") != std::string::npos ||
        msg.find("Revealed") != std::string::npos) {
        if (msg.find("trigger") != std::string::npos || msg.find("fires") != std::string::npos ||
            msg.find("Token") != std::string::npos)
            showToast(msg, 2.f);
    }
    if (msg.find(" wins!") != std::string::npos) showToast(msg, 4.f);

    // Turn history recording
    if (msg.find("Turn ") == 0 && msg.find(": ") != std::string::npos) {
        // New turn starting — archive previous turn summary
        TurnSummary ts;
        ts.turnNum = m_game.turnNumber();
        ts.activePlayer = msg.substr(msg.find(": ") + 2);
        m_turnHistory.push_back(ts);
        m_currentTurnSpellsCast = 0;
    }
    if (!m_turnHistory.empty() && msg.find("casts ") != std::string::npos) {
        ++m_turnHistory.back().spellsCast;
        ++m_currentTurnSpellsCast;
        // Extract card name from "X casts CardName"
        auto pos = msg.find("casts ");
        if (pos != std::string::npos) {
            std::string cardName = msg.substr(pos + 6);
            if (cardName.size() > 24) cardName = cardName.substr(0, 22) + "..";
            m_turnHistory.back().notableCards.push_back(cardName);
        }
    }

    if (msg.find("casts ")         != std::string::npos) m_sound.play(SND_SPELL_CAST);
    else if (msg.find("taps ")     != std::string::npos &&
             msg.find("land")      != std::string::npos) m_sound.play(SND_LAND_TAP);
    else if (msg.find("attacks with")!= std::string::npos) m_sound.play(SND_COMBAT_HIT);
    else if (msg.find("Token")     != std::string::npos) m_sound.play(SND_TOKEN_CREATE);
    else if (msg.find(" wins!")    != std::string::npos) m_sound.play(SND_WIN);
    else if (msg.find("concedes")  != std::string::npos) m_sound.play(SND_LOSE);
    else if (msg.find("Turn ")     != std::string::npos) m_sound.play(SND_PHASE_CHANGE);
}

// ── Construction / Destruction ────────────────────────────────────────────────

GameWindow::~GameWindow() {
    CardImageDownloader::stop();
    // Write the clean-exit marker so the NEXT launch knows this session ended
    // normally and doesn't falsely report a crash. (The marker is removed at
    // startup; if we crash, the destructor never runs and it stays absent.)
    if (const char* ap = std::getenv("APPDATA")) {
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::create_directories(fs::path(ap) / "CitadelMTG", ec);
        std::ofstream f(fs::path(ap) / "CitadelMTG" / "clean_exit.txt");
        if (f) f << "ok\n";
    }
}

GameWindow::GameWindow(const std::filesystem::path& cardFolder,
                       const std::filesystem::path& dataDir)
    : m_window(sf::VideoMode(static_cast<unsigned>(WIN_W),
                              static_cast<unsigned>(WIN_H)),
               "Citadel MTG", sf::Style::Default,
               sf::ContextSettings(0, 0, 8))   // 8x MSAA — smoother edges when scaled
    , m_renderer(m_font, m_game, m_tm)
{
    namespace fs = std::filesystem;
    m_window.setFramerateLimit(60);
    updateView();
    m_renderer.setStackSource(&m_abilities);   // show activated abilities on the stack

    if (!loadFont())
        std::cerr << "WARNING: Could not load a system font. Text may not render.\n";

    // ── Loading screen with a real progress bar ──────────────────────────────
    // The window opens immediately above; heavy I/O (card DB scan, AI catalog
    // load, oracle cache lookup, salt scores) runs on a worker thread while
    // this loop pumps events and redraws the loading bar so the OS never
    // marks us "Not Responding". The previous design opened a separate splash
    // window first and then swapped it for the main window.
    struct LoadState {
        std::atomic<int>  stage{0};      // 0..kStageCount as steps complete
        std::atomic<int>  pctInStage{0}; // 0..100 within current stage (for the big card scan)
        std::atomic<bool> error{false};
        std::string       errorMsg;
    };
    static constexpr const char* kStageLabels[] = {
        "Loading card database",
        "Wiring double-faced cards",
        "Loading AI combo definitions",
        "Loading value network",
        "Loading EDHREC salt scores",
        "Indexing card images (Scryfall oracle)",
        "Initialising audio + collection",
    };
    constexpr int kStageCount = sizeof(kStageLabels) / sizeof(kStageLabels[0]);

    LoadState ld;

    std::thread loader([&] {
        // Stage 0 — cards
        if (fs::exists(cardFolder / "cardsfolder.zip"))
            m_db.loadFromZip((cardFolder / "cardsfolder.zip").string());
        else
            m_db.loadFromDirectory(cardFolder.string());
        if (m_db.empty()) {
            ld.errorMsg = "0 cards loaded. Check the cardsfolder path.";
            ld.error.store(true);
            return;
        }
        // Custom-card overlay (generated via training/scryfall_to_cards.py or
        // hand-authored). Loaded before wiring so overlaid DFCs link correctly.
        m_db.loadCustomCards();
        ld.stage.store(1);

        // Stage 1 — DFC wire
        m_db.wireBackFaces();
        ld.stage.store(2);

        m_dataDir = dataDir;  // remember for on-demand main-menu downloads

        // Stage 2 — combos (cache load only; refresh is now a main-menu action)
        {
            fs::path comboPath = dataDir / "combos.json";
            if (fs::exists(comboPath))
                mtg::AiPlayer::loadCombos(comboPath.string());
        }
        ld.stage.store(3);

        // Stage 3 — value net (optional)
        {
            fs::path netPath = dataDir / "value_net.bin";
            if (fs::exists(netPath))
                mtg::AiPlayer::loadValueNet(netPath.string());
        }
        ld.stage.store(4);

        // Stage 4 — salt scores (cache load only — first-time download is now
        // an explicit main-menu action so the game opens fast on fresh installs
        // instead of blocking on EDHREC for several seconds).
        {
            fs::path saltCache = dataDir / "salt_scores.json";
            if (fs::exists(saltCache))
                mtg::SaltDatabase::load(saltCache.string());
        }
        ld.stage.store(5);

        // Stage 5 — pics dir + Scryfall oracle index (best-effort) + sets
        m_picsDir = CardImageDownloader::initDir();
        CardImageDownloader::autoLoadOracle();
        // Editions live next to cardsfolder under res/. cardFolder ends in
        // ".../res/cardsfolder" so res = parent().
        namespace fsl = std::filesystem;
        fsl::path editionsDir = cardFolder.parent_path() / "editions";
        m_editions.loadFromDirectory(editionsDir);
        ld.stage.store(6);

        // Stages 6+ happen on the UI thread (audio/sfml objects bind to GL),
        // so just mark ready and let the constructor body continue.
        ld.stage.store(kStageCount);
    });

    auto drawLoading = [&] {
        m_window.clear(sf::Color(11, 10, 9));
        int st = std::min(ld.stage.load(), kStageCount);

        // Title
        sf::Text title("CITADEL MTG", m_font, 36);
        title.setStyle(sf::Text::Bold);
        title.setFillColor(sf::Color(230, 193, 112));
        title.setPosition(WIN_W * 0.5f - title.getLocalBounds().width * 0.5f, 180.f);
        drawText(title);

        sf::Text sub("Loading", m_font, 16);
        sub.setFillColor(sf::Color(148, 139, 124));
        sub.setPosition(WIN_W * 0.5f - sub.getLocalBounds().width * 0.5f, 230.f);
        drawText(sub);

        // Stage label
        std::string label = (st < kStageCount) ? kStageLabels[st] : "Finishing up";
        if (ld.error.load()) label = "ERROR: " + ld.errorMsg;
        sf::Text stageText(label, m_font, 18);
        stageText.setFillColor(ld.error.load() ? sf::Color(220, 110, 110)
                                                : sf::Color(199, 189, 172));
        stageText.setPosition(WIN_W * 0.5f - stageText.getLocalBounds().width * 0.5f, 380.f);
        drawText(stageText);

        // Stage counter
        sf::Text counter(std::to_string(std::min(st + 1, kStageCount)) + " / " +
                         std::to_string(kStageCount), m_font, 12);
        counter.setFillColor(sf::Color(120, 113, 100));
        counter.setPosition(WIN_W * 0.5f - counter.getLocalBounds().width * 0.5f, 410.f);
        drawText(counter);

        // Progress bar
        const float barW = 600.f, barH = 12.f;
        const float barX = (WIN_W - barW) * 0.5f, barY = 440.f;
        sf::RectangleShape track({barW, barH});
        track.setPosition(barX, barY);
        track.setFillColor(sf::Color(34, 31, 27));
        track.setOutlineColor(sf::Color(240, 220, 180, 30));
        track.setOutlineThickness(1.f);
        m_window.draw(track);

        float frac = static_cast<float>(st) / static_cast<float>(kStageCount);
        sf::RectangleShape fill({barW * frac, barH});
        fill.setPosition(barX, barY);
        fill.setFillColor(ld.error.load() ? sf::Color(180, 70, 70)
                                          : sf::Color(203, 163, 90));
        m_window.draw(fill);

        m_window.display();
    };

    while (m_window.isOpen() && ld.stage.load() < kStageCount && !ld.error.load()) {
        sf::Event ev;
        while (m_window.pollEvent(ev)) {
            if (ev.type == sf::Event::Closed) {
                m_window.close();
            }
        }
        drawLoading();
        sf::sleep(sf::milliseconds(33));   // ~30 fps; cheap pump
    }
    if (loader.joinable()) loader.join();

    if (ld.error.load()) {
        // Keep the error visible briefly so the user sees what went wrong.
        for (int i = 0; i < 60 && m_window.isOpen(); ++i) {
            sf::Event ev; while (m_window.pollEvent(ev)) if (ev.type == sf::Event::Closed) m_window.close();
            drawLoading();
            sf::sleep(sf::milliseconds(50));
        }
        if (m_window.isOpen()) m_window.close();
        throw std::runtime_error(ld.errorMsg);
    }

    // Detect skin directory
    {
        // The res root is normally the parent of the resolved card folder
        // (…/res/cardsfolder → …/res); fall back to the detector otherwise.
        auto resRoot = cardFolder.parent_path();
        if (resRoot.empty() ||
            !(fs::exists(resRoot / "skins") || fs::exists(resRoot / "editions")))
            resRoot = detectResRoot();
        if (!resRoot.empty()) {
            auto skinPath = resRoot / "skins" / "default";
            if (fs::exists(skinPath)) {
                m_skinDir = skinPath.string();
                SkinAssets::init(m_skinDir);
                std::cout << "Skin dir: " << m_skinDir << '\n';
            }
        }
        if (m_skinDir.empty())
            std::cout << "No skin directory found — using built-in graphics.\n";
    }

    m_renderer.init(m_font, m_game, m_tm, m_picsDir);

    // Initialise procedural sound system (generates PCM buffers, no external files)
    m_sound.init();
    loadDeckStats();
    loadCardCollection();

    loadSettings();
    initAchievements();
    initKeyBindings();
    loadCardTags();

    // Crash recovery: check for an autosave from a previous session
    {
        namespace fs = std::filesystem;
        if (const char* ap = std::getenv("APPDATA")) {
            auto autoPath = fs::path(ap) / "CitadelMTG" / "autosave.json";
            auto cleanPath = fs::path(ap) / "CitadelMTG" / "clean_exit.txt";
            // Previous session crashed iff it left an autosave behind WITHOUT
            // writing the clean-exit marker (the destructor writes it on a normal
            // quit). A clean exit leaves the marker, so no false positive.
            if (fs::exists(autoPath) && !fs::exists(cleanPath))
                m_pendingCrashRecovery = true;
            // Mark this session as in-progress: drop the marker now; the
            // destructor re-creates it only if we shut down cleanly.
            std::error_code ec;
            fs::remove(cleanPath, ec);
        }
    }

    // Ensure %APPDATA%\CitadelMTG\decks exists, but do NOT auto-write any
    // starter templates — the user manages the deck pool, and the install ships
    // with the official commander precons copied in.
    {
        namespace fs = std::filesystem;
        if (const char* ap = std::getenv("APPDATA")) {
            std::error_code ec;
            fs::create_directories(fs::path(ap) / "CitadelMTG" / "decks", ec);
        }
    }

    // Build screens
    {
        auto decksRoot = cardFolder.parent_path();
        if (decksRoot.empty() || !fs::exists(decksRoot)) decksRoot = detectResRoot();
        if (!decksRoot.empty())
            std::cout << "Deck root: " << decksRoot << '\n';
        m_deckEditor     = std::make_unique<DeckEditorScreen>(
                              m_font, m_db, decksRoot, m_picsDir, &m_editions);
        m_deckEditor->setCollection(m_cardSeen.empty() ? nullptr : &m_cardSeen);
        m_mainMenu       = std::make_unique<MainMenuScreen>(m_font);
        m_mainMenu->initVolume(m_sound.volume(), m_sound.muted());
        m_matchSetup     = std::make_unique<MatchSetupScreen>(m_font, decksRoot, &m_db);
        m_downloadScreen = std::make_unique<CardDownloadScreen>(m_font, m_db);
    }
    m_appState = AppState::MainMenu;
}

bool GameWindow::loadFont() {
    namespace fs = std::filesystem;
    std::vector<std::string> candidates;
    // Bundled font shipped with the app (portable) — preferred over any Forge copy.
    {
        std::vector<fs::path> bases = { fs::current_path() };
        if (const char* ap = std::getenv("APPDATA"); ap && *ap)
            bases.push_back(fs::path(ap) / "CitadelMTG");
        for (const auto& base : bases)
            candidates.push_back((base / "res" / "fonts" / "Roboto-Bold.ttf").string());
    }
    for (const char* base : {"Z:/cardforge/res/fonts", "Z:/forge/res/fonts"})
        candidates.push_back(std::string(base) + "/Roboto-Bold.ttf");
    if (const char* v = std::getenv("CITADEL_CARDS"); v) {
        namespace fs = std::filesystem;
        auto p = fs::path(v).parent_path() / "fonts" / "Roboto-Bold.ttf";
        candidates.push_back(p.string());
    }
    for (const char* var : {"WINDIR", "SystemRoot"}) {
        const char* v = std::getenv(var);
        if (!v) continue;
        std::string base(v);
        for (const char* name : {"arial.ttf", "calibri.ttf", "consola.ttf"})
            candidates.push_back(base + "\\Fonts\\" + name);
    }
    for (const char* p : {
            "C:/Windows/Fonts/arial.ttf",
            "C:/Windows/Fonts/calibri.ttf" })
        candidates.push_back(p);

    for (const auto& p : candidates) {
        if (m_font.loadFromFile(p)) {
            std::cout << "Font loaded: " << p << '\n';
            return true;
        }
    }
    return false;
}

// ── Game setup ────────────────────────────────────────────────────────────────

void GameWindow::startGameWithDeck(const DeckLoader::Deck& p0deck,
                                    const std::filesystem::path& aiPath) {
    namespace fs = std::filesystem;

    m_rematchEditorDeck  = p0deck;
    m_rematchAiDeckPath  = aiPath;
    m_rematchUseEditor   = true;
    saveSettings();

    m_game.reset();
    m_game.setNumPlayers(static_cast<uint8_t>(m_numPlayers));
    m_game.setHumanInteractive(true);
    m_game.setCardDb(&m_db);
    m_tm.reset();
    m_renderer.init(m_font, m_game, m_tm, m_picsDir);
    m_gameLog.clear();
    m_stats = {};  // reset per-game statistics
    // Truncate the persistent log at game start
    if (const char* ap = std::getenv("APPDATA"))
        std::ofstream(std::string(ap) + "\\CitadelMTG\\game_log.txt", std::ios::trunc);

    m_game.player(0) = Player{0, "Alice"};
    m_game.player(1) = Player{1, "Bob"};

    // Player 0 — from deck editor
    int n0 = DeckLoader::buildDeck(p0deck, m_db, m_game, 0);
    if (n0 > 0) m_game.player(0).library().shuffle(m_game.rng());

    // Player 1 — from selected AI deck file or fallback
    if (!aiPath.empty() && fs::exists(aiPath)) {
        DeckLoader::loadAndBuild(aiPath, m_db, m_game, 1);
    } else {
        for (auto& [name, count] : std::initializer_list<std::pair<const char*, int>>{
                {"Forest",16}, {"Llanowar Elves",4}, {"Grizzly Bears",4}}) {
            const auto* r = m_db.find(name);
            if (!r) continue;
            for (int i = 0; i < count; ++i) m_game.createCard(r, 1);
        }
        m_game.player(1).library().shuffle(m_game.rng());
    }

    // Open hands
    for (uint8_t pid = 0; pid < 2; ++pid)
        for (int i = 0; i < 7; ++i) {
            auto& lib = m_game.player(pid).library();
            if (lib.empty()) break;
            m_game.moveToZone(lib.front()->id, ZoneType::Hand, pid);
        }
    preloadHandArt(2);  // start loading opening-hand art before the mulligan screen

    int p0 = (int)(m_game.player(0).library().size() + m_game.player(0).hand().size());
    int p1 = (int)(m_game.player(1).library().size() + m_game.player(1).hand().size());
    if (p0 == 0 || p1 == 0) {
        std::cerr << "WARNING: empty deck(s) — check CITADEL_CARDS. P0=" << p0 << " P1=" << p1 << '\n';
        m_appState = AppState::MainMenu;
        return;
    }

    // Coin flip — determine who plays first
    m_aliceGoesFirst = (std::uniform_int_distribution<int>(0, 1)(m_game.rng()) == 0);
    addLog(std::string(m_aliceGoesFirst ? "Alice" : "Bob") +
           " wins the coin flip and goes first.");
    if (!m_aliceGoesFirst) {
        m_game.setActivePlayer(1);
        m_game.setPriorityPlayer(1);
        m_tm.reset();
    }

    m_mull = {};
    doMulliganForAi();
    m_appState = AppState::Mulligan;
}

void GameWindow::startGameWithDecks(const std::filesystem::path& deck0,
                                     const std::filesystem::path& deck1) {
    namespace fs = std::filesystem;

    m_rematchDeck0     = deck0;
    m_rematchDeck1     = deck1;
    m_rematchUseEditor = false;
    saveSettings();

    m_game.reset();
    m_game.setHumanInteractive(true);
    m_game.setCardDb(&m_db);
    m_tm.reset();
    m_renderer.init(m_font, m_game, m_tm, m_picsDir);
    m_gameLog.clear();
    m_stats = {};
    if (const char* ap = std::getenv("APPDATA"))
        std::ofstream(std::string(ap) + "\\CitadelMTG\\game_log.txt", std::ios::trunc);

    // Resolve seat count + per-seat deck paths. m_matchSetup carries the
    // player name and player-count selection; for seats 1..N-1 we pick a
    // random AI deck from the pool, biasing toward unique commanders.
    int numSeats = 2;
    std::string p0Name = "Player";
    std::vector<fs::path> seatDecks{ deck0, deck1 };
    if (m_matchSetup) {
        numSeats = std::clamp(m_matchSetup->numPlayers(), 2, 4);
        p0Name   = m_matchSetup->playerName();
        for (int s = (int)seatDecks.size(); s < numSeats; ++s) {
            seatDecks.push_back(m_matchSetup->pickRandomAiDeck(seatDecks));
        }
    }
    m_game.setNumPlayers(static_cast<uint8_t>(numSeats));
    m_numPlayers = numSeats;

    // Player 0 = the human (editable name); AI seats = the commander name
    // pulled from each .dck file.
    m_game.player(0) = Player{0, p0Name};
    for (int s = 1; s < numSeats; ++s) {
        std::string nm = MatchSetupScreen::commanderNameOf(seatDecks[(size_t)s]);
        m_game.player(static_cast<uint8_t>(s)) =
            Player{static_cast<uint8_t>(s), nm};
    }

    auto loadOrDefault = [&](const fs::path& path, uint8_t pid,
                              std::initializer_list<std::pair<const char*, int>> fallback) {
        if (!path.empty() && fs::exists(path)) {
            DeckLoader::loadAndBuild(path, m_db, m_game, pid);
        } else {
            for (auto& [name, count] : fallback) {
                const auto* r = m_db.find(name);
                if (!r) continue;
                for (int i = 0; i < count; ++i) m_game.createCard(r, pid);
            }
            m_game.player(pid).library().shuffle(m_game.rng());
        }
    };

    loadOrDefault(seatDecks[0], 0, {{"Mountain",16},{"Lightning Bolt",4},{"Serra Angel",4}});
    for (int s = 1; s < numSeats; ++s)
        loadOrDefault(seatDecks[(size_t)s], static_cast<uint8_t>(s),
                      {{"Forest",16},{"Llanowar Elves",4},{"Grizzly Bears",4}});

    for (int pid = 0; pid < numSeats; ++pid)
        for (int i = 0; i < 7; ++i) {
            auto& lib = m_game.player(static_cast<uint8_t>(pid)).library();
            if (lib.empty()) break;
            m_game.moveToZone(lib.front()->id, ZoneType::Hand,
                              static_cast<uint8_t>(pid));
        }
    preloadHandArt(numSeats);  // start loading opening-hand art before mulligan

    bool anyEmpty = false;
    for (int pid = 0; pid < numSeats; ++pid) {
        auto& p = m_game.player(static_cast<uint8_t>(pid));
        if ((int)(p.library().size() + p.hand().size()) == 0) anyEmpty = true;
    }
    if (anyEmpty) {
        std::cerr << "WARNING: one or more players have 0 cards.\n";
        m_appState = AppState::MainMenu;
        return;
    }

    // Apply custom starting life from match setup
    if (m_matchSetup) {
        int slife = m_matchSetup->startingLife();
        for (int pid = 0; pid < numSeats; ++pid)
            m_game.player(static_cast<uint8_t>(pid)).setLife(slife);
    }

    // Coin flip — pick a random seat to go first (supports 3-4 player).
    {
        std::uniform_int_distribution<int> seatPick(0, numSeats - 1);
        uint8_t firstPid = static_cast<uint8_t>(seatPick(m_game.rng()));
        m_aliceGoesFirst = (firstPid == 0);
        addLog(m_game.player(firstPid).name() +
               " wins the coin flip and goes first.");
        if (firstPid != 0) {
            m_game.setActivePlayer(firstPid);
            m_game.setPriorityPlayer(firstPid);
            m_tm.reset();
        }
    }

    m_mull = {};
    doMulliganForAi();
    m_appState = AppState::Mulligan;
}

void GameWindow::setupGame() {
    m_game.player(0) = Player{0, "Alice"};
    m_game.player(1) = Player{1, "Bob"};

    namespace fs = std::filesystem;
    bool loaded0 = false, loaded1 = false;
    auto tryLoad = [&](const char* env, uint8_t pid, bool& loaded) {
        const char* path = std::getenv(env);
        if (!path) return;
        int n = DeckLoader::loadAndBuild(fs::path(path), m_db, m_game, pid);
        if (n > 0) { std::cout << "[P" << (int)pid << "] " << n << " cards\n"; loaded = true; }
    };
    tryLoad("CITADEL_DECK_P0", 0, loaded0);
    tryLoad("CITADEL_DECK_P1", 1, loaded1);

    auto addCards = [&](uint8_t owner,
                        std::initializer_list<std::pair<const char*, int>> list) {
        for (auto& [name, count] : list) {
            const auto* r = m_db.find(name);
            if (!r) continue;
            for (int i = 0; i < count; ++i) m_game.createCard(r, owner);
        }
        m_game.player(owner).library().shuffle(m_game.rng());
    };
    if (!loaded0) addCards(0, {{"Mountain",16},{"Lightning Bolt",4},{"Serra Angel",4}});
    if (!loaded1) addCards(1, {{"Forest",16},{"Llanowar Elves",4},{"Grizzly Bears",4}});

    for (uint8_t pid = 0; pid < 2; ++pid)
        for (int i = 0; i < 7; ++i) {
            auto& lib = m_game.player(pid).library();
            if (lib.empty()) break;
            m_game.moveToZone(lib.front()->id, ZoneType::Hand, pid);
        }

    preloadHandArt(2);
}

// Kick off async texture loads for every card in the opening hands so the art
// is already loading (on background threads) by the time the mulligan/opening-
// hand screen first renders — avoids art popping in card-by-card. Must run on
// the main thread (TextureCache::get queues the load and is not thread-safe vs
// the render-loop's flushPending()).
void GameWindow::preloadHandArt(int numSeats) {
    for (int pid = 0; pid < numSeats; ++pid)
        for (const Card* c : m_game.player(static_cast<uint8_t>(pid)).hand().cards()) {
            if (!c || !c->rules) continue;
            std::string path = CardImageDownloader::imagePath(c->rules->name);
            if (!path.empty()) TextureCache::get(path);
        }
}

void GameWindow::prefetchVisibleArt() {
    // Throttle: rescanning every frame is wasteful; once every ~12 frames is
    // plenty to stay ahead of the player. TextureCache::get is a no-op for art
    // already loaded/in-flight, so this just queues anything newly visible.
    static int frame = 0;
    if (++frame % 12 != 0) return;
    auto pf = [](const Card* c) {
        if (!c || !c->rules) return;
        std::string p = CardImageDownloader::imagePath(c->rules->name);
        if (!p.empty()) { TextureCache::get(p); return; }
        // No art on disk. Tokens aren't in the oracle bulk, so fetch them
        // directly from Scryfall (is:token), narrowed by P/T for the right
        // variant. De-duplicated internally, so calling each frame is fine.
        if (c->isToken) {
            std::string extra;
            if (c->rules->type.isCreature()) {
                if (!c->rules->power.empty())     extra  = "pow=" + c->rules->power;
                if (!c->rules->toughness.empty()) extra += (extra.empty() ? "" : " ") +
                                                           std::string("tou=") + c->rules->toughness;
            }
            CardImageDownloader::fetchToken(c->rules->name, extra);
        }
    };
    for (const Card* c : m_game.battlefield().cards()) pf(c);   // all permanents
    for (const Card* c : m_game.player(0).hand().cards()) pf(c); // your hand
}

void GameWindow::reloadCustomCards() {
    size_t before = m_db.size();
    m_db.loadCustomCards();   // re-reads %APPDATA%/CitadelMTG/customcards
    m_db.wireBackFaces();     // re-link any new DFC faces
    if (m_deckEditor) m_deckEditor->refreshCatalog();
    if (m_matchSetup) m_matchSetup->refresh();
    std::cout << "[GameWindow] Reloaded custom cards: " << before << " -> "
              << m_db.size() << " names in DB.\n";
}

void GameWindow::launchCardCreator() {
    namespace fs = std::filesystem;
    // Find the creator script: next to the exe (if shipped there), in the
    // working dir's training/, or the dev tree.
    fs::path exeDir = fs::current_path();
    std::vector<fs::path> cands = {
        exeDir / "tools" / "card_creator.py",
        exeDir / "training" / "card_creator.py",
        fs::current_path() / "training" / "card_creator.py",
        fs::path("z:/forgeraw/mtg-project/training/card_creator.py"),
    };
    fs::path script;
    for (auto& c : cands) { std::error_code ec; if (fs::exists(c, ec)) { script = c; break; } }
    if (script.empty()) {
        std::cerr << "[GameWindow] card_creator.py not found — run training/card_creator.py manually.\n";
        return;
    }
#ifdef _WIN32
    // "start" detaches so the game keeps running; reload with F5 when done.
    std::string cmd = "start \"\" python \"" + script.string() + "\"";
#else
    std::string cmd = "python \"" + script.string() + "\" &";
#endif
    std::system(cmd.c_str());
    std::cout << "[GameWindow] Launched card creator: " << script
              << "  (press F5 here to reload after saving)\n";
}

// ── View / fullscreen ─────────────────────────────────────────────────────────

void GameWindow::drawText(sf::Text& t) {
    // Re-rasterize the glyphs at viewport scale, draw, then restore so the
    // object is unchanged for any later reuse/measurement. At g_uiScale <= 1
    // applyTextScale is a no-op, so this is exactly a plain window draw.
    unsigned     baseSize  = t.getCharacterSize();
    sf::Vector2f baseScale = t.getScale();
    ui::applyTextScale(t);
    m_window.draw(t);
    t.setCharacterSize(baseSize);
    t.setScale(baseScale);
}

void GameWindow::updateView() {
    auto size = m_window.getSize();
    float scaleX = static_cast<float>(size.x) / WIN_W;
    float scaleY = static_cast<float>(size.y) / WIN_H;
    float scale  = std::min(scaleX, scaleY);
    float vpW    = (WIN_W * scale) / static_cast<float>(size.x);
    float vpH    = (WIN_H * scale) / static_cast<float>(size.y);
    m_gameView.setSize(WIN_W, WIN_H);
    m_gameView.setCenter(WIN_W * 0.5f, WIN_H * 0.5f);
    m_gameView.setViewport(sf::FloatRect((1.f - vpW) * 0.5f,
                                          (1.f - vpH) * 0.5f,
                                          vpW, vpH));
    m_window.setView(m_gameView);
    ui::g_uiScale = scale;   // keep glyph rasterization in sync with viewport scale
}

void GameWindow::toggleFullscreen() {
    m_fullscreen = !m_fullscreen;
    TextureCache::clear();
    if (m_fullscreen) {
        m_window.create(sf::VideoMode::getDesktopMode(),
                        "Citadel MTG", sf::Style::Fullscreen,
                        sf::ContextSettings(0, 0, 8));
    } else {
        m_window.create(sf::VideoMode(static_cast<unsigned>(WIN_W),
                                       static_cast<unsigned>(WIN_H)),
                        "Citadel MTG", sf::Style::Default,
                        sf::ContextSettings(0, 0, 8));
    }
    m_window.setFramerateLimit(60);
    updateView();
}

sf::Event GameWindow::remapEvent(const sf::Event& ev) const {
    sf::Event out = ev;
    if (ev.type == sf::Event::MouseButtonPressed ||
        ev.type == sf::Event::MouseButtonReleased) {
        auto m = mapMousePos(ev.mouseButton.x, ev.mouseButton.y);
        out.mouseButton.x = static_cast<int>(m.x);
        out.mouseButton.y = static_cast<int>(m.y);
    } else if (ev.type == sf::Event::MouseMoved) {
        auto m = mapMousePos(ev.mouseMove.x, ev.mouseMove.y);
        out.mouseMove.x = static_cast<int>(m.x);
        out.mouseMove.y = static_cast<int>(m.y);
    } else if (ev.type == sf::Event::MouseWheelScrolled) {
        auto m = mapMousePos(ev.mouseWheelScroll.x, ev.mouseWheelScroll.y);
        out.mouseWheelScroll.x = static_cast<int>(m.x);
        out.mouseWheelScroll.y = static_cast<int>(m.y);
    }
    return out;
}

sf::Vector2f GameWindow::mapMousePos(int x, int y) const {
    return m_window.mapPixelToCoords(sf::Vector2i(x, y), m_gameView);
}

// ── Main loop ─────────────────────────────────────────────────────────────────

void GameWindow::run() {
    while (m_window.isOpen()) {
        // Promote any async-loaded textures (card art) once per frame. Without
        // this the menu/deck-editor screens queue loads via TextureCache::get()
        // but never see them complete, so the deck-editor image preview (and any
        // other menu art) stays blank. The in-match loop flushes separately.
        ui::TextureCache::flushPending();
        if (m_appState == AppState::MainMenu) {
            sf::Event ev;
            while (m_window.pollEvent(ev)) {
                if (ev.type == sf::Event::Closed)  { m_window.close(); return; }
                if (ev.type == sf::Event::Resized) { updateView(); }
                if (ev.type == sf::Event::KeyPressed &&
                    ev.key.code == sf::Keyboard::F11) { toggleFullscreen(); continue; }
                // Crash recovery: R = restore, any other key = dismiss
                if (m_pendingCrashRecovery && ev.type == sf::Event::KeyPressed) {
                    if (ev.key.code == sf::Keyboard::R) {
                        namespace fs = std::filesystem;
                        if (const char* ap = std::getenv("APPDATA")) {
                            loadGame(fs::path(ap) / "CitadelMTG" / "autosave.json");
                            m_appState = AppState::Playing;
                            addLog("Session restored from autosave.");
                        }
                    }
                    m_pendingCrashRecovery = false;
                    continue;
                }
                if (m_mainMenu) {
                    auto act = m_mainMenu->onEvent(remapEvent(ev));
                    if (act == MainMenuScreen::Action::DeckBuilder) {
                        m_appState = AppState::DeckEditor;
                        if (m_deckEditor)
                            m_deckEditor->setCollection(m_cardSeen.empty() ? nullptr : &m_cardSeen);
                    } else if (act == MainMenuScreen::Action::PlayVsAI) {
                        m_matchSetup->refresh();
                        m_appState = AppState::MatchSetup;
                    } else if (act == MainMenuScreen::Action::DownloadArt) {
                        m_downloadScreen = std::make_unique<CardDownloadScreen>(m_font, m_db);
                        m_appState = AppState::DownloadArt;
                    } else if (act == MainMenuScreen::Action::Settings) {
                        // Settings overlay is handled inside MainMenuScreen itself
                        // (m_showSettings toggled by onEvent on Settings click)
                    } else if (act == MainMenuScreen::Action::RefreshCombos) {
                        // Pull from Commander Spellbook (~1500 combos by
                        // default) on a worker thread so the menu stays
                        // responsive — the paginated fetch takes 10-30s.
                        m_mainMenu->setAiDataStatus(
                            "Downloading combos from Commander Spellbook…");
                        std::thread([this]() {
                            std::filesystem::path comboPath =
                                m_dataDir / "combos.json";
                            int n = mtg::AiPlayer::downloadCombos(comboPath.string());
                            if (!m_mainMenu) return;
                            if (n > 0) {
                                m_mainMenu->setAiDataCounts(
                                    mtg::AiPlayer::combosLoaded(),
                                    mtg::SaltDatabase::size());
                                m_mainMenu->setAiDataStatus(
                                    "Downloaded " + std::to_string(n) +
                                    " combos from Commander Spellbook.");
                            } else {
                                // Fall back to whatever's already on disk so
                                // the user doesn't lose their existing cache.
                                std::filesystem::path comboPath2 =
                                    m_dataDir / "combos.json";
                                if (std::filesystem::exists(comboPath2))
                                    mtg::AiPlayer::loadCombos(comboPath2.string());
                                m_mainMenu->setAiDataCounts(
                                    mtg::AiPlayer::combosLoaded(),
                                    mtg::SaltDatabase::size());
                                m_mainMenu->setAiDataStatus(
                                    "Download failed (offline?). Existing cache preserved.");
                            }
                        }).detach();
                    } else if (act == MainMenuScreen::Action::DownloadSalt) {
                        // Run the salt fetch on a worker thread so the menu
                        // stays responsive — EDHREC can take several seconds.
                        m_mainMenu->setAiDataStatus("Downloading from EDHREC…");
                        std::thread([this]() {
                            std::filesystem::path saltCache =
                                m_dataDir / "salt_scores.json";
                            int n = mtg::SaltDatabase::fetchAndCache(saltCache.string());
                            if (m_mainMenu) {
                                m_mainMenu->setAiDataCounts(
                                    mtg::AiPlayer::combosLoaded(), n);
                                m_mainMenu->setAiDataStatus(
                                    "Salt download complete: " + std::to_string(n) + " entries.");
                            }
                        }).detach();
                    } else if (act == MainMenuScreen::Action::Quit) {
                        m_window.close(); return;
                    }
                }
            }
            m_window.setView(m_gameView);
            if (m_mainMenu) {
                // Keep counts fresh in case the user just opened the overlay.
                m_mainMenu->setAiDataCounts(mtg::AiPlayer::combosLoaded(),
                                             mtg::SaltDatabase::size());
                m_mainMenu->draw(m_window);
            }

            // Crash recovery overlay
            if (m_pendingCrashRecovery) {
                sf::RectangleShape dim({WIN_W, WIN_H});
                dim.setFillColor(sf::Color(0, 0, 0, 160));
                m_window.draw(dim);
                sf::RectangleShape pan({440.f, 100.f});
                pan.setPosition((WIN_W - 440.f) * 0.5f, (WIN_H - 100.f) * 0.5f);
                pan.setFillColor(sf::Color(18, 17, 16, 250));
                pan.setOutlineColor(sf::Color(203, 163, 90));
                pan.setOutlineThickness(2.f);
                m_window.draw(pan);
                sf::Text hdr("Previous session crashed!", m_font, 14);
                hdr.setStyle(sf::Text::Bold);
                hdr.setFillColor(sf::Color(230, 193, 112));
                hdr.setPosition((WIN_W - 200.f) * 0.5f, (WIN_H - 100.f) * 0.5f + 8.f);
                drawText(hdr);
                sf::Text msg("Press R to restore autosave, or any other key to dismiss.", m_font, 10);
                msg.setFillColor(sf::Color(199, 189, 172));
                msg.setPosition((WIN_W - 340.f) * 0.5f, (WIN_H - 100.f) * 0.5f + 32.f);
                drawText(msg);
            }

            m_window.display();

        } else if (m_appState == AppState::DeckEditor) {
            sf::Event ev;
            while (m_window.pollEvent(ev)) {
                if (ev.type == sf::Event::Closed)  { m_window.close(); return; }
                if (ev.type == sf::Event::Resized) { updateView(); }
                if (ev.type == sf::Event::KeyPressed &&
                    ev.key.code == sf::Keyboard::F11) { toggleFullscreen(); continue; }
                // F5 = hot-reload custom cards (pick up anything made in the
                // card creator without restarting); F6 = launch the creator.
                if (ev.type == sf::Event::KeyPressed &&
                    ev.key.code == sf::Keyboard::F5) { reloadCustomCards(); continue; }
                if (ev.type == sf::Event::KeyPressed &&
                    ev.key.code == sf::Keyboard::F6) { launchCardCreator(); continue; }
                if (m_deckEditor) {
                    auto res = m_deckEditor->onEvent(remapEvent(ev));
                    if (res == DeckEditorScreen::Result::StartGame)
                        startGameWithDeck(m_deckEditor->playerDeck(),
                                          m_deckEditor->aiDeckPath());
                    else if (res == DeckEditorScreen::Result::Back)
                        m_appState = AppState::MainMenu;
                }
            }
            m_window.setView(m_gameView);
            if (m_deckEditor) m_deckEditor->draw(m_window);
            m_window.display();

        } else if (m_appState == AppState::MatchSetup) {
            sf::Event ev;
            while (m_window.pollEvent(ev)) {
                if (ev.type == sf::Event::Closed)  { m_window.close(); return; }
                if (ev.type == sf::Event::Resized) { updateView(); }
                if (ev.type == sf::Event::KeyPressed &&
                    ev.key.code == sf::Keyboard::F11) { toggleFullscreen(); continue; }
                if (m_matchSetup) {
                    auto act = m_matchSetup->onEvent(remapEvent(ev));
                    if (act == MatchSetupScreen::Action::StartGame) {
                        // Apply AI difficulty from the selector
                        m_bobAi.enableMcts(true,
                            MctsConfig{m_matchSetup->aiMctsIterations(), 1.5f});
                        startGameWithDecks(m_matchSetup->playerDeckPath(),
                                           m_matchSetup->aiDeckPath());
                    }
                    else if (act == MatchSetupScreen::Action::Back)
                        m_appState = AppState::MainMenu;
                }
            }
            m_window.setView(m_gameView);
            if (m_matchSetup) m_matchSetup->draw(m_window);
            m_window.display();

        } else if (m_appState == AppState::Mulligan) {
            sf::Event ev;
            while (m_window.pollEvent(ev)) {
                if (ev.type == sf::Event::Closed)  { m_window.close(); return; }
                if (ev.type == sf::Event::Resized) { updateView(); }
                if (ev.type == sf::Event::KeyPressed &&
                    ev.key.code == sf::Keyboard::F11) { toggleFullscreen(); continue; }
                if (ev.type == sf::Event::MouseMoved)
                    m_mousePos = mapMousePos(ev.mouseMove.x, ev.mouseMove.y);
                if (ev.type == sf::Event::MouseButtonPressed &&
                    ev.mouseButton.button == sf::Mouse::Left) {
                    auto m = mapMousePos(ev.mouseButton.x, ev.mouseButton.y);
                    handleMulliganClick(m.x, m.y);
                }
            }
            if (m_appState == AppState::Mulligan) renderMulligan();

        } else if (m_appState == AppState::DownloadArt) {
            sf::Event ev;
            while (m_window.pollEvent(ev)) {
                if (ev.type == sf::Event::Closed)  { m_window.close(); return; }
                if (ev.type == sf::Event::Resized) { updateView(); }
                if (ev.type == sf::Event::KeyPressed &&
                    ev.key.code == sf::Keyboard::F11) { toggleFullscreen(); continue; }
                if (m_downloadScreen) {
                    auto res = m_downloadScreen->onEvent(remapEvent(ev));
                    if (res == CardDownloadScreen::Result::Back) {
                        CardImageDownloader::stop();
                        m_appState = AppState::MainMenu;
                    }
                }
            }
            if (m_downloadScreen) {
                m_downloadScreen->update();
                m_window.setView(m_gameView);
                m_downloadScreen->draw(m_window);
                m_window.display();
            }

        } else if (m_appState == AppState::Spectate) {
            sf::Event ev;
            while (m_window.pollEvent(ev)) {
                if (ev.type == sf::Event::Closed)  { m_window.close(); return; }
                if (ev.type == sf::Event::Resized) { updateView(); }
                if (ev.type == sf::Event::KeyPressed) {
                    if (ev.key.code == sf::Keyboard::Escape) {
                        m_appState = AppState::Playing; break;
                    }
                    if (ev.key.code == sf::Keyboard::F11) { toggleFullscreen(); }
                    // Space: pause/unpause
                    if (ev.key.code == sf::Keyboard::Space) {
                        m_spectatePaused = !m_spectatePaused;
                        addLog(m_spectatePaused ? "Spectate PAUSED (Space to resume)."
                                                : "Spectate resumed.");
                    }
                    // N: step one AI turn
                    if (ev.key.code == sf::Keyboard::N && m_spectatePaused)
                        m_spectateStep = true;
                    // +/-: adjust speed
                    if (ev.key.code == sf::Keyboard::Equal || ev.key.code == sf::Keyboard::Add)
                        m_spectateSpeed = std::min(4.f, m_spectateSpeed + 0.25f);
                    if (ev.key.code == sf::Keyboard::Subtract || ev.key.code == sf::Keyboard::Hyphen)
                        m_spectateSpeed = std::max(0.25f, m_spectateSpeed - 0.25f);
                }
            }

            // Skip AI turn if paused (unless step was requested)
            bool doTurn = !m_spectatePaused || m_spectateStep;
            m_spectateStep = false;

            if (doTurn) {
                if (m_turn == WhosTurn::Human && !m_tm.isGameOver()) {
                    if (m_human.state() != HumanState::GameOver)
                        m_human.resetToMainPhase();
                    m_turn = WhosTurn::AI;
                }
                // Apply speed: insert a small sleep between turns at slow settings
                if (m_spectateSpeed < 1.f) {
                    sf::sleep(sf::milliseconds(
                        static_cast<int>((1.f - m_spectateSpeed) * 400)));
                }
                update();
            }
            render();

        } else {  // AppState::Playing
            // Wrap the per-frame mid-game tick in try/catch so a malformed
            // card script or unexpected state throws a recoverable error
            // (game returns to the main menu with a log entry) instead of
            // taking the whole process down. Previously an uncaught C++
            // exception would call std::terminate and abort, and on
            // Windows that abort could destabilise other apps sharing the
            // GPU (driver TDR cascade) before WER got to write a dump.
            try {
                handleEvents();
                update();
                render();
            } catch (const std::exception& e) {
                std::cerr << "[GameWindow] Mid-game exception: " << e.what()
                          << "\n  → bailing out to main menu so the GPU "
                             "doesn't get stuck.\n";
                addLog(std::string("ERROR: ") + e.what() +
                       " (bailing to main menu)");
                m_appState = AppState::MainMenu;
            } catch (...) {
                std::cerr << "[GameWindow] Mid-game unknown exception; "
                             "bailing to main menu.\n";
                addLog("ERROR: unknown C++ exception (bailing to main menu)");
                m_appState = AppState::MainMenu;
            }
        }
    }
}

// ── Game event handling ───────────────────────────────────────────────────────

void GameWindow::handleEvents() {
    sf::Event ev;
    while (m_window.pollEvent(ev)) {
        if (ev.type == sf::Event::Closed)  { m_window.close(); return; }
        if (ev.type == sf::Event::Resized) { updateView(); }
        if (ev.type == sf::Event::MouseMoved)
            m_mousePos = mapMousePos(ev.mouseMove.x, ev.mouseMove.y);

        // ── ESC pause menu ──────────────────────────────────────────────────
        // Highest-priority intercept while playing: ESC opens/closes the
        // pause menu, mouse moves update its hover, clicks fire its actions.
        if (m_pauseMenuOpen) {
            if (ev.type == sf::Event::MouseMoved) {
                auto p = mapMousePos(ev.mouseMove.x, ev.mouseMove.y);
                auto act = hitPauseMenu(p.x, p.y);
                m_pauseMenuHover = (act == PauseAction::None) ? -1
                                                              : static_cast<int>(act) - 1;
                continue;
            }
            if (ev.type == sf::Event::KeyPressed &&
                ev.key.code == sf::Keyboard::Escape) {
                m_pauseMenuOpen = false; continue;
            }
            if (ev.type == sf::Event::MouseButtonPressed &&
                ev.mouseButton.button == sf::Mouse::Left) {
                auto p = mapMousePos(ev.mouseButton.x, ev.mouseButton.y);
                switch (hitPauseMenu(p.x, p.y)) {
                    case PauseAction::Resume:  m_pauseMenuOpen = false; break;
                    case PauseAction::Undo:    doUndo();
                                               m_pauseMenuOpen = false; break;
                    case PauseAction::Concede: doConcede();
                                               m_pauseMenuOpen = false; break;
                    case PauseAction::Quit:    m_pauseMenuOpen = false;
                                               m_appState = AppState::MainMenu;
                                               break;
                    case PauseAction::None:    break;
                }
                continue;
            }
            // While the menu is open, swallow all other input so the game
            // underneath doesn't react to clicks the user can't see.
            continue;
        }

        // ── Mana-choice / Scry / mana-ability picker (highest priority) ────
        // These overlays block all other interaction — handle their buttons
        // before any card hit-test so an underlying land can't catch the click.
        if (ev.type == sf::Event::MouseButtonPressed &&
            ev.mouseButton.button == sf::Mouse::Left) {
            auto pm = mapMousePos(ev.mouseButton.x, ev.mouseButton.y);
            if (m_game.hasPendingManaChoice()) {
                for (const auto& h : m_manaChoiceHits) {
                    if (h.rect.contains(pm)) {
                        completeManaChoice(h.color);
                        break;
                    }
                }
                continue;  // swallow click — overlay is modal
            }
            if (m_game.hasPendingChooseType()) {
                for (const auto& h : m_chooseTypeHits) {
                    if (h.rect.contains(pm)) {
                        completeChooseType(h.type);
                        break;
                    }
                }
                continue;  // swallow click — overlay is modal
            }
            if (m_game.hasPendingScry()) {
                if (m_scryKeepRect.contains(pm))       completeScryChoice(true);
                else if (m_scryBottomRect.contains(pm)) completeScryChoice(false);
                continue;  // swallow click — overlay is modal
            }
            if (m_human.manaAbilitySource() != kInvalidId) {
                if (m_manaAbilityCancelRect.contains(pm)) {
                    m_human.cancelManaAbility();
                    continue;
                }
                bool hit = false;
                for (const auto& h : m_manaAbilityHits) {
                    if (h.rect.contains(pm)) {
                        snapshotUndo();
                        m_human.chooseManaAbility(h.abilityIndex);
                        hit = true;
                        break;
                    }
                }
                if (hit) continue;
                // Click outside the picker → cancel.
                m_human.cancelManaAbility();
                continue;
            }
        }

        // ── Cast options popup intercept ────────────────────────────────────
        // Highest-priority click intercept while a hand spell is pending so
        // the popup's mode buttons fire before the underlying card/zone
        // hit-test. Cancel "X" dismisses the popup; each mode row dispatches
        // to chooseMode().
        if (ev.type == sf::Event::MouseButtonPressed &&
            ev.mouseButton.button == sf::Mouse::Left &&
            m_human.pendingSpellId() != kInvalidId) {
            auto pm = mapMousePos(ev.mouseButton.x, ev.mouseButton.y);
            if (m_castPopupCancelRect.contains(pm)) {
                m_human.cancelPendingSpell();
                m_drag.active = false;
                continue;
            }
            bool hitMode = false;
            for (const auto& h : m_castPopupHits) {
                if (h.rect.contains(pm)) {
                    m_human.chooseMode(h.kind);
                    m_drag.active = false;
                    hitMode = true;
                    break;
                }
            }
            if (hitMode) continue;
        }

        // Open pause menu on ESC during a game, but only when nothing else
        // owns the Escape key (search box, log filter, overlays).
        if (ev.type == sf::Event::KeyPressed &&
            ev.key.code == sf::Keyboard::Escape &&
            m_appState == AppState::Playing &&
            !m_logFilterActive && !m_showCardSearch && !m_zoneBrowseActive) {
            m_pauseMenuOpen  = true;
            m_pauseMenuHover = -1;
            continue;
        }

        // GY browser scrolling
        if (ev.type == sf::Event::MouseWheelScrolled && m_zoneBrowseActive) {
            RenderHints tmpH; tmpH.showZoneBrowse = true;
            int delta = ev.mouseWheelScroll.delta > 0 ? -2 : 2;
            m_renderer.scrollZoneBrowser(delta, tmpH);
            continue;
        }

        // Log filter input (/ to enter, Escape to clear)
        if (ev.type == sf::Event::KeyPressed &&
            ev.key.code == sf::Keyboard::Slash && !m_showCardSearch) {
            m_logFilterActive = true; m_logFilter.clear(); continue;
        }
        if (m_logFilterActive) {
            if (ev.type == sf::Event::TextEntered) {
                uint32_t ch = ev.text.unicode;
                if (ch == '\b' && !m_logFilter.empty()) m_logFilter.pop_back();
                else if (ch >= 32 && ch < 127) m_logFilter += static_cast<char>(ch);
                continue;
            }
            if (ev.type == sf::Event::KeyPressed) {
                if (ev.key.code == sf::Keyboard::Escape) {
                    m_logFilterActive = false; m_logFilter.clear(); continue;
                }
                if (ev.key.code == sf::Keyboard::Return) {
                    m_logFilterActive = false; continue; // keep filter active, just stop typing
                }
            }
        }

        // Text input for card search
        if (ev.type == sf::Event::TextEntered && m_showCardSearch) {
            uint32_t ch = ev.text.unicode;
            if (ch == 8 && !m_cardSearchQuery.empty())  // backspace
                m_cardSearchQuery.pop_back();
            else if (ch >= 32 && ch < 127)
                m_cardSearchQuery += static_cast<char>(ch);
            continue;
        }
        if (ev.type == sf::Event::KeyPressed && m_showCardSearch) {
            // Backspace already handled above; Enter or Escape closes
            if (ev.key.code == sf::Keyboard::Escape || ev.key.code == sf::Keyboard::Return) {
                m_showCardSearch = false; m_cardSearchQuery.clear();
            }
            continue;
        }

        if (ev.type == sf::Event::KeyPressed) {
            // Split overlay keyboard navigation
            if (m_splitChoice.active) {
                if (ev.key.code == sf::Keyboard::Tab ||
                    ev.key.code == sf::Keyboard::Left ||
                    ev.key.code == sf::Keyboard::Right)
                    m_splitChoice.selection ^= 1;
                else if (ev.key.code == sf::Keyboard::Return) {
                    // Confirm selected half (same logic as mouse click)
                    mtg::ObjectId sid = m_splitChoice.cardId;
                    m_splitChoice.active = false;
                    snapshotUndo();
                    m_human.onCardClick(sid, ZoneType::Hand, 0);
                }
                else if (ev.key.code == sf::Keyboard::Escape)
                    m_splitChoice.active = false;
                continue;
            }

            // R while zoomed: fetch rulings from Scryfall
            if (ev.key.code == sf::Keyboard::R && !ev.key.control && m_zoomCardId != kInvalidId) {
                const Card* zc = m_game.findCard(m_zoomCardId);
                if (zc && m_rulingsFetchedFor != zc->rules->name) {
                    m_rulingsFetchedFor = zc->rules->name;
                    m_cachedRulings = { "(Fetching from Scryfall...)" };
                    // Fetch in a detached thread to avoid blocking the UI
                    std::thread([this, name = zc->rules->name]() {
                        auto r = ui::CardImageDownloader::fetchRulings(name);
                        m_cachedRulings = std::move(r);
                    }).detach();
                }
                m_showRulings = !m_showRulings;
                continue;
            }
            if (ev.key.code == sf::Keyboard::Escape) {
                if (m_artZoomCardId != kInvalidId) { m_artZoomCardId = kInvalidId; continue; }
                if (m_zoomCardId != kInvalidId)   { m_zoomCardId = kInvalidId; m_showRulings = false; continue; }
                if (m_showStats)                  { m_showStats = false; continue; }
                if (m_showAchievements)           { m_showAchievements = false; continue; }
                if (m_splitChoice.active) { m_splitChoice.active = false; continue; }
                if (m_showCardSearch) { m_showCardSearch = false; m_cardSearchQuery.clear(); continue; }
                if (m_showHelp)    { m_showHelp    = false; continue; }
                if (m_showOptions) { m_showOptions = false; continue; }
                m_appState = AppState::MainMenu; return;
            }
            // Tab: stats overlay
            if (ev.key.code == sf::Keyboard::Tab) {
                m_showStats = !m_showStats; m_showAchievements = false; continue;
            }
            // Ctrl+A: achievements overlay
            if (ev.key.code == sf::Keyboard::A && ev.key.control) {
                m_showAchievements = !m_showAchievements; m_showStats = false; continue;
            }
            // Ctrl+Q: collection statistics
            if (ev.key.code == sf::Keyboard::Q && ev.key.control) {
                m_showCollection = !m_showCollection; m_showStats = false; continue;
            }
            // Ctrl+F — toggle card search overlay
            if (ev.key.code == sf::Keyboard::F && ev.key.control) {
                m_showCardSearch = !m_showCardSearch;
                if (m_showCardSearch) m_cardSearchQuery.clear();
                continue;
            }
            if (ev.key.code == sf::Keyboard::F11)    { toggleFullscreen(); continue; }
            // F3: toggle FPS/profiler overlay
            if (ev.key.code == sf::Keyboard::F3) {
                m_showFpsOverlay = !m_showFpsOverlay; continue;
            }
            // F5: hot-reload card rules for all battlefield cards
            if (ev.key.code == sf::Keyboard::F5) {
                int reloaded = 0;
                namespace fs = std::filesystem;
                fs::path cardsDir;
                for (const char* c : {R"(Z:\cardforge\res\cardsfolder)",
                                       R"(Z:\cardforge\cardsfolder)"})
                    if (fs::exists(c)) { cardsDir = c; break; }
                if (!cardsDir.empty()) {
                    for (const Card* bc : m_game.battlefield().cards()) {
                        if (bc->isToken) continue;
                        if (const_cast<mtg::CardDb&>(m_db).reloadCard(cardsDir, bc->rules->name)) ++reloaded;
                    }
                    m_game.recomputeStaticBonuses();
                }
                addLog("F5 hot-reload: refreshed " + std::to_string(reloaded) + " card(s).");
                continue;
            }
            if (m_tm.isGameOver()) {
                if (ev.key.code == sf::Keyboard::R && !ev.key.control) { doRematch(); return; }
                if (ev.key.code == sf::Keyboard::R && ev.key.control) {
                    // Save replay of last game
                    namespace fs = std::filesystem;
                    if (const char* ap = std::getenv("APPDATA")) {
                        auto replayPath = fs::path(ap) / "CitadelMTG" / "last_game_replay.json";
                        // Build minimal replay from current game state for post-game inspection
                        saveGame(replayPath.replace_extension(".json").string());
                        addLog("Replay saved to last_game_replay.json");
                    }
                    continue;
                }
                continue;
            }
            // S — enter spectate (AI-vs-AI) mode mid-game
            if (ev.key.code == sf::Keyboard::S && !ev.key.control) {
                startSpectate(); continue;
            }
            // Ctrl+K — open keybindings panel
            if (ev.key.code == sf::Keyboard::K && ev.key.control) {
                m_showKeyBindings = !m_showKeyBindings; continue;
            }
            // G — toggle graveyard browser (Alice's GY)
            auto kb = [&](const char* action) -> sf::Keyboard::Key {
                auto it = m_keyBindings.find(action);
                return it != m_keyBindings.end() ? it->second : sf::Keyboard::Unknown;
            };
            if (ev.key.code != sf::Keyboard::Unknown && ev.key.code == kb("my_gy") && !ev.key.control) {
                if (m_zoneBrowseActive) { m_zoneBrowseActive = false; }
                else { m_zoneBrowseActive = true; m_zoneBrowsePlayer = 0; m_zoneBrowseZone = ui::BrowseZone::Graveyard; m_renderer.invalidateZoneBrowserCache(); }
                continue;
            }
            // H — toggle opponent GY browser (Bob's GY)
            if (ev.key.code != sf::Keyboard::Unknown && ev.key.code == kb("opp_gy") && !ev.key.control) {
                if (m_zoneBrowseActive) { m_zoneBrowseActive = false; }
                else { m_zoneBrowseActive = true; m_zoneBrowsePlayer = 1; m_zoneBrowseZone = ui::BrowseZone::Graveyard; m_renderer.invalidateZoneBrowserCache(); }
                continue;
            }
            // Accept input on the human's turn OR whenever they have a
            // pending decision during the AI's turn (blocker declaration,
            // trigger target, etc.) — otherwise the user can't pass when
            // they have no legal blockers to assign.
            if ((m_turn == WhosTurn::Human || m_human.needsInput()) &&
                !m_human.isGameOver()) {
                // Proliferate: confirm selection with Space/Enter
                if (m_game.hasPendingProliferate() &&
                    (ev.key.code == sf::Keyboard::Space || ev.key.code == sf::Keyboard::Return)) {
                    m_game.resolveProliferateChoice();
                    if (!m_game.hasPendingProliferate()) {
                        while (StateBasedActions::run(m_game)) {}
                        m_abilities.drainPendingTriggers();
                    }
                    continue;
                }
                if (ev.key.code == sf::Keyboard::Space)  doAiResponseThenPass();
                if (ev.key.code == sf::Keyboard::Return) m_human.onConfirm();

                // Ctrl+Z — undo last action
                if (ev.key.code == sf::Keyboard::Z && ev.key.control) {
                    doUndo(); continue;
                }
                // Ctrl+1/2/3 — save to numbered slot; Ctrl+Shift+1/2/3 — load from slot
                if (ev.key.control) {
                    int slot = -1;
                    if (ev.key.code == sf::Keyboard::Num1) slot = 1;
                    if (ev.key.code == sf::Keyboard::Num2) slot = 2;
                    if (ev.key.code == sf::Keyboard::Num3) slot = 3;
                    if (slot > 0) {
                        namespace fs = std::filesystem;
                        if (const char* ap = std::getenv("APPDATA")) {
                            auto slotPath = fs::path(ap) / "CitadelMTG" /
                                           ("slot" + std::to_string(slot) + ".json");
                            if (ev.key.shift) {
                                loadGame(slotPath);
                                addLog("Loaded slot " + std::to_string(slot) + ".");
                            } else {
                                saveGame(slotPath);
                                addLog("Saved to slot " + std::to_string(slot) + ".");
                            }
                        }
                        continue;
                    }
                }
                // Ctrl+B — toggle colour-blind mode
                if (ev.key.code == sf::Keyboard::B && ev.key.control) {
                    m_colorBlindMode = !m_colorBlindMode;
                    addLog(m_colorBlindMode ? "Colour-blind mode ON." : "Colour-blind mode OFF.");
                    continue;
                }
                // Ctrl+L — load quick-save
                if (ev.key.code == sf::Keyboard::L && ev.key.control) {
                    namespace fs = std::filesystem;
                    if (const char* ap = std::getenv("APPDATA")) {
                        auto savePath = fs::path(ap) / "CitadelMTG" / "quicksave.json";
                        loadGame(savePath);
                    }
                    continue;
                }
                // Ctrl+S — quick-save game state
                if (ev.key.code == sf::Keyboard::S && ev.key.control) {
                    namespace fs = std::filesystem;
                    if (const char* ap = std::getenv("APPDATA")) {
                        auto savePath = fs::path(ap) / "CitadelMTG" / "quicksave.json";
                        saveGame(savePath);
                    }
                    continue;
                }

                // ── Gameplay hotkeys ──────────────────────────────────────────
                // Ctrl+T — toggle tournament mode (best-of-3)
                if (ev.key.code == sf::Keyboard::T && ev.key.control) {
                    m_tournamentMode = !m_tournamentMode;
                    m_matchWins[0] = m_matchWins[1] = 0; m_gamesPlayed = 0;
                    addLog(m_tournamentMode ? "Tournament mode ON (best-of-3)." : "Tournament mode OFF.");
                    continue;
                }
                // T — tap all untapped lands to fill mana pool
                if (ev.key.code == sf::Keyboard::T && !ev.key.control) doTapAllMana();

                // A — alpha strike: all eligible creatures attack
                if (ev.key.code == sf::Keyboard::A &&
                    m_human.state() == HumanState::DeclareAttack)
                    doAlphaStrike();

                // E — end phase (alias for Space, useful single-hand)
                // (Only when NO card is selected — selected card's E = ability 0)
                if (ev.key.code == sf::Keyboard::E &&
                    m_browseSelect == kInvalidId)
                    doAiResponseThenPass();

                // Q/W/E with a battlefield card selected: activate that card's 0th/1st/2nd ability
                if (m_browseSelect != kInvalidId) {
                    int abilIdx = -1;
                    if (ev.key.code == sf::Keyboard::Q) abilIdx = 0;
                    else if (ev.key.code == sf::Keyboard::W) abilIdx = 1;
                    else if (ev.key.code == sf::Keyboard::E) abilIdx = 2;
                    if (abilIdx >= 0) {
                        m_abilities.activateAbility(m_browseSelect, abilIdx, 0, {});
                        continue;
                    }
                }

                // T key (Ctrl+T is tournament mode) — turn history
                if (ev.key.code == sf::Keyboard::T && !ev.key.control) {
                    m_showTurnHistory = !m_showTurnHistory; continue;
                }
                // L — life chart overlay (not Ctrl+L which loads)
                if (ev.key.code == sf::Keyboard::L && !ev.key.control) {
                    m_showLifeChart = !m_showLifeChart; continue;
                }
                // C — concede
                if (ev.key.code == sf::Keyboard::C && !ev.key.control) {
                    doConcede(); continue;
                }
                // Ctrl+P — load puzzle (looks for puzzle.json next to exe or in APPDATA)
            if (ev.key.code == sf::Keyboard::P && ev.key.control) {
                namespace fs = std::filesystem;
                fs::path puzzlePath;
                if (const char* ap = std::getenv("APPDATA"))
                    puzzlePath = fs::path(ap) / "CitadelMTG" / "puzzle.json";
                if (!puzzlePath.empty() && fs::exists(puzzlePath))
                    startPuzzleMode(puzzlePath);
                else
                    addLog("No puzzle.json found. Save a game state as puzzle.json.");
                continue;
            }
            // Ctrl+G — enter goldfish/draw-hand practice mode
            if (ev.key.code == sf::Keyboard::G && ev.key.control) {
                startGoldfishMode(); continue;
            }
            // In goldfish mode: R = new hand, Esc = exit
            if (m_goldfishMode) {
                if (ev.key.code == sf::Keyboard::R) {
                    ++m_goldfishDraws;
                    // Discard all hand cards back to library, then shuffle and redraw
                    {
                        std::vector<ObjectId> handIds;
                        for (const Card* c : m_game.player(0).hand().cards())
                            handIds.push_back(c->id);
                        for (ObjectId hid : handIds)
                            m_game.moveToZone(hid, ZoneType::Library, 0);
                    }
                    m_game.player(0).library().shuffle(m_game.rng());
                    for (int i = 0; i < 7; ++i) {
                        auto& lib = m_game.player(0).library();
                        if (lib.empty()) break;
                        m_game.moveToZone(lib.front()->id, ZoneType::Hand, 0);
                    }
                    addLog("Goldfish: Hand #" + std::to_string(m_goldfishDraws));
                    continue;
                }
                if (ev.key.code == sf::Keyboard::Escape) {
                    m_goldfishMode = false;
                    addLog("Exited goldfish mode.");
                    m_appState = AppState::MainMenu;
                    continue;
                }
            }
            // Ctrl+Shift+E — export replay as shareable JSON with metadata
                if (ev.key.code == sf::Keyboard::E && ev.key.control && ev.key.shift) {
                    namespace fs = std::filesystem;
                    if (const char* ap = std::getenv("APPDATA")) {
                        auto sharePath = fs::path(ap) / "CitadelMTG" / "shared_replay.json";
                        if (saveGame(sharePath)) {
                            // Append metadata to the replay
                            std::ifstream rin(sharePath);
                            std::string content((std::istreambuf_iterator<char>(rin)),
                                                 std::istreambuf_iterator<char>());
                            rin.close();
                            // Insert share metadata at the start
                            auto bracket = content.find('{');
                            if (bracket != std::string::npos) {
                                std::string meta = "\"shared_by\":\"Alice\","
                                                   "\"app_version\":2,"
                                                   "\"game_turns\":" +
                                                   std::to_string(m_stats.turnsPlayed) + ",";
                                content.insert(bracket + 1, meta);
                                std::ofstream wout(sharePath);
                                wout << content;
                            }
                            addLog("Shareable replay: " + sharePath.string());
                        }
                    }
                    continue;
                }
                // Ctrl+E — export game log to text file
                if (ev.key.code == sf::Keyboard::E && ev.key.control) {
                    namespace fs = std::filesystem;
                    if (const char* ap = std::getenv("APPDATA")) {
                        auto logPath = fs::path(ap) / "CitadelMTG" / "exported_log.txt";
                        std::ofstream lf(logPath);
                        for (const auto& line : m_gameLog) lf << line << '\n';
                        addLog("Log exported to exported_log.txt");
                    }
                    continue;
                }
                // + / = — faster animations
                if (ev.key.code == sf::Keyboard::Equal || ev.key.code == sf::Keyboard::Add) {
                    m_animSpeed = std::min(4.f, m_animSpeed + 0.5f);
                    addLog("Animation speed: " + std::to_string(static_cast<int>(m_animSpeed * 100)) + "%");
                    continue;
                }
                // - — slower animations
                if (ev.key.code == sf::Keyboard::Subtract || ev.key.code == sf::Keyboard::Hyphen) {
                    m_animSpeed = std::max(0.25f, m_animSpeed - 0.25f);
                    addLog("Animation speed: " + std::to_string(static_cast<int>(m_animSpeed * 100)) + "%");
                    continue;
                }
                // O — toggle options panel
                if (ev.key.code == sf::Keyboard::O) {
                    m_showOptions = !m_showOptions; continue;
                }
                // ? — toggle keybinding help
                if (ev.key.code == sf::Keyboard::Slash && ev.key.shift) {
                    m_showHelp = !m_showHelp; continue;
                }

                // 1–7 / Numpad1–7 — click the corresponding hand card
                {
                    int handIdx = -1;
                    auto kc = ev.key.code;
                    if (kc >= sf::Keyboard::Num1 && kc <= sf::Keyboard::Num7)
                        handIdx = static_cast<int>(kc - sf::Keyboard::Num1);
                    if (kc >= sf::Keyboard::Numpad1 && kc <= sf::Keyboard::Numpad7)
                        handIdx = static_cast<int>(kc - sf::Keyboard::Numpad1);
                    if (handIdx >= 0) {
                        const auto& hand = m_game.player(0).hand().cards();
                        if (handIdx < static_cast<int>(hand.size()))
                            m_human.onCardClick(hand[static_cast<size_t>(handIdx)]->id,
                                                ZoneType::Hand, 0);
                    }
                }
            }
        }

        // Right-click: zoom card; Ctrl+right-click: art-only zoom
        if (ev.type == sf::Event::MouseButtonPressed &&
            ev.mouseButton.button == sf::Mouse::Right) {
            auto mapped = mapMousePos(ev.mouseButton.x, ev.mouseButton.y);
            auto hit = m_renderer.hitTest(mapped.x, mapped.y);
            if (sf::Keyboard::isKeyPressed(sf::Keyboard::LControl) ||
                sf::Keyboard::isKeyPressed(sf::Keyboard::RControl)) {
                // Art-only zoom
                m_artZoomCardId = (m_artZoomCardId != kInvalidId) ? kInvalidId
                                : (hit.id != kInvalidId ? hit.id : kInvalidId);
                m_zoomCardId    = kInvalidId;
            } else {
                // Standard zoom with oracle text
                m_zoomCardId = (m_zoomCardId != kInvalidId) ? kInvalidId
                             : (hit.id != kInvalidId ? hit.id : kInvalidId);
                m_artZoomCardId = kInvalidId;
            }
            continue;
        }

        if (ev.type == sf::Event::MouseButtonPressed &&
            ev.mouseButton.button == sf::Mouse::Left) {

            auto mapped = mapMousePos(ev.mouseButton.x, ev.mouseButton.y);
            float px = mapped.x, py = mapped.y;

            // Dismiss zoom on left-click too
            if (m_zoomCardId != kInvalidId) { m_zoomCardId = kInvalidId; }

            // Game-over overlay: click "Play Again" or "Main Menu"
            if (m_tm.isGameOver()) {
                const float panW = 520.f, panH = 280.f;
                const float panX = (WIN_W - panW) * 0.5f;
                const float panY = (WIN_H - panH) * 0.5f;
                const float btnW = 200.f, btnH = 44.f, btnGap = 20.f;
                const float btn0X = panX + (panW - 2 * btnW - btnGap) * 0.5f;
                const float btnY  = panY + panH - btnH - 24.f;
                if (py >= btnY && py <= btnY + btnH) {
                    if (px >= btn0X && px <= btn0X + btnW) { doRematch(); return; }
                    float btn1X = btn0X + btnW + btnGap;
                    if (px >= btn1X && px <= btn1X + btnW) { m_appState = AppState::MainMenu; return; }
                }
                // Load Replay button (row below the main buttons)
                {
                    constexpr float repW = 115.f, repH = 22.f, repGap = 8.f;
                    float repTotalW = repW * 2 + repGap;
                    float repX0 = panX + (panW - repTotalW) * 0.5f;
                    float repY  = btnY + btnH + 8.f;
                    // "Load Replay" is the right button
                    float repX1 = repX0 + repW + repGap;
                    if (px >= repX1 && px <= repX1 + repW && py >= repY && py <= repY + repH) {
                        namespace fs = std::filesystem;
                        if (const char* ap = std::getenv("APPDATA")) {
                            auto replayPath = fs::path(ap) / "CitadelMTG" / "last_game_replay.json";
                            if (fs::exists(replayPath)) {
                                loadGame(replayPath);
                                m_replayMode = true;
                                m_replayIdx  = 0;
                                addLog("Replay loaded. N = next autosave, P = prev, Esc = exit replay.");
                            } else {
                                addLog("No replay found (save one with Ctrl+R).");
                            }
                        }
                        continue;
                    }

                    // Replay step buttons (N = next turn, P = prev turn)
                    if (m_replayMode) {
                        if (ev.key.code == sf::Keyboard::N) {
                            namespace fs = std::filesystem;
                            if (const char* ap = std::getenv("APPDATA")) {
                                auto slot = fs::path(ap) / "CitadelMTG" / "autosave.json";
                                if (fs::exists(slot)) { loadGame(slot); addLog("Replay: loaded autosave"); }
                            }
                        }
                        if (ev.key.code == sf::Keyboard::Escape) {
                            m_replayMode = false;
                            addLog("Exited replay mode.");
                        }
                    }
                }
                continue;
            }

            // Options panel clicks
            if (m_showOptions) {
                using namespace Layout;
                constexpr float pw = 400.f, ph = 480.f;
                const float px0 = (WIN_W - pw) * 0.5f;
                const float py0 = (WIN_H - ph) * 0.5f;

                // Fullscreen / colour-blind buttons
                constexpr float btnW = 150.f, btnH = 28.f;
                float bx  = px0 + 14.f,          by = py0 + 50.f;
                float bx2 = px0 + pw - 14.f - btnW;
                if (px >= bx  && px <= bx  + btnW && py >= by && py <= by + btnH) toggleFullscreen();
                if (px >= bx2 && px <= bx2 + btnW && py >= by && py <= by + btnH) m_colorBlindMode = !m_colorBlindMode;

                // Phase stop toggle rows
                {
                    float ry = by + btnH + 38.f;
                    bool* stops[13] = {
                        &m_stops.untap,&m_stops.upkeep,&m_stops.draw,
                        &m_stops.main1,&m_stops.beginCombat,&m_stops.attackers,
                        &m_stops.blockers,&m_stops.firstStrike,&m_stops.combatDmg,
                        &m_stops.endCombat,&m_stops.main2,&m_stops.endStep,&m_stops.cleanup
                    };
                    for (int i = 0; i < 13; ++i) {
                        float col  = (i < 7) ? px0 + 16.f : px0 + pw * 0.5f + 6.f;
                        float rowY = ry + (i % 7) * 22.f;
                        if (px >= col && px <= col + 130.f &&
                            py >= rowY && py <= rowY + 18.f) {
                            *stops[i] = !*stops[i];
                            m_human.setPhaseStops(m_stops);
                            saveSettings();
                        }
                    }
                    // Volume slider
                    float sliderY = ry + 7 * 22.f + 12.f;
                    constexpr float slW = pw - 32.f;
                    float slX = px0 + 16.f;
                    if (py >= sliderY - 5.f && py <= sliderY + 15.f
                        && px >= slX && px <= slX + slW) {
                        float newVol = std::clamp((px - slX) / slW * 100.f, 0.f, 100.f);
                        m_sound.setVolume(newVol);
                    }
                    // Mute toggle
                    float muteX = slX + slW + 8.f;
                    if (px >= muteX && px <= muteX + 32.f
                        && py >= sliderY - 2.f && py <= sliderY + 14.f)
                        m_sound.setMuted(!m_sound.muted());

                    // Priority chime toggle (row below the slider)
                    float chimeY = sliderY + 24.f;
                    float chBtnX = slX + 92.f;
                    if (px >= chBtnX && px <= chBtnX + 40.f
                        && py >= chimeY - 1.f && py <= chimeY + 15.f) {
                        m_priorityChime = !m_priorityChime;
                        if (m_priorityChime) m_sound.play(SND_PRIORITY);  // preview
                        saveSettings();
                    }
                }

                // Resolution buttons
                {
                    float ry = py0 + ph - 54.f;
                    static const struct { unsigned w, h; } kRes[] = {
                        {1280, 720}, {1560, 800}, {1920, 1080}
                    };
                    for (int ri = 0; ri < 3; ++ri) {
                        float rbx = px0 + 14.f + ri * 110.f;
                        if (px >= rbx && px <= rbx + 105.f && py >= ry && py <= ry + 22.f) {
                            // Resize window to selected resolution
                            m_window.setSize({kRes[ri].w, kRes[ri].h});
                            updateView();
                        }
                    }
                }
                continue;  // don't process board actions while options are open
            }

            // Split card choice overlay
            if (m_splitChoice.active) {
                using namespace Layout;
                const auto* card = m_game.findCard(m_splitChoice.cardId);
                if (card && card->rules) {
                    constexpr float panW = 520.f, panH = 180.f;
                    float px0 = (WIN_W - panW) * 0.5f, py0 = (WIN_H - panH) * 0.5f;
                    constexpr float btnW = 220.f, btnH = 60.f, gap = 16.f;
                    float bx0 = px0 + (panW - 2 * btnW - gap) * 0.5f, by0 = py0 + 50.f;

                    // Left half (normal card)
                    if (px >= bx0 && px <= bx0 + btnW && py >= by0 && py <= by0 + btnH) {
                        m_splitChoice.active = false;
                        snapshotUndo();
                        m_human.onCardClick(m_splitChoice.cardId, ZoneType::Hand, 0);
                        m_drag = { m_splitChoice.cardId, true };
                    }
                    // Right half (split) — cast with splitCost override
                    float bx1 = bx0 + btnW + gap;
                    if (px >= bx1 && px <= bx1 + btnW && py >= by0 && py <= by0 + btnH) {
                        m_splitChoice.active = false;
                        // Cast right half: temporarily swap rules to splitCost
                        // For simplicity: change manaCost to splitCost in a temporary rules
                        // The engine will resolve it using the normal castSpell path
                        // (Full per-half scripting requires a more complex approach)
                        snapshotUndo();
                        m_human.onCardClick(m_splitChoice.cardId, ZoneType::Hand, 0);
                    }
                } else {
                    m_splitChoice.active = false;
                }
                continue;
            }

            // Zone browser: close on click-outside; activate own GY/exile
            // cards via double-tap (click again on an already-zoomed card)
            // so single clicks still just zoom for inspection.
            if (m_zoneBrowseActive) {
                RenderHints tmpH; tmpH.showZoneBrowse = true;
                tmpH.zoneBrowsePlayer = m_zoneBrowsePlayer;
                tmpH.zoneBrowseZone = m_zoneBrowseZone;
                if (m_renderer.hitZoneBrowserClose(px, py, tmpH)) {
                    m_zoneBrowseActive = false;
                } else {
                    ObjectId clicked = m_renderer.hitZoneBrowserCard(px, py, tmpH);
                    if (clicked != kInvalidId) {
                        // Click your own browser entry → try to activate it.
                        //   GY: flashback / escape / unearth / jump-start /
                        //       disturb / aftermath / embalm / eternalize /
                        //       scavenge.
                        //   Exile: foretold cards on a turn after the one
                        //       they were foretold.
                        // HumanController dispatches to the right path; the
                        // engine's castSpell / activate{Embalm,Eternalize,
                        // Scavenge} auto-pick the alt-cost.
                        if (m_zoneBrowsePlayer == 0 &&
                            m_zoneBrowseZone != ui::BrowseZone::Library &&
                            m_turn == WhosTurn::Human &&
                            m_human.state() == HumanState::MainPhase) {
                            ZoneType targetZone = (m_zoneBrowseZone == ui::BrowseZone::Exile)
                                ? ZoneType::Exile : ZoneType::Graveyard;
                            snapshotUndo();
                            if (m_human.onCardClick(clicked, targetZone, 0)) {
                                m_zoneBrowseActive = false;  // close, popup opens
                                continue;
                            }
                        }
                        // Otherwise: zoom only (opponent's zone, inspecting
                        // cards out of phase, or no usable mechanic).
                        m_zoomCardId = clicked;
                    }
                }
                continue;  // don't process board clicks while browser is open
            }

            // Phase tracker row click — toggle stop for that step
            {
                int row = m_renderer.hitPhaseRow(px, py);
                if (row >= 0) {
                    bool* stops[13] = {
                        &m_stops.untap, &m_stops.upkeep, &m_stops.draw,
                        &m_stops.main1, &m_stops.beginCombat, &m_stops.attackers,
                        &m_stops.blockers, &m_stops.firstStrike, &m_stops.combatDmg,
                        &m_stops.endCombat, &m_stops.main2, &m_stops.endStep,
                        &m_stops.cleanup
                    };
                    *stops[row] = !*stops[row];
                    m_human.setPhaseStops(m_stops);
                    saveSettings();
                    continue;
                }
            }

            // Riot choice overlay takes top priority
            // Miracle: cast the just-drawn card at its miracle cost?
            if (m_game.hasPendingMiracle()) {
                int btn = hitChoiceButton(px, py, 2);
                const auto& mc = m_game.pendingMiracle();
                if (btn == 0) {  // Cast at miracle cost
                    const Card* miCard = m_game.findCard(mc.cardId);
                    if (miCard && miCard->miracleEligible) {
                        m_game.clearPendingMiracle();
                        // Override to use miracle cost
                        snapshotUndo();
                        m_human.castMiracleCard(mc.cardId);
                    } else {
                        m_game.clearPendingMiracle();
                    }
                } else if (btn == 1) {  // Skip miracle
                    const Card* miCard = m_game.findCard(mc.cardId);
                    if (miCard) const_cast<Card*>(miCard)->miracleEligible = false;
                    m_game.clearPendingMiracle();
                }
                continue;
            }
            // Phyrexian mana payment choice
            if (m_game.hasPendingPhyrexian()) {
                int btn = hitChoiceButton(px, py, 2);
                const auto& pc = m_game.pendingPhyrexian();
                if (btn == 0) {
                    // Pay mana: clear the pending state and advance to targeting
                    m_game.clearPendingPhyrexian();
                    m_human.advanceAfterPhyrexianChoice(false);
                } else if (btn == 1) {
                    // Pay 2 life per shard
                    for (int i = 0; i < pc.numShards; ++i)
                        m_game.loseLife(0, 2);
                    m_game.clearPendingPhyrexian();
                    m_human.advanceAfterPhyrexianChoice(true);
                }
                continue;
            }
            // Cascade choice: cast the revealed card for free, or skip
            if (m_game.hasPendingCascade()) {
                int btn = hitChoiceButton(px, py, 2);
                if (btn == 0) { // Cast it
                    auto& cc = m_game.pendingCascade();
                    ObjectId cid = cc.cardId;
                    m_game.clearPendingCascade();
                    m_abilities.castSpell(cid, 0, {});
                    m_abilities.drainPendingTriggers();
                } else if (btn == 1) { // Skip
                    // Put the card on the bottom of library
                    Card* c = m_game.findCard(m_game.pendingCascade().cardId);
                    if (c) m_game.moveToZone(c->id, ZoneType::Library, 0);
                    m_game.clearPendingCascade();
                }
                continue;
            }
            if (m_game.hasPendingRiot()) {
                int btn = hitChoiceButton(px, py, 2);
                if (btn == 0) completeRiotChoice(true);
                else if (btn == 1) completeRiotChoice(false);
                continue;
            }
            // Pay-life-or-tapped overlay (shock lands)
            if (m_game.hasPendingPayLife()) {
                int btn = hitChoiceButton(px, py, 2);
                if (btn == 0) completePayLifeChoice(true);
                else if (btn == 1) completePayLifeChoice(false);
                continue;
            }
            // Fabricate choice overlay
            if (m_game.hasPendingFabricate()) {
                int btn = hitChoiceButton(px, py, 2);
                if (btn == 0) completeFabricateChoice(false); // Servo tokens
                else if (btn == 1) completeFabricateChoice(true); // +1/+1 counters
                continue;
            }
            // Madness cast-or-GY overlay
            if (m_game.hasPendingMadnessCast()) {
                int btn = hitChoiceButton(px, py, 2);
                if (btn == 0) completeMadnessCast(true);  // Cast for madness cost
                else if (btn == 1) completeMadnessCast(false); // Put in graveyard
                continue;
            }
            // Charm mode overlay takes top priority
            if (m_game.hasPendingCharm()) {
                int n = static_cast<int>(m_game.pendingCharm().labels.size());
                int btn = hitChoiceButton(px, py, n);
                if (btn >= 0) completeCharmChoice(btn);
                continue;
            }
            // Library search overlay takes priority over all other clicks
            if (m_game.hasPendingSearch()) {
                ObjectId chosen = m_renderer.hitSearchChoice(px, py,
                    [&]{ RenderHints h; h.showLibrarySearch = true;
                         h.searchChoices = m_searchChoices; return h; }());
                if (chosen != kInvalidId) completePendingSearch(chosen);
                continue;
            }
            // Pending Connive discard — click a hand card to discard it
            if (m_game.hasPendingConnive()) {
                auto hit = m_renderer.hitTest(px, py);
                if (hit.id != kInvalidId && hit.zone == ZoneType::Hand && hit.player == 0) {
                    auto& pc = m_game.pendingConnive;
                    const Card* c = m_game.findCard(hit.id);
                    if (c) {
                        bool nonland = !c->rules->type.isLand();
                        m_game.moveToZone(hit.id, ZoneType::Graveyard, 0);
                        m_abilities.drainPendingTriggers();
                        if (nonland) {
                            Card* creature = m_game.findCard(pc.creatureId);
                            if (creature) creature->addCounter("+1/+1", 1);
                        }
                        ++pc.discardsDone;
                        if (pc.discardsDone >= pc.amount) pc.active = false;
                    }
                }
                continue;
            }
            // Pending forced discard — human must click a hand card
            if (m_game.hasPendingDiscard()) {
                auto hit = m_renderer.hitTest(px, py);
                if (hit.id != kInvalidId && hit.zone == ZoneType::Hand && hit.player == 0)
                    completeDiscardChoice(hit.id);
                continue;
            }
            // Planeswalker ability choice overlay
            if (m_human.pendingPlaneswalker() != kInvalidId) {
                int n   = static_cast<int>(m_human.pwAbilIdxs().size());
                int btn = hitChoiceButton(px, py, n);
                if (btn >= 0) m_human.activatePWAbility(btn);
                else          m_human.cancelPWChoice();
                continue;
            }

            if ((m_turn == WhosTurn::Human || m_human.needsInput()) &&
                !m_human.isGameOver()) {
                // Dock buttons (bottom row of play area)
                auto dock = m_renderer.hitDock(px, py);
                if (dock != BoardRenderer::DockBtn::None) {
                    switch (dock) {
                        case BoardRenderer::DockBtn::EndPhase:
                        case BoardRenderer::DockBtn::PassPriority:
                            doAiResponseThenPass(); break;
                        case BoardRenderer::DockBtn::AlphaStrike:
                            doAlphaStrike(); break;
                        case BoardRenderer::DockBtn::Concede:
                            doConcede(); break;
                        default: break;
                    }
                    continue;
                }

                // Sidebar prompt buttons
                auto prompt = m_renderer.hitPrompt(px, py);
                if (prompt == BoardRenderer::PromptBtn::Ok) {
                    m_human.onConfirm(); continue;
                }
                if (prompt == BoardRenderer::PromptBtn::Cancel) {
                    doAiResponseThenPass(); continue;
                }

                // Exploit choice: click a creature to sacrifice, or Skip
            if (m_game.hasPendingExploit()) {
                const auto& ec = m_game.pendingExploit();
                auto hit = m_renderer.hitTest(px, py);
                if (hit.id != kInvalidId && hit.zone == ZoneType::Battlefield) {
                    Card* clicked = m_game.findCard(hit.id);
                    if (clicked && clicked->controllerId == ec.controller &&
                        clicked->isCreature() && clicked->id != ec.exploiterId) {
                        Card* exploiter = m_game.findCard(ec.exploiterId);
                        m_game.moveToZone(clicked->id, ZoneType::Graveyard, ec.controller);
                        if (exploiter) exploiter->exploited = true;
                        m_game.clearPendingExploit();
                        continue;
                    }
                }
                int btn = hitChoiceButton(px, py, 2);
                if (btn == 1)   // Skip
                    m_game.clearPendingExploit();
                continue;
            }

                // Tribute choice: pay counters to prevent the "not paid" bonus?
            if (m_game.hasPendingTribute()) {
                int btn = hitChoiceButton(px, py, 2);
                const auto& tc = m_game.pendingTribute();
                if (btn == 0) {  // Pay tribute (put counters)
                    Card* tCard = m_game.findCard(tc.cardId);
                    if (tCard) {
                        tCard->addCounter("+1/+1", tc.amount);
                        tCard->tributed = true;
                    }
                    m_game.clearPendingTribute();
                } else if (btn == 1) {  // Don't pay (creature's bonus fires)
                    Card* tCard = m_game.findCard(tc.cardId);
                    if (tCard) tCard->tributed = false;
                    m_game.clearPendingTribute();
                }
                continue;
            }
            // Proliferate selection: click permanents with counters to toggle
                if (m_game.hasPendingProliferate()) {
                    auto hit2 = m_renderer.hitTest(px, py);
                    if (hit2.id != kInvalidId) {
                        const Card* pc = m_game.findCard(hit2.id);
                        if (pc && !pc->counters.empty()) {
                            auto& pp = m_game.pendingProliferate();
                            auto it = std::find(pp.selectedIds.begin(), pp.selectedIds.end(), hit2.id);
                            if (it == pp.selectedIds.end()) pp.selectedIds.push_back(hit2.id);
                            else pp.selectedIds.erase(it);
                        }
                    }
                    continue;  // don't process as normal click
                }

                // Click the gravestone / exile gate icon in either player's
                // info bar → open that player's GY / Exile browser. Must
                // come BEFORE the card hit-test so the icon wins over any
                // card stacked underneath the bar.
                {
                    auto zh = m_renderer.hitInfoBarZone(px, py);
                    if (zh.player >= 0) {
                        if (m_zoneBrowseActive &&
                            m_zoneBrowsePlayer == zh.player &&
                            m_zoneBrowseZone   == zh.zone) {
                            // Clicking the same icon a second time closes it.
                            m_zoneBrowseActive = false;
                        } else {
                            m_zoneBrowseActive  = true;
                            m_zoneBrowsePlayer  = static_cast<uint8_t>(zh.player);
                            m_zoneBrowseZone    = zh.zone;
                            m_renderer.invalidateZoneBrowserCache();
                        }
                        continue;
                    }
                }

                // GY / Exile info-bar icons → open that player's zone browser
                // (works for any player/AI; click the same icon again or the
                // backdrop to close).
                {
                    auto zi = m_renderer.hitInfoBarZone(px, py);
                    if (zi.player >= 0) {
                        bool same = m_zoneBrowseActive &&
                                    m_zoneBrowsePlayer == static_cast<uint8_t>(zi.player) &&
                                    m_zoneBrowseZone   == zi.zone;
                        m_zoneBrowseActive  = !same;
                        m_zoneBrowsePlayer  = static_cast<uint8_t>(zi.player);
                        m_zoneBrowseZone    = zi.zone;
                        m_renderer.invalidateZoneBrowserCache();
                        continue;
                    }
                }

                // Card / player hit-test
                // Clicking a player's info bar: targets that player for a spell
                // (TargetSelect) or chooses which opponent to attack (DeclareAttack).
                if (m_human.state() == HumanState::TargetSelect ||
                    m_human.state() == HumanState::DeclareAttack) {
                    int tp = m_renderer.hitPlayerArea(px, py);
                    if (tp >= 0) {
                        if (m_human.state() == HumanState::TargetSelect) snapshotUndo();
                        if (m_human.onPlayerClick(static_cast<uint8_t>(tp))) continue;
                    }
                }

                auto hit = m_renderer.hitTest(px, py);
                if (hit.id != kInvalidId) {
                    m_browseSelect = hit.id;

                    // Tap-undo: clicking a tapped own land in main phase
                    // un-taps it IF the most recent undo snapshot has it
                    // untapped (i.e. the tap was the most recent action and
                    // nothing else has happened since). This gives an
                    // discoverable "click to undo" affordance for misclicks.
                    if (hit.player == 0 && hit.zone == ZoneType::Battlefield &&
                        m_human.state() == HumanState::MainPhase &&
                        m_undoSize > 0) {
                        const Card* clk = m_game.findCard(hit.id);
                        if (clk && clk->rules && clk->tapped && clk->isLand()) {
                            // Capture the name NOW — doUndo() move-assigns
                            // m_game, which destroys the old m_objects map
                            // and invalidates this pointer. Logging through
                            // a dangling clk after doUndo crashes the game.
                            std::string nameCopy = clk->name();
                            auto idx = (m_undoHead + kUndoDepth - 1) % kUndoDepth;
                            const auto& slot = m_undoStack[idx];
                            if (slot.valid) {
                                const Card* prev = slot.game.findCard(hit.id);
                                if (prev && !prev->tapped) {
                                    doUndo();
                                    addLog("Untapped " + nameCopy + ".");
                                    continue;
                                }
                            }
                        }
                    }

                    // Snapshot for undo BEFORE the click mutates state. Hand
                    // clicks already snapshot inside the cast popup paths, but
                    // battlefield clicks (tap-for-mana, activate ability,
                    // attack toggle) had no snapshot — undoing after tapping
                    // a land was a no-op.
                    if (hit.player == 0 &&
                        (hit.zone == ZoneType::Battlefield ||
                         hit.zone == ZoneType::Graveyard))
                        snapshotUndo();
                    m_human.onCardClick(hit.id, hit.zone, hit.player);
                } else {
                    m_browseSelect = kInvalidId;
                    if (py >= BOB_INFO_Y && py < BOB_INFO_Y + BOB_INFO_H &&
                        px >= PLAY_X && px < PLAY_X + PLAY_W)
                        m_human.onPlayerClick(1);
                }
            }
        }
    }
}

void GameWindow::update() {
    // Advance visual animations each frame (scaled by animation speed setting)
    {
        float dt = m_animClock.restart().asSeconds();
        if (dt > 0.1f) dt = 0.1f;
        m_renderer.update(dt * m_animSpeed);
    }

    // Flush any textures that finished loading on the background thread
    ui::TextureCache::flushPending();

    // ── Morph reveal logging ─────────────────────────────────────────────────
    {
        auto revealed = m_game.drainPendingReveal();
        if (!revealed.empty())
            addLog("Revealed face-down card: " + revealed);
    }

    // ── Life history tracking ─────────────────────────────────────────────────
    {
        static int s_lastRecordedTurn = -1;
        int cur = m_game.turnNumber();
        if (cur != s_lastRecordedTurn && cur > 0) {
            std::array<int,2> entry = {m_game.player(0).life(), m_game.player(1).life()};
            m_lifeHistory.push_back(entry);
            s_lastRecordedTurn = cur;
        }
    }

    // ── Auto-save every N turns ──────────────────────────────────────────────
    if (m_autoSaveInterval > 0 && !m_tm.isGameOver()) {
        int curTurn = m_game.turnNumber();
        if (curTurn - m_turnAtLastAutoSave >= m_autoSaveInterval) {
            namespace fs = std::filesystem;
            if (const char* ap = std::getenv("APPDATA")) {
                saveGame(fs::path(ap) / "CitadelMTG" / "autosave.json");
                m_turnAtLastAutoSave = curTurn;
            }
        }
    }

    // ── Toast timer ───────────────────────────────────────────────────────────
    if (m_toast.ttl > 0.f) {
        float dt2 = m_animClock.getElapsedTime().asSeconds();
        m_toast.ttl = std::max(0.f, m_toast.ttl - dt2 * 0.5f); // rough decay
    }

    // ── Per-game statistics sampling ──────────────────────────────────────────
    // Track life lost (damage dealt) and cards drawn each turn.
    {
        static int s_lastLife[2] = {-1, -1};
        for (int pid = 0; pid < 2; ++pid) {
            int cur = m_game.player(pid).life();
            if (s_lastLife[pid] >= 0 && cur < s_lastLife[pid])
                m_stats.damageDealt[pid] += (s_lastLife[pid] - cur);
            s_lastLife[pid] = cur;
        }
        // Turns: turnNumber increments each time a turn changes
        m_stats.turnsPlayed = m_game.turnNumber();
        // Cards drawn: summed from per-turn counter (approximate — resets each cleanup)
        for (int pid = 0; pid < 2; ++pid)
            m_stats.cardsDrawn[pid] = std::max(m_stats.cardsDrawn[pid],
                                               m_game.cardsDrawnThisTurn[pid]);
        // Spells cast: accumulated from per-turn counter
        m_stats.spellsCast[0] = std::max(m_stats.spellsCast[0],
                                         m_game.spellsCastByPlayer[0]);
    }

    // ── Sound: detect card zone-change events for audio feedback ─────────────
    // Compare current hand size to last frame to detect draw events.
    {
        static int s_lastHandSize = -1;
        int curHand = static_cast<int>(m_game.player(0).hand().size());
        if (s_lastHandSize >= 0 && curHand > s_lastHandSize)
            m_sound.play(SND_DRAW_CARD);
        s_lastHandSize = curHand;

        // Detect creature ETBs / deaths
        static int s_lastBfSize = 0;
        int curBf = static_cast<int>(m_game.battlefield().size());
        if (curBf > s_lastBfSize) m_sound.play(SND_CREATURE_ETB);
        else if (curBf < s_lastBfSize) m_sound.play(SND_CREATURE_DIES);
        s_lastBfSize = curBf;
    }

    if (m_tm.isGameOver()) return;

    if (m_turn == WhosTurn::Human && !m_human.isGameOver())
        m_human.checkAndHandleTriggers();

    if (m_turn == WhosTurn::Human) {
        if (m_human.state() == HumanState::Idle) m_turn = WhosTurn::AI;
        if (m_human.isGameOver()) return;
    }

    if (m_turn == WhosTurn::AI && !m_aiRunning) {
        m_aiRunning = true;
        // Render one "Bob is thinking…" frame BEFORE blocking so the player
        // gets visual feedback that the AI is working.
        render();

        // Write checkpoint to persistent log so crash site is identifiable.
        persistLog("[AI] T=" + std::to_string(m_game.turnNumber()) +
                   " P0=" + std::to_string(m_game.player(0).life()) +
                   "hp P1=" + std::to_string(m_game.player(1).life()) + "hp");
#ifdef _WIN32
        int sehCode = runAiTurnSafe(this);
        if (sehCode != 0) {
            std::cerr << "AI turn SEH crash code=0x"
                      << std::hex << static_cast<unsigned>(sehCode)
                      << std::dec << " — forfeiting game.\n";
            addLog("AI crashed — game over.");
            m_game.player(1).lose();
        }
#else
        runAiTurn();
#endif
        m_aiRunning = false;

        if (!m_tm.isGameOver()) {
            m_turn = WhosTurn::Human;
            addLog("Turn " + std::to_string(m_game.turnNumber()) + ": Alice");
            m_human.setPhaseStops(m_stops);
            m_human.startHumanTurn();
        } else {
            recordGameResult(m_tm.winnerId());
            recordGameCards();
            if (m_tournamentMode) checkTournamentEnd();
        }
    }
}

// ── Priority window (instants / responses during AI turn) ─────────────────────

void GameWindow::humanPriorityWindow() {
    if (!m_window.isOpen()) return;
    // If the player quit to the menu (via the pause modal), don't prompt again —
    // let the turn flow unwind quickly back to run().
    if (m_appState != AppState::Playing) return;
    m_human.beginResponsePhase();

    // Helper: true when any overlay state requires human interaction before passing.
    auto anyPending = [&] {
        return m_game.hasPendingDiscard()       || m_game.hasPendingCharm()          ||
               m_game.hasPendingSearch()        || m_game.hasPendingRiot()           ||
               m_game.hasPendingPayLife()       ||
               m_game.hasPendingFabricate()     || m_game.hasPendingMadnessCast()    ||
               m_game.hasPendingCascade()       || m_game.hasPendingPhyrexian()      ||
               m_game.hasPendingMiracle()       || m_game.hasPendingProliferate()    ||
               m_game.hasPendingExploit()       ||
               m_game.hasPendingTribute()       ||
               m_game.hasPendingManaChoice()    || m_game.hasPendingScry()           ||
               m_game.hasPendingChooseType()    ||
               m_abilities.hasPendingHumanTrigger() ||
               m_human.pendingPlaneswalker() != kInvalidId;
    };

    // Helper: attempt to pass priority — resolves one stack item, or exits the window.
    auto tryPass = [&]() -> bool /* true = window should close */ {
        if (!m_abilities.stackEmpty()) {
            m_abilities.resolveTop();
            while (StateBasedActions::run(m_game)) {}
            m_abilities.drainPendingTriggers();
            if (m_game.hasPendingDiscard()) handlePendingDiscard();
            if (!m_window.isOpen() || m_tm.isGameOver()) return true;
            // If stack now empty and no pending overlays, close the window.
            if (m_abilities.stackEmpty() && !anyPending()) {
                m_human.endResponsePhase();
                return true;
            }
            return false;
        }
        if (!anyPending()) {
            m_human.endResponsePhase();
            return true;
        }
        return false;
    };

    // Soft chime to alert the player that they hold priority and may act — only
    // when there's an actual decision (stack item or a pending choice), so it
    // doesn't fire on windows that immediately auto-pass. Toggleable in Options.
    if (m_priorityChime && (!m_abilities.stackEmpty() || anyPending()))
        m_sound.play(SND_PRIORITY);

    while (m_window.isOpen() && m_human.inResponseMode()) {
        sf::Event ev;
        while (m_window.pollEvent(ev)) {
            if (ev.type == sf::Event::Closed)  { m_window.close(); return; }
            if (ev.type == sf::Event::Resized) { updateView(); }
            if (ev.type == sf::Event::MouseMoved)
                m_mousePos = mapMousePos(ev.mouseMove.x, ev.mouseMove.y);

            if (ev.type == sf::Event::KeyPressed) {
                if (ev.key.code == sf::Keyboard::Escape) {
                    // Open the pause menu (don't close the window — that crashed).
                    if (pauseMenuModal()) return;   // Concede/Quit → unwind
                    continue;                        // Resume → keep responding
                }
                if (ev.key.code == sf::Keyboard::F11)    { toggleFullscreen(); continue; }
                // Ctrl+Z undo also works during opponent's priority window
                if (ev.key.code == sf::Keyboard::Z && ev.key.control) {
                    doUndo(); continue;
                }
                // Up/Down: reorder pending human triggers (choose order)
                if (m_abilities.pendingHumanTriggerCount() > 1) {
                    if (ev.key.code == sf::Keyboard::Up) {
                        m_abilities.reorderHumanTrigger(0, 1);
                        addLog("Trigger reordered (Up to top).");
                        continue;
                    }
                    if (ev.key.code == sf::Keyboard::Down) {
                        int n = m_abilities.pendingHumanTriggerCount();
                        m_abilities.reorderHumanTrigger(0, n - 1);
                        addLog("Trigger moved to bottom.");
                        continue;
                    }
                }
                if (ev.key.code == sf::Keyboard::Space ||
                    ev.key.code == sf::Keyboard::Return) {
                    if (tryPass()) break; else continue;
                }
            }

            if (ev.type == sf::Event::MouseButtonPressed &&
                ev.mouseButton.button == sf::Mouse::Left) {
                auto mapped = mapMousePos(ev.mouseButton.x, ev.mouseButton.y);
                float px = mapped.x, py = mapped.y;

                if (m_game.hasPendingManaChoice()) {
                    for (const auto& h : m_manaChoiceHits)
                        if (h.rect.contains(mapped)) { completeManaChoice(h.color); break; }
                    continue;
                }
                if (m_game.hasPendingChooseType()) {
                    for (const auto& h : m_chooseTypeHits)
                        if (h.rect.contains(mapped)) { completeChooseType(h.type); break; }
                    continue;
                }
                if (m_game.hasPendingScry()) {
                    if (m_scryKeepRect.contains(mapped))        completeScryChoice(true);
                    else if (m_scryBottomRect.contains(mapped)) completeScryChoice(false);
                    continue;
                }
                if (m_game.hasPendingCascade()) {
                    int btn = hitChoiceButton(px, py, 2);
                    if (btn == 0) { // Cast it
                        ObjectId cid = m_game.pendingCascade().cardId;
                        m_game.clearPendingCascade();
                        m_abilities.castSpell(cid, 0, {});
                        m_abilities.drainPendingTriggers();
                    } else if (btn == 1) {
                        Card* c = m_game.findCard(m_game.pendingCascade().cardId);
                        if (c) m_game.moveToZone(c->id, ZoneType::Library, 0);
                        m_game.clearPendingCascade();
                    }
                    continue;
                }
                if (m_game.hasPendingRiot()) {
                    int btn = hitChoiceButton(px, py, 2);
                    if (btn == 0) completeRiotChoice(true);
                    else if (btn == 1) completeRiotChoice(false);
                    continue;
                }
                if (m_game.hasPendingPayLife()) {
                    int btn = hitChoiceButton(px, py, 2);
                    if (btn == 0) completePayLifeChoice(true);
                    else if (btn == 1) completePayLifeChoice(false);
                    continue;
                }
                if (m_game.hasPendingFabricate()) {
                    int btn = hitChoiceButton(px, py, 2);
                    if (btn == 0) completeFabricateChoice(false); // Servo tokens
                    else if (btn == 1) completeFabricateChoice(true); // +1/+1 counters
                    continue;
                }
                if (m_game.hasPendingMadnessCast()) {
                    int btn = hitChoiceButton(px, py, 2);
                    if (btn == 0) completeMadnessCast(true);
                    else if (btn == 1) completeMadnessCast(false);
                    continue;
                }
                if (m_game.hasPendingCharm()) {
                    int n = static_cast<int>(m_game.pendingCharm().labels.size());
                    int btn = hitChoiceButton(px, py, n);
                    if (btn >= 0) completeCharmChoice(btn);
                    continue;
                }
                if (m_game.hasPendingSearch()) {
                    ObjectId chosen = m_renderer.hitSearchChoice(px, py,
                        [&]{ RenderHints h; h.showLibrarySearch = true;
                             h.searchChoices = m_searchChoices; return h; }());
                    if (chosen != kInvalidId) completePendingSearch(chosen);
                    continue;
                }
                if (m_game.hasPendingDiscard()) {
                    auto hit = m_renderer.hitTest(px, py);
                    if (hit.id != kInvalidId && hit.zone == ZoneType::Hand && hit.player == 0)
                        completeDiscardChoice(hit.id);
                    continue;
                }
                if (m_human.pendingPlaneswalker() != kInvalidId) {
                    int n   = static_cast<int>(m_human.pwAbilIdxs().size());
                    int btn = hitChoiceButton(px, py, n);
                    if (btn >= 0) m_human.activatePWAbility(btn);
                    else          m_human.cancelPWChoice();
                    continue;
                }

                // GY/Exile browsing is handled precisely above via
                // hitInfoBarZone() (the actual drawn icon rects). The old crude
                // x-position heuristic here opened the WRONG zone (e.g. clicking
                // graveyard opened exile) and has been removed.

                auto dock = m_renderer.hitDock(px, py);
                if (dock == BoardRenderer::DockBtn::EndPhase ||
                    dock == BoardRenderer::DockBtn::PassPriority) {
                    if (tryPass()) break; else continue;
                }

                // ── Sidebar prompt buttons ──────────────────────────────────
                // The sidebar "Pass / End Phase" and "Confirm [Enter]" buttons
                // need to work during the opponent's priority window too —
                // otherwise the player can only pass with Space/Enter and
                // clicking the visible button silently does nothing.
                auto prompt = m_renderer.hitPrompt(px, py);
                if (prompt == BoardRenderer::PromptBtn::Cancel) {
                    if (tryPass()) break; else continue;
                }
                if (prompt == BoardRenderer::PromptBtn::Ok) {
                    // If a hand spell is queued, commit it; otherwise treat as
                    // pass (no separate "confirm" action exists during the AI's
                    // turn — the only thing to confirm is a pending cast).
                    if (m_human.pendingSpellId() != kInvalidId) {
                        m_human.onConfirm();
                        if (m_abilities.stackEmpty() && !anyPending()) {
                            m_human.endResponsePhase();
                            break;
                        }
                    } else {
                        if (tryPass()) break;
                    }
                    continue;
                }

                // ── Resolve-All button in stack sidebar ─────────────────────
                if (m_renderer.hitResolveAll(px, py) && !m_abilities.stackEmpty()) {
                    doResolveAll();
                    continue;
                }

                auto hit = m_renderer.hitTest(px, py);
                // Split card: show half-choice before processing the click
                if (m_splitChoice.active) {
                    m_splitChoice.active = false;
                    // The actual click was already intercepted below
                }

                if (hit.id != kInvalidId) {
                    m_browseSelect = hit.id;
                    // Companion special action: clicking your set-aside companion
                    // pays {3} (sorcery speed) to put it into your hand, once per
                    // game. Auto-tap lands if the pool can't cover it.
                    if (hit.zone == ZoneType::Command && hit.player == 0 &&
                        hit.id == m_game.companionId[0] && !m_game.companionUsed[0]) {
                        if (m_human.state() == HumanState::MainPhase &&
                            m_abilities.stackEmpty()) {
                            snapshotUndo();
                            bool ok = m_abilities.activateCompanion(0);
                            if (!ok) { doTapAllMana(); ok = m_abilities.activateCompanion(0); }
                            const auto* comp = m_game.findCard(m_game.companionId[0]);
                            std::string nm = comp && comp->rules ? comp->rules->name : "companion";
                            addLog(ok ? ("Companion " + nm + " enters your hand (paid {3}).")
                                      : ("Need {3} available to bring in " + nm + "."));
                            if (ok) m_sound.play(SND_SPELL_CAST);
                        } else {
                            addLog("Companion can only be brought in during your main phase with an empty stack.");
                        }
                        continue;
                    }
                    // If this is a split card in hand, show the choice overlay
                    if (hit.zone == ZoneType::Hand && hit.player == 0 && !m_splitChoice.active) {
                        const auto* c = m_game.findCard(hit.id);
                        if (c && c->rules && c->rules->hasSplit) {
                            m_splitChoice = {true, hit.id};
                            continue;  // don't process as regular click yet
                        }
                    }
                    // Snapshot before any state-changing card click
                    if (hit.zone == ZoneType::Hand && hit.player == 0)
                        snapshotUndo();
                    m_human.onCardClick(hit.id, hit.zone, hit.player);
                    // Start drag if clicking own hand card (visual feedback)
                    if (hit.zone == ZoneType::Hand && hit.player == 0) {
                        m_drag = { hit.id, true };
                    }
                } else {
                    m_browseSelect = kInvalidId;
                    if (py >= BOB_INFO_Y && py < BOB_INFO_Y + BOB_INFO_H &&
                        px >= PLAY_X && px < PLAY_X + PLAY_W)
                        m_human.onPlayerClick(1);
                }
            }
        }

        // ── Human triggers that need targeting during opponent's turn ────────────
        // (e.g. "when this ETBs, target creature gets -1/-1")
        m_human.checkAndHandleTriggers();

        // ── Drag: release to target / cancel / cast ──────────────────────────
        if (ev.type == sf::Event::MouseButtonReleased &&
            ev.mouseButton.button == sf::Mouse::Left && m_drag.active) {
            auto mapped = mapMousePos(ev.mouseButton.x, ev.mouseButton.y);
            float px = mapped.x, py = mapped.y;

            // If in TargetSelect state, treat release position as target click
            if (m_turn == WhosTurn::Human &&
                m_human.state() == HumanState::TargetSelect) {
                auto tgt = m_renderer.hitTest(px, py);
                if (tgt.id != kInvalidId)
                    m_human.onCardClick(tgt.id, tgt.zone, tgt.player);
                else if (py >= BOB_INFO_Y && py < BOB_INFO_Y + BOB_INFO_H &&
                         px >= PLAY_X && px < PLAY_X + PLAY_W)
                    m_human.onPlayerClick(1);
            }
            // Drag-to-cast: dropping a hand card onto either battlefield zone
            // commits the Cast mode (skipping the popup for the common case).
            // The press already routed through onCardClick → m_pendingSpell is
            // set and state is TargetSelect; chooseMode(Cast) either casts
            // immediately (untargeted spells) or leaves the popup up so the
            // player can click a target.
            else if (m_turn == WhosTurn::Human &&
                     m_human.pendingSpellId() != kInvalidId &&
                     px >= PLAY_X && px < PLAY_X + PLAY_W &&
                     py >= BOB_BF_Y && py < ALICE_BF_Y + ALICE_BF_H) {
                m_human.chooseMode(HumanController::CastModeKind::Cast);
            }

            m_drag.active = false;
        }

        render();
    }
}

// ── AI turn ───────────────────────────────────────────────────────────────────

#ifdef _WIN32
// Write a minidump to %APPDATA%\CitadelMTG\crash_<timestamp>.dmp.
// Must be called from within the SEH except handler frame.
static void writeMiniDump(DWORD exceptionCode) {
    HMODULE dbghelp = LoadLibraryW(L"dbghelp.dll");
    if (!dbghelp) return;
    typedef BOOL(WINAPI* MiniDumpWriteDump_t)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
        PMINIDUMP_EXCEPTION_INFORMATION, PMINIDUMP_USER_STREAM_INFORMATION,
        PMINIDUMP_CALLBACK_INFORMATION);
    auto dumpFn = reinterpret_cast<MiniDumpWriteDump_t>(
        GetProcAddress(dbghelp, "MiniDumpWriteDump"));
    if (!dumpFn) { FreeLibrary(dbghelp); return; }

    char dumpPath[MAX_PATH] = {};
    if (const char* ap = std::getenv("APPDATA")) {
        SYSTEMTIME st; GetLocalTime(&st);
        snprintf(dumpPath, sizeof(dumpPath),
                 "%s\\CitadelMTG\\crash_%04d%02d%02d_%02d%02d%02d.dmp",
                 ap, st.wYear, st.wMonth, st.wDay,
                 st.wHour, st.wMinute, st.wSecond);
    } else {
        snprintf(dumpPath, sizeof(dumpPath), "forge_crash.dmp");
    }

    HANDLE hFile = CreateFileA(dumpPath, GENERIC_WRITE, 0, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile != INVALID_HANDLE_VALUE) {
        dumpFn(GetCurrentProcess(), GetCurrentProcessId(), hFile,
               MiniDumpNormal, nullptr, nullptr, nullptr);
        CloseHandle(hFile);
        std::cerr << "[Crash] Minidump written to: " << dumpPath << '\n';
    }
    FreeLibrary(dbghelp);
    (void)exceptionCode;
}

// Static member — no C++ destructors in this frame, required for __try/__except.
// Returns 0 on success, the SEH exception code on crash.
// Catches ALL SEH exceptions so that heap corruption, invalid instruction, etc.
// all forfeit the game instead of silently killing the process.
int GameWindow::runAiTurnSafe(GameWindow* self) {
    __try {
        self->runAiTurn();
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        DWORD code = GetExceptionCode();
        if (code == STATUS_STACK_OVERFLOW)
            _resetstkoflw();
        writeMiniDump(code);  // write crash dump before returning
        return static_cast<int>(code);
    }
}
#endif

void GameWindow::runAiTurn() {
    // In 3/4 player games the active AI seat changes between turns; rebind
    // the shared m_bobAi to the seat that owns this turn so its targeting,
    // mana, and combat decisions act for the right player.
    m_bobAi.setId(m_game.activePlayerId());
    addLog("Turn " + std::to_string(m_game.turnNumber()) + ": " +
           m_game.activePlayer().name() + " (AI)");
    m_aiPhase = "Untap";

    auto autoStep = [&]() { m_tm.beginStep(); m_tm.endStep(); m_tm.advanceStep(); };
    auto setPhase = [&](const char* ph) { m_aiPhase = ph; };

    // Untap
    autoStep();
    if (m_tm.isGameOver()) return;
    // Upkeep — process suspend/rebound/echo/cumulative before firing triggers
    m_tm.beginStep();
    m_abilities.processSuspendUpkeep(m_game.activePlayerId());
    m_abilities.processReboundUpkeep(m_game.activePlayerId());
    m_abilities.processEchoUpkeep(m_game.activePlayerId());
    m_abilities.processCumulativeUpkeep(m_game.activePlayerId());
    m_abilities.firePhaseTriggersAndDrain(TurnStep::Upkeep, m_game.activePlayerId());
    m_tm.endStep(); if (m_tm.isGameOver()) return; m_tm.advanceStep();
    // Draw
    autoStep();
    if (m_tm.isGameOver()) return;

    humanPriorityWindow();
    if (m_tm.isGameOver()) return;

    setPhase("Main Phase 1");
    m_tm.beginStep();
    m_abilities.firePhaseTriggersAndDrain(TurnStep::PreCombatMain, m_game.activePlayerId());
    {
        // Land drop only — DON'T pre-tap any sources. tryCastBestSpell and
        // friends use canAffordWithUntapped() to simulate mana from untapped
        // lands and lazy-tap exactly the colour combination they need. The
        // old pre-tap pass burned every land regardless of whether the AI
        // had anything to cast, leaving everything tapped at end of turn.
        for (int i = 0; i < 8 && m_bobAi.tryPlayLand(); ++i) {}  // extra land plays
        if (m_bobAi.tryActivateCompanion())
            addLog("Opponent brings in its companion.");
        bool cast = true;
        while (cast && m_window.isOpen()) {
            cast = m_bobAi.tryCastBestSpell();
            if (cast) {
                // Give human a response window (can counter, cast instants, etc.).
                // humanPriorityWindow resolves the top when human presses Pass.
                humanPriorityWindow();
                if (!m_window.isOpen() || m_tm.isGameOver()) return;
                // Drain any remaining stack items from sub-effects.
                while (!m_abilities.stackEmpty()) {
                    m_abilities.resolveTop();
                    while (StateBasedActions::run(m_game)) {}
                    m_abilities.drainPendingTriggers();
                    if (m_game.hasPendingDiscard()) { handlePendingDiscard(); if (!m_window.isOpen()) return; }
                }
            }
        }
        if (m_bobAi.tryActivateAbilities()) {
            humanPriorityWindow();
            if (!m_window.isOpen() || m_tm.isGameOver()) return;
            while (!m_abilities.stackEmpty()) {
                m_abilities.resolveTop();
                while (StateBasedActions::run(m_game)) {}
                m_abilities.drainPendingTriggers();
            }
        }
    }
    m_tm.endStep(); m_tm.advanceStep();
    if (m_tm.isGameOver()) return;

    autoStep(); if (m_tm.isGameOver()) return;  // Begin Combat

    m_tm.beginStep();
    m_bobAi.doAttackers(m_tm);
    m_tm.endStep(); m_tm.advanceStep();

    humanPriorityWindow();
    if (m_tm.isGameOver()) return;

    m_tm.beginStep();
    if (!m_tm.combatState().empty()) {
        m_human.startBlockPhase();
        while (m_human.state() == HumanState::DeclareBlock && m_window.isOpen()) {
            handleEvents(); render();
        }
        if (m_tm.isGameOver()) return;
        // Bob may use Ninjutsu on any of his unblocked attackers
        m_bobAi.tryNinjutsu(m_tm);
        while (StateBasedActions::run(m_game)) {}
        m_abilities.drainPendingTriggers();
    }
    m_tm.endStep(); m_tm.advanceStep();

    m_tm.beginStep();
    if (m_tm.hasFirstStrikers()) {
        m_tm.dealCombatDamage(true);
        while (StateBasedActions::run(m_game)) {}
        m_abilities.drainPendingTriggers();
    }
    m_tm.endStep(); if (m_tm.isGameOver()) return; m_tm.advanceStep();

    // Pause between first-strike and regular damage so the human can respond
    // (e.g. cast a trick, see what died to first strike, choose not to block).
    if (m_tm.hasFirstStrikers()) {
        humanPriorityWindow();
        if (m_tm.isGameOver()) return;
    }

    m_tm.beginStep();
    if (!m_tm.combatState().empty()) {
        m_tm.dealCombatDamage(false);
        while (StateBasedActions::run(m_game)) {}
        m_abilities.drainPendingTriggers();
    }
    m_tm.endStep(); if (m_tm.isGameOver()) return; m_tm.advanceStep();

    autoStep(); if (m_tm.isGameOver()) return;  // End of Combat

    humanPriorityWindow();
    if (m_tm.isGameOver()) return;

    // Extra combat phases (e.g. Aggravated Assault)
    while (m_tm.extraCombats() > 0 && !m_tm.isGameOver()) {
        m_tm.consumeExtraCombat();
        m_tm.jumpToStep(TurnStep::BeginCombat);
        autoStep(); if (m_tm.isGameOver()) return;   // Begin Combat

        m_tm.beginStep();
        m_bobAi.doAttackers(m_tm);
        m_tm.endStep(); m_tm.advanceStep();

        humanPriorityWindow(); if (m_tm.isGameOver()) return;

        m_tm.beginStep();
        if (!m_tm.combatState().empty()) {
            m_human.startBlockPhase();
            while (m_human.state() == HumanState::DeclareBlock && m_window.isOpen()) {
                handleEvents(); render();
            }
            if (m_tm.isGameOver()) return;
            while (StateBasedActions::run(m_game)) {}
            m_abilities.drainPendingTriggers();
        }
        m_tm.endStep(); m_tm.advanceStep();

        m_tm.beginStep();
        if (m_tm.hasFirstStrikers()) {
            m_tm.dealCombatDamage(true);
            while (StateBasedActions::run(m_game)) {}
            m_abilities.drainPendingTriggers();
        }
        m_tm.endStep(); if (m_tm.isGameOver()) return; m_tm.advanceStep();

        m_tm.beginStep();
        if (!m_tm.combatState().empty()) {
            m_tm.dealCombatDamage(false);
            while (StateBasedActions::run(m_game)) {}
            m_abilities.drainPendingTriggers();
        }
        m_tm.endStep(); if (m_tm.isGameOver()) return; m_tm.advanceStep();

        autoStep(); if (m_tm.isGameOver()) return;  // End of Combat
        humanPriorityWindow(); if (m_tm.isGameOver()) return;
    }

    m_tm.beginStep();
    m_abilities.firePhaseTriggersAndDrain(TurnStep::PostCombatMain, m_game.activePlayerId());
    {
        bool cast = true;
        while (cast && m_window.isOpen()) {
            cast = m_bobAi.tryCastBestSpell();
            if (cast) {
                humanPriorityWindow();
                if (!m_window.isOpen() || m_tm.isGameOver()) return;
                while (!m_abilities.stackEmpty()) {
                    m_abilities.resolveTop();
                    while (StateBasedActions::run(m_game)) {}
                    m_abilities.drainPendingTriggers();
                    if (m_game.hasPendingDiscard()) { handlePendingDiscard(); if (!m_window.isOpen()) return; }
                }
            }
        }
        // Main 2: skip the speculative ability + planeswalker passes. Most of
        // their work happens in Main 1 anyway; running them post-combat just
        // drains floating mana into "draw 1" / "gain 1 life" type activations
        // that don't materially affect the board — the user sees the result
        // as "AI tapped out at end of turn for nothing." Reserving mana means
        // the lands stay untapped, which is also less misleading visually.
        (void)0;
        if (!m_abilities.stackEmpty()) {
            humanPriorityWindow();
            if (!m_window.isOpen() || m_tm.isGameOver()) return;
            while (!m_abilities.stackEmpty()) {
                m_abilities.resolveTop();
                while (StateBasedActions::run(m_game)) {}
                m_abilities.drainPendingTriggers();
            }
        }
    }
    m_tm.endStep(); if (m_tm.isGameOver()) return; m_tm.advanceStep();

    humanPriorityWindow();
    if (m_tm.isGameOver()) return;

    m_tm.beginStep();
    m_abilities.firePhaseTriggersAndDrain(TurnStep::EndStep, m_game.activePlayerId());
    m_tm.endStep(); if (m_tm.isGameOver()) return; m_tm.advanceStep();

    autoStep();  // Cleanup
}

// ── Dock actions ──────────────────────────────────────────────────────────────

void GameWindow::doAiResponseThenPass() {
    // Bob gets a window to respond: either on a non-empty stack OR proactively
    // flashing in creatures/instants (e.g. at Alice's end step with empty stack).
    {
        // Don't pre-tap — tryCastInstant uses canAfford() to evaluate options
        // off the untapped pool and lazy-taps only the source(s) for the chosen
        // spell. Pre-tapping here burned every land on Shivan Reef / pain lands
        // (and any other PayLife mana source) even when nothing got cast.
        bool bobActed = m_bobAi.tryCastInstant();
        if (bobActed) {
            humanPriorityWindow();
            if (!m_window.isOpen() || m_tm.isGameOver()) return;
            while (!m_abilities.stackEmpty()) {
                m_abilities.resolveTop();
                while (StateBasedActions::run(m_game)) {}
                m_abilities.drainPendingTriggers();
            }
            if (m_tm.isGameOver()) return;
        }
    }
    m_human.onEndPhase();
}

void GameWindow::doAlphaStrike() {
    if (m_human.state() != HumanState::DeclareAttack) return;
    for (auto* card : m_game.battlefield().cards()) {
        if (card->controllerId == 0 && card->isCreature() &&
            !card->tapped && !card->summoningSickness && !card->cantAttack)
            m_human.onCardClick(card->id, ZoneType::Battlefield, 0);
    }
    m_human.onConfirm();
    addLog("Alpha Strike declared.");
}

void GameWindow::doConcede() {
    m_game.player(0).lose();
    StateBasedActions::run(m_game);
    addLog("Alice concedes. Bob wins!");
}

// ── ESC pause menu (replaces the old bottom dock) ─────────────────────────────

bool GameWindow::pauseMenuModal() {
    // Runs its own event/draw loop so Esc works even inside nested loops like
    // humanPriorityWindow (which previously called m_window.close() on Esc — that
    // tore down the match mid-flow and crashed). Returns true if the match should
    // unwind (Concede/Quit), false on Resume.
    m_pauseMenuOpen  = true;
    m_pauseMenuHover = -1;
    while (m_window.isOpen() && m_pauseMenuOpen) {
        sf::Event ev;
        while (m_window.pollEvent(ev)) {
            if (ev.type == sf::Event::Closed)  { m_window.close(); return true; }
            if (ev.type == sf::Event::Resized) { updateView(); }
            if (ev.type == sf::Event::MouseMoved) {
                auto p = mapMousePos(ev.mouseMove.x, ev.mouseMove.y);
                auto act = hitPauseMenu(p.x, p.y);
                m_pauseMenuHover = (act == PauseAction::None) ? -1
                                                              : static_cast<int>(act) - 1;
            }
            if (ev.type == sf::Event::KeyPressed && ev.key.code == sf::Keyboard::Escape) {
                m_pauseMenuOpen = false;   // Esc again → resume
            }
            if (ev.type == sf::Event::MouseButtonPressed &&
                ev.mouseButton.button == sf::Mouse::Left) {
                auto p = mapMousePos(ev.mouseButton.x, ev.mouseButton.y);
                switch (hitPauseMenu(p.x, p.y)) {
                    case PauseAction::Resume:  m_pauseMenuOpen = false; break;
                    case PauseAction::Undo:    doUndo(); m_pauseMenuOpen = false; break;
                    case PauseAction::Concede: doConcede(); m_pauseMenuOpen = false; return true;
                    case PauseAction::Quit:    m_pauseMenuOpen = false;
                                               m_appState = AppState::MainMenu; return true;
                    case PauseAction::None:    break;
                }
            }
        }
        m_window.clear(sf::Color(11, 10, 9));
        drawPauseMenu();
        m_window.display();
        sf::sleep(sf::milliseconds(8));
    }
    return false;
}

void GameWindow::drawPauseMenu() {
    // Dim full window
    sf::RectangleShape dim({WIN_W, WIN_H});
    dim.setFillColor(sf::Color(0, 0, 0, 170));
    m_window.draw(dim);

    constexpr float pw = 360.f, ph = 320.f;
    float px = (WIN_W - pw) * 0.5f, py = (WIN_H - ph) * 0.5f;

    // Panel
    sf::RectangleShape pan({pw, ph});
    pan.setPosition(px, py);
    pan.setFillColor(sf::Color(18, 17, 16, 252));
    pan.setOutlineColor(sf::Color(203, 163, 90));
    pan.setOutlineThickness(2.f);
    m_window.draw(pan);

    // Title
    sf::Text title("PAUSED", m_font, 22);
    title.setStyle(sf::Text::Bold);
    title.setFillColor(sf::Color(230, 193, 112));
    title.setPosition(px + (pw - title.getLocalBounds().width) * 0.5f, py + 18.f);
    drawText(title);

    sf::Text hint("Press Esc to resume", m_font, 10);
    hint.setFillColor(sf::Color(148, 139, 124));
    hint.setPosition(px + (pw - hint.getLocalBounds().width) * 0.5f, py + 48.f);
    drawText(hint);

    // Buttons
    static const char* kLabels[] = { "Resume",
                                     "Undo last action (Ctrl+Z)",
                                     "Concede",
                                     "Quit to main menu" };
    constexpr float btnH = 44.f, gap = 10.f;
    float btnW = pw - 40.f, btnX = px + 20.f;
    float by = py + 78.f;
    for (int i = 0; i < 4; ++i) {
        bool hov = (m_pauseMenuHover == i);
        sf::RectangleShape btn({btnW, btnH});
        btn.setPosition(btnX, by + i * (btnH + gap));
        // Concede + Quit get a red tint to mark them as destructive.
        sf::Color base = (i >= 2) ? sf::Color(48, 22, 18)
                                  : sf::Color(34, 31, 27);
        btn.setFillColor(hov ? sf::Color(base.r + 14, base.g + 14, base.b + 14, 255)
                              : base);
        btn.setOutlineColor(hov ? sf::Color(230, 193, 112)
                                : sf::Color(240, 220, 180, 30));
        btn.setOutlineThickness(1.f);
        m_window.draw(btn);

        sf::Text lbl(kLabels[i], m_font, 14);
        lbl.setStyle(sf::Text::Bold);
        lbl.setFillColor((i >= 2) ? sf::Color(230, 170, 130)
                                  : sf::Color(199, 189, 172));
        auto b = lbl.getLocalBounds();
        lbl.setPosition(btnX + (btnW - b.width) * 0.5f - b.left,
                        by + i * (btnH + gap) + (btnH - b.height) * 0.5f - b.top);
        drawText(lbl);
    }
}

GameWindow::PauseAction GameWindow::hitPauseMenu(float px, float py) const {
    constexpr float pw = 360.f, ph = 320.f;
    float pxp = (WIN_W - pw) * 0.5f, pyp = (WIN_H - ph) * 0.5f;
    constexpr float btnH = 44.f, gap = 10.f;
    float btnW = pw - 40.f, btnX = pxp + 20.f;
    float by = pyp + 78.f;
    for (int i = 0; i < 4; ++i) {
        float y0 = by + i * (btnH + gap);
        if (px >= btnX && px <= btnX + btnW && py >= y0 && py <= y0 + btnH) {
            switch (i) {
                case 0: return PauseAction::Resume;
                case 1: return PauseAction::Undo;
                case 2: return PauseAction::Concede;
                case 3: return PauseAction::Quit;
            }
        }
    }
    return PauseAction::None;
}

// ── Game statistics persistence ───────────────────────────────────────────────

static std::filesystem::path statsPath() {
    namespace fs = std::filesystem;
    if (const char* v = std::getenv("APPDATA"); v) {
        auto p = fs::path(v) / "CitadelMTG";
        std::error_code ec; fs::create_directories(p, ec);
        return p / "game_stats.json";
    }
    return "game_stats.json";
}

void GameWindow::loadCardTags() {
    namespace fs = std::filesystem;
    if (const char* ap = std::getenv("APPDATA")) {
        std::ifstream f(fs::path(ap) / "CitadelMTG" / "card_tags.txt");
        std::string line;
        while (std::getline(f, line)) {
            auto eq = line.find('=');
            if (eq != std::string::npos)
                m_cardTags[line.substr(0, eq)] = line.substr(eq + 1);
        }
    }
}

void GameWindow::saveCardTags() const {
    namespace fs = std::filesystem;
    if (const char* ap = std::getenv("APPDATA")) {
        std::ofstream f(fs::path(ap) / "CitadelMTG" / "card_tags.txt");
        for (const auto& [card, tag] : m_cardTags)
            f << card << "=" << tag << '\n';
    }
}

void GameWindow::loadDeckStats() {
    std::ifstream f(statsPath());
    if (!f) return;
    try {
        auto j = nlohmann::json::parse(f);
        for (auto& [key, val] : j.items()) {
            DeckStats s;
            s.wins   = val.value("wins",   0);
            s.losses = val.value("losses", 0);
            s.draws  = val.value("draws",  0);
            m_deckStats[key] = s;
        }
    } catch (...) {}
}

void GameWindow::saveDeckStats() const {
    nlohmann::json j;
    for (const auto& [key, s] : m_deckStats) {
        j[key]["wins"]   = s.wins;
        j[key]["losses"] = s.losses;
        j[key]["draws"]  = s.draws;
    }
    std::ofstream f(statsPath());
    if (f) f << j.dump(2);
}

void GameWindow::recordGameResult(int winnerPlayerId) {
    checkAchievements();  // check achievements after each game result
    // Build a key from the two deck names
    auto deckKey = [&]() -> std::string {
        namespace fs = std::filesystem;
        std::string d0 = m_rematchDeck0.empty()
                       ? (m_rematchEditorDeck.name.empty() ? "Alice" : m_rematchEditorDeck.name)
                       : m_rematchDeck0.stem().string();
        std::string d1 = m_rematchAiDeckPath.empty()
                       ? "AI"
                       : m_rematchAiDeckPath.stem().string();
        return d0 + " vs " + d1;
    };
    std::string key = deckKey();
    DeckStats& s = m_deckStats[key];
    if (winnerPlayerId == 0)       ++s.wins;
    else if (winnerPlayerId == 1)  ++s.losses;
    else                            ++s.draws;
    // Record commander name for per-commander stats
    for (const Card* c : m_game.command().cards())
        if (c->ownerId == 0 && c->isCommander) { s.commanderName = c->rules->name; break; }
    saveDeckStats();
}

// ── Card collection tracker ───────────────────────────────────────────────────

static std::filesystem::path collectionPath() {
    namespace fs = std::filesystem;
    if (const char* v = std::getenv("APPDATA"); v) {
        auto p = fs::path(v) / "CitadelMTG";
        std::error_code ec; fs::create_directories(p, ec);
        return p / "card_collection.json";
    }
    return "card_collection.json";
}

void GameWindow::loadCardCollection() {
    std::ifstream f(collectionPath());
    if (!f) return;
    try {
        auto j = nlohmann::json::parse(f);
        for (auto& [name, cnt] : j.items())
            m_cardSeen[name] = cnt.get<int>();
    } catch (...) {}
}

void GameWindow::saveCardCollection() const {
    nlohmann::json j;
    for (const auto& [name, cnt] : m_cardSeen)
        j[name] = cnt;
    std::ofstream f(collectionPath());
    if (f) f << j.dump(2);
}

void GameWindow::recordGameCards() {
    // Track every non-token card across all zones
    auto trackZone = [&](const mtg::Zone& z) {
        for (const mtg::Card* c : z.cards())
            if (c && c->rules && !c->isToken)
                ++m_cardSeen[c->rules->name];
    };
    trackZone(m_game.battlefield());
    trackZone(m_game.exile());
    for (uint8_t pid = 0; pid < m_game.numPlayers(); ++pid) {
        trackZone(m_game.player(pid).graveyard());
        trackZone(m_game.player(pid).hand());
        trackZone(m_game.player(pid).library());
    }
    saveCardCollection();
}

void GameWindow::checkTournamentEnd() {
    if (!m_tournamentMode) return;
    uint8_t wid = m_tm.winnerId();
    if (wid < 2) ++m_matchWins[wid];
    ++m_gamesPlayed;

    int needed = 2;  // best-of-3 needs 2 wins
    for (int p = 0; p < 2; ++p) {
        if (m_matchWins[p] >= needed) {
            addLog("=== MATCH OVER: " + m_game.player(p).name() + " wins the match "
                   + std::to_string(m_matchWins[p]) + "-" + std::to_string(m_matchWins[p^1]) + "! ===");
            m_tournamentMode = false;
            m_matchWins[0] = m_matchWins[1] = 0;
            m_gamesPlayed = 0;
            return;
        }
    }
    // Match not over: log current score and auto-rematch
    addLog("Match score: " + m_game.player(0).name() + " " + std::to_string(m_matchWins[0])
           + " - " + m_game.player(1).name() + " " + std::to_string(m_matchWins[1])
           + "  (Game " + std::to_string(m_gamesPlayed + 1) + " of 3)");
    // Swap who goes first for the next game and rematch
    m_aliceGoesFirst = !m_aliceGoesFirst;
    doRematch();
}

bool GameWindow::saveGame(const std::filesystem::path& path) {
    namespace fs = std::filesystem;
    try {
        nlohmann::json j;
        j["save_version"] = 2;          // increment when format changes
        j["turn"]    = m_game.turnNumber();
        j["active"]  = m_game.activePlayerId();
        j["monarch"] = m_game.monarchPlayer;

        // Save each player's life, hand, library, GY, and BF card names
        for (uint8_t pid = 0; pid < 2; ++pid) {
            const Player& p = m_game.player(pid);
            auto& pj = j["players"][pid];
            pj["life"]    = p.life();
            pj["poison"]  = p.poisonCounters();
            pj["cmd_tax"] = m_game.commanderCastCount[pid];

            auto cardsOf = [&](const Zone& z) {
                nlohmann::json arr = nlohmann::json::array();
                for (const Card* c : z.cards())
                    if (c && c->rules) {
                        nlohmann::json ce;
                        ce["name"]    = c->rules->name;
                        ce["tapped"]  = c->tapped;
                        ce["sick"]    = c->summoningSickness;
                        ce["damage"]  = c->markedDamage;
                        ce["cmd"]     = c->isCommander;
                        for (const auto& [type, cnt] : c->counters)
                            ce["counters"][type] = cnt;
                        arr.push_back(ce);
                    }
                return arr;
            };
            pj["hand"]      = cardsOf(p.hand());
            pj["library"]   = cardsOf(p.library());
            pj["graveyard"] = cardsOf(p.graveyard());
        }

        // Battlefield
        auto& bfj = j["battlefield"];
        for (const Card* c : m_game.battlefield().cards()) {
            if (!c || !c->rules) continue;
            nlohmann::json ce;
            ce["name"]    = c->rules->name;
            ce["ctrl"]    = c->controllerId;
            ce["owner"]   = c->ownerId;
            ce["tapped"]  = c->tapped;
            ce["sick"]    = c->summoningSickness;
            ce["damage"]  = c->markedDamage;
            ce["cmd"]     = c->isCommander;
            for (const auto& [type, cnt] : c->counters)
                ce["counters"][type] = cnt;
            bfj.push_back(ce);
        }

        std::string newJson = j.dump(2);

        // Delta save: if a previous save exists at this path, write only the diff
        // by checking if the JSON is identical (no-op) or appending a delta marker.
        bool isDelta = false;
        {
            std::ifstream prev(path);
            if (prev.good()) {
                std::string prevJson((std::istreambuf_iterator<char>(prev)),
                                      std::istreambuf_iterator<char>());
                // If only turn changed and life is the same, mark as delta-friendly
                if (prevJson.find("\"turn\"") != std::string::npos &&
                    newJson.size() < prevJson.size() + 512) {
                    isDelta = true;  // could store incremental; for now just note it
                }
            }
        }
        (void)isDelta;  // Future: write diff; for now always write full state

        std::ofstream f(path);
        f << newJson;
        addLog("Game saved.");
        return true;
    } catch (const std::exception& e) {
        addLog(std::string("Save failed: ") + e.what());
        return false;
    }
}

bool GameWindow::loadGame(const std::filesystem::path& path) {
    namespace fs = std::filesystem;
    if (!fs::exists(path)) { addLog("Save file not found."); return false; }
    try {
        auto j = nlohmann::json::parse(std::ifstream(path));
        // Version check: warn if the save was made by a different format version
        int sv = j.value("save_version", 1);
        if (sv < 2) addLog("Warning: save file is from an older version — some data may be missing.");

        // Reset the game and build a fresh state from the saved JSON.
        m_game.reset();
        m_game.setHumanInteractive(true);
        m_game.setCardDb(&m_db);
        m_tm.reset();

        // Restore players (life, poison, commander tax, and ALL zones —
        // including the library, without which the resumed game can't draw).
        for (int pid = 0; pid < 2; ++pid) {
            auto& pj = j["players"][pid];
            m_game.player(pid).setLife(pj.value("life", 40));
            m_game.commanderCastCount[pid] = pj.value("cmd_tax", 0);
            int poison = pj.value("poison", 0);
            for (int k = 0; k < poison; ++k) m_game.player(pid).addPoison(1);

            auto rebuildZone = [&](const nlohmann::json& arr, mtg::ZoneType dest) {
                if (!arr.is_array()) return;
                for (auto& ce : arr) {
                    std::string nm = ce.value("name", "");
                    const auto* rules = m_db.find(nm);
                    if (!rules) continue;
                    mtg::Card* c = m_game.createCard(rules, static_cast<uint8_t>(pid));
                    mtg::Card* placed = m_game.moveToZone(c->id, dest, static_cast<uint8_t>(pid));
                    if (!placed) continue;
                    placed->tapped            = ce.value("tapped", false);
                    placed->isCommander       = ce.value("cmd", false);
                    placed->summoningSickness  = ce.value("sick", false);
                    placed->markedDamage      = ce.value("damage", 0);
                    if (ce.contains("counters"))
                        for (auto& [k, v] : ce["counters"].items())
                            placed->addCounter(k, v.get<int>());
                }
            };
            rebuildZone(pj["hand"],      mtg::ZoneType::Hand);
            rebuildZone(pj["library"],   mtg::ZoneType::Library);   // was missing!
            rebuildZone(pj["graveyard"], mtg::ZoneType::Graveyard);
        }

        // Battlefield
        for (auto& ce : j["battlefield"]) {
            std::string nm = ce.value("name", "");
            const auto* rules = m_db.find(nm);
            if (!rules) continue;
            uint8_t ctrl  = ce.value("ctrl", 0);
            uint8_t owner = ce.value("owner", ctrl);
            mtg::Card* c = m_game.createCard(rules, owner);
            mtg::Card* placed = m_game.moveToZone(c->id, mtg::ZoneType::Battlefield, ctrl);
            if (placed) {
                placed->tapped           = ce.value("tapped", false);
                placed->isCommander      = ce.value("cmd", false);
                placed->controllerId     = ctrl;
                placed->summoningSickness = ce.value("sick", false);
                placed->markedDamage     = ce.value("damage", 0);
                if (ce.contains("counters"))
                    for (auto& [k, v] : ce["counters"].items())
                        placed->addCounter(k, v.get<int>());
            }
        }

        // Game-wide state.
        m_game.monarchPlayer = static_cast<uint8_t>(j.value("monarch", 255));
        uint8_t active = static_cast<uint8_t>(j.value("active", 0));
        m_game.setActivePlayer(active);
        m_game.recomputeStaticBonuses();

        m_renderer.init(m_font, m_game, m_tm, m_picsDir);
        m_human.setPhaseStops(m_stops);
        // Autosaves are taken during the human's turn, so resume there. (If the
        // save was the opponent's turn we still hand control to the player — a
        // minor inaccuracy that keeps the game playable rather than stuck.)
        m_human.startHumanTurn();
        m_turn = WhosTurn::Human;
        m_appState = AppState::Playing;
        addLog("Game loaded from " + path.filename().string());
        return true;
    } catch (const std::exception& e) {
        addLog(std::string("Load failed: ") + e.what());
        return false;
    }
}

void GameWindow::renderSplitChoice() {
    using namespace Layout;
    const auto* card = m_game.findCard(m_splitChoice.cardId);
    if (!card || !card->rules) { m_splitChoice.active = false; return; }
    const auto& r = *card->rules;

    // Dim
    sf::RectangleShape dim({WIN_W, WIN_H});
    dim.setFillColor(sf::Color(0, 0, 0, 160));
    m_window.draw(dim);

    constexpr float panW = 520.f, panH = 180.f;
    float px0 = (WIN_W - panW) * 0.5f, py0 = (WIN_H - panH) * 0.5f;

    sf::RectangleShape pan({panW, panH});
    pan.setPosition(px0, py0);
    pan.setFillColor(sf::Color(12, 18, 30, 252));
    pan.setOutlineColor(sf::Color(60, 100, 80));
    pan.setOutlineThickness(2.f);
    m_window.draw(pan);

    sf::Text title("Cast which half?", m_font, 13);
    title.setStyle(sf::Text::Bold);
    title.setFillColor(sf::Color(180, 220, 190));
    auto tb = title.getLocalBounds();
    title.setPosition(px0 + (panW - tb.width) * 0.5f, py0 + 12.f);
    drawText(title);

    // Two buttons: left half / right half
    const char* lName = r.name.c_str();
    std::string lCost = r.manaCost.toString();
    const char* rName = r.splitName.empty() ? "—" : r.splitName.c_str();
    std::string rCost = r.splitCost.toString();

    constexpr float btnW = 220.f, btnH = 60.f, gap = 16.f;
    float bx0 = px0 + (panW - 2 * btnW - gap) * 0.5f;
    float by0 = py0 + 50.f;

    for (int i = 0; i < 2; ++i) {
        float bx = bx0 + i * (btnW + gap);
        sf::RectangleShape btn({btnW, btnH});
        btn.setPosition(bx, by0);
        bool sel = (m_splitChoice.selection == i);
        btn.setFillColor(sel ? sf::Color(30, 80, 50) : sf::Color(25, 50, 35));
        btn.setOutlineColor(sel ? sf::Color(90, 200, 110) : sf::Color(55, 120, 70));
        btn.setOutlineThickness(1.5f);
        m_window.draw(btn);

        const char* nm  = (i == 0) ? lName  : rName;
        std::string cost = (i == 0) ? lCost  : rCost;
        sf::Text nt(nm, m_font, 13);
        nt.setStyle(sf::Text::Bold);
        nt.setFillColor(sf::Color(220, 235, 215));
        auto nb = nt.getLocalBounds();
        nt.setPosition(bx + (btnW - nb.width) * 0.5f, by0 + 8.f);
        drawText(nt);
        sf::Text ct(cost, m_font, 11);
        ct.setFillColor(sf::Color(180, 200, 160));
        auto cb2 = ct.getLocalBounds();
        ct.setPosition(bx + (btnW - cb2.width) * 0.5f, by0 + 34.f);
        drawText(ct);
    }

    sf::Text hint("[Esc to cancel]", m_font, 9);
    hint.setFillColor(sf::Color(80, 100, 80));
    auto hb = hint.getLocalBounds();
    hint.setPosition(px0 + (panW - hb.width) * 0.5f, py0 + panH - 18.f);
    drawText(hint);
}

void GameWindow::renderCardSearch() {
    using namespace Layout;
    constexpr float pw = 480.f, ph = 500.f;
    const float px0 = (WIN_W - pw) * 0.5f, py0 = (WIN_H - ph) * 0.5f;

    sf::RectangleShape dim({WIN_W, WIN_H});
    dim.setFillColor(sf::Color(0, 0, 0, 150));
    m_window.draw(dim);

    sf::RectangleShape pan({pw, ph});
    pan.setPosition(px0, py0);
    pan.setFillColor(sf::Color(10, 16, 26, 252));
    pan.setOutlineColor(sf::Color(60, 100, 80));
    pan.setOutlineThickness(2.f);
    m_window.draw(pan);

    // Search box
    constexpr float boxH = 28.f;
    sf::RectangleShape box({pw - 20.f, boxH});
    box.setPosition(px0 + 10.f, py0 + 10.f);
    box.setFillColor(sf::Color(20, 30, 45));
    box.setOutlineColor(sf::Color(80, 140, 100));
    box.setOutlineThickness(1.5f);
    m_window.draw(box);

    std::string qDisplay = m_cardSearchQuery.empty() ? "[Type to search cards]" : m_cardSearchQuery + "|";
    sf::Text qTxt(qDisplay, m_font, 11);
    qTxt.setFillColor(m_cardSearchQuery.empty() ? sf::Color(90, 110, 90) : sf::Color(200, 225, 200));
    qTxt.setPosition(px0 + 14.f, py0 + 14.f);
    drawText(qTxt);

    sf::Text hdr("Ctrl+F: Search  [Esc/Enter to close]", m_font, 9);
    hdr.setFillColor(sf::Color(80, 110, 80));
    hdr.setPosition(px0 + 14.f, py0 + boxH + 14.f);
    drawText(hdr);

    if (m_cardSearchQuery.empty()) return;

    // Collect matches across hand, battlefield, graveyard, exile
    struct Match { const mtg::Card* card; std::string zone; };
    std::vector<Match> matches;
    auto query = m_cardSearchQuery;
    std::transform(query.begin(), query.end(), query.begin(), ::tolower);

    auto tryAdd = [&](const mtg::Card* c, const char* zone) {
        if (!c || !c->rules) return;
        std::string nm = c->rules->name;
        std::string nml = nm; std::transform(nml.begin(), nml.end(), nml.begin(), ::tolower);
        if (nml.find(query) != std::string::npos)
            matches.push_back({c, zone});
    };
    for (const mtg::Card* c : m_game.player(0).hand().cards())       tryAdd(c, "Hand");
    for (const mtg::Card* c : m_game.battlefield().cards())
        if (c->controllerId == 0)                                     tryAdd(c, "Battlefield");
    for (const mtg::Card* c : m_game.player(0).graveyard().cards())  tryAdd(c, "Graveyard");
    for (const mtg::Card* c : m_game.exile().cards())
        if (c->ownerId == 0)                                          tryAdd(c, "Exile");
    // Also search library (shows location in deck but not the card face to avoid cheating)
    int libIdx = 0;
    for (const mtg::Card* c : m_game.player(0).library().cards()) {
        if (!c || !c->rules) { ++libIdx; continue; }
        std::string nm2 = c->rules->name;
        std::string nml2 = nm2;
        std::transform(nml2.begin(), nml2.end(), nml2.begin(), ::tolower);
        if (nml2.find(query) != std::string::npos)
            matches.push_back({c, "Library #" + std::to_string(++libIdx)});
        else ++libIdx;
    }
    for (const mtg::Card* c : m_game.command().cards())
        if (c->ownerId == 0)                                          tryAdd(c, "Command");

    float ty = py0 + boxH + 36.f;
    constexpr float kRowH = 26.f, kRowGap = 2.f;
    int shown = 0;
    for (const auto& m : matches) {
        if (ty + kRowH > py0 + ph - 8.f) break;
        sf::Color bg = m.zone == "Hand"
                     ? sf::Color(25, 45, 30)
                     : m.zone == "Battlefield" ? sf::Color(20, 40, 55)
                     : sf::Color(35, 28, 40);
        sf::RectangleShape row({pw - 16.f, kRowH});
        row.setPosition(px0 + 8.f, ty);
        row.setFillColor(bg);
        row.setOutlineColor(sf::Color(50, 70, 55));
        row.setOutlineThickness(1.f);
        m_window.draw(row);

        sf::Text nm(m.card->rules->name, m_font, 10);
        nm.setStyle(sf::Text::Bold);
        nm.setFillColor(sf::Color(225, 230, 215));
        nm.setPosition(px0 + 12.f, ty + 4.f);
        drawText(nm);

        sf::Text zn("[" + m.zone + "]", m_font, 9);
        zn.setFillColor(sf::Color(130, 155, 130));
        auto zb = zn.getLocalBounds();
        zn.setPosition(px0 + pw - zb.width - 14.f, ty + 7.f);
        drawText(zn);

        ty += kRowH + kRowGap;
        ++shown;
    }
    if (matches.empty()) {
        sf::Text empty("No cards match \"" + m_cardSearchQuery + "\"", m_font, 10);
        empty.setFillColor(sf::Color(90, 100, 90));
        empty.setPosition(px0 + 14.f, ty + 8.f);
        drawText(empty);
    }
}

void GameWindow::renderHelp() {
    using namespace Layout;
    constexpr float pw = 360.f, ph = 440.f;
    const float px0 = (WIN_W - pw) * 0.5f, py0 = (WIN_H - ph) * 0.5f;

    sf::RectangleShape dim({WIN_W, WIN_H});
    dim.setFillColor(sf::Color(0, 0, 0, 160));
    m_window.draw(dim);

    sf::RectangleShape pan({pw, ph});
    pan.setPosition(px0, py0);
    pan.setFillColor(sf::Color(12, 18, 28, 250));
    pan.setOutlineColor(sf::Color(60, 100, 80));
    pan.setOutlineThickness(2.f);
    m_window.draw(pan);

    sf::Text title("Keyboard Shortcuts  [Esc to close]", m_font, 12);
    title.setStyle(sf::Text::Bold);
    title.setFillColor(sf::Color(160, 210, 180));
    auto tb = title.getLocalBounds();
    title.setPosition(px0 + (pw - tb.width) * 0.5f, py0 + 10.f);
    drawText(title);

    static constexpr struct { const char* key; const char* desc; } kBindings[] = {
        { "Space / E",     "End Phase / Pass Priority" },
        { "Enter",         "Confirm (declare attackers)" },
        { "A",             "Alpha Strike (all creatures attack)" },
        { "T",             "Tap all lands for mana" },
        { "1 – 7",         "Click corresponding hand card" },
        { "Ctrl+Z",        "Undo last card play" },
        { "Ctrl+S",        "Quick-save game state" },
        { "O",             "Open Options panel (phase stops)" },
        { "?",             "This help screen" },
        { "Escape",        "Close overlay / Main Menu" },
        { "F11",           "Toggle fullscreen" },
        { "R (game over)", "Rematch with same decks" },
        { "",              "" },
        { "Click GY count","Browse Graveyard / Exile" },
        { "Right-click",   "Set commander in deck editor" },
        { "Drag card",     "Card follows cursor; release = target" },
    };
    float ty = py0 + 36.f;
    for (const auto& b : kBindings) {
        if (b.key[0] == '\0') { ty += 6.f; continue; }
        sf::Text kTxt(b.key, m_font, 10);
        kTxt.setStyle(sf::Text::Bold);
        kTxt.setFillColor(sf::Color(210, 200, 140));
        kTxt.setPosition(px0 + 12.f, ty);
        drawText(kTxt);
        sf::Text dTxt(b.desc, m_font, 10);
        dTxt.setFillColor(sf::Color(180, 185, 180));
        dTxt.setPosition(px0 + 150.f, ty);
        drawText(dTxt);
        ty += 22.f;
    }
}

void GameWindow::renderOptions() {
    using namespace Layout;
    constexpr float pw = 400.f, ph = 480.f;
    constexpr float px0 = (WIN_W - pw) * 0.5f;
    constexpr float py0 = (WIN_H - ph) * 0.5f;

    // Dim
    sf::RectangleShape dim({WIN_W, WIN_H});
    dim.setFillColor(sf::Color(0, 0, 0, 160));
    m_window.draw(dim);

    // Panel
    sf::RectangleShape pan({pw, ph});
    pan.setPosition(px0, py0);
    pan.setFillColor(sf::Color(14, 20, 30, 248));
    pan.setOutlineColor(sf::Color(60, 100, 80));
    pan.setOutlineThickness(2.f);
    m_window.draw(pan);

    // Title
    sf::Text title("Options  [O to close]", m_font, 14);
    title.setStyle(sf::Text::Bold);
    title.setFillColor(sf::Color(180, 220, 190));
    auto tb = title.getLocalBounds();
    title.setPosition(px0 + (pw - tb.width) * 0.5f, py0 + 10.f);
    drawText(title);

    // Fullscreen toggle button
    constexpr float btnW = 150.f, btnH = 28.f;
    float bx = px0 + 14.f;
    float by = py0 + 50.f;
    sf::RectangleShape fbtn({btnW, btnH});
    fbtn.setPosition(bx, by);
    fbtn.setFillColor(m_fullscreen ? sf::Color(30,80,50) : sf::Color(50,30,30));
    fbtn.setOutlineColor(sf::Color(80, 130, 90));
    fbtn.setOutlineThickness(1.5f);
    m_window.draw(fbtn);
    std::string fsLabel = m_fullscreen ? "Fullscreen: ON" : "Fullscreen: OFF";
    sf::Text fsT(fsLabel, m_font, 11);
    fsT.setFillColor(sf::Color(210, 230, 215));
    auto flb = fsT.getLocalBounds();
    fsT.setPosition(bx + (btnW - flb.width) * 0.5f, by + (btnH - flb.height) * 0.5f - 2.f);
    drawText(fsT);

    // Colour-blind mode button
    float bx2 = px0 + pw - 14.f - btnW;
    sf::RectangleShape cbBtn({btnW, btnH});
    cbBtn.setPosition(bx2, by);
    cbBtn.setFillColor(m_colorBlindMode ? sf::Color(50, 30, 80) : sf::Color(30, 40, 55));
    cbBtn.setOutlineColor(sf::Color(80, 130, 90));
    cbBtn.setOutlineThickness(1.5f);
    m_window.draw(cbBtn);
    sf::Text cbT(m_colorBlindMode ? "Colour-blind: ON" : "Colour-blind: OFF", m_font, 11);
    cbT.setFillColor(sf::Color(210, 230, 215));
    auto clb = cbT.getLocalBounds();
    cbT.setPosition(bx2 + (btnW - clb.width) * 0.5f, by + (btnH - clb.height) * 0.5f - 2.f);
    drawText(cbT);

    // Phase stops section
    float ry = by + btnH + 18.f;
    sf::Text phHdr("Phase Stops  (click to toggle)", m_font, 11);
    phHdr.setStyle(sf::Text::Bold);
    phHdr.setFillColor(sf::Color(160, 180, 165));
    phHdr.setPosition(px0 + 10.f, ry);
    drawText(phHdr);
    ry += 20.f;

    static constexpr const char* kNames[13] = {
        "Untap","Upkeep","Draw","Main 1","Begin Combat",
        "Attackers","Blockers","1st Strike","Combat Damage",
        "End Combat","Main 2","End Step","Cleanup"
    };
    bool* stops[13] = {
        &m_stops.untap,&m_stops.upkeep,&m_stops.draw,
        &m_stops.main1,&m_stops.beginCombat,&m_stops.attackers,
        &m_stops.blockers,&m_stops.firstStrike,&m_stops.combatDmg,
        &m_stops.endCombat,&m_stops.main2,&m_stops.endStep,&m_stops.cleanup
    };

    for (int i = 0; i < 13; ++i) {
        float col   = (i < 7) ? px0 + 16.f : px0 + pw * 0.5f + 6.f;
        float rowY  = ry + (i % 7) * 22.f;
        bool  isOn  = *stops[i];

        sf::CircleShape dot(5.f);
        dot.setFillColor(isOn ? sf::Color(60, 200, 100) : sf::Color(60, 70, 80));
        dot.setPosition(col, rowY + 4.f);
        m_window.draw(dot);

        sf::Text lbl(kNames[i], m_font, 10);
        lbl.setFillColor(isOn ? sf::Color(180, 230, 190) : sf::Color(120, 135, 120));
        lbl.setPosition(col + 14.f, rowY + 2.f);
        drawText(lbl);
    }

    // ── Volume slider ──────────────────────────────────────────────────────────
    float sliderY = ry + 7 * 22.f + 12.f;  // below the second column of phase stops
    {
        constexpr float slW = pw - 32.f, slH = 10.f;
        float slX = px0 + 16.f;
        float vol = m_sound.volume() / 100.f;

        // Track
        sf::RectangleShape track({slW, slH});
        track.setPosition(slX, sliderY);
        track.setFillColor(sf::Color(30, 50, 40));
        track.setOutlineColor(sf::Color(50, 80, 60));
        track.setOutlineThickness(1.f);
        m_window.draw(track);

        // Fill
        sf::RectangleShape fill({slW * vol, slH});
        fill.setPosition(slX, sliderY);
        fill.setFillColor(m_sound.muted() ? sf::Color(80, 80, 80) : sf::Color(60, 180, 100));
        m_window.draw(fill);

        // Thumb
        sf::CircleShape thumb(7.f);
        thumb.setPosition(slX + slW * vol - 7.f, sliderY - 2.f);
        thumb.setFillColor(sf::Color(180, 230, 190));
        m_window.draw(thumb);

        // Labels
        sf::Text volLbl("Volume: " + std::to_string(static_cast<int>(m_sound.volume())),
                        m_font, 9);
        volLbl.setFillColor(sf::Color(160, 180, 165));
        volLbl.setPosition(slX, sliderY - 14.f);
        drawText(volLbl);

        // Mute button
        float muteX = slX + slW + 8.f;
        sf::RectangleShape mute({32.f, 14.f});
        mute.setPosition(muteX, sliderY - 2.f);
        mute.setFillColor(m_sound.muted() ? sf::Color(100, 30, 30) : sf::Color(30, 60, 45));
        mute.setOutlineColor(sf::Color(60, 90, 70));
        mute.setOutlineThickness(1.f);
        m_window.draw(mute);
        sf::Text muteT(m_sound.muted() ? "ON" : "OFF", m_font, 8);
        muteT.setFillColor(sf::Color(200, 215, 205));
        auto mb = muteT.getLocalBounds();
        muteT.setPosition(muteX + (32.f - mb.width) * 0.5f, sliderY);
        drawText(muteT);

        // Priority chime toggle (one row below the volume slider)
        float chimeY = sliderY + 24.f;
        sf::Text chLbl("Priority chime:", m_font, 9);
        chLbl.setFillColor(sf::Color(160, 180, 165));
        chLbl.setPosition(slX, chimeY + 1.f);
        drawText(chLbl);
        float chBtnX = slX + 92.f;
        sf::RectangleShape chBtn({40.f, 16.f});
        chBtn.setPosition(chBtnX, chimeY - 1.f);
        chBtn.setFillColor(m_priorityChime ? sf::Color(30, 60, 45) : sf::Color(100, 30, 30));
        chBtn.setOutlineColor(sf::Color(60, 90, 70));
        chBtn.setOutlineThickness(1.f);
        m_window.draw(chBtn);
        sf::Text chT(m_priorityChime ? "ON" : "OFF", m_font, 8);
        chT.setFillColor(sf::Color(200, 215, 205));
        auto chb = chT.getLocalBounds();
        chT.setPosition(chBtnX + (40.f - chb.width) * 0.5f, chimeY + 1.f);
        drawText(chT);
    }

    // Gameplay section
    {
        using namespace Layout;
        constexpr float pw = 400.f, ph = 480.f;
        constexpr float px0 = (WIN_W - pw) * 0.5f;
        constexpr float py0 = (WIN_H - ph) * 0.5f;
        float gy = py0 + ph - 130.f;

        sf::Text gHdr("[Gameplay]", m_font, 10);
        gHdr.setFillColor(sf::Color(130, 150, 135));
        gHdr.setPosition(px0 + 14.f, gy);
        drawText(gHdr);
        gy += 14.f;

        // Auto-save interval
        sf::Text asHdr("Auto-save every N turns (0=off):", m_font, 9);
        asHdr.setFillColor(sf::Color(110, 130, 115));
        asHdr.setPosition(px0 + 14.f, gy);
        drawText(asHdr);
        for (int iv : {0, 3, 5, 10}) {
            float bx2 = px0 + 14.f + (iv == 0 ? 0.f : (iv == 3 ? 68.f : (iv == 5 ? 116.f : 156.f)));
            sf::RectangleShape btn2({48.f, 18.f});
            btn2.setPosition(bx2 + 180.f, gy - 1.f);
            btn2.setFillColor(m_autoSaveInterval == iv ? sf::Color(30,80,50) : sf::Color(25,35,30));
            btn2.setOutlineColor(sf::Color(55,95,65));
            btn2.setOutlineThickness(1.f);
            m_window.draw(btn2);
            sf::Text btnT((iv == 0 ? "OFF" : std::to_string(iv)), m_font, 8);
            btnT.setFillColor(m_autoSaveInterval == iv ? sf::Color(140,220,160) : sf::Color(100,120,105));
            btnT.setPosition(bx2 + 188.f, gy + 3.f);
            drawText(btnT);
        }
    }

    // Resolution selector
    {
        using namespace Layout;
        constexpr float pw = 400.f, ph = 480.f;
        constexpr float px0 = (WIN_W - pw) * 0.5f;
        constexpr float py0 = (WIN_H - ph) * 0.5f;
        float ry = py0 + ph - 70.f;
        sf::Text resHdr("Window Size (restart needed):", m_font, 10);
        resHdr.setFillColor(sf::Color(130, 150, 135));
        resHdr.setPosition(px0 + 14.f, ry);
        drawText(resHdr);
        ry += 16.f;
        static const struct { unsigned w, h; const char* label; } kRes[] = {
            {1280, 720, "1280x720"}, {1560, 800, "1560x800 (default)"},
            {1920, 1080, "1920x1080"}
        };
        for (int ri = 0; ri < 3; ++ri) {
            sf::RectangleShape rb({105.f, 22.f});
            rb.setPosition(px0 + 14.f + ri * 110.f, ry);
            auto wsz = m_window.getSize();
            bool cur = (wsz.x == kRes[ri].w && wsz.y == kRes[ri].h);
            rb.setFillColor(cur ? sf::Color(30, 80, 50) : sf::Color(25, 35, 30));
            rb.setOutlineColor(sf::Color(60, 100, 70));
            rb.setOutlineThickness(1.f);
            m_window.draw(rb);
            sf::Text rlt(kRes[ri].label, m_font, 8);
            rlt.setFillColor(cur ? sf::Color(140, 220, 160) : sf::Color(120, 140, 125));
            rlt.setPosition(px0 + 16.f + ri * 110.f, ry + 6.f);
            drawText(rlt);
        }
    }
}

void GameWindow::startSpectate() {
    // In spectate mode, the human passes all priority immediately and
    // both players are driven by the AI.  We re-use the existing AI turn
    // infrastructure: just put the game into the AI's hands.
    m_appState = AppState::Spectate;
    m_turn     = WhosTurn::AI;
    addLog("[Spectate] Watching AI-vs-AI.");
}

void GameWindow::snapshotUndo() {
    // Write into the ring buffer at m_undoHead
    auto& slot = m_undoStack[static_cast<size_t>(m_undoHead)];
    slot.game  = m_game.clone();
    slot.tm    = m_tm.saveSnapshot();
    slot.valid = true;
    m_undoHead = (m_undoHead + 1) % kUndoDepth;
    if (m_undoSize < kUndoDepth) ++m_undoSize;
}

void GameWindow::doUndo() {
    if (m_undoSize == 0) return;
    // Pop the most-recently written slot
    m_undoHead = (m_undoHead + kUndoDepth - 1) % kUndoDepth;
    auto& slot = m_undoStack[static_cast<size_t>(m_undoHead)];
    if (!slot.valid) { m_undoSize = 0; return; }

    m_game = std::move(slot.game);
    m_tm.restoreSnapshot(slot.tm);
    slot.valid = false;
    --m_undoSize;

    m_renderer.init(m_font, m_game, m_tm, m_picsDir);
    m_human.resetToMainPhase();
    m_human.setPhaseStops(m_stops);
    m_turn = WhosTurn::Human;
    addLog("Undo (" + std::to_string(m_undoSize) + " more available).");
}

// ── Key bindings ──────────────────────────────────────────────────────────────

void GameWindow::initKeyBindings() {
    // Default bindings
    m_keyBindings["pass_priority"] = sf::Keyboard::Space;
    m_keyBindings["concede"]       = sf::Keyboard::C;
    m_keyBindings["my_gy"]         = sf::Keyboard::G;
    m_keyBindings["opp_gy"]        = sf::Keyboard::H;
    m_keyBindings["alpha_strike"]  = sf::Keyboard::A;
    m_keyBindings["tap_lands"]     = sf::Keyboard::T;
    m_keyBindings["options"]       = sf::Keyboard::O;
    m_keyBindings["undo"]          = sf::Keyboard::Z;   // Ctrl+Z
    m_keyBindings["search"]        = sf::Keyboard::F;   // Ctrl+F
    m_keyBindings["zoom"]          = sf::Keyboard::Unknown; // right-click only
    loadKeyBindings();
}

void GameWindow::saveKeyBindings() {
    namespace fs = std::filesystem;
    if (const char* ap = std::getenv("APPDATA")) {
        std::ofstream f(fs::path(ap) / "CitadelMTG" / "keybindings.txt");
        for (const auto& [action, key] : m_keyBindings)
            f << action << "=" << static_cast<int>(key) << '\n';
    }
}

void GameWindow::loadKeyBindings() {
    namespace fs = std::filesystem;
    if (const char* ap = std::getenv("APPDATA")) {
        std::ifstream f(fs::path(ap) / "CitadelMTG" / "keybindings.txt");
        std::string line;
        while (std::getline(f, line)) {
            auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string action = line.substr(0, eq);
            int keyInt = 0;
            try { keyInt = std::stoi(line.substr(eq + 1)); } catch (...) { continue; }
            if (m_keyBindings.count(action))
                m_keyBindings[action] = static_cast<sf::Keyboard::Key>(keyInt);
        }
    }
}

// ── Goldfish mode ─────────────────────────────────────────────────────────────

// ── Puzzle mode ───────────────────────────────────────────────────────────────

void GameWindow::startPuzzleMode(const std::filesystem::path& puzzleFile) {
    // Puzzles are saved game states with a "puzzle_title" field.
    // Load the state, show the title, and let the player solve "win this turn".
    if (!loadGame(puzzleFile)) return;
    m_puzzleMode = true;

    // Read puzzle title from the JSON
    try {
        auto j = nlohmann::json::parse(std::ifstream(puzzleFile));
        m_puzzleTitle = j.value("puzzle_title", "Win This Turn");
    } catch (...) {
        m_puzzleTitle = "Win This Turn";
    }
    addLog("PUZZLE: " + m_puzzleTitle + "  [Win to solve, Esc to exit]");
    m_appState = AppState::Playing;

    // Check at end of each turn if the player won
}

// ── Turn history overlay ──────────────────────────────────────────────────────

void GameWindow::renderTurnHistory() {
    using namespace Layout;
    constexpr float pw = 420.f, ph = 450.f;
    float px0 = (WIN_W - pw) * 0.5f, py0 = (WIN_H - ph) * 0.5f;

    sf::RectangleShape dim({WIN_W, WIN_H});
    dim.setFillColor(sf::Color(0, 0, 0, 180));
    m_window.draw(dim);
    sf::RectangleShape pan({pw, ph});
    pan.setPosition(px0, py0);
    pan.setFillColor(sf::Color(11, 10, 9, 252));
    pan.setOutlineColor(sf::Color(203, 163, 90));
    pan.setOutlineThickness(1.f);
    m_window.draw(pan);

    sf::Text hdr("TURN HISTORY  (T=hide)", m_font, 12);
    hdr.setStyle(sf::Text::Bold);
    hdr.setFillColor(sf::Color(230, 193, 112));
    hdr.setPosition(px0 + 10.f, py0 + 8.f);
    drawText(hdr);

    float ry = py0 + 30.f;
    int start = std::max(0, (int)m_turnHistory.size() - 14);
    for (int i = start; i < (int)m_turnHistory.size() && ry < py0 + ph - 10.f; ++i) {
        const auto& ts = m_turnHistory[i];
        sf::Color col = (ts.activePlayer.find("Alice") != std::string::npos)
                      ? sf::Color(203, 163, 90) : sf::Color(217, 116, 63);
        std::string summary = "T" + std::to_string(ts.turnNum) + " " +
                              ts.activePlayer + " | " +
                              std::to_string(ts.spellsCast) + " spells";
        sf::Text tl(summary, m_font, 10);
        tl.setFillColor(col);
        tl.setPosition(px0 + 8.f, ry);
        drawText(tl);
        ry += 13.f;
        // Show notable cards
        for (size_t ci = 0; ci < std::min((size_t)2, ts.notableCards.size()); ++ci) {
            sf::Text cl("  " + ts.notableCards[ci], m_font, 8);
            cl.setFillColor(sf::Color(148, 139, 124));
            cl.setPosition(px0 + 8.f, ry);
            drawText(cl);
            ry += 11.f;
        }
    }
}

// ── Life chart overlay ────────────────────────────────────────────────────────

void GameWindow::renderLifeChart() {
    using namespace Layout;
    if (m_lifeHistory.empty()) return;
    constexpr float pw = 500.f, ph = 300.f;
    float px0 = (WIN_W - pw) * 0.5f, py0 = (WIN_H - ph) * 0.5f;

    sf::RectangleShape dim({WIN_W, WIN_H});
    dim.setFillColor(sf::Color(0, 0, 0, 180));
    m_window.draw(dim);
    sf::RectangleShape pan({pw, ph});
    pan.setPosition(px0, py0);
    pan.setFillColor(sf::Color(11, 10, 9, 252));
    pan.setOutlineColor(sf::Color(203, 163, 90));
    pan.setOutlineThickness(1.f);
    m_window.draw(pan);

    sf::Text hdr("LIFE TOTAL HISTORY  (L to close)", m_font, 12);
    hdr.setStyle(sf::Text::Bold);
    hdr.setFillColor(sf::Color(230, 193, 112));
    hdr.setPosition(px0 + 10.f, py0 + 8.f);
    drawText(hdr);

    float chartX = px0 + 30.f, chartY = py0 + 35.f;
    float chartW = pw - 50.f, chartH = ph - 55.f;

    // Find min/max life
    int minLife = 40, maxLife = 40;
    for (const auto& e : m_lifeHistory) {
        minLife = std::min({minLife, e[0], e[1]});
        maxLife = std::max({maxLife, e[0], e[1]});
    }
    if (maxLife == minLife) maxLife = minLife + 1;

    auto lifeToY = [&](int life) -> float {
        return chartY + chartH - (life - minLife) * chartH / (maxLife - minLife);
    };
    auto turnToX = [&](int t) -> float {
        int n = static_cast<int>(m_lifeHistory.size());
        return chartX + static_cast<float>(t) * chartW / std::max(1, n - 1);
    };

    // Grid lines
    for (int g = 0; g <= 4; ++g) {
        float gy = chartY + g * chartH / 4.f;
        sf::Vertex line[2] = {{{chartX, gy}, sf::Color(50,45,40)},
                               {{chartX + chartW, gy}, sf::Color(50,45,40)}};
        m_window.draw(line, 2, sf::Lines);
        int lifeLabel = maxLife - g * (maxLife - minLife) / 4;
        sf::Text gt(std::to_string(lifeLabel), m_font, 8);
        gt.setFillColor(sf::Color(100, 95, 85));
        gt.setPosition(px0 + 4.f, gy - 5.f);
        drawText(gt);
    }

    // Draw life lines for each player
    sf::Color cols[2] = {sf::Color(203, 163, 90), sf::Color(217, 116, 63)};
    for (int pid = 0; pid < 2; ++pid) {
        for (int i = 1; i < (int)m_lifeHistory.size(); ++i) {
            sf::Vertex seg[2] = {
                {{turnToX(i-1), lifeToY(m_lifeHistory[i-1][pid])}, cols[pid]},
                {{turnToX(i),   lifeToY(m_lifeHistory[i][pid])},   cols[pid]}
            };
            m_window.draw(seg, 2, sf::Lines);
        }
        std::string label = (pid == 0 ? "Alice: " : "Bob: ") +
                            std::to_string(m_lifeHistory.back()[pid]);
        sf::Text lt(label, m_font, 9);
        lt.setFillColor(cols[pid]);
        lt.setPosition(chartX + chartW + 4.f, (pid == 0 ? chartY : chartY + 14.f));
        drawText(lt);
    }
}

void GameWindow::startGoldfishMode() {
    // Load the player's deck, shuffle, draw 7 — no opponent, no game rules
    m_goldfishMode = true;
    m_goldfishDraws = 1;
    m_game.reset();
    m_game.setCardDb(&m_db);
    m_game.player(0) = Player{0, "Alice"};
    if (!m_rematchDeck0.empty()) DeckLoader::loadAndBuild(m_rematchDeck0, m_db, m_game, 0);
    m_game.player(0).library().shuffle(m_game.rng());
    for (int i = 0; i < 7; ++i) {
        auto& lib = m_game.player(0).library();
        if (lib.empty()) break;
        m_game.moveToZone(lib.front()->id, ZoneType::Hand, 0);
    }
    m_renderer.init(m_font, m_game, m_tm, m_picsDir);
    addLog("Goldfish mode: Hand #" + std::to_string(m_goldfishDraws) + ". [R]=redraw, [Esc]=exit.");
}

void GameWindow::renderGoldfishOverlay() {
    using namespace Layout;
    // Show a small banner with controls
    sf::RectangleShape bar({WIN_W, 28.f});
    bar.setFillColor(sf::Color(14, 10, 6, 220));
    bar.setPosition(0.f, 0.f);
    m_window.draw(bar);
    sf::Text info("GOLDFISH MODE  |  Hand #" + std::to_string(m_goldfishDraws) +
                  "  |  R = new hand  |  Esc = exit", m_font, 11);
    info.setStyle(sf::Text::Bold);
    info.setFillColor(sf::Color(203, 163, 90));
    info.setPosition(10.f, 6.f);
    ui::applyTextScale(info);
    drawText(info);
}

void GameWindow::showToast(const std::string& msg, float duration) {
    m_toast.msg = msg;
    m_toast.ttl = duration;
}

void GameWindow::renderToast() {
    if (m_toast.ttl <= 0.f || m_toast.msg.empty()) return;

    using namespace Layout;
    constexpr float H = 34.f;
    float alpha = std::min(1.f, m_toast.ttl * 2.f);   // fade in/out
    if (m_toast.ttl < 0.5f) alpha = m_toast.ttl / 0.5f;

    sf::RectangleShape bg({PLAY_W * 0.6f, H});
    bg.setPosition(PLAY_X + PLAY_W * 0.2f, BOB_INFO_Y + BOB_INFO_H + 6.f);
    bg.setFillColor(sf::Color(30, 20, 12, static_cast<sf::Uint8>(200 * alpha)));
    bg.setOutlineColor(sf::Color(203, 163, 90, static_cast<sf::Uint8>(150 * alpha)));
    bg.setOutlineThickness(1.f);
    m_window.draw(bg);

    sf::Text txt(m_toast.msg, m_font, 12);
    txt.setStyle(sf::Text::Bold);
    txt.setFillColor(sf::Color(230, 193, 112, static_cast<sf::Uint8>(255 * alpha)));
    auto tb = txt.getLocalBounds();
    txt.setPosition(PLAY_X + PLAY_W * 0.2f + (PLAY_W * 0.6f - tb.width) * 0.5f - tb.left,
                    BOB_INFO_Y + BOB_INFO_H + 6.f + (H - tb.height) * 0.5f - tb.top);
    ui::applyTextScale(txt);
    drawText(txt);
}

// ── Statistics overlay ────────────────────────────────────────────────────────

void GameWindow::renderStatsOverlay() {
    using namespace Layout;
    constexpr float pw = 560.f, ph = 420.f;
    float px0 = (WIN_W - pw) * 0.5f, py0 = (WIN_H - ph) * 0.5f;

    sf::RectangleShape dim({WIN_W, WIN_H});
    dim.setFillColor(sf::Color(0, 0, 0, 180));
    m_window.draw(dim);

    sf::RectangleShape panel({pw, ph});
    panel.setPosition(px0, py0);
    panel.setFillColor(sf::Color(18, 17, 16, 252));
    panel.setOutlineColor(sf::Color(203, 163, 90));
    panel.setOutlineThickness(2.f);
    m_window.draw(panel);

    auto drawLine = [&](const std::string& s, float x, float y, unsigned sz,
                        sf::Color col, bool bold = false) {
        sf::Text t(s, m_font, sz);
        if (bold) t.setStyle(sf::Text::Bold);
        t.setFillColor(col);
        t.setPosition(x, y);
        ui::applyTextScale(t);
        drawText(t);
    };

    drawLine("SESSION STATISTICS", px0 + 16.f, py0 + 12.f, 16, sf::Color(230, 193, 112), true);

    float ry = py0 + 42.f;
    int totalGames = 0, totalWins = 0;
    for (const auto& [key, s] : m_deckStats) { totalGames += s.wins + s.losses + s.draws; totalWins += s.wins; }

    drawLine("Total games:  " + std::to_string(totalGames), px0 + 16.f, ry, 12, sf::Color(241, 234, 220));
    ry += 18.f;
    if (totalGames > 0) {
        drawLine("Win rate:     " + std::to_string(totalWins * 100 / totalGames) + "%",
                 px0 + 16.f, ry, 12, sf::Color(109, 185, 127));
        ry += 18.f;
    }

    drawLine("BY MATCHUP:", px0 + 16.f, ry + 8.f, 11, sf::Color(148, 139, 124), true);
    ry += 28.f;

    int shown = 0;
    for (const auto& [key, s] : m_deckStats) {
        if (shown >= 10 || ry > py0 + ph - 50.f) break;
        int tot = s.wins + s.losses + s.draws;
        if (tot == 0) continue;
        std::string label = key.size() > 40 ? key.substr(0, 38) + ".." : key;
        std::string rec   = std::to_string(s.wins) + "W/" + std::to_string(s.losses) + "L";
        if (s.draws > 0) rec += "/" + std::to_string(s.draws) + "D";
        drawLine(label, px0 + 16.f, ry, 10, sf::Color(199, 189, 172));
        drawLine(rec, px0 + pw - 80.f, ry, 10, sf::Color(203, 163, 90), true);
        ry += 15.f;
        ++shown;
    }

    // Per-commander stats
    ry += 10.f;
    drawLine("BY COMMANDER:", px0 + 16.f, ry, 11, sf::Color(148, 139, 124), true);
    ry += 18.f;
    // Aggregate wins/losses by commanderName
    std::unordered_map<std::string, std::pair<int,int>> byCmd;  // name -> (wins, losses)
    for (const auto& [k, s] : m_deckStats) {
        if (s.commanderName.empty()) continue;
        byCmd[s.commanderName].first  += s.wins;
        byCmd[s.commanderName].second += s.losses;
    }
    int cmdShown = 0;
    for (const auto& [cmd, wl] : byCmd) {
        if (cmdShown >= 5 || ry > py0 + ph - 30.f) break;
        std::string cname = cmd.size() > 30 ? cmd.substr(0, 28) + ".." : cmd;
        std::string rec   = std::to_string(wl.first) + "W/" + std::to_string(wl.second) + "L";
        drawLine(cname, px0 + 16.f, ry, 9, sf::Color(199, 189, 172));
        drawLine(rec, px0 + pw - 70.f, ry, 9, sf::Color(109, 185, 127));
        ry += 14.f;
        ++cmdShown;
    }

    drawLine("Tab=stats  Ctrl+A=achievements  Esc=close",
             px0 + 8.f, py0 + ph - 16.f, 8, sf::Color(107, 99, 87));
}

// ── Achievement system ────────────────────────────────────────────────────────

void GameWindow::initAchievements() {
    m_achievements = {
        {"first_win",      "First Victory",         "Win your first game.",            false},
        {"cmd_damage",     "Commander's Will",       "Win via 21 commander damage.",    false},
        {"played_10",      "Dedicated",              "Play 10 games.",                  false},
        {"played_50",      "Veteran",                "Play 50 games.",                  false},
        {"50pct_win",      "Contender",              "Reach 50% win rate (min 10 games).",false},
        {"poison",         "Toxic",                  "Win with poison counters.",        false},
        {"flashback",      "From the Ashes",         "Cast 5 spells via Flashback.",    false},
        {"graveyard",      "Graveyard Enthusiast",   "Have 20+ cards in GY at once.",   false},
        {"full_hand",      "Card Advantage",         "Have 10+ cards in hand.",         false},
        {"no_lands",       "No Land Challenge",      "Win without playing a land.",     false},
    };
    // Load persisted state from APPDATA
    namespace fs = std::filesystem;
    if (const char* ap = std::getenv("APPDATA")) {
        std::ifstream f(fs::path(ap) / "CitadelMTG" / "achievements.txt");
        std::string id;
        while (std::getline(f, id)) {
            for (auto& a : m_achievements)
                if (a.id == id) { a.unlocked = true; break; }
        }
    }
}

void GameWindow::checkAchievements() {
    bool anyNew = false;
    auto unlock = [&](const std::string& id) {
        for (auto& a : m_achievements)
            if (a.id == id && !a.unlocked) { a.unlocked = true; anyNew = true; addLog("Achievement: " + a.name + "!"); }
    };

    int totalGames = 0, totalWins = 0;
    for (const auto& [k, s] : m_deckStats) { totalGames += s.wins + s.losses + s.draws; totalWins += s.wins; }

    if (totalWins >= 1) unlock("first_win");
    if (totalGames >= 10) unlock("played_10");
    if (totalGames >= 50) unlock("played_50");
    if (totalGames >= 10 && totalWins * 100 / totalGames >= 50) unlock("50pct_win");
    if (m_game.player(0).hand().size() >= 10) unlock("full_hand");
    for (uint8_t pid = 0; pid < 2; ++pid)
        if (m_game.player(pid).graveyard().size() >= 20) unlock("graveyard");

    if (anyNew) {
        namespace fs = std::filesystem;
        if (const char* ap = std::getenv("APPDATA")) {
            std::error_code ec;
            fs::create_directories(fs::path(ap) / "CitadelMTG", ec);
            std::ofstream f(fs::path(ap) / "CitadelMTG" / "achievements.txt");
            for (const auto& a : m_achievements)
                if (a.unlocked) f << a.id << '\n';
        }
    }
}

void GameWindow::renderAchievementsOverlay() {
    using namespace Layout;
    constexpr float pw = 500.f, ph = 420.f;
    float px0 = (WIN_W - pw) * 0.5f, py0 = (WIN_H - ph) * 0.5f;

    sf::RectangleShape dim({WIN_W, WIN_H});
    dim.setFillColor(sf::Color(0, 0, 0, 180));
    m_window.draw(dim);

    sf::RectangleShape panel({pw, ph});
    panel.setPosition(px0, py0);
    panel.setFillColor(sf::Color(18, 17, 16, 252));
    panel.setOutlineColor(sf::Color(203, 163, 90));
    panel.setOutlineThickness(2.f);
    m_window.draw(panel);

    auto drawT = [&](const std::string& s, float x, float y, unsigned sz, sf::Color col, bool bold = false) {
        sf::Text t(s, m_font, sz);
        if (bold) t.setStyle(sf::Text::Bold);
        t.setFillColor(col);
        t.setPosition(x, y);
        ui::applyTextScale(t);
        drawText(t);
    };

    int unlocked = 0;
    for (const auto& a : m_achievements) if (a.unlocked) ++unlocked;
    drawT("ACHIEVEMENTS  (" + std::to_string(unlocked) + "/" +
          std::to_string(m_achievements.size()) + ")",
          px0 + 16.f, py0 + 12.f, 14, sf::Color(230, 193, 112), true);

    float ry = py0 + 42.f;
    for (const auto& a : m_achievements) {
        if (ry > py0 + ph - 30.f) break;
        sf::Color ic = a.unlocked ? sf::Color(109, 185, 127) : sf::Color(60, 55, 50);
        sf::Color tc = a.unlocked ? sf::Color(241, 234, 220) : sf::Color(100, 95, 85);
        drawT(a.unlocked ? "[X]" : "[ ]", px0 + 10.f, ry, 10, ic, a.unlocked);
        drawT(a.name, px0 + 34.f, ry, 11, tc, a.unlocked);
        drawT(a.desc, px0 + 34.f, ry + 12.f, 9, sf::Color(100, 95, 85));
        ry += 30.f;
    }
    drawT("Press Esc to close", px0 + pw * 0.5f - 50.f, py0 + ph - 20.f, 9, sf::Color(107, 99, 87));
}

// ── Collection statistics overlay ────────────────────────────────────────────

void GameWindow::renderCollectionStats() {
    using namespace Layout;
    constexpr float pw = 480.f, ph = 380.f;
    float px0 = (WIN_W - pw) * 0.5f, py0 = (WIN_H - ph) * 0.5f;

    sf::RectangleShape dim({WIN_W, WIN_H});
    dim.setFillColor(sf::Color(0, 0, 0, 180));
    m_window.draw(dim);
    sf::RectangleShape pan({pw, ph});
    pan.setPosition(px0, py0);
    pan.setFillColor(sf::Color(18, 17, 16, 252));
    pan.setOutlineColor(sf::Color(203, 163, 90));
    pan.setOutlineThickness(2.f);
    m_window.draw(pan);

    auto drawL = [&](const std::string& s, float x, float y, unsigned sz, sf::Color col, bool bold=false) {
        sf::Text t(s, m_font, sz);
        if (bold) t.setStyle(sf::Text::Bold);
        t.setFillColor(col);
        t.setPosition(x, y);
        ui::applyTextScale(t);
        drawText(t);
    };

    drawL("COLLECTION STATISTICS  (Ctrl+Q to close)", px0+14.f, py0+10.f, 13,
          sf::Color(230, 193, 112), true);

    int total = static_cast<int>(m_cardSeen.size());
    int dbSize = static_cast<int>(m_db.size());

    float ry = py0 + 38.f;
    drawL("Cards seen in games:  " + std::to_string(total), px0+14.f, ry, 12,
          sf::Color(241, 234, 220)); ry += 20.f;
    drawL("Total cards in DB:    " + std::to_string(dbSize), px0+14.f, ry, 12,
          sf::Color(241, 234, 220)); ry += 20.f;
    if (dbSize > 0)
        drawL("Coverage:             " + std::to_string(total * 100 / dbSize) + "%",
              px0+14.f, ry, 12, sf::Color(109, 185, 127)); ry += 28.f;

    // Top 10 most-seen cards
    drawL("MOST PLAYED CARDS:", px0+14.f, ry, 11, sf::Color(148, 139, 124), true);
    ry += 18.f;
    std::vector<std::pair<std::string,int>> sorted(m_cardSeen.begin(), m_cardSeen.end());
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b){
        return a.second > b.second;
    });
    int shown = 0;
    for (const auto& [name, cnt] : sorted) {
        if (shown >= 10 || ry > py0 + ph - 24.f) break;
        std::string nm = name.size() > 34 ? name.substr(0, 32) + ".." : name;
        drawL(nm, px0+14.f, ry, 10, sf::Color(199, 189, 172));
        drawL(std::to_string(cnt) + "x", px0+pw-50.f, ry, 10, sf::Color(203, 163, 90));
        ry += 14.f;
        ++shown;
    }
    drawL("Esc = close", px0+pw*0.5f-25.f, py0+ph-16.f, 8, sf::Color(107, 99, 87));
}

void GameWindow::doTapAllMana() {
    // Tap all untapped lands Alice controls to fill her mana pool.
    // Works from MainPhase or DeclareAttack (before declaring attackers).
    auto state = m_human.state();
    if (state != HumanState::MainPhase && state != HumanState::DeclareAttack) return;
    bool tapped = false;
    for (Card* c : m_game.battlefield().cards()) {
        if (c->controllerId == 0 && c->rules->type.isLand() && !c->tapped)
            if (m_abilities.activateManaAbility(c->id, 0)) tapped = true;
    }
    if (tapped) {
        addLog("Alice taps all lands for mana.");
        m_sound.play(SND_LAND_TAP);
    }
}

void GameWindow::doResolveAll() {
    // Resolve every item on the stack, giving Bob one chance to respond
    // at the end.  Stops early if a human-interaction overlay appears.
    auto anyPending = [&]() {
        return m_game.hasPendingDiscard()   || m_game.hasPendingCharm()  ||
               m_game.hasPendingRiot()      || m_game.hasPendingFabricate() ||
               m_game.hasPendingPayLife()   ||
               m_game.hasPendingSearch()    || m_game.hasPendingMadnessCast() ||
               m_human.pendingPlaneswalker() != kInvalidId;
    };

    int safety = 200;
    while (!m_abilities.stackEmpty() && !m_tm.isGameOver() && --safety > 0) {
        if (anyPending()) break;

        m_abilities.resolveTop();
        while (StateBasedActions::run(m_game)) {}
        m_abilities.drainPendingTriggers();

        if (anyPending()) break;
        if (m_game.hasPendingDiscard()) { handlePendingDiscard(); break; }
    }

    // Give Bob one final response window if the stack is now empty. Don't
    // tap anything speculatively — Bob's instant-cast loop calls canAfford()
    // and lazy-taps the sources for whatever it actually decides to cast.
    (void)0;
}

// ── Mulligan ──────────────────────────────────────────────────────────────────

void GameWindow::doMulliganForAi() {
    Player& bob = m_game.player(1);
    int lands = 0, lowCurve = 0, highOnly = 0;
    for (const Card* c : bob.hand().cards()) {
        if (!c->rules) continue;
        if (c->rules->type.isLand()) { ++lands; continue; }
        int cmc = c->rules->manaCost.cmc();
        if (cmc <= 3) ++lowCurve;   // early plays
        else          ++highOnly;   // no early plays
    }
    int handSz = static_cast<int>(bob.hand().size());
    // Keep if: 2-4 lands AND at least one early play, or running out of mulligans
    bool keep = (m_mull.aiMulls >= 2) ||
                (lands >= 2 && lands <= 4 && lowCurve >= 1) ||
                (lands >= 2 && lands <= 5 && handSz <= 5);  // keep any reasonable 5-card hand
    if (!keep) {
        ++m_mull.aiMulls;
        std::vector<ObjectId> ids;
        for (const Card* c : bob.hand().cards()) ids.push_back(c->id);
        for (ObjectId id : ids) m_game.moveToZone(id, ZoneType::Library, 1);
        bob.library().shuffle(m_game.rng());
        for (int i = 0; i < 7; ++i) {
            if (bob.library().empty()) break;
            m_game.moveToZone(bob.library().front()->id, ZoneType::Hand, 1);
        }
        doMulliganForAi();
    } else {
        m_mull.aiKept = true;
        if (m_mull.aiMulls > 0) aiPutToBottom(m_mull.aiMulls);
        std::string msg = "Bob keeps";
        if (m_mull.aiMulls > 0)
            msg += " (mulliganed " + std::to_string(m_mull.aiMulls) + "x)";
        addLog(msg + ".");
    }
}

void GameWindow::aiPutToBottom(int n) {
    Player& bob = m_game.player(1);
    // cards() returns by value (snapshot); calling it twice would yield iterators
    // from two different temporary vectors and trip the MSVC debug check
    // "iterators in range are from different containers".
    std::vector<Card*> hand = bob.hand().cards();
    int landCount = 0;
    for (const Card* c : hand)
        if (c->rules && c->rules->type.isLand()) ++landCount;

    // Sort by "least desirable": excess lands first, then lowest CMC
    std::sort(hand.begin(), hand.end(), [landCount](const Card* a, const Card* b) {
        bool al = a->rules && a->rules->type.isLand();
        bool bl = b->rules && b->rules->type.isLand();
        if (al != bl) {
            // Too many lands → bottom lands; too few → bottom non-lands
            return (landCount > 3) ? (al > bl) : (al < bl);
        }
        int ac = a->rules ? a->rules->manaCost.cmc() : 0;
        int bc = b->rules ? b->rules->manaCost.cmc() : 0;
        return ac < bc;
    });

    int count = std::min(n, (int)hand.size());
    std::vector<ObjectId> ids;
    for (int i = 0; i < count; ++i) ids.push_back(hand[i]->id);
    for (ObjectId id : ids) m_game.moveToZone(id, ZoneType::Library, 1);
}

void GameWindow::handleMulliganClick(float px, float py) {
    using namespace Layout;

    if (m_mull.toBottomCount > 0) {
        // "Put N to bottom" phase
        const float confirmX = PLAY_X + (PLAY_W - 160.f) * 0.5f;
        const float confirmY = WIN_H * 0.5f + 70.f;
        bool confirmHit = (px >= confirmX && px < confirmX + 160.f &&
                           py >= confirmY && py < confirmY + 44.f &&
                           (int)m_mull.toBottomSelected.size() == m_mull.toBottomCount);
        if (confirmHit) {
            for (ObjectId id : m_mull.toBottomSelected)
                m_game.moveToZone(id, ZoneType::Library, 0);
            m_mull.toBottomSelected.clear();
            m_mull.toBottomCount = 0;
            startGameFromMulligan();
            return;
        }
        // Click a hand card to toggle selection
        auto hit = m_renderer.hitTest(px, py);
        if (hit.id != kInvalidId && hit.zone == ZoneType::Hand && hit.player == 0) {
            auto& sel = m_mull.toBottomSelected;
            auto it = std::find(sel.begin(), sel.end(), hit.id);
            if (it != sel.end()) sel.erase(it);
            else if ((int)sel.size() < m_mull.toBottomCount) sel.push_back(hit.id);
        }
        return;
    }

    // Button positions (screen coords)
    const float btnY = WIN_H * 0.5f - 22.f;
    const float btnH = 44.f;
    const float btnW = 160.f;
    const float keepX = PLAY_X + PLAY_W * 0.65f - btnW * 0.5f;
    const float mullX = PLAY_X + PLAY_W * 0.35f - btnW * 0.5f;

    bool keepHit = (px >= keepX && px < keepX + btnW && py >= btnY && py < btnY + btnH);
    bool mullHit = (px >= mullX && px < mullX + btnW && py >= btnY && py < btnY + btnH);

    if (keepHit) {
        if (m_mull.humanMulls > 0) {
            m_mull.toBottomCount = m_mull.humanMulls;
        } else {
            startGameFromMulligan();
        }
    } else if (mullHit) {
        ++m_mull.humanMulls;
        Player& alice = m_game.player(0);
        std::vector<ObjectId> ids;
        for (const Card* c : alice.hand().cards()) ids.push_back(c->id);
        for (ObjectId id : ids) m_game.moveToZone(id, ZoneType::Library, 0);
        alice.library().shuffle(m_game.rng());
        for (int i = 0; i < 7; ++i) {
            if (alice.library().empty()) break;
            m_game.moveToZone(alice.library().front()->id, ZoneType::Hand, 0);
        }
    }
}

void GameWindow::renderMulligan() {
    using namespace Layout;
    m_window.setView(m_gameView);

    // Show the full board (Alice's hand is already rendered face-up)
    RenderHints hints{};
    hints.mousePos = m_mousePos;
    hints.phase    = "Mulligan";
    hints.logLines = std::vector<std::string>(m_gameLog.begin(), m_gameLog.end());
    // Highlight cards selected for bottoming
    for (ObjectId id : m_mull.toBottomSelected) hints.selectedCards.insert(id);
    prefetchVisibleArt();
    m_renderer.draw(m_window, hints);

    // Overlay panel ──────────────────────────────────────────────────────────
    const float OW = 520.f, OH = 100.f;
    const float OX = PLAY_X + (PLAY_W - OW) * 0.5f;
    const float OY = WIN_H * 0.5f - OH * 0.5f - 60.f;

    sf::RectangleShape panel({OW, OH});
    panel.setPosition(OX, OY);
    panel.setFillColor(sf::Color(15, 22, 32, 220));
    panel.setOutlineColor(sf::Color(70, 100, 140));
    panel.setOutlineThickness(2.f);
    m_window.draw(panel);

    // Title / instruction
    std::string instrTxt;
    if (m_mull.toBottomCount > 0) {
        int rem = m_mull.toBottomCount - (int)m_mull.toBottomSelected.size();
        instrTxt = "Put " + std::to_string(m_mull.toBottomCount) + " card" +
                   (m_mull.toBottomCount != 1 ? "s" : "") +
                   " to the bottom  (" + std::to_string(rem) + " remaining)";
    } else {
        instrTxt = "Opening Hand  (mulligan #" + std::to_string(m_mull.humanMulls) + ")";
    }
    sf::Text instr(instrTxt, m_font, 15);
    instr.setStyle(sf::Text::Bold);
    instr.setFillColor(sf::Color(200, 215, 235));
    auto ib = instr.getLocalBounds();
    instr.setPosition(OX + (OW - ib.width) * 0.5f, OY + 12.f);
    ui::applyTextScale(instr);
    drawText(instr);

    // AI status
    std::string aiTxt = "Bob: keeps";
    if (!m_mull.aiKept) aiTxt = "Bob: deciding...";
    else if (m_mull.aiMulls > 0)
        aiTxt = "Bob: keeps (mulliganed " + std::to_string(m_mull.aiMulls) + "x)";
    sf::Text aiSt(aiTxt, m_font, 12);
    aiSt.setFillColor(sf::Color(160, 170, 160));
    aiSt.setPosition(OX + 10.f, OY + 40.f);
    ui::applyTextScale(aiSt);
    drawText(aiSt);

    // Hand size
    size_t handSz = m_game.player(0).hand().size();
    sf::Text hsSt(std::to_string(handSz) + "-card hand", m_font, 12);
    hsSt.setFillColor(sf::Color(160, 170, 160));
    auto hb = hsSt.getLocalBounds();
    hsSt.setPosition(OX + OW - hb.width - 10.f, OY + 40.f);
    ui::applyTextScale(hsSt);
    drawText(hsSt);

    const float btnY = WIN_H * 0.5f - 22.f;
    const float btnH = 44.f;
    const float btnW = 160.f;

    if (m_mull.toBottomCount > 0) {
        // Confirm button (enabled when enough cards selected)
        bool ready = (int)m_mull.toBottomSelected.size() == m_mull.toBottomCount;
        const float confirmX = PLAY_X + (PLAY_W - btnW) * 0.5f;
        const float confirmY = btnY + 70.f;
        sf::Color cfill = ready ? sf::Color(30, 100, 50) : sf::Color(50, 50, 50);
        sf::RectangleShape cbtn({btnW, btnH});
        cbtn.setPosition(confirmX, confirmY);
        cbtn.setFillColor(cfill);
        cbtn.setOutlineColor(sf::Color(20, 25, 30));
        cbtn.setOutlineThickness(1.5f);
        m_window.draw(cbtn);
        sf::Text ctxt("Confirm", m_font, 14);
        ctxt.setStyle(sf::Text::Bold);
        ctxt.setFillColor(ready ? sf::Color(220, 240, 220) : sf::Color(100, 100, 100));
        auto cb = ctxt.getLocalBounds();
        ctxt.setPosition(confirmX + (btnW - cb.width) * 0.5f,
                         confirmY + (btnH - cb.height) * 0.5f - 2.f);
        ui::applyTextScale(ctxt);
        drawText(ctxt);
    } else {
        const float keepX = PLAY_X + PLAY_W * 0.65f - btnW * 0.5f;
        const float mullX = PLAY_X + PLAY_W * 0.35f - btnW * 0.5f;

        // KEEP button
        sf::RectangleShape kb({btnW, btnH});
        kb.setPosition(keepX, btnY);
        kb.setFillColor(sf::Color(20, 80, 40));
        kb.setOutlineColor(sf::Color(20, 25, 30));
        kb.setOutlineThickness(1.5f);
        m_window.draw(kb);
        sf::Text kt("Keep", m_font, 14);
        kt.setStyle(sf::Text::Bold);
        kt.setFillColor(sf::Color(180, 240, 180));
        auto klb = kt.getLocalBounds();
        kt.setPosition(keepX + (btnW - klb.width) * 0.5f,
                       btnY  + (btnH - klb.height) * 0.5f - 2.f);
        ui::applyTextScale(kt);
        drawText(kt);

        // MULLIGAN button
        sf::RectangleShape mb({btnW, btnH});
        mb.setPosition(mullX, btnY);
        mb.setFillColor(sf::Color(90, 30, 20));
        mb.setOutlineColor(sf::Color(20, 25, 30));
        mb.setOutlineThickness(1.5f);
        m_window.draw(mb);
        sf::Text mt("Mulligan", m_font, 14);
        mt.setStyle(sf::Text::Bold);
        mt.setFillColor(sf::Color(240, 180, 180));
        auto mlb = mt.getLocalBounds();
        mt.setPosition(mullX + (btnW - mlb.width) * 0.5f,
                       btnY  + (btnH - mlb.height) * 0.5f - 2.f);
        ui::applyTextScale(mt);
        drawText(mt);
    }

    m_window.display();
}

void GameWindow::startGameFromMulligan() {
    Player& alice = m_game.player(0);
    std::string msg = "Alice keeps a " + std::to_string((int)alice.hand().size()) + "-card hand";
    if (m_mull.humanMulls > 0)
        msg += " (mulliganed " + std::to_string(m_mull.humanMulls) + "x)";
    addLog(msg + ".");

    if (m_aliceGoesFirst) {
        addLog("Turn 1: Alice");
        m_human.setPhaseStops(m_stops);
        m_human.startHumanTurn();
        m_turn = WhosTurn::Human;
    } else {
        m_turn = WhosTurn::AI;
    }
    m_appState = AppState::Playing;
}

// ── Riot / Charm choice helpers ───────────────────────────────────────────────

// Compute button hit-test for modal overlays (Riot = 2 buttons, Charm = 2-4).
// Buttons are centered in the play area horizontally.
int GameWindow::hitChoiceButton(float px, float py, int numButtons) const {
    using namespace Layout;
    const float btnW = 160.f, btnH = 44.f, gap = 12.f;
    float totalW = numButtons * btnW + (numButtons - 1) * gap;
    float startX = PLAY_X + (PLAY_W - totalW) * 0.5f;
    float startY = WIN_H * 0.5f + 20.f;
    for (int i = 0; i < numButtons; ++i) {
        float bx = startX + i * (btnW + gap);
        if (px >= bx && px < bx + btnW && py >= startY && py < startY + btnH)
            return i;
    }
    return -1;
}

void GameWindow::completeRiotChoice(bool giveHaste) {
    if (!m_game.hasPendingRiot()) return;
    ObjectId id = m_game.pendingRiotCardId();
    m_game.clearPendingRiot();
    Card* c = m_game.findCard(id);
    if (!c || !c->isOnBattlefield()) return;
    if (giveHaste) {
        auto haste = static_cast<uint32_t>(KeywordAbility::Haste);
        c->tempKeywords  |= haste;
        c->keywordMask   |= haste;
        c->summoningSickness = false;
        addLog(c->name() + " gains Haste (Riot).");
    } else {
        c->addCounter("+1/+1", 1);
        addLog(c->name() + " enters with a +1/+1 counter (Riot).");
        std::vector<PendingTrigger> t;
        TriggerSystem::onCounterAdded(*c, "+1/+1", 1, m_game, t);
        m_game.queueTriggers(std::move(t));
        m_abilities.drainPendingTriggers();
    }
    m_game.recomputeStaticBonuses();
}

void GameWindow::completePayLifeChoice(bool pay) {
    if (!m_game.hasPendingPayLife()) return;
    ObjectId id    = m_game.pendingPayLifeCardId();
    int      amt   = m_game.pendingPayLifeAmount();
    uint8_t  payer = m_game.pendingPayLifePayer();
    m_game.clearPendingPayLife();
    Card* c = m_game.findCard(id);
    if (pay && m_game.player(payer).life() > amt) {
        m_game.loseLife(payer, amt);
        if (c) addLog(c->name() + " enters untapped (paid " + std::to_string(amt) + " life).");
    } else {
        if (c && c->isOnBattlefield()) c->tapped = true;   // declined / can't pay
        if (c) addLog(c->name() + " enters tapped.");
    }
    m_game.recomputeStaticBonuses();
}

void GameWindow::completeFabricateChoice(bool wantCounters) {
    if (!m_game.hasPendingFabricate()) return;
    ObjectId id  = m_game.pendingFabricateCardId();
    int      n   = m_game.pendingFabricateAmount();
    m_game.clearPendingFabricate();
    Card* c = m_game.findCard(id);
    if (wantCounters || !c || !c->isOnBattlefield()) {
        // Put +1/+1 counters (also fallback if card no longer on battlefield)
        if (c && c->isOnBattlefield()) {
            c->addCounter("+1/+1", n);
            std::vector<PendingTrigger> t;
            TriggerSystem::onCounterAdded(*c, "+1/+1", n, m_game, t);
            m_game.queueTriggers(std::move(t));
            addLog(c->name() + " enters with " + std::to_string(n) + " +1/+1 counter(s) (Fabricate).");
        }
    } else {
        // Create N 1/1 colorless Servo artifact creature tokens
        for (int i = 0; i < n; ++i)
            m_game.createToken("Servo", "Artifact Creature Servo", 0, "1", "1", 0);
        addLog(c->name() + " creates " + std::to_string(n) + " Servo token(s) (Fabricate).");
    }
    while (StateBasedActions::run(m_game)) {}
    m_abilities.drainPendingTriggers();
    m_game.recomputeStaticBonuses();
}

void GameWindow::completeCharmChoice(int modeIndex) {
    if (!m_game.hasPendingCharm()) return;
    PendingCharmChoice charm = m_game.pendingCharm();
    m_game.clearPendingCharm();
    if (modeIndex < 0 || modeIndex >= (int)charm.bodies.size()) return;
    const std::string& body = charm.bodies[modeIndex];
    if (body.empty()) return;
    auto script = parseScriptLine(body);
    if (script.empty()) return;
    Card* src = m_game.findCard(charm.sourceId);
    EffectContext ctx{ m_game, src, charm.controller, charm.targets, charm.xValue };
    executeEffect(script, ctx);
    while (StateBasedActions::run(m_game)) {}
    m_abilities.drainPendingTriggers();
    if (!charm.labels.empty() && modeIndex < (int)charm.labels.size())
        addLog("Charm: " + charm.labels[modeIndex] + " resolved.");
}

void GameWindow::completeMadnessCast(bool wantCast) {
    if (!m_game.hasPendingMadnessCast()) return;
    const auto& pm = m_game.pendingMadnessCast();
    ObjectId id   = pm.cardId;
    uint8_t  ctrl = pm.ctrl;
    std::string name = pm.cardName;
    m_game.clearPendingMadnessCast();
    if (wantCast) {
        m_abilities.castMadnessCard(id, ctrl);
        addLog(name + " cast via Madness.");
    } else {
        if (m_game.findCard(id))
            m_game.moveToZone(id, ZoneType::Graveyard, ctrl);
        addLog(name + " put in graveyard (Madness declined).");
    }
    while (StateBasedActions::run(m_game)) {}
    m_abilities.drainPendingTriggers();
    m_game.recomputeStaticBonuses();
    m_human.checkResumeCleanup(); // resume end-of-turn cleanup if deferred for Madness
}

// ── Library search ────────────────────────────────────────────────────────────

void GameWindow::rebuildSearchChoices() {
    m_searchChoices.clear();
    if (!m_game.hasPendingSearch()) return;
    const auto& ps = m_game.pendingSearch();
    const Player& p = m_game.player(ps.libPlayer);
    for (const Card* c : p.library().cards())
        if (cardMatchesAnyFilter(*c, ps.filter, ps.libPlayer))
            m_searchChoices.push_back(c->id);
}

// ── Forced discard helpers ────────────────────────────────────────────────────

void GameWindow::completeDiscardChoice(ObjectId cardId) {
    if (!m_game.hasPendingDiscard()) return;
    Card* c = m_game.findCard(cardId);
    if (!c || c->controllerId != 0) return;
    ObjectId cid = c->id;
    Card* inGY = m_game.moveToZone(cid, ZoneType::Graveyard, 0);
    if (inGY) {
        std::vector<PendingTrigger> t;
        TriggerSystem::onDiscard(*inGY, 0, m_game, t);
        m_game.queueTriggers(std::move(t));
    }
    m_game.decrementPendingDiscard();
    if (!m_game.hasPendingDiscard()) {
        while (StateBasedActions::run(m_game)) {}
        m_abilities.drainPendingTriggers();
        addLog("Alice discards " + (inGY ? inGY->name() : "a card") + ".");
    }
}

void GameWindow::handlePendingDiscard() {
    while (m_game.hasPendingDiscard() && m_window.isOpen()) {
        sf::Event ev;
        while (m_window.pollEvent(ev)) {
            if (ev.type == sf::Event::Closed)  { m_window.close(); return; }
            if (ev.type == sf::Event::Resized) { updateView(); }
            if (ev.type == sf::Event::MouseMoved)
                m_mousePos = mapMousePos(ev.mouseMove.x, ev.mouseMove.y);
            if (ev.type == sf::Event::MouseButtonPressed &&
                ev.mouseButton.button == sf::Mouse::Left) {
                auto mapped = mapMousePos(ev.mouseButton.x, ev.mouseButton.y);
                auto hit = m_renderer.hitTest(mapped.x, mapped.y);
                if (hit.id != kInvalidId && hit.zone == ZoneType::Hand && hit.player == 0)
                    completeDiscardChoice(hit.id);
            }
        }
        render();
    }
}

void GameWindow::completeManaChoice(char color) {
    if (!m_game.hasPendingManaChoice()) return;
    const auto choice = m_game.pendingManaChoice();  // copy before clearing
    m_game.clearPendingManaChoice();
    ManaPool& pool = m_game.player(choice.controller).manaPool();
    using namespace mtg::ManaAtom;
    ManaCostShard sh = ManaCostShard::WHITE;
    switch (color) {
        case 'W': sh = ManaCostShard::WHITE; break;
        case 'U': sh = ManaCostShard::BLUE;  break;
        case 'B': sh = ManaCostShard::BLACK; break;
        case 'R': sh = ManaCostShard::RED;   break;
        case 'G': sh = ManaCostShard::GREEN; break;
        default: pool.addGeneric(choice.amount); return;
    }
    pool.add(sh, choice.amount);
}

void GameWindow::completeChooseType(const std::string& type) {
    if (!m_game.hasPendingChooseType()) return;
    const auto choice = m_game.pendingChooseType();  // copy before clearing
    m_game.clearPendingChooseType();
    if (Card* c = m_game.findCard(choice.cardId))
        c->chosenType = type;
    // Mirror to the global slot for any consumer that reads chosenTypeName.
    m_game.chosenTypeName = type;
    m_game.recomputeStaticBonuses();
    addLog("Chose creature type: " + type + ".");
}

void GameWindow::completeScryChoice(bool keepTop) {
    if (!m_game.hasPendingScry()) return;
    auto& sc = m_game.pendingScry();
    if (sc.looking.empty()) return;
    ObjectId id = sc.looking.front();
    sc.looking.erase(sc.looking.begin());
    if (keepTop) sc.keepTop.push_back(id);
    else         sc.putBottom.push_back(id);
    // When all decisions are made, place cards back: kept cards go on top
    // (original order — first decided is the topmost), bottomed cards stack
    // beneath the library bottom in decision order.
    if (sc.looking.empty()) {
        Player& p = m_game.player(sc.controller);
        for (auto it = sc.keepTop.rbegin(); it != sc.keepTop.rend(); ++it) {
            Card* c = m_game.findCard(*it);
            if (c) { c->revealedToOwner = true; p.library().addToFront(c); }
        }
        for (ObjectId bid : sc.putBottom) {
            Card* c = m_game.findCard(bid);
            if (c) p.library().addToBack(c);
        }
        addLog("Scry: " + std::to_string(sc.keepTop.size()) + " kept on top, " +
               std::to_string(sc.putBottom.size()) + " sent to bottom.");
        m_game.clearPendingScry();
    }
}

void GameWindow::completePendingSearch(ObjectId selectedId) {
    if (!m_game.hasPendingSearch()) return;
    const auto ps = m_game.pendingSearch();  // copy before clearing
    m_game.clearPendingSearch();
    Card* moved = m_game.moveToZone(selectedId, ps.dest, ps.destCtrl);
    m_game.player(ps.libPlayer).library().shuffle(m_game.rng());
    m_searchChoices.clear();
    std::string name = (moved && moved->rules) ? moved->rules->name : "card";
    addLog("Alice finds " + name + " from library.");
}

// ── Rendering ─────────────────────────────────────────────────────────────────

void GameWindow::render() {
    m_window.setView(m_gameView);

    // Surface any cast-failure message that chooseMode/onConfirm recorded since
    // the last tick. Logged once per attempt; popup stays up so the player can
    // tap more lands or pick a different mode without re-clicking the card.
    {
        const std::string& err = m_human.consumeCastError();
        if (!err.empty()) addLog(err);
    }

    // Rebuild library search choices every frame when a search is pending
    if (m_game.hasPendingSearch()) rebuildSearchChoices();

    RenderHints hints = (m_turn == WhosTurn::Human && !m_human.isGameOver())
                        ? m_human.buildHints()
                        : RenderHints{};
    // While AI is running, show current phase
    if (m_aiRunning)
        hints.instruction = "Bob: " + (m_aiPhase.empty() ? "thinking..." : m_aiPhase);
    hints.mousePos = m_mousePos;
    hints.phase    = std::string(m_tm.currentStepName());
    hints.logLines = std::vector<std::string>(m_gameLog.begin(), m_gameLog.end());
    // Card collection data for tooltip
    hints.cardCollection    = m_cardSeen.empty() ? nullptr : &m_cardSeen;
    hints.colorBlindMode    = m_colorBlindMode;

    // Card comparison: when Alt held while hovering, pin the previous card for compare
    if (sf::Keyboard::isKeyPressed(sf::Keyboard::LAlt) ||
        sf::Keyboard::isKeyPressed(sf::Keyboard::RAlt)) {
        if (m_browseSelect != kInvalidId)
            hints.compareCardId = m_browseSelect;
    }
    hints.logFilter         = m_logFilter;

    // Contextual help tooltips based on mouse position
    {
        using namespace Layout;
        float mx = m_mousePos.x, my = m_mousePos.y;
        // Phase tracker
        if (mx >= SIDE_X && mx < SIDE_X + SIDE_W && my >= SIDE_PHASE_Y && my < SIDE_PHASE_Y + SIDE_PHASE_H)
            hints.uiTooltip = "Phase tracker: click a phase to toggle stop points.\nGame pauses here for instants/tricks.";
        // Stack section
        else if (mx >= SIDE_X && mx < SIDE_X + SIDE_W && my >= SIDE_STACK_Y && my < SIDE_STACK_Y + SIDE_STACK_H / 2)
            hints.uiTooltip = "Stack: spells and abilities waiting to resolve.\nLast-in, first-out. Space/Enter to pass priority.";
        // Prompt section
        else if (mx >= SIDE_X && mx < SIDE_X + SIDE_W && my >= SIDE_PROMPT_Y)
            hints.uiTooltip = "Prompt: current action required.\nSpace = pass/end phase. Esc = main menu.";
        if (!hints.uiTooltip.empty())
            hints.uiTooltipPos = {mx + 12.f, my - 30.f};
    }

    // Log card-name hover: scan log lines near the mouse position
    {
        using namespace Layout;
        float mx = m_mousePos.x, my = m_mousePos.y;
        if (mx >= SIDE_X && mx < SIDE_X + SIDE_W && my >= SIDE_LOG_Y && my < WIN_H) {
            // Rough row estimate
            constexpr float kLineH = 15.f;
            int lineIdx = static_cast<int>((my - SIDE_LOG_Y - 22.f) / kLineH);
            int start = std::max(0, (int)m_gameLog.size() - 20);
            int idx   = start + lineIdx;
            if (idx >= 0 && idx < (int)m_gameLog.size()) {
                const std::string& logLine = m_gameLog[idx];
                // Check if any word in this log line matches a card name in DB
                std::istringstream ss(logLine);
                std::string word;
                std::string candidateName;
                while (ss >> word) {
                    // Try 1, 2, and 3 word matches
                    if (!candidateName.empty()) candidateName += " ";
                    candidateName += word;
                    if (m_db.find(candidateName)) {
                        hints.logHoverCardName = candidateName;
                        break;
                    }
                }
            }
        }
    }
    // Pending trigger queue for reorder overlay
    if (m_abilities.pendingHumanTriggerCount() > 1) {
        for (const auto& trig : m_abilities.humanTriggerQueue()) {
            const Card* src = m_game.findCard(trig.sourceCardId);
            hints.pendingTriggerNames.push_back(src ? src->rules->name : "trigger");
        }
    }

    // Zone browser overlay state
    hints.showZoneBrowse    = m_zoneBrowseActive;
    hints.zoneBrowsePlayer  = m_zoneBrowsePlayer;
    hints.zoneBrowseZone    = m_zoneBrowseZone;

    // Populate phase-stop toggle state for the phase tracker UI
    hints.stepStops = {
        m_stops.untap, m_stops.upkeep, m_stops.draw,
        m_stops.main1, m_stops.beginCombat, m_stops.attackers, m_stops.blockers,
        m_stops.firstStrike, m_stops.combatDmg, m_stops.endCombat,
        m_stops.main2, m_stops.endStep, m_stops.cleanup
    };

    // Exploit instruction
    if (m_game.hasPendingExploit()) {
        const auto& ec = m_game.pendingExploit();
        const Card* ex = m_game.findCard(ec.exploiterId);
        std::string ename = ex ? ex->rules->name : "creature";
        hints.instruction = "EXPLOIT: Click a creature to sacrifice for " + ename + "'s bonus, or Skip";
    }

    // Tribute instruction
    if (m_game.hasPendingTribute()) {
        const auto& tc = m_game.pendingTribute();
        const Card* tCard = m_game.findCard(tc.cardId);
        std::string cname = tCard ? tCard->rules->name : "creature";
        hints.instruction = "TRIBUTE: Put " + std::to_string(tc.amount) +
                            " +1/+1 counter(s) on " + cname + "? [Pay / Don't Pay]";
    }

    // Proliferate instruction
    if (m_game.hasPendingProliferate()) {
        auto& pc = m_game.pendingProliferate();
        int sel = static_cast<int>(pc.selectedIds.size());
        hints.instruction = "PROLIFERATE: click permanents with counters to select them. [Pass] = confirm.";
        // Show selected cards as highlighted
        for (ObjectId sid : pc.selectedIds)
            hints.selectedCards.insert(sid);
    }

    // Miracle overlay
    if (m_game.hasPendingMiracle()) {
        const Card* mc = m_game.findCard(m_game.pendingMiracle().cardId);
        std::string mname = mc ? mc->rules->name : "spell";
        std::string mcost = mc ? mc->rules->miracleCost.toString() : "";
        hints.instruction = "MIRACLE! Cast " + mname + " for " + mcost + "? [Cast / Skip]";
    }

    // Phyrexian mana overlay
    if (m_game.hasPendingPhyrexian()) {
        const auto& pc = m_game.pendingPhyrexian();
        int lifeCost = pc.numShards * 2;
        hints.instruction = "Phyrexian mana: Pay " + std::to_string(lifeCost) +
                            " life? [Pay Life / Pay Mana]";
    }
    // Cascade overlay
    if (m_game.hasPendingCascade()) {
        const Card* cc = m_game.findCard(m_game.pendingCascade().cardId);
        std::string cname = cc ? cc->rules->name : "Unknown";
        hints.instruction = "CASCADE: Cast " + cname + " for free? [Cast / Skip]";
    }

    // Library search overlay
    if (m_game.hasPendingSearch()) {
        hints.showLibrarySearch  = true;
        hints.searchChoices      = m_searchChoices;
        hints.searchInstruction  = "Choose a card from your library";
    }

    if (m_tm.isGameOver()) {
        hints.instruction = m_game.player(m_tm.winnerId()).name() + " wins!";
        if (m_gameLog.empty() || m_gameLog.back().find("wins") == std::string::npos)
            addLog(m_game.player(m_tm.winnerId()).name() + " wins!");
    }

    // Preview pane: hover always wins so the player can move the cursor to
    // inspect other cards without losing context. Falls back to the
    // explicitly-selected card (m_browseSelect) only when nothing is under
    // the cursor — keeps the "selected" card pinned when the mouse moves
    // off the board entirely.
    {
        auto hover = m_renderer.hitTest(m_mousePos.x, m_mousePos.y);
        if (hover.id != kInvalidId)         hints.previewCardId = hover.id;
        else if (m_browseSelect != kInvalidId) hints.previewCardId = m_browseSelect;
    }

    // Draw the base board (hints fully populated above)
    prefetchVisibleArt();
    m_renderer.draw(m_window, hints);

    // ── Drag ghost: card follows cursor while holding a hand card ─────────────
    if (m_drag.active && m_drag.cardId != kInvalidId) {
        if (const auto* dc = m_game.findCard(m_drag.cardId)) {
            CardDrawOptions opts;
            opts.alpha    = 0.80f;
            opts.selected = true;
            float cx = m_mousePos.x - Layout::CARD_W * 0.5f;
            float cy = m_mousePos.y - Layout::CARD_H * 0.5f;
            ui::drawCard(m_window, m_font, dc, cx, cy, opts, m_picsDir);
        }
    }

    // ── Riot choice overlay (drawn on top of the board) ──────────────────────
    if (m_game.hasPendingRiot()) {
        using namespace Layout;
        const float btnW = 160.f, btnH = 44.f, gap = 12.f;
        float totalW = 2 * btnW + gap;
        float startX = PLAY_X + (PLAY_W - totalW) * 0.5f;
        float startY = WIN_H * 0.5f + 20.f;
        float panW = totalW + 40.f, panH = btnH + 70.f;
        sf::RectangleShape panel({panW, panH});
        panel.setPosition(startX - 20.f, startY - 45.f);
        panel.setFillColor(sf::Color(15, 22, 32, 230));
        panel.setOutlineColor(sf::Color(70, 100, 140));
        panel.setOutlineThickness(2.f);
        m_window.draw(panel);
        sf::Text title("Riot: Haste or +1/+1 counter?", m_font, 13);
        title.setStyle(sf::Text::Bold);
        title.setFillColor(sf::Color(200, 215, 235));
        auto tb = title.getLocalBounds();
        title.setPosition(startX - 20.f + (panW - tb.width) * 0.5f, startY - 35.f);
        drawText(title);
        const char* riotLabels[2] = { "Haste", "+1/+1 Counter" };
        for (int i = 0; i < 2; ++i) {
            float bx = startX + i * (btnW + gap);
            sf::RectangleShape btn({btnW, btnH});
            btn.setPosition(bx, startY);
            btn.setFillColor(sf::Color(30, 60, 100));
            btn.setOutlineColor(sf::Color(60, 120, 180));
            btn.setOutlineThickness(1.5f);
            m_window.draw(btn);
            sf::Text lt(riotLabels[i], m_font, 13);
            lt.setStyle(sf::Text::Bold);
            lt.setFillColor(sf::Color(200, 220, 240));
            auto lb = lt.getLocalBounds();
            lt.setPosition(bx + (btnW - lb.width) * 0.5f, startY + (btnH - lb.height) * 0.5f - 2.f);
            drawText(lt);
        }
        m_window.display();
        return;
    }

    // ── Pay-life-or-enter-tapped overlay (shock lands) ───────────────────────
    if (m_game.hasPendingPayLife()) {
        using namespace Layout;
        const float btnW = 170.f, btnH = 44.f, gap = 12.f;
        float totalW = 2 * btnW + gap;
        float startX = PLAY_X + (PLAY_W - totalW) * 0.5f;
        float startY = WIN_H * 0.5f + 20.f;
        float panW = totalW + 40.f, panH = btnH + 70.f;
        sf::RectangleShape panel({panW, panH});
        panel.setPosition(startX - 20.f, startY - 45.f);
        panel.setFillColor(sf::Color(15, 22, 32, 230));
        panel.setOutlineColor(sf::Color(70, 100, 140));
        panel.setOutlineThickness(2.f);
        m_window.draw(panel);
        const Card* lc = m_game.findCard(m_game.pendingPayLifeCardId());
        int amt = m_game.pendingPayLifeAmount();
        std::string nm = (lc && lc->rules) ? lc->rules->name : "Land";
        sf::Text title(nm + ": pay " + std::to_string(amt) + " life to enter untapped?",
                       m_font, 13);
        title.setStyle(sf::Text::Bold);
        title.setFillColor(sf::Color(200, 215, 235));
        auto tb = title.getLocalBounds();
        title.setPosition(startX - 20.f + (panW - tb.width) * 0.5f, startY - 35.f);
        drawText(title);
        std::string l0 = "Pay " + std::to_string(amt) + " life (untapped)";
        const std::string labels[2] = { l0, "Enter tapped" };
        for (int i = 0; i < 2; ++i) {
            float bx = startX + i * (btnW + gap);
            sf::RectangleShape btn({btnW, btnH});
            btn.setPosition(bx, startY);
            btn.setFillColor(i == 0 ? sf::Color(80, 40, 40) : sf::Color(30, 60, 100));
            btn.setOutlineColor(sf::Color(60, 120, 180));
            btn.setOutlineThickness(1.5f);
            m_window.draw(btn);
            sf::Text lt(labels[i], m_font, 12);
            lt.setStyle(sf::Text::Bold);
            lt.setFillColor(sf::Color(200, 220, 240));
            auto lb = lt.getLocalBounds();
            lt.setPosition(bx + (btnW - lb.width) * 0.5f, startY + (btnH - lb.height) * 0.5f - 2.f);
            drawText(lt);
        }
        m_window.display();
        return;
    }

    // ── Fabricate choice overlay (drawn on top of the board) ─────────────────
    if (m_game.hasPendingFabricate()) {
        using namespace Layout;
        int n = m_game.pendingFabricateAmount();
        std::string titleStr = "Fabricate " + std::to_string(n) + ": Servos or counters?";
        const float btnW = 180.f, btnH = 44.f, gap = 12.f;
        float totalW = 2 * btnW + gap;
        float startX = PLAY_X + (PLAY_W - totalW) * 0.5f;
        float startY = WIN_H * 0.5f + 20.f;
        float panW = totalW + 40.f, panH = btnH + 70.f;
        sf::RectangleShape panel({panW, panH});
        panel.setPosition(startX - 20.f, startY - 45.f);
        panel.setFillColor(sf::Color(15, 22, 32, 230));
        panel.setOutlineColor(sf::Color(70, 140, 100));
        panel.setOutlineThickness(2.f);
        m_window.draw(panel);
        sf::Text title(titleStr, m_font, 13);
        title.setStyle(sf::Text::Bold);
        title.setFillColor(sf::Color(200, 235, 215));
        auto tb = title.getLocalBounds();
        title.setPosition(startX - 20.f + (panW - tb.width) * 0.5f, startY - 35.f);
        drawText(title);
        const char* fabLabels[2] = { "Create Servo Tokens", "+1/+1 Counters" };
        for (int i = 0; i < 2; ++i) {
            float bx = startX + i * (btnW + gap);
            sf::RectangleShape btn({btnW, btnH});
            btn.setPosition(bx, startY);
            btn.setFillColor(sf::Color(20, 60, 40));
            btn.setOutlineColor(sf::Color(50, 140, 80));
            btn.setOutlineThickness(1.5f);
            m_window.draw(btn);
            sf::Text lt(fabLabels[i], m_font, 13);
            lt.setStyle(sf::Text::Bold);
            lt.setFillColor(sf::Color(200, 235, 215));
            auto lb = lt.getLocalBounds();
            lt.setPosition(bx + (btnW - lb.width) * 0.5f, startY + (btnH - lb.height) * 0.5f - 2.f);
            drawText(lt);
        }
        m_window.display();
        return;
    }

    // ── Madness cast overlay ──────────────────────────────────────────────────
    if (m_game.hasPendingMadnessCast()) {
        using namespace Layout;
        const auto& pm = m_game.pendingMadnessCast();
        std::string titleStr = "Cast " + pm.cardName + " for its Madness cost?";
        const float btnW = 180.f, btnH = 44.f, gap = 12.f;
        float totalW = 2 * btnW + gap;
        float startX = PLAY_X + (PLAY_W - totalW) * 0.5f;
        float startY = WIN_H * 0.5f + 20.f;
        float panW = totalW + 40.f, panH = btnH + 70.f;
        sf::RectangleShape panel({panW, panH});
        panel.setPosition(startX - 20.f, startY - 45.f);
        panel.setFillColor(sf::Color(15, 22, 32, 230));
        panel.setOutlineColor(sf::Color(140, 70, 70));
        panel.setOutlineThickness(2.f);
        m_window.draw(panel);
        sf::Text title(titleStr, m_font, 13);
        title.setStyle(sf::Text::Bold);
        title.setFillColor(sf::Color(235, 215, 200));
        auto tb = title.getLocalBounds();
        title.setPosition(startX - 20.f + (panW - tb.width) * 0.5f, startY - 35.f);
        drawText(title);
        const char* madnessLabels[2] = { "Cast (Madness)", "Put in Graveyard" };
        for (int i = 0; i < 2; ++i) {
            float bx = startX + i * (btnW + gap);
            sf::RectangleShape btn({btnW, btnH});
            btn.setPosition(bx, startY);
            btn.setFillColor(i == 0 ? sf::Color(60, 20, 20) : sf::Color(30, 30, 30));
            btn.setOutlineColor(i == 0 ? sf::Color(160, 60, 60) : sf::Color(90, 90, 90));
            btn.setOutlineThickness(1.5f);
            m_window.draw(btn);
            sf::Text lt(madnessLabels[i], m_font, 13);
            lt.setStyle(sf::Text::Bold);
            lt.setFillColor(sf::Color(235, 215, 200));
            auto lb = lt.getLocalBounds();
            lt.setPosition(bx + (btnW - lb.width) * 0.5f, startY + (btnH - lb.height) * 0.5f - 2.f);
            drawText(lt);
        }
        m_window.display();
        return;
    }

    // ── Planeswalker ability choice overlay ──────────────────────────────────
    if (m_human.pendingPlaneswalker() != kInvalidId) {
        using namespace Layout;
        const auto& labels = m_human.pwAbilLabels();
        int n = static_cast<int>(labels.size());
        if (n > 0) {
            Card* pw = m_game.findCard(m_human.pendingPlaneswalker());
            std::string pwName = (pw && pw->rules) ? pw->rules->name : "Planeswalker";
            const float btnW = 120.f, btnH = 44.f, gap = 10.f;
            float totalW = n * btnW + (n - 1) * gap;
            float startX = PLAY_X + (PLAY_W - totalW) * 0.5f;
            float startY = WIN_H * 0.5f + 20.f;
            float panW = totalW + 40.f, panH = btnH + 75.f;
            sf::RectangleShape panel({panW, panH});
            panel.setPosition(startX - 20.f, startY - 50.f);
            panel.setFillColor(sf::Color(15, 22, 32, 230));
            panel.setOutlineColor(sf::Color(100, 70, 140));
            panel.setOutlineThickness(2.f);
            m_window.draw(panel);
            sf::Text title(pwName + ": choose an ability", m_font, 12);
            title.setStyle(sf::Text::Bold);
            title.setFillColor(sf::Color(200, 215, 235));
            auto tb = title.getLocalBounds();
            title.setPosition(startX - 20.f + (panW - tb.width) * 0.5f, startY - 38.f);
            drawText(title);
            for (int i = 0; i < n; ++i) {
                float bx = startX + i * (btnW + gap);
                sf::RectangleShape btn({btnW, btnH});
                btn.setPosition(bx, startY);
                btn.setFillColor(sf::Color(50, 30, 90));
                btn.setOutlineColor(sf::Color(110, 70, 160));
                btn.setOutlineThickness(1.5f);
                m_window.draw(btn);
                sf::Text lt(labels[i], m_font, 15);
                lt.setStyle(sf::Text::Bold);
                lt.setFillColor(sf::Color(220, 200, 255));
                auto lb = lt.getLocalBounds();
                lt.setPosition(bx + (btnW - lb.width) * 0.5f,
                               startY + (btnH - lb.height) * 0.5f - 2.f);
                drawText(lt);
            }
        }
        m_window.display();
        return;
    }

    // ── Charm mode overlay (drawn on top of the board) ───────────────────────
    if (m_game.hasPendingCharm()) {
        using namespace Layout;
        const auto& charm = m_game.pendingCharm();
        int n = static_cast<int>(charm.labels.size());
        if (n > 0) {
            const float btnW = 160.f, btnH = 44.f, gap = 12.f;
            float totalW = n * btnW + (n - 1) * gap;
            float startX = PLAY_X + (PLAY_W - totalW) * 0.5f;
            float startY = WIN_H * 0.5f + 20.f;
            float panW = totalW + 40.f, panH = btnH + 70.f;
            sf::RectangleShape panel({panW, panH});
            panel.setPosition(startX - 20.f, startY - 45.f);
            panel.setFillColor(sf::Color(15, 22, 32, 230));
            panel.setOutlineColor(sf::Color(70, 100, 140));
            panel.setOutlineThickness(2.f);
            m_window.draw(panel);
            sf::Text title("Choose a mode:", m_font, 13);
            title.setStyle(sf::Text::Bold);
            title.setFillColor(sf::Color(200, 215, 235));
            auto tb = title.getLocalBounds();
            title.setPosition(startX - 20.f + (panW - tb.width) * 0.5f, startY - 35.f);
            drawText(title);
            for (int i = 0; i < n; ++i) {
                float bx = startX + i * (btnW + gap);
                sf::RectangleShape btn({btnW, btnH});
                btn.setPosition(bx, startY);
                btn.setFillColor(sf::Color(30, 60, 100));
                btn.setOutlineColor(sf::Color(60, 120, 180));
                btn.setOutlineThickness(1.5f);
                m_window.draw(btn);
                std::string lbl = (i < (int)charm.labels.size())
                                  ? charm.labels[i] : "Mode " + std::to_string(i + 1);
                sf::Text lt(lbl, m_font, 11);
                lt.setStyle(sf::Text::Bold);
                lt.setFillColor(sf::Color(200, 220, 240));
                auto lb = lt.getLocalBounds();
                lt.setPosition(bx + (btnW - lb.width) * 0.5f,
                               startY + (btnH - lb.height) * 0.5f - 2.f);
                drawText(lt);
            }
        }
        m_window.display();
        return;
    }

    if (m_tm.isGameOver()) renderGameOver();

    // FPS overlay
    if (m_showFpsOverlay) {
        ++m_frameCount;
        if (m_fpsClock.getElapsedTime().asSeconds() >= 1.f) {
            m_fpsDisplay = m_frameCount / m_fpsClock.restart().asSeconds();
            m_frameCount = 0;
        }
        char fpsBuf[64];
        std::snprintf(fpsBuf, sizeof(fpsBuf), "FPS: %.0f  |  Turn: %d  |  F3 to hide",
                      m_fpsDisplay, m_game.turnNumber());
        sf::RectangleShape fpsBg({280.f, 18.f});
        fpsBg.setPosition(4.f, 4.f);
        fpsBg.setFillColor(sf::Color(0, 0, 0, 160));
        m_window.draw(fpsBg);
        sf::Text fpsText(fpsBuf, m_font, 10);
        fpsText.setFillColor(sf::Color(180, 230, 180));
        fpsText.setPosition(6.f, 5.f);
        drawText(fpsText);
    }

    // Stats, achievements, and collection overlays
    if (m_showStats)        { renderStatsOverlay();        }
    if (m_showAchievements) { renderAchievementsOverlay(); }
    if (m_showCollection)   { renderCollectionStats();     }

    if (m_goldfishMode)   renderGoldfishOverlay();
    if (m_showLifeChart)  renderLifeChart();
    if (m_showTurnHistory) renderTurnHistory();
    if (m_showOptions)    renderOptions();
    if (m_showHelp)       renderHelp();
    if (m_showCardSearch) renderCardSearch();
    if (m_splitChoice.active) renderSplitChoice();
    renderToast();

    // Art-only zoom (Ctrl+right-click) — full-screen art, no text
    if (m_artZoomCardId != kInvalidId) {
        const mtg::Card* ac = m_game.findCard(m_artZoomCardId);
        if (ac && ac->rules) {
            using namespace Layout;
            sf::RectangleShape dim({WIN_W, WIN_H});
            dim.setFillColor(sf::Color(0, 0, 0, 220));
            m_window.draw(dim);
            auto imgPath = ui::findCardImage(ac->rules->name, m_picsDir);
            bool drew = false;
            if (!imgPath.empty()) {
                const auto* tex = ui::TextureCache::get(imgPath);
                if (tex) {
                    sf::Sprite spr(*tex);
                    auto sz = tex->getSize();
                    float sc = std::min(WIN_W * 0.8f / sz.x, WIN_H * 0.9f / sz.y);
                    spr.setScale(sc, sc);
                    spr.setPosition((WIN_W - sz.x * sc) * 0.5f, (WIN_H - sz.y * sc) * 0.5f);
                    m_window.draw(spr);
                    drew = true;
                }
            }
            if (!drew) {
                sf::Text nm(ac->rules->name, m_font, 24);
                nm.setStyle(sf::Text::Bold);
                nm.setFillColor(sf::Color(230, 193, 112));
                nm.setPosition((WIN_W - 200.f) * 0.5f, WIN_H * 0.5f);
                drawText(nm);
            }
            sf::Text hint("Ctrl+right-click or Esc to close", m_font, 10);
            hint.setFillColor(sf::Color(100, 95, 85));
            hint.setPosition(WIN_W * 0.5f - 100.f, WIN_H - 20.f);
            drawText(hint);
        } else {
            m_artZoomCardId = kInvalidId;
        }
    }

    // Card zoom overlay (right-click on any card) — shows large art + full oracle text
    if (m_zoomCardId != kInvalidId) {
        const mtg::Card* zc = m_game.findCard(m_zoomCardId);
        if (zc && zc->rules) {
            using namespace Layout;
            sf::RectangleShape dim({WIN_W, WIN_H});
            dim.setFillColor(sf::Color(0, 0, 0, 200));
            m_window.draw(dim);

            constexpr float zW = 240.f, zH = 340.f;
            constexpr float textPanW = 320.f;
            constexpr float panH = zH;
            float totalW = zW + textPanW + 16.f;
            float zx = (WIN_W - totalW) * 0.5f;
            float zy = (WIN_H - panH) * 0.5f;

            // Card image on the left
            ui::drawCardLarge(m_window, m_font, zc, zx, zy, zW, zH, m_picsDir);

            // Text panel on the right
            float tx = zx + zW + 16.f;
            sf::RectangleShape panel({textPanW, panH});
            panel.setPosition(tx, zy);
            panel.setFillColor(sf::Color(18, 17, 16, 240));
            panel.setOutlineColor(sf::Color(240, 220, 180, 40));
            panel.setOutlineThickness(1.f);
            m_window.draw(panel);

            float ty = zy + 10.f;
            auto drawLine = [&](const std::string& s, unsigned sz, sf::Color col, bool bold = false) {
                if (s.empty() || ty > zy + panH - 8.f) return;
                // word-wrap
                std::string rem = s;
                int wrapAt = static_cast<int>((textPanW - 16.f) / (sz * 0.58f));
                if (wrapAt < 8) wrapAt = 8;
                while (!rem.empty() && ty < zy + panH - 8.f) {
                    std::string line;
                    if ((int)rem.size() <= wrapAt) { line = rem; rem.clear(); }
                    else {
                        size_t sp = rem.rfind(' ', (size_t)wrapAt);
                        if (sp == std::string::npos) sp = (size_t)wrapAt;
                        line = rem.substr(0, sp);
                        rem  = rem.substr(sp + 1);
                    }
                    sf::Text t2(line, m_font, sz);
                    if (bold) t2.setStyle(sf::Text::Bold);
                    t2.setFillColor(col);
                    t2.setPosition(tx + 8.f, ty);
                    ui::applyTextScale(t2);
                    drawText(t2);
                    ty += sz + 3.f;
                }
            };

            drawLine(zc->rules->name, 14, sf::Color(230, 193, 112), true);
            ty += 2.f;
            drawLine(zc->rules->type.toString(), 10, sf::Color(148, 139, 124));
            if (!zc->rules->manaCost.toString().empty())
                drawLine("Cost: " + zc->rules->manaCost.toString(), 10, sf::Color(180, 170, 140));
            if (zc->rules->hasPT()) {
                int pw = effectivePower(*zc), tg = effectiveToughness(*zc);
                drawLine(std::to_string(pw) + " / " + std::to_string(tg), 12,
                         sf::Color(200, 215, 200), true);
            }
            if (zc->rules->type.isPlaneswalker()) {
                drawLine("Loyalty: " + std::to_string(zc->counterCount("loyalty")),
                         11, sf::Color(130, 155, 220), true);
            }
            ty += 6.f;
            // Oracle text — full text
            if (!zc->rules->oracleText.empty())
                drawLine(zc->rules->oracleText, 10, sf::Color(200, 195, 182));

            // Rulings panel (R key to toggle)
            if (m_showRulings && !m_cachedRulings.empty()) {
                float rlX = zx, rlY = zy + panH + 14.f;
                float rlW = zW + textPanW + 16.f, rlH = 180.f;
                sf::RectangleShape rlBg({rlW, rlH});
                rlBg.setPosition(rlX, rlY);
                rlBg.setFillColor(sf::Color(14, 12, 10, 240));
                rlBg.setOutlineColor(sf::Color(240, 220, 180, 30));
                rlBg.setOutlineThickness(1.f);
                m_window.draw(rlBg);
                sf::Text rlHdr("SCRYFALL RULINGS  (R to hide)", m_font, 10);
                rlHdr.setStyle(sf::Text::Bold);
                rlHdr.setFillColor(sf::Color(203, 163, 90));
                rlHdr.setPosition(rlX + 8.f, rlY + 4.f);
                drawText(rlHdr);
                float rty = rlY + 18.f;
                for (const auto& r : m_cachedRulings) {
                    if (rty > rlY + rlH - 12.f) break;
                    // word-wrap to panel width
                    std::string rem = r;
                    int wrapAt = static_cast<int>((rlW - 16.f) / 6.f);
                    while (!rem.empty() && rty < rlY + rlH - 12.f) {
                        std::string line;
                        if ((int)rem.size() <= wrapAt) { line = rem; rem.clear(); }
                        else {
                            size_t sp = rem.rfind(' ', (size_t)wrapAt);
                            if (sp == std::string::npos) sp = (size_t)wrapAt;
                            line = rem.substr(0, sp); rem = rem.substr(sp + 1);
                        }
                        sf::Text rt2(line, m_font, 9);
                        rt2.setFillColor(sf::Color(190, 183, 170));
                        rt2.setPosition(rlX + 8.f, rty);
                        drawText(rt2);
                        rty += 12.f;
                    }
                    rty += 3.f;
                }
            }

            // Close hint
            sf::Text hint("Right-click or Esc to close  |  R = Scryfall rulings", m_font, 9);
            hint.setFillColor(sf::Color(100, 95, 85));
            hint.setPosition(zx, zy + panH + 6.f);
            drawText(hint);
        } else {
            m_zoomCardId = kInvalidId;
        }
    }

    // Pause menu sits on top of everything else (incl. zoom overlay).
    // ── Multi-mode cast options popup ───────────────────────────────────────
    // When the human has a pending spell, list every available mode (Cast /
    // Foretell / …) as its own row, with the printed cost on the right and
    // the current mana pool below for reference. Affordable rows are gold;
    // un-affordable rows are dimmed. The Cancel "X" is on the right.
    m_castPopupHits.clear();
    m_castPopupCancelRect = {};
    if (m_human.pendingSpellId() != kInvalidId) {
        ObjectId pid = m_human.pendingSpellId();
        if (const Card* c = m_game.findCard(pid)) {
            auto modes = m_human.availableModes();
            const std::string nm = c->rules->name;
            const std::string poolStr = m_game.player(0).manaPool().toString();

            constexpr float rowH    = 28.f;
            constexpr float pad     = 8.f;
            constexpr float cancelW = 32.f;
            constexpr float headerH = 22.f;
            constexpr float footerH = 18.f;
            constexpr float panelW  = 360.f;
            const float panelH = headerH + footerH + rowH * std::max<size_t>(1, modes.size()) + pad * 2.f;
            const float x = (WIN_W - panelW) * 0.5f;
            const float y = WIN_H - panelH - 12.f;

            // Panel background
            sf::RectangleShape pan({panelW, panelH});
            pan.setPosition(x, y);
            pan.setFillColor(sf::Color(18, 17, 16, 240));
            pan.setOutlineColor(sf::Color(203, 163, 90));
            pan.setOutlineThickness(2.f);
            m_window.draw(pan);

            // Header: card name (left) + Cancel "X" (right)
            sf::Text nameText(sf::String::fromUtf8(nm.begin(), nm.end()), m_font, 13);
            nameText.setStyle(sf::Text::Bold);
            nameText.setFillColor(sf::Color(220, 210, 190));
            nameText.setPosition(x + pad, y + 4.f);
            drawText(nameText);

            sf::RectangleShape cancelBtn({cancelW, headerH - 2.f});
            cancelBtn.setPosition(x + panelW - cancelW - 4.f, y + 4.f);
            cancelBtn.setFillColor(sf::Color(48, 22, 18));
            m_window.draw(cancelBtn);
            sf::Text cancelText("X", m_font, 13);
            cancelText.setStyle(sf::Text::Bold);
            cancelText.setFillColor(sf::Color(230, 170, 130));
            auto cb = cancelText.getLocalBounds();
            cancelText.setPosition(x + panelW - cancelW - 4.f + (cancelW - cb.width) * 0.5f - cb.left,
                                   y + 4.f + (headerH - 2.f - cb.height) * 0.5f - cb.top);
            drawText(cancelText);
            m_castPopupCancelRect = sf::FloatRect(x + panelW - cancelW - 4.f, y + 4.f,
                                                  cancelW, headerH - 2.f);

            // One row per available mode
            float ry = y + headerH + pad;
            for (const auto& m : modes) {
                sf::FloatRect r(x + pad, ry, panelW - pad * 2.f, rowH - 4.f);
                sf::RectangleShape btn({r.width, r.height});
                btn.setPosition(r.left, r.top);
                btn.setFillColor(m.affordable ? sf::Color(44, 34, 14)
                                              : sf::Color(34, 28, 24));
                btn.setOutlineColor(m.affordable ? sf::Color(110, 90, 50)
                                                 : sf::Color(70, 58, 48));
                btn.setOutlineThickness(1.f);
                m_window.draw(btn);

                const sf::Color txt = m.affordable ? sf::Color(230, 193, 112)
                                                   : sf::Color(140, 130, 110);
                sf::Text lbl(m.label, m_font, 13);
                lbl.setStyle(sf::Text::Bold);
                lbl.setFillColor(txt);
                lbl.setPosition(r.left + 10.f, r.top + 4.f);
                drawText(lbl);

                // Mana cost as pips (fall back to text if no skin is loaded).
                if (SkinAssets::ready() && m.costStr.find('{') != std::string::npos) {
                    constexpr float pipSz = 15.f;
                    int nsym = static_cast<int>(
                        std::count(m.costStr.begin(), m.costStr.end(), '{') +
                        std::count(m.costStr.begin(), m.costStr.end(), '('));
                    if (nsym < 1) nsym = 1;
                    float estW = nsym * (pipSz + 1.f);
                    SkinAssets::drawManaCost(m_window, m.costStr,
                                             r.left + r.width - estW - 10.f,
                                             r.top + (rowH - 4.f - pipSz) * 0.5f, pipSz);
                } else {
                    sf::Text costT(m.costStr, m_font, 12);
                    costT.setFillColor(txt);
                    float cw = costT.getLocalBounds().width;
                    costT.setPosition(r.left + r.width - cw - 10.f, r.top + 5.f);
                    drawText(costT);
                }

                m_castPopupHits.push_back({r, m.kind});
                ry += rowH;
            }

            // Footer: current mana pool — "Pool:" label then pips for each mana.
            {
                float fy = y + panelH - footerH;
                sf::Text foot("Pool:", m_font, 11);
                foot.setFillColor(sf::Color(170, 165, 150));
                foot.setPosition(x + pad, fy);
                drawText(foot);
                float px2 = x + pad + foot.getLocalBounds().width + 6.f;
                if (poolStr.empty()) {
                    sf::Text empt("(empty)", m_font, 11);
                    empt.setFillColor(sf::Color(140, 135, 122));
                    empt.setPosition(px2, fy);
                    drawText(empt);
                } else if (SkinAssets::ready()) {
                    SkinAssets::drawManaCost(m_window, poolStr, px2, fy - 2.f, 14.f);
                } else {
                    sf::Text pl(poolStr, m_font, 11);
                    pl.setFillColor(sf::Color(170, 165, 150));
                    pl.setPosition(px2, fy);
                    drawText(pl);
                }
            }
        }
    }

    // ── Combo mana-choice overlay ───────────────────────────────────────────
    // Temple-style lands trigger this: "Add {B} or {G}" → click a button to
    // pick the colour and add it to the pool.
    m_manaChoiceHits.clear();
    if (m_game.hasPendingManaChoice()) {
        const auto& mc = m_game.pendingManaChoice();
        constexpr float bw = 48.f, bh = 32.f, pad = 6.f, hdrH = 22.f;
        const float w = pad * 2.f + bw * static_cast<float>(mc.colors.size()) + pad * std::max<size_t>(0, mc.colors.size() - 1);
        const float h = hdrH + bh + pad * 2.f;
        const float x = (WIN_W - w) * 0.5f;
        const float y = WIN_H * 0.5f - h * 0.5f;
        sf::RectangleShape pan({w, h});
        pan.setPosition(x, y);
        pan.setFillColor(sf::Color(18, 17, 16, 240));
        pan.setOutlineColor(sf::Color(203, 163, 90));
        pan.setOutlineThickness(2.f);
        m_window.draw(pan);
        sf::Text hdr("Choose colour to add:", m_font, 12);
        hdr.setStyle(sf::Text::Bold);
        hdr.setFillColor(sf::Color(230, 220, 200));
        auto hb = hdr.getLocalBounds();
        hdr.setPosition(x + (w - hb.width) * 0.5f, y + 4.f);
        drawText(hdr);
        float bx = x + pad;
        const float by = y + hdrH;
        for (char col : mc.colors) {
            sf::Color bgCol(50, 50, 50), txtCol(230, 230, 230);
            switch (col) {
                case 'W': bgCol = sf::Color(240, 232, 200); txtCol = sf::Color(40, 30, 20); break;
                case 'U': bgCol = sf::Color( 60, 100, 180); txtCol = sf::Color(230, 240, 255); break;
                case 'B': bgCol = sf::Color( 40,  35,  40); txtCol = sf::Color(220, 210, 220); break;
                case 'R': bgCol = sf::Color(190,  60,  50); txtCol = sf::Color(255, 230, 220); break;
                case 'G': bgCol = sf::Color( 50, 130,  70); txtCol = sf::Color(220, 240, 220); break;
            }
            sf::RectangleShape btn({bw, bh});
            btn.setPosition(bx, by);
            btn.setFillColor(bgCol);
            btn.setOutlineColor(sf::Color(203, 163, 90));
            btn.setOutlineThickness(1.f);
            m_window.draw(btn);
            std::string lblStr = "{" + std::string(1, col) + "}";
            sf::Text lbl(lblStr, m_font, 16);
            lbl.setStyle(sf::Text::Bold);
            lbl.setFillColor(txtCol);
            auto lb = lbl.getLocalBounds();
            lbl.setPosition(bx + (bw - lb.width) * 0.5f - lb.left,
                            by + (bh - lb.height) * 0.5f - lb.top);
            drawText(lbl);
            m_manaChoiceHits.push_back({sf::FloatRect(bx, by, bw, bh), col});
            bx += bw + pad;
        }
    }

    // ── Choose-a-creature-type overlay (Herald's Horn ETB) ───────────────────
    // A vertical list of candidate creature types; click one to set the chosen
    // type on the entering permanent.
    m_chooseTypeHits.clear();
    if (m_game.hasPendingChooseType()) {
        const auto& ct = m_game.pendingChooseType();
        constexpr float bw = 200.f, bh = 26.f, pad = 6.f, hdrH = 24.f;
        const size_t n = ct.options.size();
        const float w = bw + pad * 2.f;
        const float h = hdrH + pad + (bh + pad) * static_cast<float>(n) + pad;
        const float x = (WIN_W - w) * 0.5f;
        const float y = (WIN_H - h) * 0.5f;
        sf::RectangleShape pan({w, h});
        pan.setPosition(x, y);
        pan.setFillColor(sf::Color(18, 17, 16, 245));
        pan.setOutlineColor(sf::Color(203, 163, 90));
        pan.setOutlineThickness(2.f);
        m_window.draw(pan);
        sf::Text hdr("Choose a creature type:", m_font, 13);
        hdr.setStyle(sf::Text::Bold);
        hdr.setFillColor(sf::Color(230, 220, 200));
        auto hb = hdr.getLocalBounds();
        hdr.setPosition(x + (w - hb.width) * 0.5f, y + 5.f);
        drawText(hdr);
        float by = y + hdrH + pad;
        for (const auto& type : ct.options) {
            sf::RectangleShape btn({bw, bh});
            btn.setPosition(x + pad, by);
            btn.setFillColor(sf::Color(44, 34, 14));
            btn.setOutlineColor(sf::Color(110, 90, 50));
            btn.setOutlineThickness(1.f);
            m_window.draw(btn);
            sf::Text lbl(type, m_font, 14);
            lbl.setFillColor(sf::Color(230, 193, 112));
            auto lb = lbl.getLocalBounds();
            lbl.setPosition(x + pad + (bw - lb.width) * 0.5f - lb.left,
                            by + (bh - lb.height) * 0.5f - lb.top);
            drawText(lbl);
            m_chooseTypeHits.push_back({sf::FloatRect(x + pad, by, bw, bh), type});
            by += bh + pad;
        }
    }

    // ── Scry overlay ────────────────────────────────────────────────────────
    // Sequential decision: for each looked-at card show name + cost +
    // "Top" / "Bottom" buttons. After the last card is decided the kept-on-top
    // cards return to the top of library (original order) and bottomed cards
    // go to the bottom.
    m_scryKeepRect = {};
    m_scryBottomRect = {};
    if (m_game.hasPendingScry()) {
        const auto& sc = m_game.pendingScry();
        if (!sc.looking.empty()) {
            const Card* c = m_game.findCard(sc.looking.front());
            if (c) {
                constexpr float w = 320.f, h = 200.f, btnH = 36.f, gap = 8.f, pad = 10.f;
                const float x = (WIN_W - w) * 0.5f;
                const float y = WIN_H * 0.5f - h * 0.5f;
                sf::RectangleShape pan({w, h});
                pan.setPosition(x, y);
                pan.setFillColor(sf::Color(18, 17, 16, 245));
                pan.setOutlineColor(sf::Color(203, 163, 90));
                pan.setOutlineThickness(2.f);
                m_window.draw(pan);

                int idx = static_cast<int>(sc.keepTop.size() + sc.putBottom.size()) + 1;
                int tot = idx + static_cast<int>(sc.looking.size()) - 1;
                std::string hdr = "Scry  (" + std::to_string(idx) + " of " +
                                  std::to_string(tot) + ")";
                sf::Text hdrT(hdr, m_font, 12);
                hdrT.setStyle(sf::Text::Bold);
                hdrT.setFillColor(sf::Color(230, 193, 112));
                hdrT.setPosition(x + pad, y + 4.f);
                drawText(hdrT);

                sf::Text nm(sf::String::fromUtf8(c->rules->name.begin(), c->rules->name.end()),
                            m_font, 14);
                nm.setStyle(sf::Text::Bold);
                nm.setFillColor(sf::Color(230, 220, 200));
                nm.setPosition(x + pad, y + 24.f);
                drawText(nm);

                std::string cost = c->rules->manaCost.toString();
                std::string typeLine;
                for (auto mt : c->rules->type.types)
                    typeLine += std::string(mtg::CardType::mainTypeName(mt)) + " ";
                std::string meta = cost.empty() ? typeLine : (typeLine + "  " + cost);
                sf::Text metaT(meta, m_font, 11);
                metaT.setFillColor(sf::Color(170, 165, 150));
                metaT.setPosition(x + pad, y + 44.f);
                drawText(metaT);

                if (!c->rules->oracleText.empty()) {
                    std::string oracle = c->rules->oracleText;
                    if (oracle.size() > 120) oracle = oracle.substr(0, 118) + "...";
                    sf::Text otext(oracle, m_font, 10);
                    otext.setFillColor(sf::Color(180, 175, 160));
                    otext.setPosition(x + pad, y + 64.f);
                    drawText(otext);
                }

                const float by = y + h - btnH - pad;
                const float bw = (w - pad * 2.f - gap) * 0.5f;
                sf::RectangleShape keep({bw, btnH});
                keep.setPosition(x + pad, by);
                keep.setFillColor(sf::Color(30, 80, 60));
                keep.setOutlineColor(sf::Color(60, 160, 100));
                keep.setOutlineThickness(1.f);
                m_window.draw(keep);
                sf::Text keepT("Keep on top", m_font, 13);
                keepT.setStyle(sf::Text::Bold);
                keepT.setFillColor(sf::Color(170, 230, 180));
                auto kb = keepT.getLocalBounds();
                keepT.setPosition(x + pad + (bw - kb.width) * 0.5f - kb.left,
                                  by + (btnH - kb.height) * 0.5f - kb.top);
                drawText(keepT);
                m_scryKeepRect = sf::FloatRect(x + pad, by, bw, btnH);

                sf::RectangleShape bot({bw, btnH});
                bot.setPosition(x + pad + bw + gap, by);
                bot.setFillColor(sf::Color(80, 30, 30));
                bot.setOutlineColor(sf::Color(160, 60, 60));
                bot.setOutlineThickness(1.f);
                m_window.draw(bot);
                sf::Text botT("Put on bottom", m_font, 13);
                botT.setStyle(sf::Text::Bold);
                botT.setFillColor(sf::Color(230, 170, 170));
                auto bb = botT.getLocalBounds();
                botT.setPosition(x + pad + bw + gap + (bw - bb.width) * 0.5f - bb.left,
                                 by + (btnH - bb.height) * 0.5f - bb.top);
                drawText(botT);
                m_scryBottomRect = sf::FloatRect(x + pad + bw + gap, by, bw, btnH);
            }
        }
    }

    // ── Multi-mana-ability picker overlay (Shivan Reef / Temple / etc.) ────
    m_manaAbilityHits.clear();
    m_manaAbilityCancelRect = {};
    if (m_human.manaAbilitySource() != kInvalidId) {
        const auto& opts = m_human.manaAbilityOptions();
        constexpr float rowH    = 30.f;
        constexpr float pad     = 8.f;
        constexpr float cancelW = 32.f;
        constexpr float headerH = 22.f;
        const float panelW = 470.f;   // wide enough for rider text (pain-land damage, etc.)
        const float panelH = headerH + pad * 2.f + rowH * std::max<size_t>(1, opts.size());
        const float x = (WIN_W - panelW) * 0.5f;
        const float y = WIN_H * 0.5f - panelH * 0.5f;

        sf::RectangleShape pan({panelW, panelH});
        pan.setPosition(x, y);
        pan.setFillColor(sf::Color(18, 17, 16, 245));
        pan.setOutlineColor(sf::Color(203, 163, 90));
        pan.setOutlineThickness(2.f);
        m_window.draw(pan);

        const Card* src = m_game.findCard(m_human.manaAbilitySource());
        std::string hdrStr = src ? ("Choose ability — " + src->rules->name)
                                 : std::string("Choose ability");
        sf::Text hdr(sf::String::fromUtf8(hdrStr.begin(), hdrStr.end()), m_font, 12);
        hdr.setStyle(sf::Text::Bold);
        hdr.setFillColor(sf::Color(230, 220, 200));
        hdr.setPosition(x + pad, y + 4.f);
        drawText(hdr);

        // Cancel "X" top-right
        sf::RectangleShape cancel({cancelW, headerH - 2.f});
        cancel.setPosition(x + panelW - cancelW - 4.f, y + 4.f);
        cancel.setFillColor(sf::Color(48, 22, 18));
        m_window.draw(cancel);
        sf::Text cx("X", m_font, 13);
        cx.setStyle(sf::Text::Bold);
        cx.setFillColor(sf::Color(230, 170, 130));
        auto cb = cx.getLocalBounds();
        cx.setPosition(x + panelW - cancelW - 4.f + (cancelW - cb.width) * 0.5f - cb.left,
                       y + 4.f + (headerH - 2.f - cb.height) * 0.5f - cb.top);
        drawText(cx);
        m_manaAbilityCancelRect = sf::FloatRect(x + panelW - cancelW - 4.f, y + 4.f,
                                                cancelW, headerH - 2.f);

        float ry = y + headerH + pad;
        for (const auto& o : opts) {
            sf::FloatRect r(x + pad, ry, panelW - pad * 2.f, rowH - 4.f);
            sf::RectangleShape btn({r.width, r.height});
            btn.setPosition(r.left, r.top);
            btn.setFillColor(sf::Color(34, 44, 28));
            btn.setOutlineColor(sf::Color(80, 130, 90));
            btn.setOutlineThickness(1.f);
            m_window.draw(btn);
            // Draw the label, rendering any {W}{U}… tokens as mana pips inline.
            {
                const sf::Color col(180, 230, 180);
                const bool useSkin = SkinAssets::ready();
                float lx = r.left + 10.f;
                const std::string& s = o.label;
                size_t i = 0;
                while (i < s.size()) {
                    if (useSkin && s[i] == '{') {
                        size_t j = s.find('}', i + 1);
                        if (j == std::string::npos) break;
                        SkinAssets::drawManaToken(m_window, s.substr(i + 1, j - i - 1),
                                                  lx, r.top + 3.f, 15.f);
                        lx += 16.f;
                        i = j + 1;
                    } else {
                        size_t j = s.find('{', i);
                        std::string run = s.substr(i, (j == std::string::npos ? s.size() : j) - i);
                        sf::Text lbl(run, m_font, 13);
                        lbl.setStyle(sf::Text::Bold);
                        lbl.setFillColor(col);
                        lbl.setPosition(lx, r.top + 5.f);
                        drawText(lbl);
                        lx += lbl.getLocalBounds().left + lbl.getLocalBounds().width + 2.f;
                        i = (j == std::string::npos ? s.size() : j);
                    }
                }
            }
            m_manaAbilityHits.push_back({r, o.abilityIndex});
            ry += rowH;
        }
    }

    if (m_pauseMenuOpen) drawPauseMenu();

    m_window.display();
}

// ── Win/loss overlay ──────────────────────────────────────────────────────────

void GameWindow::renderGameOver() {
    using namespace Layout;

    // Dim the board
    sf::RectangleShape dim({WIN_W, WIN_H});
    dim.setFillColor(sf::Color(0, 0, 0, 160));
    m_window.draw(dim);

    // Central panel
    const float panW = 520.f, panH = 280.f;
    const float panX = (WIN_W - panW) * 0.5f;
    const float panY = (WIN_H - panH) * 0.5f;

    sf::RectangleShape panel({panW, panH});
    panel.setPosition(panX, panY);
    panel.setFillColor(sf::Color(12, 18, 28, 245));
    panel.setOutlineColor(sf::Color(180, 150, 40));
    panel.setOutlineThickness(3.f);
    m_window.draw(panel);

    uint8_t wid = m_tm.winnerId();
    std::string winner = m_game.player(wid).name();
    bool aliceWon = (wid == 0);

    // "[Winner] Wins!" headline
    {
        sf::Text headline(winner + " Wins!", m_font, 42);
        headline.setStyle(sf::Text::Bold);
        headline.setFillColor(aliceWon ? sf::Color(80, 220, 120) : sf::Color(220, 80, 80));
        auto lb = headline.getLocalBounds();
        headline.setPosition(panX + (panW - lb.width) * 0.5f, panY + 24.f);
        ui::applyTextScale(headline);
        drawText(headline);
    }

    // Life total summary
    {
        int life0 = m_game.player(0).life();
        int life1 = m_game.player(1).life();
        std::string summary = "Alice: " + std::to_string(life0) + " HP    "
                            + "Bob: "   + std::to_string(life1) + " HP";
        sf::Text sumTxt(summary, m_font, 16);
        sumTxt.setFillColor(sf::Color(190, 190, 180));
        auto lb = sumTxt.getLocalBounds();
        sumTxt.setPosition(panX + (panW - lb.width) * 0.5f, panY + 100.f);
        ui::applyTextScale(sumTxt);
        drawText(sumTxt);
    }

    // Win/loss record for this matchup
    {
        auto deckKey = [&]() -> std::string {
            std::string d0 = m_rematchDeck0.empty()
                           ? (m_rematchEditorDeck.name.empty() ? "Alice" : m_rematchEditorDeck.name)
                           : m_rematchDeck0.stem().string();
            std::string d1 = m_rematchAiDeckPath.empty() ? "AI" : m_rematchAiDeckPath.stem().string();
            return d0 + " vs " + d1;
        };
        auto it = m_deckStats.find(deckKey());
        if (it != m_deckStats.end()) {
            const DeckStats& s = it->second;
            int tot = s.wins + s.losses + s.draws;
            std::string rec = "Record: " + std::to_string(s.wins) + "W / "
                            + std::to_string(s.losses) + "L / "
                            + std::to_string(s.draws) + "D";
            if (tot > 0) rec += "  (" + std::to_string(s.wins * 100 / tot) + "% win)";
            sf::Text recTxt(rec, m_font, 11);
            recTxt.setFillColor(sf::Color(160, 170, 155));
            auto lb2 = recTxt.getLocalBounds();
            recTxt.setPosition(panX + (panW - lb2.width) * 0.5f, panY + 132.f);
            ui::applyTextScale(recTxt);
            drawText(recTxt);
        }
    }

    // Game statistics
    {
        char buf[128];
        std::snprintf(buf, sizeof(buf),
            "Turn %d  |  Alice dealt %d dmg  |  Bob dealt %d dmg",
            m_stats.turnsPlayed,
            m_stats.damageDealt[1],  // damage dealt TO Bob = Alice's damage
            m_stats.damageDealt[0]); // damage dealt TO Alice = Bob's damage
        sf::Text statTxt(buf, m_font, 10);
        statTxt.setFillColor(sf::Color(130, 140, 130));
        auto lb3 = statTxt.getLocalBounds();
        statTxt.setPosition(panX + (panW - lb3.width) * 0.5f - lb3.left, panY + 162.f);
        ui::applyTextScale(statTxt);
        drawText(statTxt);
    }

    // Buttons — "Play Again [R]" and "Main Menu [Esc]"
    const float btnW = 200.f, btnH = 44.f, btnGap = 20.f;
    const float btn0X = panX + (panW - 2 * btnW - btnGap) * 0.5f;
    const float btnY  = panY + panH - btnH - 24.f;

    auto drawBtn = [&](const std::string& label, float bx, sf::Color fill) {
        sf::RectangleShape btn({btnW, btnH});
        btn.setPosition(bx, btnY);
        btn.setFillColor(fill);
        btn.setOutlineColor(sf::Color(100, 120, 140));
        btn.setOutlineThickness(1.5f);
        m_window.draw(btn);
        sf::Text lt(label, m_font, 14);
        lt.setStyle(sf::Text::Bold);
        lt.setFillColor(sf::Color(220, 225, 215));
        auto lb = lt.getLocalBounds();
        lt.setPosition(bx + (btnW - lb.width) * 0.5f,
                       btnY + (btnH - lb.height) * 0.5f - 2.f);
        ui::applyTextScale(lt);
        drawText(lt);
    };

    // Tournament score bar
    if (m_tournamentMode || m_matchWins[0] > 0 || m_matchWins[1] > 0) {
        std::string matchScore = m_game.player(0).name() + " " + std::to_string(m_matchWins[0])
                               + " - " + std::to_string(m_matchWins[1])
                               + " " + m_game.player(1).name()
                               + "  (best-of-3)";
        sf::Text ms(matchScore, m_font, 11);
        ms.setStyle(sf::Text::Bold);
        ms.setFillColor(sf::Color(200, 220, 150));
        auto mb = ms.getLocalBounds();
        ms.setPosition(panX + (panW - mb.width) * 0.5f, panY + 152.f);
        drawText(ms);
    }

    // ── Per-game statistics ────────────────────────────────────────────────────
    {
        int turns = m_game.turnNumber();
        sf::Text statsT("Turn " + std::to_string(turns)
                        + "  •  Alice " + std::to_string(m_game.player(0).life()) + " HP"
                        + "  •  Bob "   + std::to_string(m_game.player(1).life()) + " HP",
                        m_font, 10);
        statsT.setFillColor(sf::Color(150, 165, 150));
        auto sb = statsT.getLocalBounds();
        statsT.setPosition(panX + (panW - sb.width) * 0.5f, panY + 132.f);
        drawText(statsT);

        // Poison / commander damage summary if relevant
        std::string extra;
        for (uint8_t pid = 0; pid < 2; ++pid) {
            if (m_game.player(pid).poisonCounters() > 0)
                extra += std::string(pid == 0 ? "Alice" : "Bob")
                      + " " + std::to_string(m_game.player(pid).poisonCounters()) + " poison  ";
            uint8_t opp = pid ^ 1;
            int cmd = m_game.player(pid).commanderDamageFrom(opp);
            if (cmd > 0)
                extra += std::string(pid == 0 ? "Alice" : "Bob")
                      + " " + std::to_string(cmd) + " cmd-dmg  ";
        }
        if (!extra.empty()) {
            sf::Text et(extra, m_font, 9);
            et.setFillColor(sf::Color(140, 150, 130));
            auto eb = et.getLocalBounds();
            et.setPosition(panX + (panW - eb.width) * 0.5f, panY + 146.f);
            drawText(et);
        }
    }

    drawBtn("Play Again  [R]",   btn0X,              sf::Color(35, 80, 50));
    drawBtn("Main Menu  [Esc]",  btn0X + btnW + btnGap, sf::Color(60, 40, 40));
    // Small "Save Replay" and "Load Replay" buttons below the main buttons
    {
        constexpr float repW = 115.f, repH = 22.f, repGap = 8.f;
        float repTotalW = repW * 2 + repGap;
        float repX0 = panX + (panW - repTotalW) * 0.5f;
        float repY = btnY + btnH + 8.f;

        auto drawSmallBtn = [&](const std::string& lbl, float bx) {
            sf::RectangleShape r({repW, repH});
            r.setPosition(bx, repY);
            r.setFillColor(sf::Color(28, 45, 68));
            r.setOutlineColor(sf::Color(50, 80, 110));
            r.setOutlineThickness(1.f);
            m_window.draw(r);
            sf::Text t(lbl, m_font, 9);
            t.setFillColor(sf::Color(140, 165, 195));
            auto rb = t.getLocalBounds();
            t.setPosition(bx + (repW - rb.width) * 0.5f - rb.left, repY + 5.f);
            drawText(t);
        };
        drawSmallBtn("Save Replay  [Ctrl+R]", repX0);
        drawSmallBtn("Load Replay",            repX0 + repW + repGap);
    }

    // Handle mouse click on game-over buttons (polled inline since we return early)
    // Note: actual click routing happens in handleEvents() which checks m_tm.isGameOver().
    // We expose the button regions via hitGameOverBtn() used by handleEvents().
}

// ── Rematch ───────────────────────────────────────────────────────────────────

void GameWindow::doRematch() {
    if (m_rematchUseEditor)
        startGameWithDeck(m_rematchEditorDeck, m_rematchAiDeckPath);
    else
        startGameWithDecks(m_rematchDeck0, m_rematchDeck1);
}

// ── Settings persistence ──────────────────────────────────────────────────────

static std::filesystem::path settingsPath() {
    namespace fs = std::filesystem;
    // %APPDATA%\mtg-forge\settings.ini  (Windows)
    if (const char* v = std::getenv("APPDATA"); v) {
        fs::path p = fs::path(v) / "mtg-forge";
        std::error_code ec;
        fs::create_directories(p, ec);
        return p / "settings.ini";
    }
    return "settings.ini";  // fallback: next to exe
}

void GameWindow::saveSettings() const {
    // Write forge.ini with INI sections for better readability and extensibility
    std::ofstream f(settingsPath());
    if (!f) return;
    f << "; Arcanum MTG Engine — forge.ini\n";
    f << "; Auto-generated. Edit with caution.\n\n";
    f << "[meta]\n";
    f << "version=2\n\n";
    f << "[decks]\n";
    f << "playerDeck="    << m_rematchDeck0.string() << '\n';
    f << "aiDeck="        << m_rematchDeck1.string() << '\n';
    f << "editorAiDeck="  << m_rematchAiDeckPath.string() << '\n';
    f << "useEditor="     << (m_rematchUseEditor ? 1 : 0) << '\n';
    f << '\n';

    f << "[display]\n";
    f << "fullscreen="    << (m_fullscreen ? 1 : 0) << '\n';
    f << "colorblind="    << (m_colorBlindMode ? 1 : 0) << '\n';
    f << "animspeed="     << m_animSpeed << '\n';
    f << '\n';

    f << "[audio]\n";
    f << "volume="        << static_cast<int>(m_sound.volume()) << '\n';
    f << "muted="         << (m_sound.muted() ? 1 : 0) << '\n';
    f << "sfxvolume="     << static_cast<int>(m_sound.sfxVolume()) << '\n';
    f << "uivolume="      << static_cast<int>(m_sound.uiVolume())  << '\n';
    f << "prioritychime=" << (m_priorityChime ? 1 : 0) << '\n';
    f << '\n';

    f << "[gameplay]\n";
    // Phase stop toggles (13 chars: '0' or '1' in step order)
    f << "phaseStops=";
    const bool arr[13] = {
        m_stops.untap, m_stops.upkeep, m_stops.draw,
        m_stops.main1, m_stops.beginCombat, m_stops.attackers, m_stops.blockers,
        m_stops.firstStrike, m_stops.combatDmg, m_stops.endCombat,
        m_stops.main2, m_stops.endStep, m_stops.cleanup
    };
    for (bool b : arr) f << (b ? '1' : '0');
    f << '\n';
}

void GameWindow::loadSettings() {
    std::ifstream f(settingsPath());
    if (!f) return;
    std::string line;
    while (std::getline(f, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        if      (key == "playerDeck")  m_rematchDeck0 = val;
        else if (key == "aiDeck")      m_rematchDeck1 = val;
        else if (key == "editorAiDeck") m_rematchAiDeckPath = val;
        else if (key == "useEditor")   m_rematchUseEditor = (val == "1");
        else if (key == "fullscreen" && val == "1") {
            m_fullscreen = false;
            toggleFullscreen();
        }
        else if (key == "colorblind") m_colorBlindMode = (val == "1");
        else if (key == "volume") {
            try { m_sound.setVolume(static_cast<float>(std::stoi(val))); } catch (...) {}
        }
        else if (key == "muted") m_sound.setMuted(val == "1");
        else if (key == "prioritychime") m_priorityChime = (val == "1");
        else if (key == "animspeed") {
            try { m_animSpeed = std::clamp(std::stof(val), 0.25f, 4.f); } catch (...) {}
        }
        else if (key == "sfxvolume") {
            try { m_sound.setSfxVolume(static_cast<float>(std::stoi(val))); } catch (...) {}
        }
        else if (key == "uivolume") {
            try { m_sound.setUiVolume(static_cast<float>(std::stoi(val)));  } catch (...) {}
        }
        else if (key == "language") {
            ui::Localization::instance().load(val);
        }
        else if (key == "phaseStops" && val.size() == 13) {
            bool* ptrs[13] = {
                &m_stops.untap, &m_stops.upkeep, &m_stops.draw,
                &m_stops.main1, &m_stops.beginCombat, &m_stops.attackers,
                &m_stops.blockers, &m_stops.firstStrike, &m_stops.combatDmg,
                &m_stops.endCombat, &m_stops.main2, &m_stops.endStep, &m_stops.cleanup
            };
            for (int i = 0; i < 13; ++i)
                *ptrs[i] = (val[static_cast<size_t>(i)] == '1');
        }
    }
}

} // namespace ui
