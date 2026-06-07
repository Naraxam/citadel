#include "DeckBrowserScreen.h"
#include "UIColors.h"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <numeric>

namespace ui {

namespace {
// Use the sf::Color(r,g,b) constructor — NOT {r,g,b} aggregate init.
// Aggregate init zero-initialises the alpha member → fully transparent.
const sf::Color kPanelActive  (50,  70,  60);
const sf::Color kPanelInactive(35,  45,  40);
const sf::Color kItemSelected (80,  130, 100);
const sf::Color kItemNormal   (30,  40,  35);
const sf::Color kTextBright   (230, 230, 200);
const sf::Color kTextDim      (180, 180, 160); // slightly brighter for readability
} // namespace

// ── Construction ──────────────────────────────────────────────────────────────

DeckBrowserScreen::DeckBrowserScreen(const sf::Font& font,
                                      const mtg::CardDb& db,
                                      const std::filesystem::path& searchRoot)
    : m_font(font), m_db(db)
{
    scanDecks(searchRoot);
    // Pre-select first deck for both players if any exist
    if (!m_decks.empty()) {
        m_selected[0] = m_decks[0];
        m_selected[1] = m_decks[std::min(1, static_cast<int>(m_decks.size())-1)];
        m_names[0]    = m_deckNames[0];
        m_names[1]    = m_deckNames[std::min(1, static_cast<int>(m_decks.size())-1)];
        m_selectedIdx[1] = std::min(1, static_cast<int>(m_decks.size())-1);
    }
}

void DeckBrowserScreen::scanDecks(const std::filesystem::path& root) {
    namespace fs = std::filesystem;
    if (root.empty() || !fs::exists(root)) return;

    // Only search subdirectories known to contain decks — avoid scanning the
    // entire res tree (cardsfolder alone has 32,000+ txt files).
    // Use the filename stem as the display name; loading every file would freeze.
    // Directories known to contain human-readable named decks.
    // Deliberately excludes "quest", "ai", "deckgendecks", "geneticaidecks"
    // which store thousands of anonymously numbered files (1.dck, 2.dck…).
    static constexpr const char* kDeckDirs[] = {
        "adventure", "decks", "cube", "conquest", "sealed", "draft"
    };

    // Returns true if every character in s is a digit (e.g. "1", "100").
    // Used to skip numbered quest/AI deck files.
    auto isPurelyNumeric = [](const std::string& s) {
        return !s.empty() &&
               std::all_of(s.begin(), s.end(),
                           [](unsigned char c){ return std::isdigit(c); });
    };

    auto addDir = [&](const fs::path& dir) {
        if (!fs::exists(dir)) return;
        std::error_code ec;
        for (const auto& e : fs::recursive_directory_iterator(dir, ec)) {
            if (ec) { ec.clear(); continue; }
            if (!e.is_regular_file(ec)) continue;
            if (e.path().extension() != ".dck") continue;
            std::string stem = e.path().stem().string();
            if (isPurelyNumeric(stem)) continue; // skip "1.dck", "100.dck" etc.
            m_decks.push_back(e.path());
            m_deckNames.push_back(std::move(stem));
        }
    };

    for (const char* sub : kDeckDirs)
        addDir(root / sub);

    // Fallback: one level deep at root (no recursion to avoid numbered floods)
    if (m_decks.empty()) {
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(root, ec)) {
            if (ec) { ec.clear(); continue; }
            if (!e.is_regular_file()) continue;
            if (e.path().extension() != ".dck") continue;
            std::string stem = e.path().stem().string();
            if (isPurelyNumeric(stem)) continue;
            m_decks.push_back(e.path());
            m_deckNames.push_back(std::move(stem));
        }
    }

    // Sort alphabetically by display name
    std::vector<size_t> indices(m_decks.size());
    std::iota(indices.begin(), indices.end(), 0);
    std::sort(indices.begin(), indices.end(),
              [&](size_t a, size_t b){ return m_deckNames[a] < m_deckNames[b]; });

    std::vector<fs::path>   sortedPaths;
    std::vector<std::string> sortedNames;
    sortedPaths.reserve(m_decks.size());
    sortedNames.reserve(m_decks.size());
    for (size_t i : indices) {
        sortedPaths.push_back(std::move(m_decks[i]));
        sortedNames.push_back(std::move(m_deckNames[i]));
    }
    m_decks     = std::move(sortedPaths);
    m_deckNames = std::move(sortedNames);

    std::cout << "Deck browser: found " << m_decks.size() << " decks.\n";
}

// ── Events ────────────────────────────────────────────────────────────────────

