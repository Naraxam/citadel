#pragma once
#include "CardImageDownloader.h"
#include "../core/db/CardDb.h"
#include <SFML/Graphics.hpp>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace ui {

class CardDownloadScreen {
public:
    CardDownloadScreen(const sf::Font& font, const mtg::CardDb& db);
    ~CardDownloadScreen();

    enum class Result { None, Back };
    Result onEvent(const sf::Event& ev);
    void   update();   // poll download progress each frame
    void   draw(sf::RenderWindow& w) const;

private:
    const sf::Font&    m_font;
    const mtg::CardDb& m_db;

    std::vector<std::string> m_missing;   // all card names not on disk (at refresh time)
    std::vector<DownloadEntry> m_snap;    // latest snapshot from downloader

    bool m_started   = false;   // Download All has been clicked
    bool m_complete  = false;
    int  m_scroll    = 0;

    // Bulk-data fetch state: when the Scryfall catalog is missing, clicking
    // Download triggers a background fetch (~100 MB) before image downloads
    // can start. m_oracleThread runs ensureOracleLoaded(); the UI polls
    // CardImageDownloader::fetchOracleProgress() for the progress bar.
    bool m_fetchingOracle = false;
    std::unique_ptr<std::thread> m_oracleThread;

    static constexpr float WIN_W     = 1560.f;
    static constexpr float WIN_H     = 800.f;
    static constexpr float LIST_X    = 80.f;
    static constexpr float LIST_W    = WIN_W - 160.f;
    static constexpr float LIST_TOP  = 210.f;
    static constexpr float LIST_BOT  = 720.f;
    static constexpr float ROW_H     = 30.f;
    static constexpr float BTN_Y     = 735.f;
    static constexpr float BTN_H     = 48.f;
    static constexpr float BTN_W     = 380.f;

    int visibleRows() const { return static_cast<int>((LIST_BOT - LIST_TOP) / ROW_H); }
    bool hitBack(float px, float py) const;
    bool hitDownload(float px, float py) const;
    bool hitCheckUpdates(float px, float py) const;
    bool m_checkingUpdates = false;

    void rebuild();
    void drawRow(sf::RenderTarget& t, const DownloadEntry& e, float y) const;
    void drawProgressBar(sf::RenderTarget& t, float x, float y, float w, float h,
                         int percent, sf::Color fill) const;
    sf::Text makeText(const std::string& s, unsigned sz, sf::Color col) const;
};

} // namespace ui
