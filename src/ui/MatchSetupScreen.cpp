#include "MatchSetupScreen.h"
#include "UiScale.h"
#include "../core/db/CardDb.h"
#include "../core/mana/ManaAtom.h"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>

namespace ui {
namespace fs = std::filesystem;

MatchSetupScreen::MatchSetupScreen(const sf::Font& font,
                                   const fs::path& decksRoot,
                                   const mtg::CardDb* db)
    : m_font(font), m_decksRoot(decksRoot), m_db(db)
{
    scanDecks();
    if (!m_decks.empty()) {
        m_playerSel = 0;
        m_aiSel     = m_decks.size() > 1 ? 1 : 0;
    }
}

void MatchSetupScreen::refresh() {
    int prevP = m_playerSel, prevA = m_aiSel;
    scanDecks();
    m_playerSel = m_decks.empty() ? -1 : std::clamp(prevP, 0, (int)m_decks.size()-1);
    m_aiSel     = m_decks.empty() ? -1 : std::clamp(prevA, 0, (int)m_decks.size()-1);
}

// Lower-case all ASCII letters in `s`, in place.
static void ascii_lower(std::string& s) {
    for (char& c : s) c = static_cast<char>(std::tolower((unsigned char)c));
}

// Draw tiny WUBRG colour-identity dots at (x,y); colourless → one grey dot.
// Returns the width consumed so callers can keep text clear of it.
static float drawColorPips(sf::RenderTarget& t, float x, float y, uint8_t ci) {
    static const struct { uint8_t bit; sf::Color col; } kP[5] = {
        {mtg::ManaAtom::WHITE, sf::Color(248, 246, 235)},
        {mtg::ManaAtom::BLUE,  sf::Color(70, 130, 210)},
        {mtg::ManaAtom::BLACK, sf::Color(120, 112, 120)},
        {mtg::ManaAtom::RED,   sf::Color(208, 80, 70)},
        {mtg::ManaAtom::GREEN, sf::Color(80, 170, 100)},
    };
    float cx = x;
    auto dot = [&](sf::Color c) {
        sf::CircleShape d(4.f);
        d.setPosition(cx, y);
        d.setFillColor(c);
        d.setOutlineThickness(1.f);
        d.setOutlineColor(sf::Color(0, 0, 0, 90));
        t.draw(d);
        cx += 11.f;
    };
    bool any = false;
    for (auto& p : kP)
        if (ci & p.bit) { dot(p.col); any = true; }
    if (!any) dot(sf::Color(150, 143, 130));   // colourless
    return cx - x;
}

void MatchSetupScreen::scanDecks() {
    m_decks.clear();
    m_deckNames.clear();
    m_deckBodyLower.clear();

    auto scanDir = [&](const fs::path& dir) {
        if (dir.empty() || !fs::exists(dir)) return;
        try {
            // Non-recursive: the appdata dir is flat by construction.
            for (auto& e : fs::directory_iterator(dir))
                if (e.path().extension() == ".dck")
                    m_decks.push_back(e.path());
        } catch (...) {}
    };

    if (const char* appdata = std::getenv("APPDATA"))
        scanDir(fs::path(appdata) / "CitadelMTG" / "decks");

    std::sort(m_decks.begin(), m_decks.end());
    m_decks.erase(std::unique(m_decks.begin(), m_decks.end()), m_decks.end());

    m_decks.erase(std::remove_if(m_decks.begin(), m_decks.end(),
        [](const fs::path& p) {
            std::string s = p.stem().string();
            return (s.size() >= 2 && s[0] == 'A' && s[1] == '-');
        }), m_decks.end());
    m_deckNames.reserve(m_decks.size());
    m_deckBodyLower.reserve(m_decks.size());
    m_deckCI.reserve(m_decks.size());
    m_deckCIKnown.reserve(m_decks.size());
    for (auto& p : m_decks) {
        m_deckNames.push_back(p.stem().string());

        // Slurp the file once so per-keystroke filtering stays in memory.
        // (~210 decks × ~2 KB ≈ 0.5 MB — trivial.)
        std::ifstream f(p, std::ios::binary);
        std::ostringstream ss; ss << f.rdbuf();
        std::string body = ss.str();
        // Append the filename (lower-cased) so name matches go through the
        // same code path as card-content matches.
        body += "\n";
        body += p.stem().string();
        ascii_lower(body);
        m_deckBodyLower.push_back(std::move(body));

        bool known = false;
        uint8_t ci = deckColorIdentity(p, known);
        m_deckCI.push_back(ci);
        m_deckCIKnown.push_back(known ? 1 : 0);
    }

    rebuildFilter();
}

uint8_t MatchSetupScreen::deckColorIdentity(const fs::path& deck, bool& known) const {
    known = false;
    uint8_t ci = 0;
    if (!m_db) return 0;
    std::ifstream f(deck);
    if (!f) return 0;
    std::string line;
    bool inCmd = false;
    while (std::getline(f, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' ||
                                  line.back() == '\t'))
            line.pop_back();
        if (line.empty()) continue;
        if (line.front() == '[') {
            std::string lc;
            for (char c : line) lc += static_cast<char>(std::tolower((unsigned char)c));
            inCmd = (lc == "[commander]");
            continue;
        }
        if (!inCmd) continue;
        // "1 Name|SET|num" → "Name"
        auto sp = line.find(' ');
        std::string name = (sp == std::string::npos) ? line : line.substr(sp + 1);
        auto pipe = name.find('|');
        if (pipe != std::string::npos) name = name.substr(0, pipe);
        while (!name.empty() && (name.back() == ' ' || name.back() == '\t'))
            name.pop_back();
        if (const mtg::CardRules* r = m_db->find(name)) {
            ci |= r->manaCost.colorIdentity();
            known = true;
        }
    }
    return ci;
}