bool DeckBrowserScreen::onEvent(const sf::Event& ev) {
    if (ev.type == sf::Event::MouseButtonPressed &&
        ev.mouseButton.button == sf::Mouse::Left) {
        float px = static_cast<float>(ev.mouseButton.x);
        float py = static_cast<float>(ev.mouseButton.y);

        // Start button
        if (hitStartButton(px, py) && !m_decks.empty()) return true;

        // Random buttons
        for (int p = 0; p < 2; ++p)
            if (hitRandomButton(p, px, py)) selectRandom(p);

        // List click
        auto [panel, row] = hitTestList(px, py);
        if (panel >= 0 && row >= 0) {
            m_activePanel   = panel;
            int idx         = m_scrollOffset[panel] + row;
            if (idx < static_cast<int>(m_decks.size())) {
                m_selectedIdx[panel] = idx;
                m_selected[panel]    = m_decks[idx];
                m_names[panel]       = m_deckNames[idx];
            }
        }
    }

    if (ev.type == sf::Event::MouseWheelScrolled) {
        float px = static_cast<float>(ev.mouseWheelScroll.x);
        int panel = (px < P1_X) ? 0 : (px < P1_X + PANEL_W) ? 1 : -1;
        if (panel >= 0) {
            m_scrollOffset[panel] -= static_cast<int>(ev.mouseWheelScroll.delta);
            clampScroll(panel);
        }
    }

    if (ev.type == sf::Event::KeyPressed) {
        int p = m_activePanel;
        if (ev.key.code == sf::Keyboard::Up) {
            if (m_selectedIdx[p] > 0) {
                --m_selectedIdx[p];
                m_selected[p] = m_decks[m_selectedIdx[p]];
                m_names[p]    = m_deckNames[m_selectedIdx[p]];
                if (m_selectedIdx[p] < m_scrollOffset[p]) --m_scrollOffset[p];
            }
        } else if (ev.key.code == sf::Keyboard::Down) {
            if (m_selectedIdx[p] + 1 < static_cast<int>(m_decks.size())) {
                ++m_selectedIdx[p];
                m_selected[p] = m_decks[m_selectedIdx[p]];
                m_names[p]    = m_deckNames[m_selectedIdx[p]];
                if (m_selectedIdx[p] >= m_scrollOffset[p] + VISIBLE)
                    ++m_scrollOffset[p];
            }
        } else if (ev.key.code == sf::Keyboard::Tab) {
            m_activePanel ^= 1;
        } else if (ev.key.code == sf::Keyboard::Return && !m_decks.empty()) {
            return true;
        }
    }
    return false;
}

// ── Drawing ───────────────────────────────────────────────────────────────────

void DeckBrowserScreen::drawButton(sf::RenderTarget& t, const std::string& label,
                                    float x, float y, float w, float h, bool enabled) const {
    sf::RectangleShape btn({w, h});
    btn.setPosition(x, y);
    btn.setFillColor(enabled ? sf::Color(60, 120, 80) : sf::Color(50, 55, 50));
    btn.setOutlineColor(kBorderDark);
    btn.setOutlineThickness(1.5f);
    t.draw(btn);
    sf::Text txt(label, m_font, 14);
    txt.setFillColor(enabled ? sf::Color::White : sf::Color(110, 110, 110));
    auto bounds = txt.getLocalBounds();
    txt.setPosition(x + (w - bounds.width) / 2.f, y + (h - bounds.height) / 2.f - 2.f);
    t.draw(txt);
}

