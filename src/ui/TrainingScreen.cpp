#include "TrainingScreen.h"
#include "../sim/GameRunner.h"
#include "../game/DeckLoader.h"
#include "UiScale.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <random>
#include <sstream>
#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <malloc.h>   // _resetstkoflw
#endif

namespace ui {

namespace fs = std::filesystem;

static sf::Color identityColor(const std::string& ci) {
    if (ci == "GW") return sf::Color(100, 200, 120);
    if (ci == "WU") return sf::Color(120, 160, 230);
    if (ci == "UB") return sf::Color( 80, 110, 200);
    if (ci == "BR") return sf::Color(200,  70,  70);
    if (ci == "RG") return sf::Color(200, 130,  40);
    if (ci == "WB") return sf::Color(180, 160, 200);
    if (ci == "UR") return sf::Color(140,  90, 210);
    if (ci == "BG") return sf::Color( 80, 160,  90);
    if (ci == "RW") return sf::Color(230, 160,  60);
    if (ci == "GU") return sf::Color( 60, 190, 175);
    return sf::Color(150, 150, 150);
}

/*static*/ std::filesystem::path TrainingScreen::defaultStatsPath() {
    if (const char* ap = std::getenv("APPDATA"))
        return fs::path(ap) / "CitadelMTG" / "training" / "personality_stats.json";
    return fs::current_path() / "training" / "personality_stats.json";
}

TrainingScreen::TrainingScreen(const sf::Font& font, const mtg::CardDb& db)
    : m_font(font), m_db(db), m_tracker(defaultStatsPath())
{
    m_profiles = mtg::PlaystyleProfile::allPresets();
    scanDecks();
    if ((int)m_decks.size() > 1) { m_deckSel[0] = 0; m_deckSel[1] = 1; }
    else if (!m_decks.empty())   { m_deckSel[0] = 0; m_deckSel[1] = 0; }
}

TrainingScreen::~TrainingScreen() {
    if (m_thread.joinable()) m_thread.join();
}

// Returns true if the filename stem begins with "ai" (case-insensitive).
static bool stemStartsWithAi(const fs::path& p) {
    auto stem = p.stem().string();
    return stem.size() >= 2 &&
           std::tolower(static_cast<unsigned char>(stem[0])) == 'a' &&
           std::tolower(static_cast<unsigned char>(stem[1])) == 'i';
}

void TrainingScreen::scanDecks() {
    m_decks.clear();
    m_deckNames.clear();
    m_deckPersonalities.clear();
    m_deckProfileIdx.clear();

    auto scanDir = [&](const fs::path& dir) {
        if (dir.empty() || !fs::exists(dir)) return;
        try {
            for (auto& e : fs::recursive_directory_iterator(dir))
                if (e.path().extension() == ".dck" && stemStartsWithAi(e.path()))
                    m_decks.push_back(e.path());
        } catch (...) {}
    };
    if (const char* ap = std::getenv("APPDATA"))
        scanDir(fs::path(ap) / "CitadelMTG" / "decks");
    std::sort(m_decks.begin(), m_decks.end());

    for (auto& p : m_decks) {
        std::string stem = p.stem().string();
        m_deckNames.push_back(stem);

        // Prefer explicit Personality= from the deck file's [metadata] section.
        std::string locked;
        if (auto deck = mtg::DeckLoader::loadFromFile(p))
            locked = deck->personality;

        // Fallback: derive from filename by stripping the leading "AI-" / "ai_" prefix
        // and running through the normalized matcher.  This lets future AI decks
        // auto-bind without needing an explicit Personality= line.
        if (locked.empty()) {
            std::string candidate = stem;
            if (candidate.size() > 3) {
                std::string pre3 = candidate.substr(0, 3);
                for (char& c : pre3) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (pre3 == "ai-" || pre3 == "ai_")
                    candidate = candidate.substr(3);
            }
            int idx = findProfileByName(candidate);
            if (idx >= 0) locked = m_profiles[idx].name;
        }

        m_deckPersonalities.push_back(locked);
        m_deckProfileIdx.push_back(findProfileByName(locked));
    }
}

int TrainingScreen::findProfileByName(const std::string& name) const {
    if (name.empty()) return -1;
    // Exact match on name, colorIdentity, or archetype
    for (int i = 0; i < (int)m_profiles.size(); ++i) {
        if (m_profiles[i].name          == name) return i;
        if (m_profiles[i].colorIdentity == name) return i;
        if (m_profiles[i].archetype     == name) return i;
    }
    // Case-insensitive fallback
    auto lower = [](std::string s) {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    const std::string nl = lower(name);
    for (int i = 0; i < (int)m_profiles.size(); ++i) {
        if (lower(m_profiles[i].name)          == nl) return i;
        if (lower(m_profiles[i].colorIdentity) == nl) return i;
        if (lower(m_profiles[i].archetype)     == nl) return i;
    }
    // Normalized fallback: strip all non-alphanumeric chars and compare lowercase.
    // Handles "AI-AzoriousMill" matching "Azorius Mill", "DimirNinjutsu" matching
    // "Dimir Ninjutsu", etc. for future AI decks without an explicit Personality= line.
    auto normalize = [](const std::string& s) {
        std::string r;
        r.reserve(s.size());
        for (char c : s)
            if (std::isalnum(static_cast<unsigned char>(c)))
                r += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return r;
    };
    const std::string nn = normalize(name);
    if (!nn.empty()) {
        for (int i = 0; i < (int)m_profiles.size(); ++i) {
            if (normalize(m_profiles[i].name)      == nn) return i;
            if (normalize(m_profiles[i].archetype) == nn) return i;
        }
    }
    return -1;
}

#ifdef _WIN32
// Must be a plain function with no C++ objects so __try/__except is legal.
static int runGameSafe(mtg::GameRunner& runner, int maxTurns, DWORD& outCode) {
    __try {
        return runner.runGame(maxTurns);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        outCode = GetExceptionCode();
        // Stack overflow destroys the guard page — restore it so the thread
        // remains safe to continue executing after the SEH catch.
        if (outCode == 0xC00000FD)
            _resetstkoflw();
        return -99;
    }
}

// Worker thread proc for CreateThread — plain WINAPI function so
// std::function<> is allocated on the heap and passed via LPVOID.
static DWORD WINAPI trainingWorkerProc(LPVOID param) {
    auto* fn = static_cast<std::function<void()>*>(param);
    (*fn)();
    delete fn;
    return 0;
}
#endif

// ── Training report ───────────────────────────────────────────────────────────

using RawStats = WinRateTracker::RawStats;

static std::string fmtPct(float pct) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%.0f%%", pct);
    return buf;
}

static std::string fmtDelta(float delta, bool hasBaseline) {
    if (!hasBaseline) return "  new";
    char buf[12];
    if (delta > 0.4f)       std::snprintf(buf, sizeof(buf), " +%.0f%%", delta);
    else if (delta < -0.4f) std::snprintf(buf, sizeof(buf), "  %.0f%%", delta);
    else                    std::snprintf(buf, sizeof(buf), "  ~~");
    return buf;
}

static void writeTrainingReport(
    const std::string& reportPath,
    bool randomMode,
    int workerCount,
    int totalGames,
    int crashes,
    int64_t durationMs,
    const std::unordered_map<std::string, RawStats>& runByPersonality,
    const std::unordered_map<std::string, RawStats>& runByMatchup,
    const std::unordered_map<std::string, RawStats>& beforeSnapshot,
    const WinRateTracker& tracker)
{
    std::ofstream f(reportPath);
    if (!f) return;

    // ── Timestamp ─────────────────────────────────────────────────────────────
    {
        std::time_t t = std::time(nullptr);
        char tsBuf[32];
        std::strftime(tsBuf, sizeof(tsBuf), "%Y-%m-%d %H:%M", std::localtime(&t));
        f << "===== AI TRAINING REPORT =====\n";
        f << "Date     : " << tsBuf << "\n";
    }
    {
        int64_t s  = durationMs / 1000;
        int64_t m  = s / 60; s %= 60;
        f << "Mode     : " << (randomMode ? "Random Match" : "Fixed")
          << "  (" << workerCount << " worker" << (workerCount != 1 ? "s" : "") << ")\n";
        f << "Games    : " << totalGames
          << "  (crashes: " << crashes << ")\n";
        f << "Duration : " << m << "m " << s << "s\n";
    }

    // ── Per-personality session results ───────────────────────────────────────
    f << "\n";
    f << "PERSONALITY RESULTS — THIS SESSION\n";
    f << std::string(66, '-') << "\n";
    f << std::left  << std::setw(26) << "Personality"
      << std::right << std::setw(6)  << "Games"
      << std::setw(5) << "W"
      << std::setw(5) << "L"
      << std::setw(5) << "D"
      << std::setw(7) << "Win%"
      << std::setw(12) << "Δ Lifetime"
      << "\n"
      << std::string(66, '-') << "\n";

    // Sort by session win% descending.
    std::vector<std::pair<std::string, RawStats>> persRows(
        runByPersonality.begin(), runByPersonality.end());
    std::sort(persRows.begin(), persRows.end(), [](const auto& a, const auto& b) {
        int ta = a.second[0] + a.second[1] + a.second[2];
        int tb = b.second[0] + b.second[1] + b.second[2];
        float wa = ta ? float(a.second[0]) / ta : 0.f;
        float wb = tb ? float(b.second[0]) / tb : 0.f;
        return wa > wb;
    });

    for (const auto& [name, arr] : persRows) {
        int games = arr[0] + arr[1] + arr[2];
        if (games == 0) continue;
        float sessionPct = float(arr[0]) / games * 100.f;

        // Lifetime win% now vs before this session.
        float lifetimeNow  = tracker.winRate(name, 0);
        float delta        = 0.f;
        bool  hasBaseline  = false;
        auto  it = beforeSnapshot.find(name);
        if (it != beforeSnapshot.end()) {
            int bt = it->second[0] + it->second[1] + it->second[2];
            if (bt > 0) {
                float beforePct = float(it->second[0]) / bt * 100.f;
                delta = (lifetimeNow >= 0.f ? lifetimeNow * 100.f : 0.f) - beforePct;
                hasBaseline = true;
            }
        }

        f << std::left  << std::setw(26) << name
          << std::right << std::setw(6)  << games
          << std::setw(5) << arr[0]
          << std::setw(5) << arr[1]
          << std::setw(5) << arr[2]
          << std::setw(7) << fmtPct(sessionPct)
          << std::setw(12) << fmtDelta(delta, hasBaseline)
          << "\n";
    }

    // ── Matchup breakdown ─────────────────────────────────────────────────────
    std::vector<std::pair<std::string, RawStats>> matchRows(
        runByMatchup.begin(), runByMatchup.end());
    // Keep only matchups with ≥ 2 games; sort by games played desc.
    matchRows.erase(
        std::remove_if(matchRows.begin(), matchRows.end(),
            [](const auto& p) {
                return p.second[0] + p.second[1] + p.second[2] < 2;
            }),
        matchRows.end());
    std::sort(matchRows.begin(), matchRows.end(), [](const auto& a, const auto& b) {
        int ta = a.second[0] + a.second[1] + a.second[2];
        int tb = b.second[0] + b.second[1] + b.second[2];
        return ta > tb;
    });

    if (!matchRows.empty()) {
        f << "\n";
        f << "MATCHUPS THIS SESSION  (min 2 games, left side = P0)\n";
        f << std::string(66, '-') << "\n";
        int shown = 0;
        for (const auto& [key, arr] : matchRows) {
            if (++shown > 20) break;
            int   games   = arr[0] + arr[1] + arr[2];
            float winPct  = float(arr[0]) / games * 100.f;
            char  record[24];
            std::snprintf(record, sizeof(record), "%d-%d-%d", arr[0], arr[1], arr[2]);
            f << std::left  << std::setw(46) << key
              << std::right << std::setw(8)  << record
              << std::setw(6) << fmtPct(winPct)
              << "\n";
        }
    }

    // ── Lifetime rankings ─────────────────────────────────────────────────────
    f << "\n";
    f << "LIFETIME WIN RATES\n";
    f << std::string(66, '-') << "\n";
    f << std::left  << std::setw(26) << "Personality"
      << std::right << std::setw(8)  << "Lifetime"
      << std::setw(7) << "L10"
      << std::setw(7) << "L100"
      << std::setw(7) << "L1K"
      << "\n"
      << std::string(66, '-') << "\n";

    // Same order as personality results table.
    for (const auto& [name, arr] : persRows) {
        auto fmt = [&](int n) -> std::string {
            float r = tracker.winRate(name, n);
            return r < 0.f ? "--" : fmtPct(r * 100.f);
        };
        f << std::left  << std::setw(26) << name
          << std::right << std::setw(8)  << fmt(0)
          << std::setw(7) << fmt(10)
          << std::setw(7) << fmt(100)
          << std::setw(7) << fmt(1000)
          << "\n";
    }

    f << "\n(report saved to: " << reportPath << ")\n";
}

void TrainingScreen::startTraining() {
    if (m_running) return;
    if (!m_randomMode) {
        if (m_deckSel[0] < 0 || m_deckSel[0] >= (int)m_decks.size()) return;
        if (m_deckSel[1] < 0 || m_deckSel[1] >= (int)m_decks.size()) return;
    } else {
        if (m_decks.empty()) return;
    }
    if (m_thread.joinable()) m_thread.join();

    m_done           = false;
    m_running        = true;
    m_gamesCompleted = 0;
    m_currentTurn    = 0;
    m_lastTurnMs     = 0;
    m_p0wins         = 0;
    m_p1wins         = 0;

    // Fixed-mode deck/profile (used when randomMode=false)
    std::string deck0Fixed = m_randomMode ? "" : m_decks[m_deckSel[0]].string();
    std::string deck1Fixed = m_randomMode ? "" : m_decks[m_deckSel[1]].string();
    mtg::PlaystyleProfile prof0Fixed = m_randomMode ? mtg::PlaystyleProfile::Default() : m_profiles[m_persSel[0]];
    mtg::PlaystyleProfile prof1Fixed = m_randomMode ? mtg::PlaystyleProfile::Default() : m_profiles[m_persSel[1]];

    // Random-mode: build (deckPath, profileIndex) pairs from all AI decks
    struct AiEntry { std::string path; int profileIdx; };
    std::vector<AiEntry> aiEntries;
    if (m_randomMode) {
        for (int i = 0; i < (int)m_decks.size(); ++i) {
            int pidx = -1;
            if (i < (int)m_deckPersonalities.size() && !m_deckPersonalities[i].empty())
                pidx = findProfileByName(m_deckPersonalities[i]);
            aiEntries.push_back({m_decks[i].string(), pidx});
        }
    }

    int  games      = m_gameCount;
    bool randomMode = m_randomMode;

    fs::path outDir;
    if (const char* ap = std::getenv("APPDATA"))
        outDir = fs::path(ap) / "CitadelMTG" / "training";
    else
        outDir = fs::current_path() / "training";
    fs::create_directories(outDir);
    m_resultPath = (outDir / "training_results.csv").string();
    std::string resultPath  = m_resultPath;
    std::string logPath     = (outDir / "training_log.txt").string();
    std::string reportPath  = (outDir / "training_report.txt").string();

    // Leave one core for the UI thread; cap at 8 to bound memory pressure.
    {
        int hw = static_cast<int>(std::thread::hardware_concurrency());
        m_workerCount = std::clamp(hw > 0 ? hw - 1 : 4, 1, 8);
    }
    int workerCount = m_workerCount;

    // Snapshot win-rate baselines before this run so the report can show deltas.
    auto beforeSnapshot = m_tracker.snapshot();

    m_thread = std::thread([this, deck0Fixed, deck1Fixed, prof0Fixed, prof1Fixed,
                             games, resultPath, logPath, reportPath, randomMode,
                             aiEntries, profiles = m_profiles, workerCount,
                             beforeSnapshot = std::move(beforeSnapshot)]() mutable {
        std::ofstream log(logPath);
        std::ofstream out(resultPath);
        if (out.is_open()) out << "winner,turns\n";

        std::mutex       logMtx, csvMtx;
        std::atomic<int> nextGame{0};

        auto nowMs = []() -> int64_t {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
        };
        auto toResult = [](int winner, int myId) -> GameResult {
            if (winner == myId) return GameResult::Win;
            if (winner == -1)   return GameResult::Draw;
            return GameResult::Loss;
        };

        // Per-run accumulators for the training report.
        struct RunData {
            std::mutex mtx;
            std::unordered_map<std::string, RawStats> byPersonality;
            std::unordered_map<std::string, RawStats> byMatchup;
            int crashes = 0;
        };
        RunData runData;
        int64_t runStartMs = nowMs();

        // Each worker claims games from nextGame until exhausted.
        auto workerFn = [&]() {
            std::mt19937 rng(std::random_device{}());
            while (true) {
                int g = nextGame.fetch_add(1, std::memory_order_relaxed);
                if (g >= games) break;

                std::string d0, d1;
                mtg::PlaystyleProfile prof0, prof1;

                if (randomMode && !aiEntries.empty()) {
                    int n  = static_cast<int>(aiEntries.size());
                    int i0 = std::uniform_int_distribution<int>(0, n - 1)(rng);
                    int i1 = (n > 1) ? i0 : 0;
                    while (i1 == i0 && n > 1)
                        i1 = std::uniform_int_distribution<int>(0, n - 1)(rng);
                    d0    = aiEntries[i0].path;
                    d1    = aiEntries[i1].path;
                    int p = static_cast<int>(profiles.size());
                    prof0 = (aiEntries[i0].profileIdx >= 0)
                                ? profiles[aiEntries[i0].profileIdx]
                                : profiles[std::uniform_int_distribution<int>(0, p - 1)(rng)];
                    prof1 = (aiEntries[i1].profileIdx >= 0)
                                ? profiles[aiEntries[i1].profileIdx]
                                : profiles[std::uniform_int_distribution<int>(0, p - 1)(rng)];
                } else {
                    d0 = deck0Fixed; d1 = deck1Fixed;
                    prof0 = prof0Fixed; prof1 = prof1Fixed;
                }

                // Buffer this game's log so output from parallel games doesn't interleave.
                std::ostringstream gamelog;
                if (randomMode)
                    gamelog << "game " << (g + 1) << " [" << prof0.name
                            << " vs " << prof1.name << "]\n";
                else
                    gamelog << "game " << (g + 1) << "\n";

                mtg::GameRunner runner(m_db, d0, d1);
                runner.setProfile(0, prof0);
                runner.setProfile(1, prof1);
                runner.setMinTurnMs(0); // no throttle — bulk parallel training
                runner.setDebugLog(&gamelog);
                runner.setTurnCallback([this, &nowMs](int /*turn*/) {
                    m_currentTurn.fetch_add(1, std::memory_order_relaxed);
                    m_lastTurnMs.store(nowMs(), std::memory_order_relaxed);
                });

#ifdef _WIN32
                DWORD sehCode = 0;
                int w = runGameSafe(runner, 200, sehCode);
                if (w == -99) {
                    gamelog << "SEH crash in game " << (g + 1) << " code=0x"
                            << std::hex << sehCode << std::dec << ", continuing\n";
                    { std::lock_guard<std::mutex> lk(logMtx);
                      if (log.is_open()) { log << gamelog.str(); log.flush(); } }
                    { std::lock_guard<std::mutex> lk(runData.mtx);
                      ++runData.crashes; }
                    m_gamesCompleted.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
#else
                int w = runner.runGame(200);
#endif
                // Post-game diagnostic markers — identify which step triggers the crash
                auto postMark = [&](const char* tag) {
                    std::lock_guard<std::mutex> lk(logMtx);
                    if (log.is_open()) {
                        log << "[post-" << tag << " g=" << (g + 1) << "]\n";
                        log.flush();
                    }
                };

                postMark("csv");
                { std::lock_guard<std::mutex> lk(csvMtx);
                  if (out.is_open()) out << w << ',' << runner.lastTurns() << '\n'; }

                postMark("tracker0");
                m_tracker.record(prof0.name, toResult(w, 0));
                postMark("tracker1");
                m_tracker.record(prof1.name, toResult(w, 1));
                postMark("tracker-matchup0");
                m_tracker.record(prof0.name + "|" + prof1.name, toResult(w, 0));
                postMark("tracker-matchup1");
                m_tracker.record(prof1.name + "|" + prof0.name, toResult(w, 1));

                postMark("atomics");
                if      (w == 0) m_p0wins.fetch_add(1, std::memory_order_relaxed);
                else if (w == 1) m_p1wins.fetch_add(1, std::memory_order_relaxed);

                // Accumulate per-run stats for the training report.
                postMark("rundata");
                {
                    std::lock_guard<std::mutex> lk(runData.mtx);
                    auto add = [w](RawStats& s, int myId) {
                        if (w == myId)  ++s[0];
                        else if (w == -1) ++s[2];
                        else            ++s[1];
                    };
                    add(runData.byPersonality[prof0.name], 0);
                    add(runData.byPersonality[prof1.name], 1);
                    add(runData.byMatchup[prof0.name + " vs " + prof1.name], 0);
                }

                postMark("log");
                { std::lock_guard<std::mutex> lk(logMtx);
                  if (log.is_open()) { log << gamelog.str(); log.flush(); } }

                postMark("done");
                m_gamesCompleted.fetch_add(1, std::memory_order_relaxed);
            }
        };

        // Spawn N workers and wait for all to finish.
        // On Windows use CreateThread with an 8 MB stack — the default 1 MB can
        // overflow in debug builds where each frame carries /RTC runtime overhead.
#ifdef _WIN32
        std::vector<HANDLE> workerHandles;
        workerHandles.reserve(workerCount);
        for (int i = 0; i < workerCount; ++i) {
            HANDLE h = CreateThread(nullptr,
                                    8ULL * 1024 * 1024,       // 8 MB stack
                                    trainingWorkerProc,
                                    new std::function<void()>(workerFn),
                                    0, nullptr);
            if (h) workerHandles.push_back(h);
        }
        if (!workerHandles.empty())
            WaitForMultipleObjects(static_cast<DWORD>(workerHandles.size()),
                                   workerHandles.data(), TRUE, INFINITE);
        for (HANDLE h : workerHandles) CloseHandle(h);
#else
        std::vector<std::thread> workers;
        workers.reserve(workerCount);
        for (int i = 0; i < workerCount; ++i)
            workers.emplace_back(workerFn);
        for (auto& t : workers)
            t.join();
#endif

        m_tracker.save();

        writeTrainingReport(reportPath, randomMode, workerCount,
                            games, runData.crashes,
                            nowMs() - runStartMs,
                            runData.byPersonality,
                            runData.byMatchup,
                            beforeSnapshot,
                            m_tracker);

        m_running = false;
        m_done    = true;
    });
}

void TrainingScreen::clearTrainingData() {
    fs::path outDir;
    if (const char* ap = std::getenv("APPDATA"))
        outDir = fs::path(ap) / "CitadelMTG" / "training";
    else
        outDir = fs::current_path() / "training";

    // Truncate the results CSV to just the header.
    { std::ofstream f(outDir / "training_results.csv", std::ios::trunc);
      if (f) f << "winner,turns\n"; }

    // Wipe in-memory and on-disk personality stats.
    m_tracker.clear();
}

// ── Hit-testing ───────────────────────────────────────────────────────────────

int TrainingScreen::hitDeckList(float px, float py, int col) const {
    float cx = (col == 0) ? COL0_X : COL1_X;
    if (px < cx || px >= cx + COL_W) return -1;
    float listY = DECK_Y + 28.f;
    if (py < listY || py >= DECK_Y + DECK_H) return -1;
    int row = static_cast<int>((py - listY) / ITEM_H) + m_deckScroll[col];
    if (row < 0 || row >= (int)m_decks.size()) return -1;
    return row;
}

int TrainingScreen::hitPersList(float px, float py, int col) const {
    float cx = (col == 0) ? COL0_X : COL1_X;
    if (px < cx || px >= cx + COL_W) return -1;
    float listY = PERS_Y + 28.f;
    if (py < listY || py >= PERS_Y + PERS_H) return -1;
    int row = static_cast<int>((py - listY) / ITEM_H);
    if (row < 0 || row >= (int)m_profiles.size()) return -1;
    return row;
}

// Layout constants shared by hit tests and draw:
//   [  Run Training  ] [  Random Match  ]
static constexpr float kBtnW   = 210.f;
static constexpr float kBtnGap = 18.f;
static constexpr float kBtnY   = TrainingScreen::WIN_H - 86.f;
static constexpr float kBtnH   = 44.f;
static constexpr float kRunX   = (TrainingScreen::WIN_W - 2 * kBtnW - kBtnGap) * 0.5f;
static constexpr float kRndX   = kRunX + kBtnW + kBtnGap;

bool TrainingScreen::hitRun(float px, float py) const {
    return px >= kRunX && px < kRunX + kBtnW && py >= kBtnY && py < kBtnY + kBtnH;
}

bool TrainingScreen::hitRandom(float px, float py) const {
    return px >= kRndX && px < kRndX + kBtnW && py >= kBtnY && py < kBtnY + kBtnH;
}

bool TrainingScreen::hitBack(float px, float py) const {
    return px >= 14.f && px < 110.f && py >= 14.f && py < 52.f;
}

bool TrainingScreen::hitMinus(float px, float py) const {
    float cx = WIN_W * 0.5f - 90.f;
    return px >= cx && px < cx + 34.f && py >= WIN_H - 126.f && py < WIN_H - 126.f + 26.f;
}

bool TrainingScreen::hitPlus(float px, float py) const {
    float cx = WIN_W * 0.5f + 56.f;
    return px >= cx && px < cx + 34.f && py >= WIN_H - 126.f && py < WIN_H - 126.f + 26.f;
}

bool TrainingScreen::hitClear(float px, float py) const {
    float cx = WIN_W * 0.5f + 104.f; // right of [+]
    return px >= cx && px < cx + 110.f && py >= WIN_H - 126.f && py < WIN_H - 126.f + 26.f;
}

// ── Event handling ────────────────────────────────────────────────────────────

TrainingScreen::Action TrainingScreen::onEvent(const sf::Event& ev) {
    if (ev.type == sf::Event::KeyPressed &&
        ev.key.code == sf::Keyboard::Escape)
        return Action::Back;

    if (ev.type == sf::Event::MouseMoved) {
        float px = static_cast<float>(ev.mouseMove.x);
        float py = static_cast<float>(ev.mouseMove.y);
        for (int c = 0; c < 2; ++c) {
            m_deckHover[c] = hitDeckList(px, py, c);
            m_persHover[c] = hitPersList(px, py, c);
        }
    }

    if (ev.type == sf::Event::MouseWheelScrolled) {
        int delta = ev.mouseWheelScroll.delta > 0 ? -3 : 3;
        int maxS  = std::max(0, (int)m_decks.size() - DECK_VIS);
        float sx  = ev.mouseWheelScroll.x;
        float sy  = ev.mouseWheelScroll.y;
        for (int c = 0; c < 2; ++c) {
            float cx = (c == 0) ? COL0_X : COL1_X;
            if (sx >= cx && sx < cx + COL_W &&
                sy >= DECK_Y && sy < DECK_Y + DECK_H)
                m_deckScroll[c] = std::clamp(m_deckScroll[c] + delta, 0, maxS);
        }
    }

    if (ev.type == sf::Event::MouseButtonPressed &&
        ev.mouseButton.button == sf::Mouse::Left) {
        float px = static_cast<float>(ev.mouseButton.x);
        float py = static_cast<float>(ev.mouseButton.y);

        if (hitBack(px, py))  return Action::Back;
        if (!m_running) {
            if (hitMinus (px, py)) { m_gameCount = std::max(1,   m_gameCount - 10); return Action::None; }
            if (hitPlus  (px, py)) { m_gameCount = std::min(500, m_gameCount + 10); return Action::None; }
            if (hitClear (px, py)) { clearTrainingData(); return Action::None; }
            if (hitRun   (px, py)) { m_randomMode = false; startTraining(); return Action::None; }
            if (hitRandom(px, py) && !m_decks.empty()) {
                m_randomMode = true; startTraining(); return Action::None;
            }
        }

        for (int c = 0; c < 2; ++c) {
            int row = hitDeckList(px, py, c);
            if (row >= 0) {
                m_deckSel[c] = row;
                // Auto-apply locked personality if the deck has one.
                if (row < (int)m_deckPersonalities.size() &&
                    !m_deckPersonalities[row].empty()) {
                    int pidx = findProfileByName(m_deckPersonalities[row]);
                    if (pidx >= 0) m_persSel[c] = pidx;
                }
                return Action::None;
            }
            row = hitPersList(px, py, c);
            if (row >= 0) { m_persSel[c] = row; return Action::None; }
        }
    }
    return Action::None;
}

// ── Drawing ───────────────────────────────────────────────────────────────────

void TrainingScreen::drawText(sf::RenderTarget& t, const std::string& s,
                               float x, float y, unsigned sz,
                               sf::Color col, bool bold) const {
    if (s.empty()) return;
    sf::Text txt;
    txt.setFont(m_font);
    txt.setString(s);
    txt.setCharacterSize(sz);
    txt.setFillColor(col);
    if (bold) txt.setStyle(sf::Text::Bold);
    txt.setPosition(x, y);
    ui::applyTextScale(txt);
    t.draw(txt);
}

void TrainingScreen::drawButton(sf::RenderTarget& t, const std::string& label,
                                 float x, float y, float w, float h,
                                 sf::Color fill, sf::Color textCol) const {
    sf::RectangleShape rect({w, h});
    rect.setPosition(x, y);
    rect.setFillColor(fill);
    rect.setOutlineColor(sf::Color(80, 100, 120));
    rect.setOutlineThickness(1.5f);
    t.draw(rect);

    sf::Text txt;
    txt.setFont(m_font);
    txt.setString(label);
    txt.setCharacterSize(14);
    txt.setStyle(sf::Text::Bold);
    txt.setFillColor(textCol);
    auto b = txt.getLocalBounds();
    txt.setPosition(x + (w - b.width) * 0.5f - b.left,
                    y + (h - b.height) * 0.5f - b.top);
    ui::applyTextScale(txt);
    t.draw(txt);
}

void TrainingScreen::drawDeckPanel(sf::RenderTarget& t, int col) const {
    float cx  = (col == 0) ? COL0_X : COL1_X;
    const char* label = (col == 0) ? "PLAYER 0 DECK" : "PLAYER 1 DECK";

    sf::RectangleShape bg({COL_W, DECK_H});
    bg.setPosition(cx, DECK_Y);
    bg.setFillColor(sf::Color(18, 22, 34));
    bg.setOutlineColor(col == 0 ? sf::Color(50, 120, 90) : sf::Color(120, 50, 50));
    bg.setOutlineThickness(1.5f);
    t.draw(bg);

    drawText(t, label, cx + 10.f, DECK_Y + 6.f, 13,
             col == 0 ? sf::Color(80, 200, 130) : sf::Color(200, 100, 100), true);

    sf::RectangleShape sep({COL_W, 1.f});
    sep.setPosition(cx, DECK_Y + 27.f);
    sep.setFillColor(sf::Color(50, 70, 90));
    t.draw(sep);

    if (m_decks.empty()) {
        drawText(t, "No decks found — convert precon JSONs first.",
                 cx + 12.f, DECK_Y + 44.f, 11, sf::Color(100, 90, 80));
        return;
    }

    float listY  = DECK_Y + 28.f;
    int   scroll = m_deckScroll[col];
    int   sel    = m_deckSel[col];
    int   hover  = m_deckHover[col];
    int   visEnd = std::min(scroll + DECK_VIS, (int)m_decks.size());

    for (int i = scroll; i < visEnd; ++i) {
        float iy   = listY + (i - scroll) * ITEM_H;
        bool  iSel = (i == sel), iHov = (i == hover);
        sf::Color rowBg = iSel ? sf::Color(35, 72, 50)
                        : iHov ? sf::Color(28, 42, 62)
                        : (i % 2 == 0 ? sf::Color(20, 24, 38) : sf::Color(22, 28, 42));
        sf::RectangleShape row({COL_W, ITEM_H});
        row.setPosition(cx, iy);
        row.setFillColor(rowBg);
        t.draw(row);
        if (iSel) {
            sf::RectangleShape bar({3.f, ITEM_H});
            bar.setPosition(cx, iy);
            bar.setFillColor(col == 0 ? sf::Color(80, 200, 120) : sf::Color(200, 80, 80));
            t.draw(bar);
        }
        // Locked-personality badge on the right side of the row
        bool hasBadge = (i < (int)m_deckProfileIdx.size() && m_deckProfileIdx[i] >= 0);
        if (hasBadge) {
            const auto& bp  = m_profiles[m_deckProfileIdx[i]];
            sf::Color ciCol = identityColor(bp.colorIdentity);
            sf::RectangleShape badge({30.f, ITEM_H - 8.f});
            badge.setPosition(cx + COL_W - 38.f, iy + 4.f);
            badge.setFillColor(ciCol);
            badge.setOutlineColor(sf::Color(0, 0, 0, 80));
            badge.setOutlineThickness(1.f);
            t.draw(badge);
            drawText(t, bp.colorIdentity, cx + COL_W - 37.f, iy + 5.f, 9,
                     sf::Color(10, 10, 10), true);
        }

        float nameMaxW = hasBadge ? 36.f : 46.f;
        std::string nm = m_deckNames[i];
        if (nm.size() > static_cast<size_t>(nameMaxW))
            nm = nm.substr(0, static_cast<size_t>(nameMaxW) - 2) + "..";
        drawText(t, nm, cx + 12.f, iy + 5.f, 12,
                 iSel ? sf::Color(100, 240, 150) : sf::Color(195, 200, 212));
    }

    int total = (int)m_decks.size();
    if (total > DECK_VIS) {
        float sbH    = DECK_H - 28.f;
        float thumbH = std::max(16.f, sbH * DECK_VIS / (float)total);
        float thumbY = DECK_Y + 28.f + (sbH - thumbH) * scroll / (float)std::max(1, total - DECK_VIS);
        sf::RectangleShape track({5.f, sbH});
        track.setPosition(cx + COL_W - 6.f, DECK_Y + 28.f);
        track.setFillColor(sf::Color(28, 34, 50));
        t.draw(track);
        sf::RectangleShape thumb({5.f, thumbH});
        thumb.setPosition(cx + COL_W - 6.f, thumbY);
        thumb.setFillColor(sf::Color(70, 110, 155));
        t.draw(thumb);
    }
}

void TrainingScreen::drawPersPanel(sf::RenderTarget& t, int col) const {
    float cx  = (col == 0) ? COL0_X : COL1_X;
    const char* label = (col == 0) ? "P0 PERSONALITY" : "P1 PERSONALITY";

    sf::RectangleShape bg({COL_W, PERS_H});
    bg.setPosition(cx, PERS_Y);
    bg.setFillColor(sf::Color(18, 22, 34));
    bg.setOutlineColor(sf::Color(90, 70, 50));
    bg.setOutlineThickness(1.5f);
    t.draw(bg);

    drawText(t, label, cx + 10.f, PERS_Y + 6.f, 13, sf::Color(210, 175, 100), true);

    // Column x-positions
    const float kC10  = cx + 280.f;
    const float kC100 = cx + 335.f;
    const float kC1K  = cx + 390.f;
    const float kCAll = cx + 445.f;
    // "vs Opp" column — shows per-matchup lifetime win rate against the other selected personality
    const int   oppCol    = col ^ 1;
    const auto& oppProf   = m_profiles[m_persSel[oppCol]];
    const float kCVs      = cx + 510.f;
    std::string vsHdr     = "vs " + oppProf.colorIdentity;

    const sf::Color kHdrCol(100, 110, 130);
    drawText(t, "L10",  kC10,  PERS_Y + 7.f, 9, kHdrCol, true);
    drawText(t, "L100", kC100, PERS_Y + 7.f, 9, kHdrCol, true);
    drawText(t, "L1K",  kC1K,  PERS_Y + 7.f, 9, kHdrCol, true);
    drawText(t, "All",  kCAll, PERS_Y + 7.f, 9, kHdrCol, true);
    drawText(t, vsHdr,  kCVs,  PERS_Y + 7.f, 9, sf::Color(160, 140, 80), true);

    sf::RectangleShape sep({COL_W, 1.f});
    sep.setPosition(cx, PERS_Y + 27.f);
    sep.setFillColor(sf::Color(90, 70, 50));
    t.draw(sep);

    const float itemH  = 24.f;
    const float listY  = PERS_Y + 28.f;
    int         sel    = m_persSel[col];
    int         hover  = m_persHover[col];

    auto fmtKey = [&](const std::string& key, int lastN) -> std::string {
        float r = m_tracker.winRate(key, lastN);
        if (r < 0.f) return "--";
        char buf[8];
        std::snprintf(buf, sizeof(buf), "%d%%", static_cast<int>(r * 100.f + 0.5f));
        return buf;
    };
    auto rateColor = [](float r) -> sf::Color {
        if (r < 0.f)    return sf::Color(70, 75, 90);
        if (r >= 0.55f) return sf::Color(80, 205, 120);
        if (r <= 0.45f) return sf::Color(210, 90, 80);
        return sf::Color(180, 185, 195);
    };

    for (int i = 0; i < (int)m_profiles.size(); ++i) {
        const auto& p   = m_profiles[i];
        float        iy = listY + i * itemH;
        bool         iS = (i == sel), iH = (i == hover);

        sf::Color rowBg = iS ? sf::Color(60, 45, 20)
                        : iH ? sf::Color(35, 32, 42)
                        : (i % 2 == 0 ? sf::Color(20, 24, 38) : sf::Color(22, 28, 42));
        sf::RectangleShape row({COL_W, itemH});
        row.setPosition(cx, iy);
        row.setFillColor(rowBg);
        t.draw(row);

        if (iS) {
            sf::RectangleShape bar({3.f, itemH});
            bar.setPosition(cx, iy);
            bar.setFillColor(sf::Color(240, 190, 60));
            t.draw(bar);
        }

        sf::Color ciCol = identityColor(p.colorIdentity);
        sf::RectangleShape badge({26.f, itemH - 8.f});
        badge.setPosition(cx + 8.f, iy + 4.f);
        badge.setFillColor(ciCol);
        badge.setOutlineColor(sf::Color(0, 0, 0, 80));
        badge.setOutlineThickness(1.f);
        t.draw(badge);
        drawText(t, p.colorIdentity, cx + 8.f, iy + 5.f, 9, sf::Color(10, 10, 10), true);

        drawText(t, p.name, cx + 42.f, iy + 5.f, 12,
                 iS ? sf::Color(240, 200, 80) : sf::Color(195, 200, 212));

        // Overall win-rate columns (L10 / L100 / L1K / All)
        static const int   kWindows[4] = { 10, 100, 1000, 0 };
        const float kColX[4] = { kC10, kC100, kC1K, kCAll };
        for (int w = 0; w < 4; ++w) {
            float r = m_tracker.winRate(p.name, kWindows[w]);
            drawText(t, fmtKey(p.name, kWindows[w]), kColX[w], iy + 6.f, 9,
                     rateColor(r));
        }

        // Matchup column: this personality's win rate specifically vs the opponent
        std::string vsKey = p.name + "|" + oppProf.name;
        float vsR = m_tracker.winRate(vsKey, 0);
        drawText(t, fmtKey(vsKey, 0), kCVs, iy + 6.f, 9, rateColor(vsR));
    }
}

void TrainingScreen::draw(sf::RenderWindow& w) const {
    w.clear(sf::Color(12, 14, 20));

    drawText(w, "AI Training", (WIN_W - 180.f) * 0.5f, 18.f, 30,
             sf::Color(200, 175, 75), true);

    for (int c = 0; c < 2; ++c) {
        drawDeckPanel(w, c);
        drawPersPanel(w, c);
    }

    // ── Game count control ────────────────────────────────────────────────
    float cy = WIN_H - 126.f;   // well above the run button
    float cx = WIN_W * 0.5f;

    drawText(w, "Games:", cx - 134.f, cy + 5.f, 14, sf::Color(160, 175, 190));

    // [-] count [+]
    sf::RectangleShape cntBox({58.f, 26.f});
    cntBox.setPosition(cx - 50.f, cy);
    cntBox.setFillColor(sf::Color(22, 28, 42));
    cntBox.setOutlineColor(sf::Color(60, 80, 100));
    cntBox.setOutlineThickness(1.f);
    w.draw(cntBox);
    drawText(w, std::to_string(m_gameCount), cx - 30.f, cy + 4.f, 14,
             sf::Color(220, 225, 230), true);

    bool idle = !m_running;
    drawButton(w, "-", cx - 90.f, cy, 34.f, 26.f,
               idle ? sf::Color(35, 42, 52) : sf::Color(22, 26, 32),
               idle ? sf::Color(190, 170, 130) : sf::Color(80, 85, 95));
    drawButton(w, "+", cx + 56.f, cy, 34.f, 26.f,
               idle ? sf::Color(35, 42, 52) : sf::Color(22, 26, 32),
               idle ? sf::Color(190, 170, 130) : sf::Color(80, 85, 95));
    drawButton(w, "Clear Stats", cx + 104.f, cy, 110.f, 26.f,
               idle ? sf::Color(60, 28, 28) : sf::Color(22, 26, 32),
               idle ? sf::Color(210, 120, 110) : sf::Color(80, 85, 95));

    // ── Run Training + Random Match buttons ──────────────────────────────
    bool canRun = idle && !m_decks.empty() && m_deckSel[0] >= 0 && m_deckSel[1] >= 0;
    bool canRnd = idle && (int)m_decks.size() >= 2;

    drawButton(w,
               (m_running && !m_randomMode) ? "Training..." :
               m_done                        ? "Run Again"   : "Run Training",
               kRunX, kBtnY, kBtnW, kBtnH,
               canRun    ? sf::Color(30, 80, 50) :
               m_running ? sf::Color(22, 40, 60) : sf::Color(35, 42, 52),
               canRun    ? sf::Color(80, 220, 120) :
               m_running ? sf::Color(100, 160, 220) : sf::Color(90, 100, 115));

    drawButton(w,
               (m_running && m_randomMode) ? "Random..." : "Random Match",
               kRndX, kBtnY, kBtnW, kBtnH,
               canRnd    ? sf::Color(45, 55, 100) :
               m_running ? sf::Color(22, 40, 60) : sf::Color(35, 42, 52),
               canRnd    ? sf::Color(140, 175, 255) :
               m_running ? sf::Color(100, 160, 220) : sf::Color(90, 100, 115));

    // ── Status bar ────────────────────────────────────────────────────────
    float sy = WIN_H - 20.f;
    if (m_running) {
        int     done     = m_gamesCompleted.load(std::memory_order_relaxed);
        int     turns    = m_currentTurn.load(std::memory_order_relaxed);
        int64_t lastMs   = m_lastTurnMs.load(std::memory_order_relaxed);
        int64_t nowMs    = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now().time_since_epoch()).count();
        int64_t stuckSec = lastMs ? (nowMs - lastMs) / 1000 : 0;

        std::string s = std::to_string(done) + " / " + std::to_string(m_gameCount) +
                        " games  |  " + std::to_string(m_workerCount) + " workers" +
                        "  |  " + std::to_string(turns) + " turns";
        sf::Color statusCol(120, 180, 240);
        if (stuckSec >= 10) {
            s += "  [stuck " + std::to_string(stuckSec) + "s]";
            statusCol = stuckSec >= 30 ? sf::Color(230, 80, 60) : sf::Color(240, 160, 50);
        }
        drawText(w, s, (WIN_W - 420.f) * 0.5f, sy, 12, statusCol);
    } else if (m_done) {
        int p0w = m_p0wins.load(), p1w = m_p1wins.load();
        std::string s = "Done!  P0=" + std::to_string(p0w) +
                        "  P1=" + std::to_string(p1w) +
                        "  Draws=" + std::to_string(m_gameCount - p0w - p1w) +
                        "  ->  " + m_resultPath;
        drawText(w, s, 16.f, sy, 11, sf::Color(140, 215, 140));
    }

    // Back button
    drawButton(w, "< Back", 14.f, 14.f, 96.f, 38.f,
               sf::Color(35, 42, 52), sf::Color(160, 175, 190));

    // Selections summary (centre)
    if (!m_decks.empty()) {
        float sumY = PERS_Y + PERS_H + 4.f;
        std::string d0 = (m_deckSel[0] >= 0) ? m_deckNames[m_deckSel[0]] : "(none)";
        std::string d1 = (m_deckSel[1] >= 0) ? m_deckNames[m_deckSel[1]] : "(none)";
        if (d0.size() > 34) d0 = d0.substr(0, 32) + "..";
        if (d1.size() > 34) d1 = d1.substr(0, 32) + "..";
        drawText(w, "P0: " + d0 + "  [" + m_profiles[m_persSel[0]].name + "]",
                 COL0_X + 4.f, sumY, 11, sf::Color(100, 200, 130));
        drawText(w, "P1: " + d1 + "  [" + m_profiles[m_persSel[1]].name + "]",
                 COL1_X + 4.f, sumY, 11, sf::Color(200, 100, 100));
    }
}

} // namespace ui
