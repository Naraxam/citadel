#pragma once
#include "../game/ai/PlaystyleProfile.h"
#include "../core/db/CardDb.h"
#include "WinRateTracker.h"
#include <SFML/Graphics.hpp>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace ui {

// Headless AI-vs-AI training launcher.
class TrainingScreen {
public:
    TrainingScreen(const sf::Font& font, const mtg::CardDb& db);
    ~TrainingScreen();

    enum class Action { None, Back };
    Action onEvent(const sf::Event& ev);
    void   draw(sf::RenderWindow& w) const;

    static constexpr float WIN_W    = 1560.f;
    static constexpr float WIN_H    = 800.f;

private:
    const sf::Font&      m_font;
    const mtg::CardDb&   m_db;

    std::vector<std::filesystem::path> m_decks;
    std::vector<std::string>           m_deckNames;
    std::vector<std::string>           m_deckPersonalities; // locked personality name per deck (empty = none)
    std::vector<int>                   m_deckProfileIdx;    // index into m_profiles for locked personality, -1 if none
    std::vector<mtg::PlaystyleProfile> m_profiles;

    int  m_deckSel[2]    = {0, 1};
    int  m_persSel[2]    = {0, 1};
    int  m_deckScroll[2] = {0, 0};
    int  m_deckHover[2]  = {-1, -1};
    int  m_persHover[2]  = {-1, -1};
    bool m_randomMode    = false;

    int m_gameCount = 10;

    std::thread        m_thread;
    std::atomic<bool>  m_running{false};
    std::atomic<bool>  m_done{false};
    std::atomic<int>      m_gamesCompleted{0};
    std::atomic<int>      m_currentTurn{0};      // total half-turns done across all workers
    std::atomic<int64_t>  m_lastTurnMs{0};        // epoch-ms of most recent completed turn
    std::atomic<int>      m_p0wins{0};
    std::atomic<int>      m_p1wins{0};
    int                   m_workerCount = 1;      // set at training start from hardware_concurrency
    std::string           m_resultPath;
    WinRateTracker        m_tracker;

    static constexpr float COL_W    = 660.f;
    static constexpr float COL0_X   = 20.f;
    static constexpr float COL1_X   = 880.f;
    static constexpr float DECK_Y   = 70.f;
    static constexpr float DECK_H   = 270.f;
    static constexpr float PERS_Y   = DECK_Y + DECK_H + 12.f;
    static constexpr float PERS_H   = 290.f;
    static constexpr float ITEM_H   = 24.f;
    static constexpr int   DECK_VIS = static_cast<int>((DECK_H - 28.f) / ITEM_H);

    static std::filesystem::path defaultStatsPath();

    // Matches by profile name, color identity, or archetype (case-insensitive).
    int findProfileByName(const std::string& name) const;

    void scanDecks();
    void startTraining();
    void clearTrainingData();

    int  hitDeckList  (float px, float py, int col) const;
    int  hitPersList  (float px, float py, int col) const;
    bool hitRun       (float px, float py) const;
    bool hitRandom    (float px, float py) const;
    bool hitBack      (float px, float py) const;
    bool hitMinus     (float px, float py) const;
    bool hitPlus      (float px, float py) const;
    bool hitClear     (float px, float py) const;

    void drawText      (sf::RenderTarget&, const std::string& s,
                        float x, float y, unsigned sz,
                        sf::Color col, bool bold = false) const;
    void drawButton    (sf::RenderTarget&, const std::string& label,
                        float x, float y, float w, float h,
                        sf::Color fill, sf::Color textCol) const;
    void drawDeckPanel (sf::RenderTarget&, int col) const;
    void drawPersPanel (sf::RenderTarget&, int col) const;
};

} // namespace ui