void DeckBrowserScreen::drawPanel(sf::RenderTarget& t, int player) const {
    float panelX = (player == 0) ? P0_X : P1_X;
    bool  active = (m_activePanel == player);

    // Panel background
    sf::RectangleShape bg({PANEL_W, LIST_H + 60.f});
    bg.setPosition(panelX, LIST_Y - 30.f);
    bg.setFillColor(active ? kPanelActive : kPanelInactive);
    bg.setOutlineColor(active ? sf::Color(100, 160, 120) : sf::Color(60, 70, 60));
    bg.setOutlineThickness(2.f);
    t.draw(bg);

    // Column header
    sf::Text hdr((player == 0 ? "Player 1 (You)" : "Player 2 (AI)"), m_font, 15);
    hdr.setFillColor(active ? sf::Color(150, 220, 170) : kTextDim);
    hdr.setPosition(panelX + 8.f, LIST_Y - 26.f);
    t.draw(hdr);

    // Selected deck name below header
    sf::Text sel("Selected: " + m_names[player], m_font, 11);
    sel.setFillColor(sf::Color(120, 200, 140));
    sel.setPosition(panelX + 8.f, LIST_Y - 10.f);
    t.draw(sel);

    // List items
    int total = static_cast<int>(m_decks.size());
    for (int row = 0; row < VISIBLE; ++row) {
        int idx = m_scrollOffset[player] + row;
        if (idx >= total) break;
        float iy = LIST_Y + row * ITEM_H;

        bool isSelected = (idx == m_selectedIdx[player]);
        sf::RectangleShape item({PANEL_W - 4.f, ITEM_H - 2.f});
        item.setPosition(panelX + 2.f, iy + 1.f);
        item.setFillColor(isSelected ? kItemSelected : kItemNormal);
        t.draw(item);

        // Name (truncated)
        std::string name = m_deckNames[idx];
        if (name.size() > 50) name = name.substr(0, 48) + "..";
        sf::Text txt(name, m_font, 14);
        txt.setFillColor(isSelected ? sf::Color::White : kTextDim);
        txt.setPosition(panelX + 8.f, iy + 5.f);
        t.draw(txt);
    }

    // Scroll indicator
    if (total > VISIBLE) {
        float trackH = LIST_H;
        float thumbH = std::max(20.f, trackH * VISIBLE / total);
        float thumbY = LIST_Y + (trackH - thumbH) *
                       m_scrollOffset[player] / std::max(1, total - VISIBLE);
        sf::RectangleShape thumb({4.f, thumbH});
        thumb.setPosition(panelX + PANEL_W - 6.f, thumbY);
        thumb.setFillColor(sf::Color(120, 160, 130));
        t.draw(thumb);
    }

    // Random button
    drawButton(t, "Random", panelX, LIST_Y + LIST_H + 8.f, 120.f, 30.f, !m_decks.empty());
}

void DeckBrowserScreen::draw(sf::RenderWindow& window) const {
    window.clear(sf::Color(22, 30, 25));

    // Title
    sf::Text title("Select Decks", m_font, 26);
    title.setFillColor(sf::Color(180, 230, 190));
    title.setPosition(WIN_W / 2.f - title.getLocalBounds().width / 2.f, 18.f);
    window.draw(title);

    sf::Text sub("Click a deck to select it  |  Tab to switch panels  |  Arrow keys to navigate", m_font, 11);
    sub.setFillColor(kTextDim);
    sub.setPosition(WIN_W / 2.f - sub.getLocalBounds().width / 2.f, 52.f);
    window.draw(sub);

    drawPanel(window, 0);
    drawPanel(window, 1);

    // Start button (centre-bottom)
    bool canStart = !m_decks.empty();
    drawButton(window, "Start Game", WIN_W / 2.f - 100.f, WIN_H - 60.f, 200.f, 44.f, canStart);

    // Deck count
    sf::Text cnt(std::to_string(m_decks.size()) + " decks found", m_font, 11);
    cnt.setFillColor(kTextDim);
    cnt.setPosition(WIN_W / 2.f - cnt.getLocalBounds().width / 2.f, WIN_H - 18.f);
    window.draw(cnt);

    window.display();
}

// ── Helpers ───────────────────────────────────────────────────────────────────

std::pair<int,int> DeckBrowserScreen::hitTestList(float px, float py) const {
    for (int p = 0; p < 2; ++p) {
        float panelX = (p == 0) ? P0_X : P1_X;
        if (px < panelX || px > panelX + PANEL_W) continue;
        if (py < LIST_Y  || py > LIST_Y + LIST_H)  continue;
        int row = static_cast<int>((py - LIST_Y) / ITEM_H);
        return {p, row};
    }
    return {-1, -1};
}

bool DeckBrowserScreen::hitStartButton(float px, float py) const {
    float bx = WIN_W / 2.f - 100.f, by = WIN_H - 60.f;
    return px >= bx && px <= bx + 200.f && py >= by && py <= by + 44.f;
}

bool DeckBrowserScreen::hitRandomButton(int player, float px, float py) const {
    float panelX = (player == 0) ? P0_X : P1_X;
    float bx = panelX, by = LIST_Y + LIST_H + 8.f;
    return px >= bx && px <= bx + 120.f && py >= by && py <= by + 30.f;
}

void DeckBrowserScreen::clampScroll(int player) {
    int maxScroll = std::max(0, static_cast<int>(m_decks.size()) - VISIBLE);
    m_scrollOffset[player] = std::clamp(m_scrollOffset[player], 0, maxScroll);
}

void DeckBrowserScreen::selectRandom(int player) {
    if (m_decks.empty()) return;
    int idx = std::rand() % static_cast<int>(m_decks.size());
    m_selectedIdx[player] = idx;
    m_selected[player]    = m_decks[idx];
    m_names[player]       = m_deckNames[idx];
    clampScroll(player);
}

} // namespace ui