void MatchSetupScreen::rebuildFilter() {
    // Shared colour mask from the pips (applies to both panels).
    uint8_t selMask = 0;
    if (m_colorFilter[0]) selMask |= mtg::ManaAtom::WHITE;
    if (m_colorFilter[1]) selMask |= mtg::ManaAtom::BLUE;
    if (m_colorFilter[2]) selMask |= mtg::ManaAtom::BLACK;
    if (m_colorFilter[3]) selMask |= mtg::ManaAtom::RED;
    if (m_colorFilter[4]) selMask |= mtg::ManaAtom::GREEN;
    bool colorlessSel = m_colorFilter[5];
    bool anyColor = (selMask != 0) || colorlessSel;

    for (int side = 0; side < 2; ++side) {
        m_visible[side].clear();
        m_visible[side].reserve(m_decks.size());
        std::string needle = m_filter[side];
        ascii_lower(needle);
        for (int i = 0; i < (int)m_decks.size(); ++i) {
            if (!needle.empty() && m_deckBodyLower[i].find(needle) == std::string::npos)
                continue;
            if (anyColor && m_deckCIKnown[i]) {
                uint8_t dci = static_cast<uint8_t>(m_deckCI[i] & mtg::ManaAtom::COLORS_MASK);
                bool pass = (dci == 0) ? colorlessSel
                                       : (selMask != 0 && (dci & ~selMask & mtg::ManaAtom::COLORS_MASK) == 0);
                if (!pass) continue;
            }
            m_visible[side].push_back(i);
        }
    }
}

fs::path MatchSetupScreen::playerDeckPath() const {
    if (m_playerSel >= 0 && m_playerSel < (int)m_decks.size())
        return m_decks[m_playerSel];
    return {};
}

fs::path MatchSetupScreen::aiDeckPath() const {
    if (m_aiSel >= 0 && m_aiSel < (int)m_decks.size())
        return m_decks[m_aiSel];
    return {};
}

// ── Hit-testing ───────────────────────────────────────────────────────────────

int MatchSetupScreen::hitList(float px, float py, float panelX, int scroll) const {
    if (px < panelX || px >= panelX + PANEL_W) return -1;
    float listY = PANEL_Y + 30.f;
    if (py < listY || py >= PANEL_Y + PANEL_H) return -1;
    const auto& vis = m_visible[sideOf(panelX)];
    int visRow = static_cast<int>((py - listY) / ITEM_H) + scroll;
    if (visRow < 0 || visRow >= (int)vis.size()) return -1;
    return vis[visRow];
}

bool MatchSetupScreen::hitStart(float px, float py) const {
    float bx = (WIN_W - 260.f) * 0.5f;
    float by = WIN_H - 68.f;
    return px >= bx && px < bx + 260.f && py >= by && py < by + 48.f;
}

bool MatchSetupScreen::hitBack(float px, float py) const {
    return px >= 14.f && px < 110.f && py >= 14.f && py < 52.f;
}

int MatchSetupScreen::hitFilter(float px, float py) const {
    // One search box above each panel. Returns 0 (player/left), 1 (AI/right), or -1.
    float fy = 60.f;
    if (py < fy || py > fy + 26.f) return -1;
    if (px >= LEFT_X  && px <= LEFT_X  + PANEL_W) return 0;
    if (px >= RIGHT_X && px <= RIGHT_X + PANEL_W) return 1;
    return -1;
}

// Colour-identity filter pips live in the top nav bar, right-aligned.
int MatchSetupScreen::hitColorPip(float px, float py) const {
    constexpr float sz = 24.f, step = 27.f, y = 16.f;
    float x0 = WIN_W - (6.f * step) - 14.f;
    for (int i = 0; i < 6; ++i) {
        float x = x0 + i * step;
        if (px >= x && px <= x + sz && py >= y && py <= y + sz) return i;
    }
    return -1;
}

// "Random" button in each panel's header (top-right of the panel).
bool MatchSetupScreen::hitRandom(float px, float py, float panelX) const {
    float bx = panelX + PANEL_W - 76.f, by = PANEL_Y + 5.f;
    return px >= bx && px <= bx + 68.f && py >= by && py <= by + 18.f;
}

bool MatchSetupScreen::hitPlayerName(float px, float py) const {
    float nx = 250.f, ny = WIN_H - 188.f;
    return px >= nx && px <= nx + 240.f && py >= ny && py <= ny + 24.f;
}

int MatchSetupScreen::hitPlayerCount(float px, float py) const {
    float dx = (WIN_W - 380.f) * 0.5f;
    float dy = WIN_H - 188.f;
    for (int i = 0; i < 3; ++i) {
        float bx = dx + 130.f + i * 56.f;
        if (px >= bx && px <= bx + 50.f && py >= dy && py <= dy + 24.f)
            return i + 1;
    }
    return 0;
}

// ── Static helpers 

