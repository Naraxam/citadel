#include "core/db/CardDb.h"
#include "ui/GameWindow.h"
#include "game/GameState.h"
#include "game/TurnManager.h"
#include "game/DeckLoader.h"
#include "game/StateBasedActions.h"
#include "game/ability/AbilityProcessor.h"
#include "game/ai/AiPlayer.h"
#include "game/ai/SaltDatabase.h"
#include <SFML/Graphics.hpp>
#include <atomic>
#include <chrono>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif

// ── Headless AI-vs-AI game for batch testing / benchmarking ──────────────────

static int runHeadless(const mtg::CardDb& db,
                       const std::string& deck0, const std::string& deck1,
                       int nGames, int maxTurns) {
    namespace fs = std::filesystem;
    int p0wins = 0, p1wins = 0, draws = 0;
    for (int g = 0; g < nGames; ++g) {
        mtg::GameState        game;
        mtg::AbilityProcessor abilities{game};
        mtg::TurnManager      tm{game};

        game.setCardDb(&db);
        game.setHumanInteractive(false);
        game.player(0) = mtg::Player{0, "P0"};
        game.player(1) = mtg::Player{1, "P1"};

        if (fs::exists(deck0)) mtg::DeckLoader::loadAndBuild(deck0, db, game, 0);
        if (fs::exists(deck1)) mtg::DeckLoader::loadAndBuild(deck1, db, game, 1);

        game.player(0).library().shuffle(game.rng());
        game.player(1).library().shuffle(game.rng());
        for (uint8_t pid = 0; pid < 2; ++pid)
            for (int i = 0; i < 7 && !game.player(pid).library().empty(); ++i)
                game.moveToZone(game.player(pid).library().front()->id,
                                mtg::ZoneType::Hand, pid);

        // AiPlayer::takeTurn() handles triggers, SBA, and game-over internally.
        mtg::AiPlayer ai0{0, game, abilities};
        mtg::AiPlayer ai1{1, game, abilities};

        int turn = 0;
        while (!tm.isGameOver() && turn < maxTurns) {
            bool over = (game.activePlayerId() == 0)
                        ? ai0.takeTurn(tm, &ai1)
                        : ai1.takeTurn(tm, &ai0);
            if (over) break;
            ++turn;
        }

        int winner = tm.isGameOver() ? tm.winnerId() : -1;
        if (winner == 0)      ++p0wins;
        else if (winner == 1) ++p1wins;
        else                   ++draws;
        std::cout << "Game " << (g+1) << "/" << nGames
                  << "  winner=" << winner
                  << "  turn=" << game.turnNumber() << '\n';
    }
    std::cout << "\nResults: P0=" << p0wins << "  P1=" << p1wins
              << "  Draw=" << draws << '\n';
    return 0;
}

int main(int /*argc*/, char* argv[]) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    // CITADEL_QUIET=1 suppresses verbose AI turn-by-turn output
    if (const char* q = std::getenv("CITADEL_QUIET"); q && q[0] == '1')
        mtg::g_aiQuiet = true;
#else
    if (const char* q = std::getenv("CITADEL_QUIET"); q && q[0] == '1')
        mtg::g_aiQuiet = true;
