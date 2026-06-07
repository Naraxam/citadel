#include "CardDownloadScreen.h"
#include "UiScale.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <unordered_map>

namespace ui {

CardDownloadScreen::CardDownloadScreen(const sf::Font& font, const mtg::CardDb& db)
    : m_font(font), m_db(db)
{
    rebuild();
}

CardDownloadScreen::~CardDownloadScreen() {
    // Detach any in-flight oracle fetch — it holds no pointers into us, so the
    // worker can finish (or be interrupted on process exit) without us blocking
    // on join here. WinHTTP cleans up its handles when the process exits.
    if (m_oracleThread && m_oracleThread->joinable())
        m_oracleThread->detach();
}

void CardDownloadScreen::rebuild() {
    m_missing.clear();
    namespace fs = std::filesystem;

    // Build cache path inline to avoid MSVC static-function parse issues
    std::string cachePath;
    {
        const char* apd = std::getenv("APPDATA");
        if (apd) cachePath = std::string(apd) + "\\CitadelMTG\\missing_art_cache.txt";
    }

    // Try loading from cache first (fast path — avoids full DB scan on every open)
    bool cacheValid = false;
    if (!cachePath.empty() && fs::exists(cachePath)) {
        std::error_code ec;
        if (fs::file_size(fs::path(cachePath), ec) > 0 && !ec) {
            std::ifstream cf(cachePath);
            std::string line;
            while (std::getline(cf, line)) {
                if (!line.empty() && !CardImageDownloader::isOnDisk(line))
                    m_missing.push_back(line);
            }
            cacheValid = true;
        }
    }

    if (!cacheValid) {
        m_missing.reserve(m_db.size());
        for (auto& [name, rules] : m_db)
            if (!CardImageDownloader::isOnDisk(name))
                m_missing.push_back(name);
        // Write cache for next time
        if (!cachePath.empty()) {
            std::error_code ec;
            fs::create_directories(fs::path(cachePath).parent_path(), ec);
            std::ofstream cf(cachePath);
            for (const auto& n : m_missing) cf << n << '\n';
        }
    }

    std::sort(m_missing.begin(), m_missing.end());
    m_scroll   = 0;
    m_started  = false;
    m_complete = false;
    m_snap.clear();
}

void CardDownloadScreen::update() {
    // If a background bulk-data fetch is running, watch for completion.
    if (m_fetchingOracle) {
        int pct = CardImageDownloader::fetchOracleProgress();
        if (pct < 0 && m_oracleThread && m_oracleThread->joinable()) {
            // Finished (success or failure).
            m_oracleThread->join();
            m_oracleThread.reset();
            m_fetchingOracle = false;
            // If we now have a catalog, kick off the image-download batch.
            // Otherwise leave m_started=false so the user can read the
            // status text (e.g. "Could not reach api.scryfall.com").
            if (!m_missing.empty()) {
                CardImageDownloader::start(m_missing);
                m_started = true;
            }
        }
    }
    if (!m_started) return;
    m_snap = CardImageDownloader::snapshot();
    if (!m_snap.empty() && !CardImageDownloader::isActive()) m_complete = true;
}

CardDownloadScreen::Result CardDownloadScreen::onEvent(const sf::Event& ev) {
    if (ev.type == sf::Event::MouseButtonPressed &&
        ev.mouseButton.button == sf::Mouse::Left) {
        float px = static_cast<float>(ev.mouseButton.x);
        float py = static_cast<float>(ev.mouseButton.y);
        if (hitBack(px, py)) return Result::Back;
        if (!m_started && !m_fetchingOracle && hitDownload(px, py)) {
            // If the Scryfall oracle catalog isn't loaded yet, fetch it on a
            // worker thread before starting image downloads. The UI polls
            // CardImageDownloader::fetchOracleProgress() / Status() for the
            // progress bar.
            if (CardImageDownloader::fetchOracleProgress() < 0 &&
                CardImageDownloader::totalCount() == 0 &&
                CardImageDownloader::fetchOracleStatus().empty()) {
                // Empty cache + idle + no prior status → first attempt.
            }
            // Always go through ensureOracleLoaded (which is a fast no-op if
            // already loaded). Kick off on a worker thread so the UI keeps
            // ticking and the progress bar updates.
            m_fetchingOracle = true;
            m_oracleThread = std::make_unique<std::thread>([] {
                CardImageDownloader::ensureOracleLoaded();
            });
        }
        // "Check for Updates" — invalidate cache and force a full rebuild
        if (!m_checkingUpdates && hitCheckUpdates(px, py)) {
            m_checkingUpdates = true;
            // Delete cache file so next rebuild does a full scan
            namespace fs = std::filesystem;
            std::string cachePath;
            const char* apd = std::getenv("APPDATA");
            if (apd) cachePath = std::string(apd) + "\\CitadelMTG\\missing_art_cache.txt";
            if (!cachePath.empty()) {
                std::error_code ec;
                fs::remove(cachePath, ec);
            }
            rebuild();
            m_checkingUpdates = false;
        }
    }
    if (ev.type == sf::Event::MouseWheelScrolled) {
        int maxScroll = std::max(0, static_cast<int>(m_missing.size()) - visibleRows());
        m_scroll = std::clamp(m_scroll - static_cast<int>(ev.mouseWheelScroll.delta),
                              0, maxScroll);
    }
    return Result::None;
}

// ── Drawing ───────────────────────────────────────────────────────────────────

sf::Text CardDownloadScreen::makeText(const std::string& s, unsigned sz, sf::Color col) const {
    sf::Text t;
    t.setFont(m_font);
    // setString(std::string) interprets bytes as Latin-1 and mangles UTF-8
    // card names like "Lim-Dûl's Vault". fromUtf8 decodes them properly.
    t.setString(sf::String::fromUtf8(s.begin(), s.end()));
    t.setCharacterSize(sz);
    t.setFillColor(col);
    ui::applyTextScale(t);
    return t;
}

bool CardDownloadScreen::hitBack(float px, float py) const {
    float bx = WIN_W - 160.f, by = 20.f;
    return px >= bx && px < bx + 130.f && py >= by && py < by + 44.f;
}

bool CardDownloadScreen::hitDownload(float px, float py) const {
    float bx = (WIN_W - BTN_W) * 0.5f;
    return px >= bx && px < bx + BTN_W && py >= BTN_Y && py < BTN_Y + BTN_H;
}

bool CardDownloadScreen::hitCheckUpdates(float px, float py) const {
    float bx = (WIN_W - BTN_W) * 0.5f + BTN_W + 12.f;
    return px >= bx && px < bx + 160.f && py >= BTN_Y && py < BTN_Y + BTN_H;
}

void CardDownloadScreen::drawProgressBar(sf::RenderTarget& t,
    float x, float y, float w, float h, int percent, sf::Color fill) const {
    sf::RectangleShape bg({w, h});
    bg.setPosition(x, y);
    bg.setFillColor(sf::Color(40, 40, 40));
    bg.setOutlineColor(sf::Color(80, 80, 80));
    bg.setOutlineThickness(1.f);
    t.draw(bg);
    if (percent > 0) {
        float fw = w * static_cast<float>(std::min(percent, 100)) / 100.f;
        sf::RectangleShape bar({fw, h});
        bar.setPosition(x, y);
        bar.setFillColor(fill);
        t.draw(bar);
    }
}

void CardDownloadScreen::drawRow(sf::RenderTarget& t,
                                  const DownloadEntry& e, float y) const {
    using State = DownloadEntry::State;

    // Row background
    sf::RectangleShape bg({LIST_W, ROW_H - 2.f});
    bg.setPosition(LIST_X, y);
    bg.setFillColor(sf::Color(22, 28, 22));
    t.draw(bg);

    // Card name
    auto nameText = makeText(e.name, 13, sf::Color(200, 210, 200));
    nameText.setPosition(LIST_X + 6.f, y + 6.f);
    t.draw(nameText);

    // State / progress on the right side
    float rightX = LIST_X + LIST_W * 0.6f;
    float rightW = LIST_W * 0.38f;

    switch (e.state) {
        case State::Queued: {
            auto s = makeText("Queued", 12, sf::Color(130, 130, 100));
            s.setPosition(rightX, y + 7.f);
            t.draw(s);
            break;
        }
        case State::Downloading: {
            drawProgressBar(t, rightX, y + 8.f, rightW - 55.f, 14.f,
                            e.percent, sf::Color(60, 140, 220));
            auto pct = makeText(std::to_string(e.percent) + "%", 12, sf::Color(180, 200, 230));
            pct.setPosition(rightX + rightW - 50.f, y + 7.f);
            t.draw(pct);
            break;
        }
        case State::Done: {
            auto s = makeText("Done", 12, sf::Color(80, 200, 100));
            s.setPosition(rightX, y + 7.f);
            t.draw(s);
            break;
        }
        case State::Failed: {
            auto s = makeText("Not found", 12, sf::Color(200, 80, 80));
            s.setPosition(rightX, y + 7.f);
            t.draw(s);
            break;
        }
    }
}

void CardDownloadScreen::draw(sf::RenderWindow& w) const {
    w.clear(sf::Color(12, 16, 12));

    // ── Title ──────────────────────────────────────────────────────────────────
    {
        auto title = makeText("Download Card Art", 32, sf::Color(210, 175, 55));
        title.setStyle(sf::Text::Bold);
        title.setPosition(LIST_X, 20.f);
        w.draw(title);
    }

    // ── Back button ────────────────────────────────────────────────────────────
    {
        float bx = WIN_W - 160.f, by = 20.f;
        sf::RectangleShape btn({130.f, 44.f});
        btn.setPosition(bx, by);
        btn.setFillColor(sf::Color(50, 30, 30));
        btn.setOutlineColor(sf::Color(120, 60, 60));
        btn.setOutlineThickness(2.f);
        w.draw(btn);
        auto bt = makeText("Back", 16, sf::Color(220, 180, 180));
        bt.setStyle(sf::Text::Bold);
        auto bb = bt.getLocalBounds();
        bt.setPosition(bx + (130.f - bb.width) * 0.5f - bb.left,
                       by + (44.f - bb.height) * 0.5f - bb.top);
        w.draw(bt);
    }

    // ── Stats bar ──────────────────────────────────────────────────────────────
    {
        int total = static_cast<int>(m_missing.size());

        // Build snap lookup
        std::unordered_map<std::string, const DownloadEntry*> snapMap;
        for (auto& e : m_snap) snapMap[e.name] = &e;

        int done = 0, failed = 0;
        for (auto& e : m_snap) {
            if (e.state == DownloadEntry::State::Done)   ++done;
            if (e.state == DownloadEntry::State::Failed) ++failed;
        }

        std::string statStr;
        int  orPct      = CardImageDownloader::fetchOracleProgress();
        auto orStatus   = CardImageDownloader::fetchOracleStatus();

        if (m_fetchingOracle || orPct >= 0) {
            // Bulk-data fetch in progress (or just completed) — show that.
            statStr = orStatus.empty() ? "Preparing Scryfall catalog…" : orStatus;
        } else if (!m_started) {
            statStr = std::to_string(total) + " card images missing from library";
            if (!orStatus.empty())   // last fetch result, e.g. an error
                statStr += "   |   " + orStatus;
        } else if (m_complete) {
            statStr = "Complete — " + std::to_string(done) + " downloaded, "
                    + std::to_string(failed) + " not found on Scryfall";
        } else {
            int active = static_cast<int>(m_snap.size());
            statStr = "Downloading: " + std::to_string(done) + " / "
                    + std::to_string(active) + " done";
            if (failed > 0) statStr += ", " + std::to_string(failed) + " failed";
        }
        auto st = makeText(statStr, 15, sf::Color(160, 180, 155));
        st.setPosition(LIST_X, 78.f);
        w.draw(st);

        // Overall progress bar
        if (m_fetchingOracle || orPct >= 0) {
            drawProgressBar(w, LIST_X, 108.f, LIST_W, 16.f,
                            std::max(0, orPct), sf::Color(180, 130, 50));
        } else if (m_started && !m_snap.empty()) {
            int pct = (int)(m_snap.size() > 0
                ? (done + failed) * 100 / (int)m_snap.size() : 0);
            drawProgressBar(w, LIST_X, 108.f, LIST_W, 16.f, pct,
                            sf::Color(50, 140, 80));
        }
    }

    // ── Column headers ─────────────────────────────────────────────────────────
    {
        auto h1 = makeText("Card Name", 12, sf::Color(100, 120, 100));
        h1.setPosition(LIST_X + 6.f, LIST_TOP - 22.f);
        w.draw(h1);
        auto h2 = makeText("Status", 12, sf::Color(100, 120, 100));
        h2.setPosition(LIST_X + LIST_W * 0.6f, LIST_TOP - 22.f);
        w.draw(h2);
    }

    // ── Card list ──────────────────────────────────────────────────────────────
    {
        // Build snap lookup
        std::unordered_map<std::string, const DownloadEntry*> snapMap;
        for (auto& e : m_snap) snapMap[e.name] = &e;

        // Collect visible entries: not-yet-started = show as missing,
        // started = show Queued/Downloading, Done/Failed = hide (already counted above)
        std::vector<DownloadEntry> rows;
        rows.reserve(m_missing.size());

        for (auto& name : m_missing) {
            auto it = snapMap.find(name);
            if (it != snapMap.end()) {
                // Hide completed entries from the list
                if (it->second->state == DownloadEntry::State::Done ||
                    it->second->state == DownloadEntry::State::Failed)
                    continue;
                rows.push_back(*it->second);
            } else {
                // Not started yet
                DownloadEntry e;
                e.name  = name;
                e.state = DownloadEntry::State::Queued;
                rows.push_back(e);
            }
        }

        int maxScroll = std::max(0, static_cast<int>(rows.size()) - visibleRows());
        int scroll    = std::min(m_scroll, maxScroll);
        int end       = std::min(scroll + visibleRows(), static_cast<int>(rows.size()));

        // Clip to list area
        sf::View listView = w.getView();
        w.setView(sf::View(sf::FloatRect(0, LIST_TOP, WIN_W, LIST_BOT - LIST_TOP)));

        for (int i = scroll; i < end; ++i) {
            float y = LIST_TOP + static_cast<float>(i - scroll) * ROW_H;
            drawRow(w, rows[i], y);
        }
        w.setView(listView);
    }

    // ── Download button (shown only before starting) ───────────────────────────
    if (!m_started) {
        float bx = (WIN_W - BTN_W) * 0.5f;
        sf::RectangleShape btn({BTN_W, BTN_H});
        btn.setPosition(bx, BTN_Y);
        btn.setFillColor(sf::Color(28, 72, 42));
        btn.setOutlineColor(sf::Color(60, 160, 80));
        btn.setOutlineThickness(2.f);
        w.draw(btn);
        auto bt = makeText("Download All Missing Art", 18, sf::Color(180, 230, 180));
        bt.setStyle(sf::Text::Bold);
        auto bb = bt.getLocalBounds();
        bt.setPosition(bx + (BTN_W - bb.width) * 0.5f - bb.left,
                       BTN_Y + (BTN_H - bb.height) * 0.5f - bb.top);
        w.draw(bt);
    }

    if (m_complete) {
        auto done = makeText("All done! Return to main menu and start playing.",
                             16, sf::Color(120, 220, 140));
        auto b = done.getLocalBounds();
        done.setPosition((WIN_W - b.width) * 0.5f - b.left, BTN_Y + 10.f);
        w.draw(done);
    }
}

} // namespace ui