std::string MatchSetupScreen::commanderNameOf(const fs::path& deck) {
    std::ifstream f(deck);
    if (!f) return "AI";
    std::string line;
    bool inCmd = false;
    while (std::getline(f, line)) {
        // strip trailing whitespace
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' ||
                                  line.back() == '\t'))
            line.pop_back();
        if (line.empty()) continue;
        if (line.front() == '[') {
            std::string lc;
            for (char c : line) lc += static_cast<char>(std::tolower((unsigned char)c));
            inCmd = (lc == "[commander]");
            continue;
        }
        if (inCmd) {
            // "1 Krenko, Mob Boss"             → "Krenko, Mob Boss"
            // "1 Szarekh, the Silent King|40K|1" → "Szarekh, the Silent King"
            auto sp = line.find(' ');
            std::string name = (sp == std::string::npos)
                                ? line : line.substr(sp + 1);
            // Forge encodes a printing as "Name|SET|COLLECTORNUM"; strip the
            // pipe suffix so the player chip shows the human-readable name.
            auto pipe = name.find('|');
            if (pipe != std::string::npos) name = name.substr(0, pipe);
            // Trim again in case of trailing whitespace before the pipe.
            while (!name.empty() && (name.back() == ' ' || name.back() == '\t'))
                name.pop_back();
            return name.empty() ? std::string("AI") : name;
        }
    }
    return "AI";
}

fs::path MatchSetupScreen::pickRandomAiDeck(
        const std::vector<fs::path>& exclude) const {
    if (m_decks.empty()) return {};
    std::vector<fs::path> pool;
    pool.reserve(m_decks.size());
    for (auto& d : m_decks) {
        if (std::find(exclude.begin(), exclude.end(), d) == exclude.end())
            pool.push_back(d);
    }
    if (pool.empty()) return m_decks.front();
    static std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<size_t> dist(0, pool.size() - 1);
    return pool[dist(rng)];
}

// ── Event handling 

