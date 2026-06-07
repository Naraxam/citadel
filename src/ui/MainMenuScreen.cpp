#include "MainMenuScreen.h"
#include "UIColors.h"
#include "UiScale.h"
#include <algorithm>
#include <cmath>
#include <string>

namespace ui {

MainMenuScreen::MainMenuScreen(const sf::Font& font) : m_font(font) {}

// ── Helpers 

float MainMenuScreen::navItemY(int idx) const {
    return NAV_ITEM_Y0 + idx * (NAV_ITEM_H + NAV_ITEM_G);
}

bool MainMenuScreen::hitNavItem(float px, float py, int idx) const {
    float y = navItemY(idx);
    return px >= NAV_X && px < NAV_X + NAV_W && py >= y && py < y + NAV_ITEM_H;
}

void MainMenuScreen::drawTxt(sf::RenderTarget& t, const std::string& s,
                              float x, float y, unsigned sz,
                              sf::Color col, bool bold) const {
    if (s.empty()) return;
    sf::Text txt(s, m_font, sz);
    if (bold) txt.setStyle(sf::Text::Bold);
    txt.setFillColor(col);
    txt.setPosition(x, y);
    applyTextScale(txt);
    t.draw(txt);
}

// ── Event handling 

MainMenuScreen::Action MainMenuScreen::onEvent(const sf::Event& ev) {
    if (ev.type == sf::Event::MouseMoved) {
        float px = static_cast<float>(ev.mouseMove.x);
        float py = static_cast<float>(ev.mouseMove.y);
        m_hover = -1;
        for (int i = 0; i < 6; ++i)
            if (hitNavItem(px, py, i)) { m_hover = i; break; }
    }
    if (ev.type == sf::Event::KeyPressed) {
        auto k = ev.key.code;
        if (k == sf::Keyboard::Escape) {
            if (m_showSettings) { m_showSettings = false; return Action::None; }
            if (m_showAiData)   { m_showAiData   = false; return Action::None; }
        }
        // Arrow key navigation
        if (k == sf::Keyboard::Up   || k == sf::Keyboard::W) {
            m_hover = std::max(0, (m_hover < 0 ? 5 : m_hover) - 1);
            return Action::None;
        }
        if (k == sf::Keyboard::Down || k == sf::Keyboard::S) {
            m_hover = std::min(5, (m_hover < 0 ? -1 : m_hover) + 1);
            return Action::None;
        }
        // Enter / Space activates the hovered item
        if ((k == sf::Keyboard::Return || k == sf::Keyboard::Space) && m_hover >= 0) {
            if (m_hover == 0) return Action::PlayVsAI;
            if (m_hover == 1) return Action::DeckBuilder;
            if (m_hover == 2) return Action::DownloadArt;
            if (m_hover == 3) { m_showAiData   = !m_showAiData;   return Action::None; }
            if (m_hover == 4) { m_showSettings = !m_showSettings; return Action::None; }
            if (m_hover == 5) return Action::Quit;
        }
    }
    if (ev.type == sf::Event::MouseButtonPressed &&
        ev.mouseButton.button == sf::Mouse::Left) {
        float px = static_cast<float>(ev.mouseButton.x);
        float py = static_cast<float>(ev.mouseButton.y);
        // Close settings overlay on click-outside
        if (m_showSettings) {
            constexpr float pw = 420.f, ph = 280.f;
            float px0 = (WIN_W - pw) * 0.5f, py0 = (WIN_H - ph) * 0.5f;
            bool inside = px >= px0 && px <= px0 + pw && py >= py0 && py <= py0 + ph;
            if (!inside) { m_showSettings = false; return Action::None; }
        }
        // AI-data overlay click: route the two action buttons; click-outside
        // closes the overlay. Panel matches drawAiDataOverlay below.
        if (m_showAiData) {
            constexpr float pw = 460.f, ph = 240.f;
            float px0 = (WIN_W - pw) * 0.5f;
            float py0 = (WIN_H - ph) * 0.5f;
            bool inside = px >= px0 && px <= px0 + pw && py >= py0 && py <= py0 + ph;
            if (!inside) { m_showAiData = false; return Action::None; }
            constexpr float bw = 200.f, bh = 36.f, gap = 12.f;
            float bx = px0 + (pw - bw * 2.f - gap) * 0.5f;
            float by = py0 + ph - bh - 22.f;
            if (px >= bx && px < bx + bw && py >= by && py < by + bh)
                return Action::RefreshCombos;
            if (px >= bx + bw + gap && px < bx + bw + gap + bw &&
                py >= by && py < by + bh)
                return Action::DownloadSalt;
            return Action::None;
        }
        if (hitNavItem(px, py, 0)) return Action::PlayVsAI;
        if (hitNavItem(px, py, 1)) return Action::DeckBuilder;
        if (hitNavItem(px, py, 2)) return Action::DownloadArt;
        if (hitNavItem(px, py, 3)) { m_showAiData   = !m_showAiData;   return Action::None; }
        if (hitNavItem(px, py, 4)) { m_showSettings = !m_showSettings; return Action::None; }
        if (hitNavItem(px, py, 5)) return Action::Quit;
    }
    return Action::None;
}

// ── Nav item ──────────────────────────────────────────────────────────────────

static sf::Color pipColour(int i) {
    switch (i) {
        case 0: return sf::Color(239, 231, 207);
        case 1: return sf::Color( 90, 160, 216);
        case 2: return sf::Color(157, 127, 182);
        case 3: return sf::Color(223, 106,  68);
        case 4: return sf::Color( 95, 168, 115);
        default: return sf::Color(185, 178, 164);
    }
}

void MainMenuScreen::drawNavItem(sf::RenderTarget& t, int idx,
                                  const char* label, const char* sub,
                                  bool primary, bool hover) const {
    float x = NAV_X;
    float y = navItemY(idx);
    float h = NAV_ITEM_H;

    sf::Color bg     = primary ? sf::Color(48, 28, 16)
                     : hover   ? kBg4 : kBg2;
    sf::Color border = primary ? sf::Color(217, 116, 63, 100)
                     : hover   ? kLineStr : kLine;

    sf::RectangleShape panel({NAV_W, h});
    panel.setPosition(x, y);
    panel.setFillColor(bg);
    panel.setOutlineColor(border);
    panel.setOutlineThickness(1.f);
    t.draw(panel);

    if (hover && !primary) {
        sf::RectangleShape bar({3.f, h});
        bar.setPosition(x, y);
        bar.setFillColor(kGold);
        t.draw(bar);
    }

    // Icon square
    constexpr float iconSz = 44.f;
    float iconX = x + 16.f;
    float iconY = y + (h - iconSz) * 0.5f;
    sf::RectangleShape iconBg({iconSz, iconSz});
    iconBg.setPosition(iconX, iconY);
    iconBg.setFillColor(primary ? kEmber : kBg3);
    iconBg.setOutlineColor(primary ? sf::Color::Transparent : kLine);
    iconBg.setOutlineThickness(1.f);
    t.draw(iconBg);

    // Icon glyph — ASCII only
    const char* glyph = (idx == 0) ? ">"
                       : (idx == 1) ? "#"
                       : (idx == 2) ? "v"
                       :              "x";
    sf::Text ico(glyph, m_font, 18);
    ico.setStyle(sf::Text::Bold);
    ico.setFillColor(primary ? sf::Color(42, 20, 8) : kGoldBrt);
    auto ib = ico.getLocalBounds();
    ico.setPosition(iconX + (iconSz - ib.width) * 0.5f - ib.left,
                    iconY + (iconSz - ib.height) * 0.5f - ib.top);
    applyTextScale(ico);
    t.draw(ico);

    // Label
    float textX = iconX + iconSz + 18.f;
    sf::Text lbl(label, m_font, primary ? 22 : 18);
    lbl.setStyle(sf::Text::Bold);
    lbl.setFillColor(kInk);
    lbl.setPosition(textX, y + 11.f);
    applyTextScale(lbl);
    t.draw(lbl);

    // Subtitle
    sf::Text subT(sub, m_font, 12);
    subT.setFillColor(kInk3);
    subT.setPosition(textX, y + 38.f);
    applyTextScale(subT);
    t.draw(subT);

    // Chevron
    sf::Text chev(">", m_font, 16);
    chev.setFillColor(hover ? kGoldBrt : kInk4);
    auto cb = chev.getLocalBounds();
    chev.setPosition(x + NAV_W - 28.f, y + (h - cb.height) * 0.5f - cb.top);
    applyTextScale(chev);
    t.draw(chev);
}

// ── Main draw ─────────────────────────────────────────────────────────────────

void MainMenuScreen::draw(sf::RenderWindow& w) const {
    w.clear(kBg1);

    // Subtle ember radial glow behind nav
    for (int ring = 1; ring < 7; ++ring) {
        sf::CircleShape glow(static_cast<float>(ring * 120));
        glow.setOrigin(glow.getRadius(), glow.getRadius());
        glow.setPosition(NAV_X + 200.f, -60.f);
        glow.setFillColor(sf::Color::Transparent);
        glow.setOutlineColor(sf::Color(217, 116, 63,
            static_cast<sf::Uint8>(std::max(0, 5 - ring / 2))));
        glow.setOutlineThickness(1.f);
        w.draw(glow);
    }

    // Brand
    sf::Text brand("CITADEL MTG", m_font, 26);
    brand.setStyle(sf::Text::Bold);
    brand.setFillColor(kInk);
    brand.setPosition(NAV_X, 24.f);
    applyTextScale(brand);
    w.draw(brand);

    // Mana pips
    for (int i = 0; i < 5; ++i) {
        sf::CircleShape pip(5.f);
        pip.setPosition(NAV_X + 150.f + i * 15.f, 31.f);
        pip.setFillColor(pipColour(i));
        w.draw(pip);
    }

    // Hero text
    drawTxt(w, "Planar Combat Client - Season 12", NAV_X, 90.f, 11, kInk3);

    sf::Text hero1("Command the", m_font, 58);
    hero1.setStyle(sf::Text::Bold);
    hero1.setFillColor(kInk);
    hero1.setPosition(NAV_X, 110.f);
    applyTextScale(hero1);
    w.draw(hero1);

    sf::Text hero2("battlefield.", m_font, 58);
    hero2.setStyle(sf::Text::Bold);
    hero2.setFillColor(kGoldBrt);
    hero2.setPosition(NAV_X, 172.f);
    applyTextScale(hero2);
    w.draw(hero2);

    drawTxt(w, "Multiplayer Commander, built for the table.",
            NAV_X, 244.f, 14, kInk3);
    drawTxt(w, "Two to four players, every duel in full clarity.",
            NAV_X, 262.f, 14, kInk3);

    // Nav items
    static constexpr struct { const char* label; const char* sub; bool primary; } kDefs[6] = {
        { "Play",         "Ranked & casual Commander",     true  },
        { "Deck Builder", "Forge and refine your decks",   false },
        { "Download Art", "Fetch card artwork from web",   false },
        { "AI Data",      "Combo + salt databases",        false },
        { "Settings",     "Audio, display, keybindings",   false },
        { "Quit",         "Exit the application",          false },
    };
    for (int i = 0; i < 6; ++i)
        drawNavItem(w, i, kDefs[i].label, kDefs[i].sub, kDefs[i].primary, m_hover == i);

    // Settings overlay (toggled by the Settings nav item)
    if (m_showSettings) drawSettingsOverlay(w);
    if (m_showAiData)   drawAiDataOverlay(w);

    // Bottom version bar
    sf::RectangleShape vbar({WIN_W, 1.f});
    vbar.setPosition(0.f, WIN_H - 36.f);
    vbar.setFillColor(kLineSoft);
    w.draw(vbar);
    drawTxt(w, "Citadel MTG v0.1  -  open-source Magic: the Gathering engine",
            20.f, WIN_H - 24.f, 10, kInk4);
}

void MainMenuScreen::drawAiDataOverlay(sf::RenderTarget& t) const {
    sf::RectangleShape dim({WIN_W, WIN_H});
    dim.setFillColor(sf::Color(0, 0, 0, 170));
    t.draw(dim);

    constexpr float pw = 460.f, ph = 240.f;
    float px0 = (WIN_W - pw) * 0.5f, py0 = (WIN_H - ph) * 0.5f;

    sf::RectangleShape pan({pw, ph});
    pan.setPosition(px0, py0);
    pan.setFillColor(sf::Color(18, 17, 16, 252));
    pan.setOutlineColor(kGold);
    pan.setOutlineThickness(2.f);
    t.draw(pan);

    drawTxt(t, "AI DATA",          px0 + 16.f, py0 + 14.f, 18, kInk, true);
    drawTxt(t, "Esc / click outside to close",
            px0 + 16.f, py0 + 38.f, 9, kInk4);

    drawTxt(t, "Combos:", px0 + 16.f, py0 + 70.f, 12, kInk3, true);
    drawTxt(t, m_comboCount < 0 ? "(not loaded)"
                                 : std::to_string(m_comboCount) + " loaded",
            px0 + 100.f, py0 + 70.f, 12, kInk);

    drawTxt(t, "Salt:", px0 + 16.f, py0 + 92.f, 12, kInk3, true);
    drawTxt(t, m_saltCount < 0 ? "(not loaded)"
                                : std::to_string(m_saltCount) + " entries",
            px0 + 100.f, py0 + 92.f, 12, kInk);

    if (!m_aiDataStatus.empty())
        drawTxt(t, m_aiDataStatus, px0 + 16.f, py0 + 120.f, 11, kGoldBrt);

    // Two action buttons centered at the bottom.
    constexpr float bw = 200.f, bh = 36.f, gap = 12.f;
    float bx = px0 + (pw - bw * 2.f - gap) * 0.5f;
    float by = py0 + ph - bh - 22.f;

    auto drawBtn = [&](float x, const char* label, sf::Color fill, sf::Color border) {
        sf::RectangleShape btn({bw, bh});
        btn.setPosition(x, by);
        btn.setFillColor(fill);
        btn.setOutlineColor(border);
        btn.setOutlineThickness(1.5f);
        t.draw(btn);
        sf::Text lt(label, m_font, 13);
        lt.setStyle(sf::Text::Bold);
        lt.setFillColor(kInk);
        auto lb = lt.getLocalBounds();
        lt.setPosition(x + (bw - lb.width) * 0.5f - lb.left,
                       by + (bh - lb.height) * 0.5f - lb.top);
        t.draw(lt);
    };
    drawBtn(bx,                  "Download Combos",   sf::Color(28, 44, 24), sf::Color(95, 168, 115));
    drawBtn(bx + bw + gap,       "Download Salt DB",  sf::Color(20, 38, 52), sf::Color(90, 160, 216));
}

void MainMenuScreen::drawSettingsOverlay(sf::RenderTarget& t) const {
    // Dim background
    sf::RectangleShape dim({WIN_W, WIN_H});
    dim.setFillColor(sf::Color(0, 0, 0, 170));
    t.draw(dim);

    constexpr float pw = 560.f, ph = 440.f;
    float px0 = (WIN_W - pw) * 0.5f, py0 = (WIN_H - ph) * 0.5f;

    sf::RectangleShape pan({pw, ph});
    pan.setPosition(px0, py0);
    pan.setFillColor(sf::Color(18, 17, 16, 252));
    pan.setOutlineColor(kGold);
    pan.setOutlineThickness(2.f);
    t.draw(pan);

    drawTxt(t, "SETTINGS", px0 + 16.f, py0 + 14.f, 18, kInk, true);
    drawTxt(t, "Press Esc / click Settings again to close",
            px0 + 16.f, py0 + 38.f, 9, kInk4);

    // Volume row
    float ry = py0 + 68.f;
    drawTxt(t, "Volume", px0 + 16.f, ry, 12, kInk3, true);
    constexpr float slW = pw - 130.f;
    float slX = px0 + 95.f;
    sf::RectangleShape slTrack({slW, 4.f});
    slTrack.setPosition(slX, ry + 6.f);
    slTrack.setFillColor(kBg4);
    t.draw(slTrack);
    float fillW = slW * std::clamp(m_volume / 100.f, 0.f, 1.f);
    sf::RectangleShape slFill({fillW, 4.f});
    slFill.setPosition(slX, ry + 6.f);
    slFill.setFillColor(kGold);
    t.draw(slFill);
    drawTxt(t, std::to_string(static_cast<int>(m_volume)) + "%",
            slX + slW + 8.f, ry + 1.f, 11, kInk2);

    // Muted row
    ry += 36.f;
    drawTxt(t, "Muted", px0 + 16.f, ry, 12, kInk3, true);
    sf::RectangleShape muteBox({18.f, 18.f});
    muteBox.setPosition(px0 + 95.f, ry - 2.f);
    muteBox.setFillColor(m_muted ? kGold : kBg3);
    muteBox.setOutlineColor(kLine);
    muteBox.setOutlineThickness(1.f);
    t.draw(muteBox);
    if (m_muted) drawTxt(t, "x", px0 + 99.f, ry - 1.f, 12, sf::Color(20,12,4), true);

    // Full keyboard shortcut reference
    ry += 38.f;
    sf::RectangleShape kbSep({pw - 4.f, 1.f});
    kbSep.setPosition(px0 + 2.f, ry - 4.f);
    kbSep.setFillColor(sf::Color(240, 220, 180, 25));
    t.draw(kbSep);
    drawTxt(t, "KEYBOARD SHORTCUTS", px0 + 16.f, ry, 11, kGold, true);
    ry += 18.f;

    // Two-column layout
    static const char* kLeft[] = {
        "Space / Enter    Pass priority / confirm",
        "C                Concede",
        "G                Your graveyard",
        "H                Opponent graveyard",
        "A                Alpha strike (all attack)",
        "T                Tap all lands for mana",
        "E                End phase",
        "O                Options panel",
        "S                Start spectate mode",
        "/                Filter game log",
    };
    static const char* kRight[] = {
        "Ctrl+Z           Undo last action",
        "Ctrl+F           Search all zones",
        "Ctrl+S           Quick-save",
        "Ctrl+L           Quick-load",
        "F5               Hot-reload card rules",
        "F11              Toggle fullscreen",
        "Right-click      Zoom card / oracle text",
        "+/-              Animation speed",
        "?                In-game help",
        "R (game over)    Rematch",
    };
    for (int i = 0; i < 10 && ry + i * 14.f < py0 + ph - 12.f; ++i) {
        drawTxt(t, kLeft[i],  px0 + 16.f,         ry + i * 14.f, 9, kInk3);
        drawTxt(t, kRight[i], px0 + pw * 0.52f,   ry + i * 14.f, 9, kInk3);
    }
}

} // namespace ui