#endif
#ifdef _WIN32
    // Tell Windows not to DPI-scale our window — SFML handles scaling itself
    // via the view transform.  Without this, Windows blurs the window when
    // the display is set to >100% scaling.
    if (auto* fn = reinterpret_cast<BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT)>(
            GetProcAddress(GetModuleHandleW(L"user32.dll"),
                           "SetProcessDpiAwarenessContext"))) {
        fn(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    } else {
        SetProcessDPIAware();   // fallback for Windows 7/8
    }
#endif
    namespace fs = std::filesystem;

    fs::path cardFolder;
    if (const char* v = std::getenv("CITADEL_CARDS"); v)
        cardFolder = v;

    // Prefer a portable `res/cardsfolder` next to the exe, then the working dir,
    // then the user's %APPDATA%. This is what makes the app self-contained on
    // any machine; the Forge-path fallbacks below are only a dev convenience.
    if (cardFolder.empty() || !fs::exists(cardFolder)) {
        fs::path exeDir = fs::path(argv[0]).parent_path();
        std::vector<fs::path> bases = { exeDir, fs::current_path() };
        if (const char* ap = std::getenv("APPDATA"); ap && *ap)
            bases.push_back(fs::path(ap) / "CitadelMTG");
        for (const auto& base : bases) {
            for (const auto& sub : { fs::path("res") / "cardsfolder", fs::path("cardsfolder") }) {
                auto p = base / sub;
                if (fs::exists(p)) { cardFolder = p; break; }
            }
            if (!cardFolder.empty() && fs::exists(cardFolder)) break;
        }
    }

    if (cardFolder.empty() || !fs::exists(cardFolder)) {
        // CITADEL_RES → append cardsfolder
        if (const char* v = std::getenv("CITADEL_RES"); v) {
            auto p = fs::path(v) / "cardsfolder";
            if (fs::exists(p)) cardFolder = p;
        }
    }
    if (cardFolder.empty() || !fs::exists(cardFolder)) {
        // citadel-path.txt next to exe
        auto cfgPath = fs::current_path() / "citadel-path.txt";
        if (fs::exists(cfgPath)) {
            std::ifstream cfg(cfgPath);
            std::string line;
            if (std::getline(cfg, line)) {
                auto p = fs::path(line) / "cardsfolder";
                if (fs::exists(p)) cardFolder = p;
                else if (fs::exists(line)) cardFolder = line;
            }
        }
    }
    if (cardFolder.empty() || !fs::exists(cardFolder)) {
        for (const char* candidate : {
                R"(Z:\cardforge\res\cardsfolder)",
                R"(Z:\cardforge\cardsfolder)",
                R"(Z:\forge\res\cardsfolder)",
                R"(Z:\forge\cardsfolder)",
                R"(C:\Forge\res\cardsfolder)",
                R"(C:\Program Files\Forge\res\cardsfolder)",
                R"(C:\Users\Chris\source\repos\forge\forge-gui\res\cardsfolder)"}) {
            if (fs::exists(candidate)) { cardFolder = candidate; break; }
        }
    }

    std::cout << "Card folder: " << cardFolder << '\n';
    std::cout << "Exists:      " << (fs::exists(cardFolder) ? "yes" : "NO") << '\n';
    if (!fs::exists(cardFolder)) {
        std::cerr << "\nHint: set CITADEL_CARDS=<path>\\cardsfolder\n"
                  << "  or CITADEL_RES=<path>\\res\n"
                  << "  or create citadel-path.txt next to the exe with the res path.\n";
    }

    if (!fs::exists(cardFolder)) {
        std::cerr << "\nCard folder not found.\nSet CITADEL_CARDS=<path>\\cardsfolder\n";
        std::cin.get();
        return 1;
    }

    // ── --validate: load all card scripts and report parse errors, then exit ───
    for (int i = 1; argv[i]; ++i) {
        if (std::string(argv[i]) == "--validate") {
            std::cout << "Validating card scripts...\n";
            mtg::CardDb valDb;
            valDb.loadFromDirectory(cardFolder);
            // Also try ZIP if present
            auto zipPath = cardFolder.parent_path() / "cardsfolder.zip";
            if (fs::exists(zipPath)) valDb.loadFromZip(zipPath);
            valDb.loadCustomCards();   // include the user custom-card overlay
            int total = static_cast<int>(valDb.size());
            std::cout << "Validation complete: " << total << " cards loaded.\n";
            std::cout << "Run citadel_tests.exe for interaction tests.\n";
            return 0;
        }
    }

    // ── --headless deck0 deck1 [nGames] [maxTurns] ───────────────────────────
    // Runs AI-vs-AI games without opening a window, prints win/loss stats.
    {
        std::string headlessDeck0, headlessDeck1;
        int hlGames = 10, hlMaxTurns = 200;
        bool headless = false;
        for (int i = 1; argv[i]; ++i) {
            std::string a(argv[i]);
            if (a == "--headless") { headless = true; continue; }
            if (headless) {
                if (headlessDeck0.empty())       headlessDeck0 = a;
                else if (headlessDeck1.empty())  headlessDeck1 = a;
                else if (hlGames == 10)          hlGames    = std::stoi(a);
                else                              hlMaxTurns = std::stoi(a);
            }
        }
        if (headless) {
            mtg::CardDb hlDb;
            if (fs::exists(cardFolder / "cardsfolder.zip"))
                hlDb.loadFromZip((cardFolder / "cardsfolder.zip").string());
            else
                hlDb.loadFromDirectory(cardFolder.string());
            hlDb.loadCustomCards();
            hlDb.wireBackFaces();
            fs::path hlDataDir = fs::path(argv[0]).parent_path() / "data";
            if (!fs::exists(hlDataDir)) hlDataDir = fs::current_path() / "data";
            mtg::AiPlayer::loadCombos((hlDataDir / "combos.json").string());
            if (fs::exists(hlDataDir / "value_net.bin"))
                mtg::AiPlayer::loadValueNet((hlDataDir / "value_net.bin").string());
            return runHeadless(hlDb, headlessDeck0, headlessDeck1, hlGames, hlMaxTurns);
        }
    }

    fs::path dataDir = fs::path(argv[0]).parent_path() / "data";
    if (!fs::exists(dataDir)) dataDir = fs::current_path() / "data";

    try {
        ui::GameWindow window(cardFolder, dataDir);
        window.run();
    } catch (const std::exception& e) {
        std::cerr << "Startup failed: " << e.what() << '\n';
        std::cin.get();
        return 1;
    }

    return 0;
}