MatchSetupScreen::Action MatchSetupScreen::onEvent(const sf::Event& ev) {
    auto visibleCount = [&](int side) { return (int)m_visible[side].size(); };

    // ── Player-name text input 
    if (m_nameEditActive && ev.type == sf::Event::TextEntered) {
        uint32_t ch = ev.text.unicode;
        if (ch == 8) {
            if (!m_playerName.empty()) m_playerName.pop_back();
        } else if (ch >= 32 && ch < 127 && m_playerName.size() < 24) {
            m_playerName += static_cast<char>(ch);
        }
        return Action::None;
    }
    if (m_nameEditActive && ev.type == sf::Event::KeyPressed) {
        if (ev.key.code == sf::Keyboard::Escape ||
            ev.key.code == sf::Keyboard::Return) {
            m_nameEditActive = false;
            if (m_playerName.empty()) m_playerName = "Player";
            return Action::None;
        }
    }

    // ── Search-box text input
    if (m_filterActiveSide >= 0 && ev.type == sf::Event::TextEntered) {
        int s = m_filterActiveSide;
        uint32_t ch = ev.text.unicode;
        if (ch == 8) {                                  // backspace
            if (!m_filter[s].empty()) m_filter[s].pop_back();
        } else if (ch >= 32 && ch < 127) {
            m_filter[s] += static_cast<char>(ch);
        }
        rebuildFilter();
        (s == 0 ? m_playerScroll : m_aiScroll) = 0;
        // Keep that side's selection in range if possible.
        int& sel = (s == 0) ? m_playerSel : m_aiSel;
        const auto& vis = m_visible[s];
        if (sel >= 0 && std::find(vis.begin(), vis.end(), sel) == vis.end())
            sel = vis.empty() ? -1 : vis.front();
        return Action::None;
    }
    if (m_filterActiveSide >= 0 && ev.type == sf::Event::KeyPressed) {
        if (ev.key.code == sf::Keyboard::Escape) {
            m_filter[m_filterActiveSide].clear(); m_filterActiveSide = -1; rebuildFilter();
            return Action::None;
        }
        if (ev.key.code == sf::Keyboard::Return) {
            m_filterActiveSide = -1; return Action::None;
        }
    }

    if (ev.type == sf::Event::MouseMoved) {
        float px = static_cast<float>(ev.mouseMove.x);
        float py = static_cast<float>(ev.mouseMove.y);

        // Active scrollbar drag overrides hover updates.
        if (m_dragWhich != DragWhich::None) {
            int total = visibleCount(m_dragWhich == DragWhich::Player ? 0 : 1);
            if (total > VISIBLE) {
                float sbH    = PANEL_H - 30.f;
                float thumbH = std::max(20.f, sbH * VISIBLE / (float)total);
                float track  = sbH - thumbH;
                float topY   = py - m_dragGrabY;
                float frac   = std::clamp((topY - (PANEL_Y + 30.f)) / std::max(1.f, track), 0.f, 1.f);
                int   maxS   = total - VISIBLE;
                int   s      = static_cast<int>(frac * maxS + 0.5f);
                if (m_dragWhich == DragWhich::Player) m_playerScroll = s;
                else                                  m_aiScroll     = s;
            }
            return Action::None;
        }

        m_playerHover = hitList(px, py, LEFT_X,  m_playerScroll);
        m_aiHover     = hitList(px, py, RIGHT_X, m_aiScroll);
    }
    if (ev.type == sf::Event::MouseButtonReleased) {
        m_dragWhich = DragWhich::None;
    }
    if (ev.type == sf::Event::MouseWheelScrolled) {
        int delta     = ev.mouseWheelScroll.delta > 0 ? -3 : 3;
        float sx = ev.mouseWheelScroll.x;
        if (sx >= LEFT_X && sx < LEFT_X + PANEL_W)
            m_playerScroll = std::clamp(m_playerScroll + delta, 0,
                                        std::max(0, visibleCount(0) - VISIBLE));
        else if (sx >= RIGHT_X)
            m_aiScroll = std::clamp(m_aiScroll + delta, 0,
                                    std::max(0, visibleCount(1) - VISIBLE));
    }
    if (ev.type == sf::Event::KeyPressed && m_filterActiveSide < 0 && !m_nameEditActive) {
        auto k = ev.key.code;
        if (k == sf::Keyboard::Escape) return Action::Back;
        // Enter/Space: start game if both decks are selected
        if ((k == sf::Keyboard::Return || k == sf::Keyboard::Space) &&
            m_playerSel >= 0 && m_aiSel >= 0 && !m_decks.empty())
            return Action::StartGame;
        // Tab: switch keyboard focus between the player and AI lists.
        if (k == sf::Keyboard::Tab) {
            m_activeCol ^= 1;
            return Action::None;
        }
        // Up/Down: navigate the focused list, respecting that side's filter.
        const auto& vis = m_visible[m_activeCol];
        if ((k == sf::Keyboard::Up || k == sf::Keyboard::Down) && !vis.empty()) {
            int  dir    = (k == sf::Keyboard::Up) ? -1 : 1;
            int& sel    = (m_activeCol == 0) ? m_playerSel : m_aiSel;
            int& scroll = (m_activeCol == 0) ? m_playerScroll : m_aiScroll;
            auto it  = std::find(vis.begin(), vis.end(), sel);
            int  pos = (it == vis.end()) ? 0 : (int)(it - vis.begin());
            pos = std::clamp(pos + dir, 0, (int)vis.size() - 1);
            sel = vis[pos];
            int maxS = std::max(0, (int)vis.size() - VISIBLE);
            if (pos < scroll)                 scroll = pos;
            else if (pos >= scroll + VISIBLE) scroll = std::min(maxS, pos - VISIBLE + 1);
            return Action::None;
        }
    }

    if (ev.type == sf::Event::MouseButtonPressed &&
        ev.mouseButton.button == sf::Mouse::Left) {
        float px = static_cast<float>(ev.mouseButton.x);
        float py = static_cast<float>(ev.mouseButton.y);

        // Search-box click → activate that side's text entry; elsewhere commits.
        if (int fb = hitFilter(px, py); fb >= 0) {
            m_filterActiveSide = fb; m_activeCol = fb; return Action::None;
        } else {
            m_filterActiveSide = -1;
        }

        // Player-name box
        if (hitPlayerName(px, py)) { m_nameEditActive = true; return Action::None; }
        else                       { m_nameEditActive = false; }

        // Player-count buttons (2 / 3 / 4)
        if (int pc = hitPlayerCount(px, py); pc > 0) {
            m_numPlayers = pc + 1;   // 1→2, 2→3, 3→4
            return Action::None;
        }

        // Colour-identity filter pips
        if (int cp = hitColorPip(px, py); cp >= 0) {
            m_colorFilter[cp] = !m_colorFilter[cp];
            rebuildFilter();
            m_playerScroll = m_aiScroll = 0;
            auto keepValid = [&](int& sel, int side) {
                const auto& vis = m_visible[side];
                if (sel >= 0 && std::find(vis.begin(), vis.end(), sel) == vis.end())
                    sel = vis.empty() ? -1 : vis.front();
            };
            keepValid(m_playerSel, 0); keepValid(m_aiSel, 1);
            return Action::None;
        }

        // Random-deck buttons (one per panel) — pick from that panel's filtered list.
        {
            auto pickRandomVisible = [&](int side, int& sel, int& scroll) {
                const auto& vis = m_visible[side];
                if (vis.empty()) return;
                static std::mt19937 rng(std::random_device{}());
                std::uniform_int_distribution<size_t> d(0, vis.size() - 1);
                size_t k = d(rng);
                sel = vis[k];
                int maxS = std::max(0, (int)vis.size() - VISIBLE);
                scroll = std::clamp((int)k - VISIBLE / 2, 0, maxS);
            };
            if (hitRandom(px, py, LEFT_X))  { pickRandomVisible(0, m_playerSel, m_playerScroll); return Action::None; }
            if (hitRandom(px, py, RIGHT_X)) { pickRandomVisible(1, m_aiSel,     m_aiScroll);     return Action::None; }
        }

        if (hitBack(px, py))  return Action::Back;
        if (hitStart(px, py) && m_playerSel >= 0 && m_aiSel >= 0)
            return Action::StartGame;

        // "Random Matchup" — randomise both selectable seats at once (extra
        // 3/4-player seats already pick random AI decks at game start).
        {
            float rbx = (WIN_W - 260.f) * 0.5f - 196.f, rby = WIN_H - 68.f;
            if (px >= rbx && px <= rbx + 184.f && py >= rby && py <= rby + 48.f) {
                static std::mt19937 rng(std::random_device{}());
                if (!m_visible[0].empty()) {
                    std::uniform_int_distribution<size_t> d(0, m_visible[0].size() - 1);
                    m_playerSel = m_visible[0][d(rng)];
                }
                if (!m_visible[1].empty()) {
                    std::uniform_int_distribution<size_t> d(0, m_visible[1].size() - 1);
                    m_aiSel = m_visible[1][d(rng)];
                }
                return Action::None;
            }
        }
        // Life total buttons
        {
            float lx = 40.f, ly = WIN_H - 150.f;
            for (int i = 0; i < 3; ++i) {
                float bx = lx + 120.f + i * 96.f;
                if (px >= bx && px <= bx + 88.f && py >= ly - 2.f && py <= ly + 22.f)
                    m_startingLife = kLifeOptions[i];
            }
        }
        // Difficulty buttons
        {
            float dx = (WIN_W - 380.f) * 0.5f;
            float dy = WIN_H - 112.f;
            for (int i = 0; i < 4; ++i) {
                float bx2 = dx + 110.f + i * 96.f;
                if (px >= bx2 && px <= bx2 + 88.f && py >= dy - 2.f && py <= dy + 20.f)
                    m_aiDifficulty = i;
            }
        }

        // Scrollbar drag init — left panel.
        auto startDrag = [&](float panelX, int scroll, DragWhich which) -> bool {
            float sbX = panelX + PANEL_W - 7.f;
            if (px < sbX || px > sbX + 6.f) return false;
            float listY = PANEL_Y + 30.f;
            if (py < listY || py > PANEL_Y + PANEL_H) return false;
            int total = visibleCount(which == DragWhich::Player ? 0 : 1);
            if (total <= VISIBLE) return false;
            float sbH    = PANEL_H - 30.f;
            float thumbH = std::max(20.f, sbH * VISIBLE / (float)total);
            float thumbY = listY + (sbH - thumbH) * scroll /
                           (float)std::max(1, total - VISIBLE);
            if (py >= thumbY && py < thumbY + thumbH)
                m_dragGrabY = py - thumbY;
            else
                m_dragGrabY = thumbH * 0.5f;
            m_dragWhich = which;
            return true;
        };
        if (startDrag(LEFT_X,  m_playerScroll, DragWhich::Player)) return Action::None;
        if (startDrag(RIGHT_X, m_aiScroll,     DragWhich::AI))      return Action::None;

        int row = hitList(px, py, LEFT_X, m_playerScroll);
        if (row >= 0) { m_playerSel = row; m_activeCol = 0; return Action::None; }
        row = hitList(px, py, RIGHT_X, m_aiScroll);
        if (row >= 0) { m_aiSel = row; m_activeCol = 1; return Action::None; }
    }
    return Action::None;
}

// ── Drawing helpers 

void MatchSetupScreen::drawText(sf::RenderTarget& t, const std::string& s,
                                 float x, float y, unsigned sz,
                                 sf::Color col, bool bold) const {
    if (s.empty()) return;
    sf::Text txt;
    txt.setFont(m_font);
    txt.setString(sf::String::fromUtf8(s.begin(), s.end()));
    txt.setCharacterSize(sz);
    txt.setFillColor(col);
    if (bold) txt.setStyle(sf::Text::Bold);
    txt.setPosition(x, y);
    ui::applyTextScale(txt);
    t.draw(txt);
}

void MatchSetupScreen::drawButton(sf::RenderTarget& t, const std::string& label,
                                   float x, float y, float w, float h,
                                   sf::Color fill, sf::Color textCol) const {
    sf::RectangleShape rect({w, h});
    rect.setPosition(x, y);
    rect.setFillColor(fill);
    rect.setOutlineColor(sf::Color(240, 220, 180, 30));
    rect.setOutlineThickness(1.f);
    t.draw(rect);

    sf::Text txt;
    txt.setFont(m_font);
    txt.setString(sf::String::fromUtf8(label.begin(), label.end()));
    txt.setCharacterSize(14);
    txt.setStyle(sf::Text::Bold);
    txt.setFillColor(textCol);
    auto b = txt.getLocalBounds();
    txt.setPosition(x + (w - b.width) * 0.5f - b.left,
                    y + (h - b.height) * 0.5f - b.top);
    ui::applyTextScale(txt);
    t.draw(txt);
}

void MatchSetupScreen::drawPanel(sf::RenderTarget& t, const std::string& title,
                                  float x, float y,
                                  int sel, int hover, int scroll) const {
    sf::RectangleShape bg({PANEL_W, PANEL_H});
    bg.setPosition(x, y);
    bg.setFillColor(sf::Color(26, 24, 22));    // --bg-2
    bg.setOutlineColor(sf::Color(240, 220, 180, 30));  // --line
    bg.setOutlineThickness(1.f);
    t.draw(bg);

    drawText(t, title, x + 12.f, y + 7.f, 12, sf::Color(148, 139, 124), true);

    // "Random" button — picks a random deck (from the filtered list) for this column.
    drawButton(t, "Random", x + PANEL_W - 76.f, y + 5.f, 68.f, 18.f,
               sf::Color(40, 36, 28), sf::Color(203, 163, 90));

    sf::RectangleShape sep({PANEL_W, 1.f});
    sep.setPosition(x, y + 28.f);
    sep.setFillColor(sf::Color(240, 220, 180, 20));
    t.draw(sep);

    if (m_decks.empty()) {
        drawText(t, "No decks found.", x + 16.f, y + 40.f, 12, sf::Color(80, 90, 110));
        drawText(t, "Use Deck Builder to create one.", x + 16.f, y + 58.f, 11, sf::Color(65, 75, 95));
        return;
    }
    int side = sideOf(x);
    const auto& vis = m_visible[side];
    if (vis.empty()) {
        drawText(t, "No decks match \"" + m_filter[side] + "\"",
                 x + 16.f, y + 40.f, 12, sf::Color(80, 90, 110));
        return;
    }

    float listY  = y + 30.f;
    int   visEnd = std::min(scroll + VISIBLE, (int)vis.size());
    for (int vi = scroll; vi < visEnd; ++vi) {
        int   deckIdx = vis[vi];
        float iy   = listY + (vi - scroll) * ITEM_H;
        bool  iSel = (deckIdx == sel), iHov = (deckIdx == hover);
        sf::Color rowBg = iSel ? sf::Color(44, 34, 16)
                        : iHov ? sf::Color(34, 31, 27)
                        : sf::Color(26, 24, 22);
        sf::RectangleShape row({PANEL_W, ITEM_H});
        row.setPosition(x, iy);
        row.setFillColor(rowBg);
        t.draw(row);
        if (iSel) {
            sf::RectangleShape bar({3.f, ITEM_H});
            bar.setPosition(x, iy);
            bar.setFillColor(sf::Color(203, 163, 90));
            t.draw(bar);
        }
        sf::RectangleShape sep2({PANEL_W, 1.f});
        sep2.setPosition(x, iy + ITEM_H - 1.f);
        sep2.setFillColor(sf::Color(240, 220, 180, 15));
        t.draw(sep2);
        // Colour-identity pips at the right edge (before the scrollbar).
        if (deckIdx < (int)m_deckCIKnown.size() && m_deckCIKnown[deckIdx]) {
            uint8_t ci = static_cast<uint8_t>(m_deckCI[deckIdx] & mtg::ManaAtom::COLORS_MASK);
            int pips = 0; for (uint8_t v = ci; v; v &= v - 1) ++pips;
            if (pips == 0) pips = 1;
            float pw = pips * 11.f;
            drawColorPips(t, x + PANEL_W - 16.f - pw, iy + 9.f, ci);
        }
        std::string nm = m_deckNames[deckIdx];
        if (nm.size() > 44) nm = nm.substr(0, 42) + "..";
        drawText(t, nm, x + 14.f, iy + 6.f, 13,
                 iSel ? sf::Color(230, 193, 112) : sf::Color(241, 234, 220));
    }

    // Scrollbar — sized to the *filtered* visible list.
    int total = (int)vis.size();
    if (total > VISIBLE) {
        float sbH    = PANEL_H - 30.f;
        float thumbH = std::max(20.f, sbH * VISIBLE / (float)total);
        float thumbY = y + 30.f + (sbH - thumbH) * scroll /
                       (float)std::max(1, total - VISIBLE);
        sf::RectangleShape track({6.f, sbH});
        track.setPosition(x + PANEL_W - 7.f, y + 30.f);
        track.setFillColor(sf::Color(28, 34, 50));
        t.draw(track);
        sf::RectangleShape thumb({6.f, thumbH});
        thumb.setPosition(x + PANEL_W - 7.f, thumbY);
        thumb.setFillColor(sf::Color(70, 110, 155));
        t.draw(thumb);
    }
}

// ── Main draw ─────────────────────────────────────────────────────────────────

void MatchSetupScreen::draw(sf::RenderWindow& w) const {
    // Citadel lobby style: warm charcoal + gold
    w.clear(sf::Color(11, 10, 9));   // --bg-0

    // ── Top navigation bar ────────────────────────────────────────────────────
    sf::RectangleShape topBar({WIN_W, 56.f});
    topBar.setFillColor(sf::Color(18, 17, 16));    // --bg-1
    topBar.setOutlineColor(sf::Color(240, 220, 180, 25));
    topBar.setOutlineThickness(0.f);
    w.draw(topBar);
    sf::RectangleShape topBorder({WIN_W, 1.f});
    topBorder.setPosition(0.f, 55.f);
    topBorder.setFillColor(sf::Color(240, 220, 180, 25));
    w.draw(topBorder);

    // Back arrow
    drawButton(w, "< Back", 14.f, 10.f, 88.f, 36.f,
               sf::Color(34, 31, 27), sf::Color(199, 189, 172));

    // Screen title
    drawText(w, "MATCHMAKING", 120.f, 12.f, 11, sf::Color(148, 139, 124), true);
    drawText(w, "Find a Game",  120.f, 26.f, 22, sf::Color(241, 234, 220), true);

    // ── Colour-identity filter pips (top-right of the nav bar) ───────────────
    {
        static const char* kPipLetters[6] = {"W", "U", "B", "R", "G", "C"};
        static const sf::Color kPipBase[6] = {
            sf::Color(248, 246, 235), sf::Color(70, 130, 210), sf::Color(90, 86, 96),
            sf::Color(208, 80, 70),   sf::Color(80, 170, 100), sf::Color(170, 160, 148)
        };
        constexpr float sz = 24.f, step = 27.f, y = 16.f;
        float x0 = WIN_W - (6.f * step) - 14.f;
        drawText(w, "Colors:", x0 - 58.f, y + 5.f, 11, sf::Color(148, 139, 124), true);
        for (int i = 0; i < 6; ++i) {
            float x = x0 + i * step;
            bool on = m_colorFilter[i];
            const sf::Color& base = kPipBase[i];
            sf::RectangleShape pip({sz, sz});
            pip.setPosition(x, y);
            pip.setFillColor(on ? base
                : sf::Color(base.r / 4 + 18, base.g / 4 + 16, base.b / 4 + 14));
            pip.setOutlineThickness(on ? 2.f : 1.f);
            pip.setOutlineColor(on ? sf::Color(240, 220, 180, 220)
                                   : sf::Color(240, 220, 180, 40));
            w.draw(pip);
            bool darkText = (i == 0 || i == 5);   // W and C pips are light-filled
            sf::Color tcol = on ? (darkText ? sf::Color(20, 18, 16) : sf::Color(248, 246, 240))
                                : sf::Color(150, 142, 128);
            sf::Text lt(kPipLetters[i], m_font, 12);
            lt.setStyle(sf::Text::Bold);
            lt.setFillColor(tcol);
            auto b = lt.getLocalBounds();
            lt.setPosition(x + (sz - b.width) * 0.5f - b.left,
                           y + (sz - b.height) * 0.5f - b.top);
            applyTextScale(lt);
            w.draw(lt);
        }
    }

    // Per-column "YOUR" / "AI" tags inside the panel headers (replaces the
    // old text labels above, which overlapped the new search bar).

    // ── Per-panel search fields (one above each deck list) ───────────────────
    {
        constexpr float fy = 60.f, fh = 26.f;
        const float xs[2] = { LEFT_X, RIGHT_X };
        for (int s = 0; s < 2; ++s) {
            bool active = (m_filterActiveSide == s);
            sf::RectangleShape sb({PANEL_W, fh});
            sb.setPosition(xs[s], fy);
            sb.setFillColor(sf::Color(20, 18, 14));
            sb.setOutlineColor(active ? sf::Color(203, 163, 90)
                                      : sf::Color(240, 220, 180, 50));
            sb.setOutlineThickness(1.f);
            w.draw(sb);
            std::string shown = m_filter[s].empty()
                ? std::string(s == 0 ? "Search your decks — click to type"
                                     : "Search AI decks — click to type")
                : m_filter[s] + (active ? "_" : "");
            sf::Color col = m_filter[s].empty() ? sf::Color(120, 113, 100)
                                                : sf::Color(241, 234, 220);
            drawText(w, shown, xs[s] + 10.f, fy + 6.f, 12, col);
            drawText(w,
                     std::to_string(m_visible[s].size()) + " / " +
                         std::to_string(m_decks.size()),
                     xs[s] + PANEL_W - 70.f, fy + 8.f, 10, sf::Color(148, 139, 124));
        }
    }

    drawPanel(w, "YOUR DECK", LEFT_X,  PANEL_Y, m_playerSel, m_playerHover, m_playerScroll);
    drawPanel(w, "AI DECK",   RIGHT_X, PANEL_Y, m_aiSel,     m_aiHover,     m_aiScroll);

    // Keyboard-focus outline on the active panel (Tab switches it).
    {
        float fx = (m_activeCol == 0) ? LEFT_X : RIGHT_X;
        sf::RectangleShape focus({PANEL_W, PANEL_H});
        focus.setPosition(fx, PANEL_Y);
        focus.setFillColor(sf::Color::Transparent);
        focus.setOutlineColor(sf::Color(203, 163, 90, 160));
        focus.setOutlineThickness(2.f);
        w.draw(focus);
    }

    // Deck preview strips (card count + first few names)
    {
        auto previewDeck = [&](float panelX, int sel, sf::Color accent) {
            if (sel < 0 || sel >= (int)m_decks.size()) return;
            float cy = PANEL_Y + PANEL_H + 8.f;
            // Count entries and collect names from the .dck file
            int total = 0;
            int lands = 0, creatures = 0, spells = 0;
            std::vector<std::string> first3;
            std::ifstream f(m_decks[sel]);
            std::string line;
            while (std::getline(f, line)) {
                if (line.empty() || line[0] == '#') continue;
                // Format: "N CardName" or "SB: N CardName"
                std::istringstream ss(line);
                std::string tok; ss >> tok;
                if (tok == "SB:") { ss >> tok; }
                int n = 0;
                try { n = std::stoi(tok); } catch (...) { continue; }
                std::string rest; std::getline(ss, rest);
                while (!rest.empty() && rest[0] == ' ') rest = rest.substr(1);
                if (rest.empty()) continue;
                total += n;
                if (first3.size() < 3) first3.push_back(rest);
                // Rough type guess from name (not perfect but good enough for preview)
                std::string lo = rest;
                for (char& c : lo) c = static_cast<char>(std::tolower((unsigned char)c));
                if (lo.find("forest") != std::string::npos || lo.find("island") != std::string::npos ||
                    lo.find("mountain") != std::string::npos || lo.find("swamp") != std::string::npos ||
                    lo.find("plains") != std::string::npos) lands += n;
            }

            drawText(w, std::to_string(total) + " cards",
                     panelX + 4.f, cy, 11, accent, true);
            float iy = cy + 16.f;
            for (const auto& nm : first3) {
                std::string disp = nm.size() > 28 ? nm.substr(0, 26) + ".." : nm;
                drawText(w, "- " + disp, panelX + 8.f, iy, 10, sf::Color(148, 139, 124));
                iy += 13.f;
            }
        };
        previewDeck(LEFT_X,  m_playerSel, sf::Color(130, 200, 150));
        previewDeck(RIGHT_X, m_aiSel,     sf::Color(200, 130, 130));
    }

    // Back button
    drawButton(w, "< Back", 14.f, 14.f, 96.f, 38.f,
               sf::Color(35, 42, 52), sf::Color(160, 175, 190));

    // Start button
    bool canStart = (m_playerSel >= 0 && m_aiSel >= 0 && !m_decks.empty());

    // ── Player name + player-count row (above Starting Life) ─────────────────
    {
        constexpr float ny = 612.f;
        // Name label + editable box on the left
        drawText(w, "Your Name:", 130.f, ny + 4.f, 11,
                 sf::Color(148, 139, 124), true);
        sf::RectangleShape nameBox({240.f, 24.f});
        nameBox.setPosition(250.f, ny);
        nameBox.setFillColor(sf::Color(20, 18, 14));
        nameBox.setOutlineColor(m_nameEditActive
                                ? sf::Color(203, 163, 90)
                                : sf::Color(240, 220, 180, 50));
        nameBox.setOutlineThickness(1.f);
        w.draw(nameBox);
        std::string shown = m_playerName + (m_nameEditActive ? "_" : "");
        drawText(w, shown, 256.f, ny + 5.f, 13, sf::Color(241, 234, 220));

        // Player-count selector on the right (2 / 3 / 4)
        float dx = (WIN_W - 380.f) * 0.5f;
        drawText(w, "Players:", dx, ny + 4.f, 11,
                 sf::Color(160, 175, 160), true);
        static const char* kCounts[] = { "2", "3", "4" };
        for (int i = 0; i < 3; ++i) {
            int n = i + 2;
            float bx = dx + 130.f + i * 56.f;
            bool active = (m_numPlayers == n);
            sf::RectangleShape btn({50.f, 24.f});
            btn.setPosition(bx, ny);
            btn.setFillColor(active ? sf::Color(44, 34, 14) : sf::Color(34, 31, 27));
            btn.setOutlineColor(active ? sf::Color(203, 163, 90, 200)
                                       : sf::Color(240, 220, 180, 30));
            btn.setOutlineThickness(1.f);
            w.draw(btn);
            sf::Text lbl(kCounts[i], m_font, 12);
            lbl.setStyle(sf::Text::Bold);
            lbl.setFillColor(active ? sf::Color(230, 193, 112)
                                    : sf::Color(148, 139, 124));
            auto lb = lbl.getLocalBounds();
            lbl.setPosition(bx + (50.f - lb.width) * 0.5f - lb.left,
                            ny + (24.f - lb.height) * 0.5f - lb.top);
            w.draw(lbl);
        }
        if (m_numPlayers > 2) {
            drawText(w,
                     "(extra seats pick random AI decks)",
                     dx + 305.f, ny + 6.f, 9, sf::Color(120, 113, 100));
        }
    }

    // ── Starting life total selector ──────────────────────────────────────────
    {
        static constexpr const char* kLifeLabels[] = {"20 (Duel)", "30", "40 (Commander)"};
        float lx = 40.f;
        float ly = WIN_H - 150.f;
        drawText(w, "Starting Life:", lx, ly, 11, sf::Color(148, 139, 124), true);
        for (int i = 0; i < 3; ++i) {
            float bx = lx + 120.f + i * 96.f;
            bool active = (kLifeOptions[i] == m_startingLife);
            sf::RectangleShape btn({88.f, 24.f});
            btn.setPosition(bx, ly - 2.f);
            btn.setFillColor(active ? sf::Color(44, 34, 14) : sf::Color(34, 31, 27));
            btn.setOutlineColor(active ? sf::Color(203, 163, 90, 200) : sf::Color(240, 220, 180, 30));
            btn.setOutlineThickness(1.f);
            w.draw(btn);
            sf::Text lbl(kLifeLabels[i], m_font, 9);
            lbl.setFillColor(active ? sf::Color(230, 193, 112) : sf::Color(148, 139, 124));
            auto lb = lbl.getLocalBounds();
            lbl.setPosition(bx + (88.f - lb.width) * 0.5f, ly + 4.f);
            w.draw(lbl);
        }
    }

    // ── AI Difficulty selector ─────────────────────────────────────────────────
    static constexpr const char* kDiffLabels[] = {"Easy (20)", "Medium (100)", "Hard (200)", "Expert (500)"};
    float dx = (WIN_W - 380.f) * 0.5f;
    float dy = WIN_H - 112.f;
    {
        sf::Text dHdr("AI Difficulty:", m_font, 11);
        dHdr.setFillColor(sf::Color(160, 175, 160));
        dHdr.setPosition(dx, dy);
        w.draw(dHdr);
    }
    for (int i = 0; i < 4; ++i) {
        float bx2 = dx + 110.f + i * 96.f;
        bool  active = (m_aiDifficulty == i);
        sf::RectangleShape db({88.f, 24.f});
        db.setPosition(bx2, dy - 2.f);
        db.setFillColor(active ? sf::Color(44, 34, 14) : sf::Color(34, 31, 27));
        db.setOutlineColor(active ? sf::Color(203, 163, 90, 200) : sf::Color(240, 220, 180, 30));
        db.setOutlineThickness(1.f);
        w.draw(db);
        sf::Text dLbl(kDiffLabels[i], m_font, 9);
        dLbl.setFillColor(active ? sf::Color(230, 193, 112) : sf::Color(148, 139, 124));
        auto dlb = dLbl.getLocalBounds();
        dLbl.setPosition(bx2 + (88.f - dlb.width) * 0.5f, dy + 4.f);
        w.draw(dLbl);
    }

    float bx = (WIN_W - 260.f) * 0.5f;
    float by = WIN_H - 68.f;

    // "Random Matchup" button to the left of Start.
    {
        const char* lbl = (m_numPlayers > 2) ? "Random (all seats)" : "Random Matchup";
        drawButton(w, lbl, bx - 196.f, by, 184.f, 48.f,
                   sf::Color(34, 31, 27), sf::Color(203, 163, 90));
    }

    // Citadel primary button style: gold gradient
    if (canStart) {
        sf::RectangleShape startBtn({260.f, 48.f});
        startBtn.setPosition(bx, by);
        startBtn.setFillColor(sf::Color(203, 163, 90));
        w.draw(startBtn);
        sf::Text st(">> Find Match", m_font, 15);
        st.setStyle(sf::Text::Bold);
        st.setFillColor(sf::Color(38, 22, 6));
        auto sb = st.getLocalBounds();
        st.setPosition(bx + (260.f - sb.width) * 0.5f - sb.left,
                       by + (48.f - sb.height) * 0.5f - sb.top);
        applyTextScale(st);
        w.draw(st);
    } else {
        drawButton(w, "Select Both Decks", bx, by, 260.f, 48.f,
                   sf::Color(34, 31, 27), sf::Color(107, 99, 87));
    }
}

} // namespace ui
