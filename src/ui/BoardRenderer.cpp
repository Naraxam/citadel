#include "BoardRenderer.h"
#include "UiScale.h"
#include "../game/CardStats.h"
#include "../game/ability/ScriptLine.h"
#include "../game/ability/AbilityProcessor.h"   // stackAbilities() for the stack view
#include <algorithm>
#include <cmath>
#include <random>
#include <sstream>
#include <vector>

namespace ui {

using namespace Layout;
using namespace mtg;

// ── Construction ──────────────────────────────────────────────────────────────

BoardRenderer::BoardRenderer(const sf::Font& font, const GameState& game,
                              const TurnManager& tm, const std::string& picsDir)
    : m_font(&font), m_game(&game), m_tm(&tm), m_picsDir(picsDir)
{}

void BoardRenderer::init(const sf::Font& font, const GameState& game,
                          const TurnManager& tm, const std::string& picsDir) {
    m_font = &font; m_game = &game; m_tm = &tm; m_picsDir = picsDir;
}

// ── Layout helpers ────────────────────────────────────────────────────────────

static void zoneRect(uint8_t player, ZoneType zone,
                     float& zx, float& zy, float& zh) {
    zx = 0.f;
    if (player == 0) {
        if (zone == ZoneType::Hand)        { zy = ALICE_HAND_Y; zh = ALICE_HAND_H; }
        else                               { zy = ALICE_BF_Y;   zh = ALICE_BF_H;   }
    } else {
        if (zone == ZoneType::Hand)        { zy = BOB_HAND_Y;   zh = BOB_HAND_H;   }
        else                               { zy = BOB_BF_Y;     zh = BOB_BF_H;     }
    }
}

sf::Vector2f BoardRenderer::cardScreenPos(int index, uint8_t pid, ZoneType zone) const {
    float zx, zy, zh;
    zoneRect(pid, zone, zx, zy, zh);
    return cardPos(index, zx, zy, zh);
}

sf::Vector2f BoardRenderer::screenCenterOf(ObjectId id) const {
    if (!m_game) return {};
    const Card* target = nullptr;
    uint8_t pid = 0;
    for (const Card* c : m_game->battlefield().cards()) {
        if (c->id == id) { target = c; pid = c->controllerId; break; }
    }
    if (!target) return {};

    float zx, zy, zh;
    zoneRect(pid, ZoneType::Battlefield, zx, zy, zh);

    std::vector<const Card*> lands, creatures, others;
    for (const Card* c : m_game->battlefield().cards()) {
        if (c->controllerId != pid) continue;
        if      (c->rules->type.isLand())     lands.push_back(c);
        else if (c->rules->type.isCreature()) creatures.push_back(c);
        else                                  others.push_back(c);
    }

    bool twoRows = !lands.empty() && !creatures.empty();
    float rowH = twoRows ? zh * 0.5f : zh;

    std::vector<const Card*> topRow;
    topRow.insert(topRow.end(), creatures.begin(), creatures.end());
    topRow.insert(topRow.end(), others.begin(), others.end());
    float topY = zy;
    float botY = twoRows ? zy + rowH : zy;
    float landRowH = twoRows ? rowH : zh;

    int idx = 0;
    for (const Card* c : topRow) {
        auto pos = cardPos(idx++, zx, topY, rowH);
        if (c->id == id) return {pos.x + CARD_W * 0.5f, pos.y + CARD_H * 0.5f};
    }
    idx = 0;
    for (const Card* c : lands) {
        auto pos = cardPos(idx++, zx, botY, landRowH);
        if (c->id == id) return {pos.x + CARD_W * 0.5f, pos.y + CARD_H * 0.5f};
    }
    return {};
}

// ── Drawing primitives ────────────────────────────────────────────────────────

void BoardRenderer::drawSectionHeader(sf::RenderTarget& t, const std::string& label,
                                       float x, float y, float w, float h) const {
    sf::RectangleShape hdr({w, h});
    hdr.setPosition(x, y);
    hdr.setFillColor(sf::Color(11, 10, 9));   // --bg-0 deepest
    t.draw(hdr);
    // Accent line at the bottom of the header — subtle warm gold hairline
    sf::RectangleShape accent({w, 1.f});
    accent.setPosition(x, y + h - 1.f);
    accent.setFillColor(sf::Color(240, 220, 180, 35));
    t.draw(accent);
    sf::Text txt(label, *m_font, 10);
    txt.setFillColor(kTextDim);
    txt.setStyle(sf::Text::Bold);
    auto lb = txt.getLocalBounds();
    txt.setPosition(x + (w - lb.width) * 0.5f, y + (h - lb.height) * 0.5f - 2.f);
    ui::applyTextScale(txt);
    t.draw(txt);
}

void BoardRenderer::drawButton(sf::RenderTarget& t, const std::string& label,
                                float x, float y, float w, float h,
                                sf::Color fill, sf::Color textCol) const {
    sf::RectangleShape btn({w, h});
    btn.setPosition(x, y);
    btn.setFillColor(fill);
    btn.setOutlineColor(sf::Color(20, 25, 32));
    btn.setOutlineThickness(1.5f);
    t.draw(btn);

    sf::Text txt(label, *m_font, 12);
    txt.setFillColor(textCol);
    auto lb = txt.getLocalBounds();
    txt.setPosition(x + (w - lb.width) * 0.5f,
                    y + (h - lb.height) * 0.5f - 2.f);
    ui::applyTextScale(txt);
    t.draw(txt);
}

void BoardRenderer::drawZoneBg(sf::RenderTarget& t,
                                float x, float y, float w, float h,
                                const std::string& label) const {
    // Citadel zone background: dark warm panel with subtle transparency
    sf::RectangleShape bg({w, h});
    bg.setPosition(x, y);
    bg.setFillColor(sf::Color(22, 20, 18, SkinAssets::ready() ? 160 : 220));
    t.draw(bg);
    // Hairline border
    sf::RectangleShape zBorder({w, 1.f});
    zBorder.setPosition(x, y);
    zBorder.setFillColor(sf::Color(240, 220, 180, 16));
    t.draw(zBorder);
    if (!label.empty()) {
        sf::Text lbl(label, *m_font, 9);
        lbl.setFillColor(kTextDim);
        lbl.setPosition(x + 4.f, y + 3.f);
        ui::applyTextScale(lbl);
        t.draw(lbl);
    }
}

// ── Local text helper ─────────────────────────────────────────────────────────
// Draws text at (x,y) with hi-DPI sharpening. Returns logical (view-space) width.
static float drawTxt(sf::RenderTarget& t, const sf::Font& font,
                     const std::string& str, float x, float y,
                     unsigned size, sf::Color col, bool bold = false) {
    if (str.empty()) return 0.f;
    sf::Text txt(str, font, size);
    if (bold) txt.setStyle(sf::Text::Bold);
    txt.setFillColor(col);
    txt.setPosition(x, y);
    float w = txt.getLocalBounds().width;
    ui::applyTextScale(txt);
    t.draw(txt);
    return w;
}

// ── Background ────────────────────────────────────────────────────────────────

// Build a dark charcoal grunge texture procedurally (no external image file):
// smooth value-noise blotches over a near-black base, a strong corner vignette,
// and fine film grain. Generated once and cached in m_bgTexture.
void BoardRenderer::buildProceduralBackground() const {
    constexpr unsigned W = 1024, H = 640;
    sf::Image img;
    img.create(W, H);

    std::mt19937 rng(0xC17ADE1u);
    std::uniform_real_distribution<float> unit(0.f, 1.f);

    // A small random lattice, bilinearly sampled with smoothstep = value noise.
    auto makeLattice = [&](int gw, int gh) {
        std::vector<float> g(static_cast<size_t>(gw) * gh);
        for (float& v : g) v = unit(rng);
        return g;
    };
    auto sample = [](const std::vector<float>& g, int gw, int gh, float u, float v) {
        float fx = u * (gw - 1), fy = v * (gh - 1);
        int x0 = static_cast<int>(fx), y0 = static_cast<int>(fy);
        int x1 = std::min(x0 + 1, gw - 1), y1 = std::min(y0 + 1, gh - 1);
        float tx = fx - x0, ty = fy - y0;
        tx = tx * tx * (3.f - 2.f * tx);   // smoothstep
        ty = ty * ty * (3.f - 2.f * ty);
        float a = g[static_cast<size_t>(y0) * gw + x0], b = g[static_cast<size_t>(y0) * gw + x1];
        float c = g[static_cast<size_t>(y1) * gw + x0], d = g[static_cast<size_t>(y1) * gw + x1];
        return (a * (1 - tx) + b * tx) * (1 - ty) + (c * (1 - tx) + d * tx) * ty;
    };
    const auto L1 = makeLattice(9, 7);     // broad smoky blotches
    const auto L2 = makeLattice(33, 21);   // medium texture clusters

    std::uniform_real_distribution<float> grain(-1.f, 1.f);
    const float invDiag = 1.f / std::sqrt(2.f);
    for (unsigned y = 0; y < H; ++y) {
        float v = static_cast<float>(y) / (H - 1);
        for (unsigned x = 0; x < W; ++x) {
            float u = static_cast<float>(x) / (W - 1);
            float n = 0.62f * sample(L1, 9, 7, u, v) + 0.38f * sample(L2, 33, 21, u, v);
            float base = 13.f + n * 27.f;                 // ~13..40 grey patches
            float dx = (u - 0.5f) * 2.f, dy = (v - 0.5f) * 2.f;
            float r = std::sqrt(dx * dx + dy * dy) * invDiag;  // 0 centre .. 1 corner
            float vig = std::clamp(1.f - 0.95f * (r * r), 0.f, 1.f);  // edges near black
            float val = base * vig + grain(rng) * 4.f;    // fine film grain
            auto g = static_cast<sf::Uint8>(std::clamp(val, 0.f, 255.f));
            img.setPixel(x, y, sf::Color(g, g, static_cast<sf::Uint8>(std::min(255, g + 1))));
        }
    }
    m_bgTexture.loadFromImage(img);
    m_bgTexture.setSmooth(true);
    m_bgBuilt = true;
}

void BoardRenderer::drawBackground(sf::RenderTarget& t) const {
    t.clear(sf::Color(11, 10, 9));   // --bg-0 (shown only if texture fails)

    if (!m_bgBuilt) buildProceduralBackground();

    // Procedural dark grunge texture is the match backdrop (full opacity).
    auto tsz = m_bgTexture.getSize();
    float scale = std::max(WIN_W / static_cast<float>(tsz.x),
                           WIN_H / static_cast<float>(tsz.y));
    sf::Sprite spr(m_bgTexture);
    spr.setScale(scale, scale);
    spr.setPosition((WIN_W - tsz.x * scale) * 0.5f,
                    (WIN_H - tsz.y * scale) * 0.5f);
    t.draw(spr);

    // Diagonal grain pattern for premium texture
    for (float d = 0; d < WIN_W + WIN_H; d += 22.f) {
        sf::VertexArray ln(sf::Lines, 2);
        float x0 = std::max(0.f, d - WIN_H);
        float y0 = std::min(d, WIN_H);
        float x1 = std::min(d, WIN_W);
        float y1 = std::max(0.f, d - WIN_W);
        ln[0].position = {x0, y0}; ln[1].position = {x1, y1};
        ln[0].color = ln[1].color = sf::Color(255, 255, 255, 4);
        t.draw(ln);
    }
}

// ── Info bars (top of each player's area) ─────────────────────────────────────

void BoardRenderer::drawInfoBar(sf::RenderTarget& t, uint8_t pid) const {
    float y = (pid == 0) ? ALICE_INFO_Y : BOB_INFO_Y;
    float h = (pid == 0) ? ALICE_INFO_H : BOB_INFO_H;

    // Citadel info bar: warm charcoal with subtle gold border
    sf::RectangleShape bar({PLAY_W, h});
    bar.setPosition(0.f, y);
    bar.setFillColor(sf::Color(18, 16, 14, 230));  // near --bg-1
    t.draw(bar);
    // Bottom hairline
    sf::RectangleShape hairline({PLAY_W, 1.f});
    hairline.setPosition(0.f, y + h - 1.f);
    hairline.setFillColor(sf::Color(240, 220, 180, 22));
    t.draw(hairline);

    const Player& p = m_game->player(pid);
    float cx = 6.f;
    float midY = y + h * 0.5f;

    // Avatar
    constexpr float kAvatarSz = 26.f;
    SkinAssets::drawAvatar(t, cx, midY - kAvatarSz * 0.5f, kAvatarSz);
    cx += kAvatarSz + 6.f;

    // Name
    cx += drawTxt(t, *m_font, p.name(), cx, midY - 13.f, 13,
                  sf::Color(210, 220, 210), true) + 12.f;

    // Life total — red when critical (≤ 5)
    {
        int life = p.life();
        bool critical = life <= 5;
        cx += drawTxt(t, *m_font, std::to_string(life), cx, midY - 11.f, 16,
                      critical ? kAccentRed : sf::Color(200, 218, 210), true) + 2.f;
        cx += drawTxt(t, *m_font, "HP", cx, midY - 3.f, 8, kTextDim) + 12.f;
    }

    // Poison counters (red if ≥ 8)
    if (p.poisonCounters() > 0) {
        bool danger = p.poisonCounters() >= 8;
        cx += drawTxt(t, *m_font, "Psn " + std::to_string(p.poisonCounters()),
                      cx, midY - 9.f, 13,
                      danger ? sf::Color(230, 40, 180) : sf::Color(190, 100, 200)) + 12.f;
    }

    // Mana pool — one colored pip per mana type with its count inside, so each
    // player's currently-available (tapped/floating) mana is visible at a glance.
    if (!p.manaPool().isEmpty()) {
        auto cc = p.manaPool().colorCounts();   // W U B R G C any generic
        static const sf::Color pipCols[8] = {
            sf::Color(248, 246, 236), sf::Color(70, 140, 225), sf::Color(58, 52, 64),
            sf::Color(214, 75, 55),   sf::Color(70, 170, 90),  sf::Color(192, 192, 200),
            sf::Color(228, 196, 92),  sf::Color(150, 150, 150)
        };
        // Light text on the dark pips (U, B, R, G); dark text on the rest.
        auto lightText = [](int i) { return i == 1 || i == 2 || i == 3 || i == 4; };
        constexpr float r = 8.f;
        for (int i = 0; i < 8; ++i) {
            if (cc[i] <= 0) continue;
            sf::CircleShape pip(r);
            pip.setPosition(cx, midY - r);
            pip.setFillColor(pipCols[i]);
            pip.setOutlineColor(sf::Color(18, 16, 12, 220));
            pip.setOutlineThickness(1.f);
            t.draw(pip);
            sf::Color tc = lightText(i) ? sf::Color(245, 245, 245) : sf::Color(28, 24, 18);
            std::string n = std::to_string(cc[i]);
            float tx = cx + r - (n.size() == 1 ? 3.f : 6.f);
            drawTxt(t, *m_font, n, tx, midY - 8.f, 11, tc, true);
            cx += 2.f * r + 3.f;
        }
        cx += 5.f;
    }

    // Zone counts on the right side
    constexpr float kIconSz = 16.f;
    float rx = PLAY_W - 6.f;
    float iconY = midY - kIconSz * 0.5f;

    size_t exileCount = 0;
    for (const Card* c : m_game->exile().cards())
        if (c->ownerId == pid) ++exileCount;

    struct ZI { const char* key; size_t n; };
    const ZI zones[] = {
        {"EXILE",     exileCount},
        {"GRAVEYARD", p.graveyard().size()},
        {"HAND",      p.hand().size()},
        {"LIBRARY",   p.library().size()},
    };
    // Reset this player's icon rects each frame; the loop below overwrites
    // GY / Exile entries when they get drawn.
    m_gyIconRect[pid]    = {};
    m_exileIconRect[pid] = {};
    m_libIconRect[pid]   = {};

    for (const auto& zi : zones) {
        std::string ns = std::to_string(zi.n);
        // Hand at max size (7) → amber warning; over 7 → red
        bool handOver   = (std::string_view(zi.key) == "HAND" && zi.n >= 7);
        bool handCrit   = (std::string_view(zi.key) == "HAND" && zi.n > 7);
        bool libLow     = (std::string_view(zi.key) == "LIBRARY" && zi.n <= 10);
        bool libCrit    = (std::string_view(zi.key) == "LIBRARY" && zi.n <= 3);
        sf::Color numCol = handCrit  ? kAccentRed
                         : handOver  ? sf::Color(230, 160, 40)
                         : libCrit   ? kAccentRed
                         : libLow    ? sf::Color(230, 160, 40)
                                     : sf::Color(180, 190, 180);
        sf::Text tmp(ns, *m_font, 11);
        float cw = tmp.getLocalBounds().width;
        // Reserve the count's slot and remember its left edge for the hit
        // rect (so clicking either the count or the icon works).
        float countLeft = rx - cw - 2.f;
        rx -= cw + 2.f;
        drawTxt(t, *m_font, ns, rx, midY - 7.f, 11, numCol);
        rx -= 2.f;
        float iconLeft = rx;
        if (SkinAssets::ready()) {
            rx -= kIconSz;
            iconLeft = rx;
            SkinAssets::drawZoneIcon(t, zi.key, rx, iconY, kIconSz);
            rx -= 6.f;
        } else {
            // Fallback: draw a small textual marker so the count alone isn't
            // floating with no icon when assets aren't loaded yet.
            std::string mark = (std::string_view(zi.key) == "GRAVEYARD") ? "[GY]"
                             : (std::string_view(zi.key) == "EXILE")     ? "[EX]"
                             : (std::string_view(zi.key) == "HAND")      ? "[H]"
                                                                          : "[L]";
            sf::Text mt(mark, *m_font, 9);
            float mw = mt.getLocalBounds().width;
            rx -= mw + 2.f;
            iconLeft = rx;
            drawTxt(t, *m_font, mark, rx, midY - 5.f, 9, sf::Color(140, 150, 140));
            rx -= 6.f;
        }
        // Capture the GY / Exile click region (count + icon together).
        float hitLeft  = iconLeft;
        float hitRight = countLeft + cw + 2.f;
        if (std::string_view(zi.key) == "GRAVEYARD")
            m_gyIconRect[pid]    = sf::FloatRect(hitLeft, iconY, hitRight - hitLeft, kIconSz);
        else if (std::string_view(zi.key) == "EXILE")
            m_exileIconRect[pid] = sf::FloatRect(hitLeft, iconY, hitRight - hitLeft, kIconSz);
        else if (std::string_view(zi.key) == "LIBRARY")
            m_libIconRect[pid]   = sf::FloatRect(hitLeft, iconY, hitRight - hitLeft, kIconSz);
    }
}

BoardRenderer::ZoneIconHit
BoardRenderer::hitInfoBarZone(float px, float py) const noexcept {
    for (int pid = 0; pid < 2; ++pid) {
        if (m_gyIconRect[pid].width > 0.f &&
            m_gyIconRect[pid].contains(px, py))
            return { pid, BrowseZone::Graveyard, true };
        if (m_exileIconRect[pid].width > 0.f &&
            m_exileIconRect[pid].contains(px, py))
            return { pid, BrowseZone::Exile, true };
        if (m_libIconRect[pid].width > 0.f &&
            m_libIconRect[pid].contains(px, py))
            return { pid, BrowseZone::Library, true };
    }
    return { -1, BrowseZone::Graveyard, false };
}

int BoardRenderer::hitPlayerArea(float px, float py) const noexcept {
    // A click on a player's info bar that ISN'T a GY/Exile zone icon targets
    // that player (used during spell/ability target selection).
    if (hitInfoBarZone(px, py).player >= 0) return -1;
    for (int pid = 0; pid < 2; ++pid) {
        float y = (pid == 0) ? ALICE_INFO_Y : BOB_INFO_Y;
        float h = (pid == 0) ? ALICE_INFO_H : BOB_INFO_H;
        if (px >= PLAY_X && px < PLAY_X + PLAY_W && py >= y && py < y + h)
            return pid;
    }
    return -1;
}

// ── Hand ──────────────────────────────────────────────────────────────────────

void BoardRenderer::drawHand(sf::RenderTarget& t, uint8_t pid,
                               const RenderHints& hints) const {
    float zx, zy, zh;
    zoneRect(pid, ZoneType::Hand, zx, zy, zh);
    drawZoneBg(t, zx, zy, PLAY_W, zh, pid == 0 ? "HAND" : "");

    const Zone& hand = m_game->player(pid).hand();
    int idx = 0;
    for (const Card* c : hand.cards()) {
        auto pos = cardPos(idx++, zx, zy, zh);
        CardDrawOptions opts;
        opts.faceDown = (pid == 1);
        opts.selected = hints.selectedCards.count(c->id) > 0;
        opts.inHand   = (pid == 0);   // show name bar on player's own hand
        drawCard(t, *m_font, c, pos.x, pos.y, opts, m_picsDir);
    }

    // Hand count label
    if (pid == 0) {
        size_t n = hand.size();
        std::string lblStr = std::to_string(n) + " card" + (n != 1 ? "s" : "");
        sf::Text tmp(lblStr, *m_font, 10);
        float lblW = tmp.getLocalBounds().width;
        drawTxt(t, *m_font, lblStr, PLAY_W - lblW - 6.f, zy + zh - 14.f,
                10, sf::Color(130, 160, 130));
    }
}

// ── Battlefield ───────────────────────────────────────────────────────────────

void BoardRenderer::drawBattlefield(sf::RenderTarget& t, uint8_t pid,
                                     const RenderHints& hints) const {
    float zx, zy, zh;
    zoneRect(pid, ZoneType::Battlefield, zx, zy, zh);

    // Gather player's battlefield cards into typed groups. Auras/equipment
    // that are attached to a host are skipped — they render as small overlay
    // thumbnails on the host below.
    std::vector<const Card*> lands, creatures, pws, artifacts, enchants;
    for (const Card* c : m_game->battlefield().cards()) {
        if (c->controllerId != pid) continue;
        if (c->attachedTo != mtg::kInvalidId)        continue;
        if      (c->rules->type.isLand())            lands.push_back(c);
        else if (c->rules->type.isCreature())        creatures.push_back(c);
        else if (c->rules->type.isPlaneswalker())    pws.push_back(c);
        else if (c->rules->type.isArtifact())        artifacts.push_back(c);
        else                                         enchants.push_back(c);
    }

    bool hasNonLand = !creatures.empty() || !pws.empty() ||
                      !artifacts.empty() || !enchants.empty();
    bool twoRows = !lands.empty() && hasNonLand;
    float rowH = twoRows ? zh * 0.5f : zh;

    drawZoneBg(t, zx, zy, PLAY_W, zh,
               pid == 0 ? "YOUR BATTLEFIELD" : "OPPONENT BATTLEFIELD");

    if (twoRows) {
        sf::RectangleShape sep({PLAY_W - 2.f, 1.f});
        sep.setPosition(1.f, zy + rowH);
        sep.setFillColor(sf::Color(60, 80, 60, 100));
        t.draw(sep);
    }

    // Build the top row in canonical category order so the same type-group
    // always sits in the same chunk. A non-empty group inserts one card-slot
    // of horizontal gap between itself and the previous group, plus a tiny
    // text label so the player can spot the boundary at a glance.
    struct Group {
        const std::vector<const Card*>* cards;
        const char*                     label;
    };
    const Group groupOrder[] = {
        {&creatures, "CREATURES"},
        {&pws,       "PLANESWALKERS"},
        {&artifacts, "ARTIFACTS"},
        {&enchants,  "ENCHANTMENTS"},
    };
    std::vector<const Card*> topRow;
    std::vector<int>         slotForCard;     // visual slot per card
    std::vector<std::pair<int,const char*>> groupHeader;  // (slot, label)
    int slot = 0;
    bool firstGroup = true;
    for (const auto& g : groupOrder) {
        if (g.cards->empty()) continue;
        if (!firstGroup) ++slot;       // one-slot gap between groups
        groupHeader.push_back({slot, g.label});
        for (const Card* c : *g.cards) {
            topRow.push_back(c);
            slotForCard.push_back(slot++);
        }
        firstGroup = false;
    }

    float topY = zy;
    float botY = twoRows ? zy + rowH : zy;

    // Tiny category labels above each group's first card.
    for (const auto& [groupSlot, label] : groupHeader) {
        auto pos = cardPos(groupSlot, zx, topY, rowH);
        if (pos.x + 80.f > WIN_W) continue;  // off-screen
        drawTxt(t, *m_font, label, pos.x + 2.f, topY + 1.f, 8,
                sf::Color(150, 140, 120), true);
    }

    for (size_t topI = 0; topI < topRow.size(); ++topI) {
        const Card* c = topRow[topI];
        auto pos = cardPos(slotForCard[topI], zx, topY, rowH);

        // Card is currently mid-flight — drawn by drawAnims() instead
        bool inFlight = false;
        for (const auto& a : m_anims)
            if (a.id == c->id && a.kind == CardAnim::Kind::FlyAttack)
                { inFlight = true; break; }
        if (inFlight) continue;

        // Viewport culling: skip cards entirely outside the visible play area
        if (pos.x + CARD_W < 0.f || pos.x > WIN_W ||
            pos.y + CARD_H < 0.f || pos.y > WIN_H)
            continue;

        CardDrawOptions opts;
        opts.tapped      = c->tapped;
        opts.selected    = hints.selectedCards.count(c->id) > 0;
        opts.attacking   = hints.attackers.count(c->id) > 0;
        opts.colorBlind  = hints.colorBlindMode;
        if (c->tapped) {
            auto it = m_tapAnims.find(c->id);
            if (it != m_tapAnims.end() && it->second < 1.f)
                opts.rotationOverride = 90.f * it->second;
        }
        drawCard(t, *m_font, c, pos.x, pos.y, opts, m_picsDir);

        // Attached auras/equipment: render as small fan along the right edge
        // of the host so the player can see what's bolted on.
        if (!c->attachments.empty()) {
            constexpr float kAttW = 22.f;
            constexpr float kAttH = 30.f;
            float ax = pos.x + CARD_W - kAttW * 0.5f;
            float ay = pos.y + 4.f;
            for (size_t ai = 0; ai < c->attachments.size(); ++ai) {
                const Card* att = m_game->findCard(c->attachments[ai]);
                if (!att) continue;
                uint8_t ci = att->rules->manaCost.colorIdentity();
                bool isArt = att->rules->type.isArtifact();
                sf::Color bgCol = isArt ? sf::Color(70, 70, 75)
                                         : cardBackground(ci, false);
                sf::RectangleShape thumb({kAttW, kAttH});
                thumb.setPosition(ax, ay + static_cast<float>(ai) * (kAttH + 2.f));
                thumb.setFillColor(bgCol);
                thumb.setOutlineColor(sf::Color(20, 18, 14));
                thumb.setOutlineThickness(1.f);
                t.draw(thumb);
                // First 3 chars of attachment name so the player can tell
                // multiples apart (e.g. "Bra" for Brainstorm-as-aura).
                std::string tag = att->rules->name.substr(0, 3);
                drawTxt(t, *m_font, tag, ax + 2.f,
                        ay + static_cast<float>(ai) * (kAttH + 2.f) + 9.f,
                        8, sf::Color(245, 240, 220), true);
                // Tap indicator on the attachment thumbnail (auras don't tap,
                // but equipment can in rare cases).
                if (att->tapped) {
                    sf::RectangleShape ts({kAttW - 4.f, 2.f});
                    ts.setPosition(ax + 2.f,
                                   ay + static_cast<float>(ai) * (kAttH + 2.f) + kAttH - 4.f);
                    ts.setFillColor(sf::Color(220, 160, 0, 220));
                    t.draw(ts);
                }
            }
        }

        // Valid-target highlight: pulsing gold border when this card is a legal target
        if (!hints.validTargets.empty() && hints.validTargets.count(c->id)) {
            sf::RectangleShape tgtBorder({CARD_W - 3.f, CARD_H - 3.f});
            tgtBorder.setPosition(pos.x + 1.5f, pos.y + 1.5f);
            tgtBorder.setFillColor(sf::Color::Transparent);
            tgtBorder.setOutlineColor(sf::Color(230, 193, 112, 200));  // gold
            tgtBorder.setOutlineThickness(3.f);
            t.draw(tgtBorder);
            // Inner highlight hint
            sf::RectangleShape inner({CARD_W - 9.f, CARD_H - 9.f});
            inner.setPosition(pos.x + 4.5f, pos.y + 4.5f);
            inner.setFillColor(sf::Color(230, 193, 112, 18));
            t.draw(inner);
        }

        // Blocker order number overlay (1, 2, 3...) during damage-order selection
        auto orderIt = hints.blockerOrder.find(c->id);
        if (orderIt != hints.blockerOrder.end()) {
            sf::Text numTxt(std::to_string(orderIt->second), *m_font, 20);
            numTxt.setStyle(sf::Text::Bold);
            numTxt.setFillColor(sf::Color(255, 220, 40));
            numTxt.setOutlineColor(sf::Color(0, 0, 0, 200));
            numTxt.setOutlineThickness(2.f);
            auto lb = numTxt.getLocalBounds();
            numTxt.setPosition(pos.x + (CARD_W - lb.width) * 0.5f,
                               pos.y + CARD_H * 0.5f - 12.f);
            ui::applyTextScale(numTxt);
            t.draw(numTxt);
        }
    }

    int  landIdx   = 0;
    float landRowH = twoRows ? rowH : zh;

    // When the human has a hand spell queued and the pool still can't cover
    // its cost, draw a soft green outline on every untapped own land/mana
    // source — a tap-assist hint pointing at the sources that could fill in
    // the missing mana. Skip for opponent rows.
    bool castingAssist = false;
    const Card* pendingCard = nullptr;
    if (pid == 0 && hints.pendingSpell != mtg::kInvalidId) {
        pendingCard = m_game->findCard(hints.pendingSpell);
        if (pendingCard) {
            const ManaPool& pool = m_game->player(0).manaPool();
            if (!pool.canPay(pendingCard->rules->manaCost) &&
                !pendingCard->rules->manaCost.isNoCost())
                castingAssist = true;
        }
    }
    auto drawTapAssist = [&](float x, float y) {
        sf::RectangleShape g({CARD_W - 2.f, CARD_H - 2.f});
        g.setPosition(x + 1.f, y + 1.f);
        g.setFillColor(sf::Color::Transparent);
        g.setOutlineColor(sf::Color(110, 220, 130, 200));
        g.setOutlineThickness(-2.f);
        t.draw(g);
    };

    for (const Card* c : lands) {
        auto pos = cardPos(landIdx++, zx, botY, landRowH);
        CardDrawOptions opts;
        opts.tapped   = c->tapped;
        opts.selected = hints.selectedCards.count(c->id) > 0;
        // Tap-rotation anim: interpolate 0→90° over the recorded progress.
        if (c->tapped) {
            auto it = m_tapAnims.find(c->id);
            if (it != m_tapAnims.end() && it->second < 1.f)
                opts.rotationOverride = 90.f * it->second;
        }
        drawCard(t, *m_font, c, pos.x, pos.y, opts, m_picsDir);
        if (castingAssist && !c->tapped)
            drawTapAssist(pos.x, pos.y);
    }

    // Also glow non-land mana sources in the top row (Treasure, mana rocks,
    // creatures with {T}: Add… abilities) when assist is active.
    if (castingAssist) {
        for (size_t topI = 0; topI < topRow.size(); ++topI) {
            const Card* c = topRow[topI];
            auto pos = cardPos(slotForCard[topI], zx, topY, rowH);
            if (c->tapped) continue;
            bool producesMana = false;
            for (const auto& raw : c->rules->abilityLines) {
                auto sl = mtg::parseScriptLine(raw);
                if (sl.abilityType == "AB" && sl.effectType == "Mana") {
                    producesMana = true; break;
                }
            }
            if (producesMana) drawTapAssist(pos.x, pos.y);
        }
    }
}

// ── Vertical phase tracker (left sidebar) ────────────────────────────────────

void BoardRenderer::drawPhaseTracker(sf::RenderTarget& t, const RenderHints& hints) const {
    using mtg::TurnStep;
    static constexpr struct { TurnStep step; const char* label; bool isCombat; } kPhases[] = {
        {TurnStep::Untap,             "Untap",       false},
        {TurnStep::Upkeep,            "Upkeep",      false},
        {TurnStep::Draw,              "Draw",        false},
        {TurnStep::PreCombatMain,     "Main 1",      false},
        {TurnStep::BeginCombat,       "Begin Combat",true},
        {TurnStep::DeclareAttackers,  "Attackers",   true},
        {TurnStep::DeclareBlockers,   "Blockers",    true},
        {TurnStep::FirstStrikeDamage, "1st Strike",  true},
        {TurnStep::CombatDamage,      "Damage",      true},
        {TurnStep::EndCombat,         "End Combat",  true},
        {TurnStep::PostCombatMain,    "Main 2",      false},
        {TurnStep::EndStep,           "End Step",    false},
        {TurnStep::Cleanup,           "Cleanup",     false},
    };
    constexpr int   kCount  = 13;
    constexpr float kHdrH   = 14.f;
    constexpr float kRowH   = (SIDE_PHASE_H - kHdrH) / static_cast<float>(kCount); // ~14px

    // Section header — colored by whose turn it is so the player can tell
    // at a glance: gold = your turn (P0), ember = opponent.
    {
        uint8_t apid = m_game->activePlayerId();
        bool yourTurn = (apid == 0);
        sf::Color hdrFill = yourTurn ? sf::Color(44, 34, 14)   // warm gold
                                      : sf::Color(48, 22, 14);  // ember-dark
        sf::Color hdrText = yourTurn ? sf::Color(230, 193, 112) // gold-bright
                                      : sf::Color(230, 140, 110); // ember-bright
        sf::RectangleShape hdrBg({SIDE_W, kHdrH});
        hdrBg.setPosition(SIDE_X, SIDE_PHASE_Y);
        hdrBg.setFillColor(hdrFill);
        t.draw(hdrBg);
        // Thin accent strip on the left so the rows underneath also visually
        // belong to the active player.
        sf::RectangleShape accent({3.f, kHdrH});
        accent.setPosition(SIDE_X, SIDE_PHASE_Y);
        accent.setFillColor(hdrText);
        t.draw(accent);

        std::string hdrTxt = (yourTurn ? std::string("> ") : std::string("> "))
                           + "TURN " + std::to_string(m_game->turnNumber())
                           + "  " + m_game->activePlayer().name();
        sf::Text hdr(hdrTxt, *m_font, 9);
        hdr.setStyle(sf::Text::Bold);
        auto lb = hdr.getLocalBounds();
        hdr.setFillColor(hdrText);
        hdr.setPosition(SIDE_X + (SIDE_W - lb.width) * 0.5f,
                        SIDE_PHASE_Y + (kHdrH - lb.height) * 0.5f - 1.f);
        ui::applyTextScale(hdr);
        t.draw(hdr);
    }

    TurnStep current = m_tm->currentStep();

    for (int i = 0; i < kCount; ++i) {
        bool active = (kPhases[i].step == current);
        float ry = SIDE_PHASE_Y + kHdrH + static_cast<float>(i) * kRowH;

        // Citadel phase row backgrounds
        sf::Color rowBg = active
            ? sf::Color(34, 28, 18)    // warm gold highlight for active step
            : kPhases[i].isCombat
                ? sf::Color(24, 16, 12) // ember-dark for combat steps
                : sf::Color(16, 15, 13); // default --bg-0-ish
        sf::RectangleShape row({SIDE_W, kRowH - 1.f});
        row.setPosition(SIDE_X, ry);
        row.setFillColor(rowBg);
        t.draw(row);

        // Left accent bar for active row — gold instead of teal
        if (active) {
            sf::RectangleShape accent({3.f, kRowH - 1.f});
            accent.setPosition(SIDE_X, ry);
            accent.setFillColor(sf::Color(203, 163, 90));  // --gold
            t.draw(accent);
        }

        // Phase label
        {
            sf::Text lbl(kPhases[i].label, *m_font, 9);
            lbl.setStyle(active ? sf::Text::Bold : sf::Text::Regular);
            lbl.setFillColor(active ? sf::Color(230, 193, 112)  // --gold-bright
                            : kPhases[i].isCombat ? sf::Color(217, 116, 63, 200)  // --ember
                                                  : kInk3);
            lbl.setPosition(SIDE_X + 6.f,
                            ry + (kRowH - lbl.getLocalBounds().height) * 0.5f - 1.f);
            ui::applyTextScale(lbl);
            t.draw(lbl);
        }

        // Toggle indicator on the right edge of each row
        // Green dot = stop enabled; dim dot = auto-advance
        {
            bool stopped = (i < static_cast<int>(hints.stepStops.size()))
                           ? hints.stepStops[static_cast<size_t>(i)] : false;
            constexpr float dotR  = 3.5f;
            float dotX = SIDE_X + SIDE_W - dotR * 2.f - 3.f;
            float dotY = ry + (kRowH - dotR * 2.f) * 0.5f;
            sf::CircleShape dot(dotR);
            dot.setPosition(dotX, dotY);
            dot.setFillColor(stopped ? sf::Color(203, 163, 90, 220)  // --gold
                                     : sf::Color(44, 40, 35, 160)); // dim
            t.draw(dot);
        }

        // Hairline separator
        sf::RectangleShape sep({SIDE_W, 1.f});
        sep.setPosition(SIDE_X, ry + kRowH - 1.f);
        sep.setFillColor(sf::Color(240, 220, 180, 14));  // warm gold hairline
        t.draw(sep);
    }
}

// ── Dock (bottom action buttons) ──────────────────────────────────────────────

void BoardRenderer::drawDock(sf::RenderTarget& t, const RenderHints& hints) const {
    // The bottom dock (End Phase / Pass / Alpha / Concede) was removed: the
    // player-prompt text used to overlay these buttons. Concede + Undo now
    // live in the ESC pause menu; End Phase / Pass / Alpha Strike are still
    // available via keyboard shortcuts (Space, P, A — see the keybindings
    // help in main-menu Settings).
    //
    // We still emit the contextual instruction text, but at the TOP of the
    // play area (just under Bob's info strip) so it never collides with the
    // hand or battlefield.
    if (hints.instruction.empty()) return;
    sf::Text inst(hints.instruction, *m_font, 11);
    inst.setFillColor(sf::Color(230, 193, 112));
    float iw = inst.getLocalBounds().width;
    float ix = PLAY_X + (PLAY_W - iw) * 0.5f;
    if (ix < PLAY_X + 4.f) ix = PLAY_X + 4.f;
    constexpr float kInstY = 4.f;          // above Bob's strip (BOB_INFO_Y = 0)
    sf::RectangleShape ibg({iw + 14.f, 18.f});
    ibg.setPosition(ix - 7.f, kInstY);
    ibg.setFillColor(sf::Color(5, 8, 12, 200));
    ibg.setOutlineColor(sf::Color(203, 163, 90, 80));
    ibg.setOutlineThickness(1.f);
    t.draw(ibg);
    inst.setPosition(ix, kInstY + 3.f);
    ui::applyTextScale(inst);
    t.draw(inst);
}

// ── Sidebar player status strips ─────────────────────────────────────────────

void BoardRenderer::drawPlayerStatus(sf::RenderTarget& t,
                                      uint8_t pid,
                                      float y, float h) const {
    if (!m_game || !m_font) return;
    const Player& p = m_game->player(pid);

    // Citadel player strip: warm charcoal with gold (active) or ember (opponent) accent
    sf::Color bg = (pid == 0) ? sf::Color(22, 20, 16, 235)   // Alice: warm charcoal
                               : sf::Color(20, 18, 14, 235);  // Bob: slightly darker
    sf::RectangleShape bgRect({SIDE_W, h});
    bgRect.setPosition(SIDE_X, y);
    bgRect.setFillColor(bg);
    t.draw(bgRect);

    // Turn / priority indicator — bright outer border on whoever currently
    // owns the turn (gold) and a narrower hollow border on whoever holds
    // priority if it's not the active player (teal). The two can coincide
    // — e.g. on your own turn you hold priority too; we draw both and let
    // the gold win visually.
    {
        uint8_t apid = m_game->activePlayerId();
        uint8_t prio = m_game->priorityPlayerId();
        bool isActive   = (pid == apid);
        bool hasPriority = (pid == prio);

        if (isActive) {
            sf::RectangleShape glow({SIDE_W, h});
            glow.setPosition(SIDE_X, y);
            glow.setFillColor(sf::Color::Transparent);
            glow.setOutlineColor(sf::Color(230, 193, 112)); // --gold-bright
            glow.setOutlineThickness(-3.f);                  // inset, doesn't grow
            t.draw(glow);
        }
        if (hasPriority && !isActive) {
            sf::RectangleShape pri({SIDE_W - 2.f, h - 2.f});
            pri.setPosition(SIDE_X + 1.f, y + 1.f);
            pri.setFillColor(sf::Color::Transparent);
            pri.setOutlineColor(sf::Color(110, 200, 220));   // teal, "you can respond"
            pri.setOutlineThickness(-2.f);
            t.draw(pri);
        }
    }

    // Citadel accent line: gold for Alice (active player), ember for Bob (opponent)
    sf::RectangleShape sep({SIDE_W, 2.f});
    sep.setPosition(SIDE_X, (pid == 0) ? y : y + h - 2.f);
    sf::Color sepCol = (pid == 0) ? sf::Color(203, 163, 90, 180)   // --gold
                                  : sf::Color(217, 116, 63, 120);   // --ember
    sep.setFillColor(sepCol);
    t.draw(sep);

    float cx = SIDE_X + 6.f;
    float midY = y + h * 0.5f;

    // Avatar (larger now that the strip is 80px tall)
    constexpr float kSz = 38.f;
    SkinAssets::drawAvatar(t, cx, midY - kSz * 0.5f, kSz);
    cx += kSz + 6.f;

    // Name
    drawTxt(t, *m_font, p.name(), cx, y + 6.f, 12,
            pid == 0 ? sf::Color(kTeal.r, kTeal.g, kTeal.b) : sf::Color(230, 120, 120),
            true);

    // Life total (large) — pill badge style
    float lifeRightEdge = cx;
    {
        int life = p.life();
        bool critical = life <= 5;
        std::string lifeStr = std::to_string(life);
        sf::Color lifeCol = critical ? kAccentRed : sf::Color(210, 225, 215);
        // Measure at base size to position "HP" label correctly
        sf::Text tmp(lifeStr, *m_font, 20);
        float lifeW = tmp.getLocalBounds().width;
        drawTxt(t, *m_font, lifeStr, cx, y + 20.f, 20, lifeCol, true);
        drawTxt(t, *m_font, "HP", cx + lifeW + 3.f, y + 28.f, 9, kTextDim);
        lifeRightEdge = cx + lifeW + 18.f;  // start mana pips just past "HP"
    }

    // Mana pool pips, rendered immediately after the life total so the player
    // can read both at a single glance. Empty pool shows a dim placeholder
    // dash so the position is always discoverable. Skipped for opponent rows
    // (their pool empties between phases anyway).
    if (pid == 0) {
        std::string ms = p.manaPool().toString();
        if (ms.empty()) {
            drawTxt(t, *m_font, "—", lifeRightEdge, y + 30.f, 10, kTextDim);
        } else if (SkinAssets::ready()) {
            SkinAssets::drawManaCost(t, ms, lifeRightEdge, y + 26.f, 14.f);
        } else {
            drawTxt(t, *m_font, ms, lifeRightEdge, y + 30.f, 10,
                    sf::Color(255, 220, 80));
        }
    } else if (!p.manaPool().isEmpty()) {
        // Opponent: keep the existing right-side pool display so floating
        // mana during their turn is still visible.
        std::string ms = p.manaPool().toString();
        float rx = SIDE_X + SIDE_W - 6.f;
        if (SkinAssets::ready()) {
            SkinAssets::drawManaCost(t, ms, rx - 90.f, y + 28.f, 16.f);
        } else {
            sf::Text tmp(ms, *m_font, 10);
            float mw = tmp.getLocalBounds().width;
            drawTxt(t, *m_font, ms, rx - mw - 4.f, y + 30.f, 10,
                    sf::Color(255, 220, 80));
        }
    }

    // Life bar (thin bar near the bottom of the strip)
    {
        float barX = cx;
        float barW = SIDE_W - barX - SIDE_X - 6.f;
        float frac = std::clamp(p.life() / 40.f, 0.f, 1.f);
        sf::RectangleShape barBg({barW, 3.f});
        barBg.setPosition(barX, y + h - 7.f);
        barBg.setFillColor(sf::Color(25, 32, 42));
        t.draw(barBg);
        if (frac > 0.f) {
            sf::Color barCol = frac > 0.5f ? sf::Color(kTeal.r, kTeal.g, kTeal.b)
                             : frac > 0.25f ? sf::Color(kAccentGold.r, kAccentGold.g, kAccentGold.b)
                                            : sf::Color(kAccentRed.r, kAccentRed.g, kAccentRed.b);
            sf::RectangleShape barFill({barW * frac, 3.f});
            barFill.setPosition(barX, y + h - 7.f);
            barFill.setFillColor(barCol);
            t.draw(barFill);
        }
    }

    // Hand / Library / GY counts stacked on the right
    {
        int handSz = static_cast<int>(p.hand().size());
        int libSz  = static_cast<int>(p.library().size());
        int gySz   = static_cast<int>(p.graveyard().size());
        auto drawCount = [&](const char* prefix, int n, float ry) {
            std::string s = std::string(prefix) + std::to_string(n);
            sf::Text tmp(s, *m_font, 9);
            float sw = tmp.getLocalBounds().width;
            drawTxt(t, *m_font, s, SIDE_X + SIDE_W - sw - 6.f, ry, 9,
                    sf::Color(160, 170, 160));
        };
        drawCount("H:", handSz, y + 6.f);
        drawCount("L:", libSz,  y + 17.f);
        if (gySz > 0) drawCount("G:", gySz, y + 28.f);
    }

    // Monarch crown icon — small gold text badge if this player is the Monarch
    if (m_game->monarchPlayer == pid) {
        drawTxt(t, *m_font, "♛ MONARCH", cx, y + 50.f, 10,
                sf::Color(255, 215, 50), true);
    }

    // Poison counters (only if > 0)
    if (p.poisonCounters() > 0) {
        drawTxt(t, *m_font, "Psn " + std::to_string(p.poisonCounters()),
                cx, y + 50.f, 10, sf::Color(190, 80, 200), true);
    }

    // Commander damage received (from each opponent's commander, threshold = 21)
    // Show per-commander breakdown when more than one commander has dealt damage.
    {
        uint8_t oppId = pid ^ 1;
        int cmdDmg = p.commanderDamageFrom(oppId);
        if (cmdDmg > 0) {
            bool lethal     = cmdDmg >= 21;
            bool critical   = cmdDmg >= 16;  // within striking range
            sf::Color col   = lethal   ? kAccentRed
                            : critical ? sf::Color(240, 100, 40)
                                       : sf::Color(220, 160, 60);
            // Try to find the attacking commander's name for better tooltip
            std::string cmdName;
            for (const Card* c : m_game->battlefield().cards()) {
                if (!c->isCommander || c->controllerId != oppId) continue;
                cmdName = c->rules->name;
                if (cmdName.size() > 12) cmdName = cmdName.substr(0, 11) + ".";
                break;
            }
            if (cmdName.empty()) {
                for (const Card* c : m_game->command().cards()) {
                    if (!c->isCommander || c->ownerId != oppId) continue;
                    cmdName = c->rules->name;
                    if (cmdName.size() > 12) cmdName = cmdName.substr(0, 11) + ".";
                    break;
                }
            }
            std::string label = cmdName.empty()
                              ? "CMD: " + std::to_string(cmdDmg) + "/21"
                              : cmdName + ": " + std::to_string(cmdDmg) + "/21";
            float dmgY = y + (p.poisonCounters() > 0 ? 62.f : 50.f);
            // Prominent warning badge when approaching lethal (15+)
            if (critical) {
                sf::RectangleShape warn({86.f, 18.f});
                warn.setPosition(cx - 2.f, dmgY - 2.f);
                warn.setFillColor(sf::Color(lethal ? 80 : 50, 15, 10, 200));
                warn.setOutlineColor(col);
                warn.setOutlineThickness(1.f);
                t.draw(warn);
            }
            drawTxt(t, *m_font, label, cx, dmgY, 10, col, lethal || critical);
            // Progress bar (wider and taller when critical)
            float kBarW = critical ? 100.f : 80.f;
            float barH  = critical ? 4.f   : 3.f;
            float frac = std::clamp(cmdDmg / 21.f, 0.f, 1.f);
            sf::RectangleShape barBg({kBarW, barH});
            barBg.setPosition(cx, dmgY + 12.f);
            barBg.setFillColor(sf::Color(30, 20, 20));
            t.draw(barBg);
            sf::RectangleShape barFill({kBarW * frac, barH});
            barFill.setPosition(cx, dmgY + 12.f);
            barFill.setFillColor(col);
            t.draw(barFill);
            // Threshold marker at the 21 position
            sf::RectangleShape mark({1.f, barH + 3.f});
            mark.setPosition(cx + kBarW - 1.f, dmgY + 11.f);
            mark.setFillColor(sf::Color(200, 60, 60, 160));
            t.draw(mark);
        }
    }

    // Commander zone text moved to the right preview panel as a thumbnail
    // (drawCommanderThumbs); nothing rendered here anymore.
}

// ── Dungeon / Initiative progress ────────────────────────────────────────────

void BoardRenderer::drawDungeonProgress(sf::RenderTarget& t) const {
    if (!m_game || m_game->initiativeHolder < 0) return;

    // Undercity rooms (simplified 10-room dungeon path)
    static const char* kRooms[] = {
        "Entrance",
        "Secret Door",
        "Forge",
        "Catacombs",
        "Arena",
        "Vampire Nest",
        "Treasure",
        "Trap",
        "Boss Room",
        "Dungeon Heart"
    };
    static constexpr int kRoomCount = 10;

    int cur = std::clamp(m_game->initiativeRoom, 0, kRoomCount - 1);
    uint8_t holder = static_cast<uint8_t>(m_game->initiativeHolder);

    float dy = SIDE_LOG_Y - 90.f;  // above the log section
    drawSectionHeader(t, "DUNGEON", SIDE_X, dy, SIDE_W, 18.f);
    dy += 20.f;

    // Initiative holder badge
    std::string holderName = m_game->player(holder).name() + " holds Initiative";
    drawTxt(t, *m_font, holderName, SIDE_X + 4.f, dy, 8,
            holder == 0 ? sf::Color(203, 163, 90) : sf::Color(217, 116, 63));
    dy += 12.f;

    // Room progress bar
    for (int i = 0; i < kRoomCount; ++i) {
        bool active = (i == cur);
        bool done   = (i < cur);
        sf::Color bg = active ? sf::Color(44, 34, 14) : done ? sf::Color(26, 22, 14) : sf::Color(18, 17, 16);
        sf::RectangleShape room({SIDE_W - 8.f, 11.f});
        room.setPosition(SIDE_X + 4.f, dy + i * 12.f);
        room.setFillColor(bg);
        if (active) {
            room.setOutlineColor(sf::Color(203, 163, 90, 160));
            room.setOutlineThickness(1.f);
        }
        t.draw(room);
        sf::Color tc = active ? sf::Color(230, 193, 112) : done ? sf::Color(100, 90, 70) : sf::Color(70, 65, 58);
        if (i < kRoomCount)
            drawTxt(t, *m_font, kRooms[i], SIDE_X + 8.f, dy + i * 12.f + 1.f, 7, tc, active);
    }
}

// ── Command zone thumbnail ────────────────────────────────────────────────────

// One entry shown in a player's command-zone slot.
struct CmdEntry { const mtg::Card* card; bool inZone; bool isCompanion; };

// All cards to show in a player's command-zone slot: every commander (so
// partner pairs both appear), whether in the command zone or currently on the
// battlefield, plus the companion set aside outside the game (if any/unused).
static std::vector<CmdEntry> gatherCommandZone(const mtg::GameState& g, uint8_t pid) {
    std::vector<CmdEntry> out;
    for (const mtg::Card* c : g.command().cards())
        if (c && c->ownerId == pid && c->isCommander) out.push_back({c, true, false});
    for (const mtg::Card* c : g.battlefield().cards())
        if (c && c->ownerId == pid && c->isCommander) out.push_back({c, false, false});
    if (pid < 4 && g.companionId[pid] != mtg::kInvalidId && !g.companionUsed[pid])
        if (const mtg::Card* comp = g.findCard(g.companionId[pid]))
            out.push_back({comp, true, true});
    return out;
}

// Split a slot rect into n side-by-side sub-rects (for partner pairs / companion).
static sf::FloatRect cmdSubRect(const sf::FloatRect& slot, int i, int n) {
    if (n <= 1) return slot;
    constexpr float gap = 4.f;
    float w = (slot.width - gap * (n - 1)) / static_cast<float>(n);
    return { slot.left + i * (w + gap), slot.top, w, slot.height };
}

// Returns the screen-space rectangle of the commander thumbnail for `pid` in
// the right preview panel (Bob at top, Alice at bottom). Used by both the
// draw code and the hit-test that feeds previewCardId.
sf::FloatRect BoardRenderer::commanderThumbRect(uint8_t pid) {
    constexpr float kPad   = 8.f;
    constexpr float kThumbH = 110.f;
    float w = PREV_W - kPad * 2;
    float x = PREV_X + kPad;
    float y = (pid == 1) ? kPad                            // opp top
                         : (WIN_H - kThumbH - kPad);       // you bottom
    return { x, y, w, kThumbH };
}

void BoardRenderer::drawCommandZoneThumbnail(sf::RenderTarget& t) const {
    if (!m_game || !m_font) return;

    // Draw a single command-zone entry (commander or companion) into sub-rect r.
    auto drawEntry = [&](const CmdEntry& e, const sf::FloatRect& r, uint8_t pid) {
        const mtg::Card* c = e.card;
        sf::RectangleShape bg({r.width, r.height});
        bg.setPosition(r.left, r.top);
        bg.setFillColor(sf::Color(22, 20, 18));
        bg.setOutlineColor(e.isCompanion ? sf::Color(90, 180, 170, 150)        // teal companion
                          : (pid == 0 ? sf::Color(203, 163, 90, 120)           // gold you
                                      : sf::Color(217, 116, 63, 120)));        // ember opp
        bg.setOutlineThickness(1.f);
        t.draw(bg);

        bool drewArt = false;
        auto imgPath = findCardImage(c->rules->name, m_picsDir);
        if (!imgPath.empty()) {
            if (const auto* tex = TextureCache::get(imgPath)) {
                sf::Sprite spr(*tex);
                auto sz = tex->getSize();
                float scale = std::min(r.width  / static_cast<float>(sz.x),
                                       r.height / static_cast<float>(sz.y));
                spr.setScale(scale, scale);
                spr.setPosition(r.left + (r.width  - sz.x * scale) * 0.5f,
                                r.top  + (r.height - sz.y * scale) * 0.5f);
                if (!e.inZone) spr.setColor(sf::Color(160, 160, 160));   // dim if on bf
                t.draw(spr);
                drewArt = true;
            }
        }
        if (!drewArt) {
            std::string nm = c->rules->name;
            size_t cap = (r.width < 120.f) ? 16 : 26;
            if (nm.size() > cap) nm = nm.substr(0, cap) + "..";
            drawTxt(t, *m_font, nm, r.left + 4.f, r.top + 6.f, 9,
                    sf::Color(230, 193, 112), true);
            drawTxt(t, *m_font, e.inZone ? "(art loading)" : "(on battlefield)",
                    r.left + 4.f, r.top + 22.f, 8, sf::Color(120, 113, 100));
        } else if (!e.inZone) {
            drawTxt(t, *m_font, "(on bf)", r.left + 3.f, r.top + r.height - 12.f, 7,
                    sf::Color(210, 210, 210));
        }

        // Tag companions so they're distinguishable from commanders.
        if (e.isCompanion)
            drawTxt(t, *m_font, "COMPANION", r.left + 3.f, r.top + 2.f, 7,
                    sf::Color(120, 220, 205), true);
    };

    auto drawOne = [&](uint8_t pid) {
        auto slot = commanderThumbRect(pid);
        auto entries = gatherCommandZone(*m_game, pid);

        // Section label above the slot.
        const char* lbl = (pid == 1) ? "OPP COMMAND ZONE" : "YOUR COMMAND ZONE";
        drawTxt(t, *m_font, lbl, slot.left + 4.f, slot.top - 12.f, 8,
                sf::Color(148, 139, 124), true);

        if (entries.empty()) {
            sf::RectangleShape bg({slot.width, slot.height});
            bg.setPosition(slot.left, slot.top);
            bg.setFillColor(sf::Color(22, 20, 18));
            bg.setOutlineColor(pid == 0 ? sf::Color(203, 163, 90, 120)
                                        : sf::Color(217, 116, 63, 120));
            bg.setOutlineThickness(1.f);
            t.draw(bg);
            drawTxt(t, *m_font, "(empty)", slot.left + 6.f,
                    slot.top + slot.height * 0.5f - 5.f, 9, sf::Color(120, 113, 100));
            return;
        }

        int n = static_cast<int>(entries.size());
        for (int i = 0; i < n; ++i)
            drawEntry(entries[i], cmdSubRect(slot, i, n), pid);

        // Commander tax (per-cast +{2}) on the first commander thumbnail.
        int tax = m_game->commanderCastCount[pid];
        if (tax > 0) {
            auto r0 = cmdSubRect(slot, 0, n);
            std::string taxStr = "+" + std::to_string(tax * 2);
            sf::RectangleShape badge({28.f, 15.f});
            badge.setPosition(r0.left + r0.width - 30.f, r0.top + 2.f);
            badge.setFillColor(sf::Color(38, 18, 10, 220));
            badge.setOutlineColor(sf::Color(217, 116, 63, 200));
            badge.setOutlineThickness(1.f);
            t.draw(badge);
            drawTxt(t, *m_font, taxStr, r0.left + r0.width - 26.f, r0.top + 4.f, 9,
                    sf::Color(230, 170, 110), true);
        }

        drawTxt(t, *m_font, "hover to enlarge",
                slot.left + 6.f, slot.top + slot.height + 2.f, 7,
                sf::Color(100, 95, 85));
    };

    drawOne(1);   // opponent at top
    drawOne(0);   // you at bottom
}

// Map a mouse position to a commander Card* if the cursor is over either
// commander thumbnail in the right preview panel. Used by the renderer's
// hitTest so hover auto-feeds previewCardId and the big preview area
// "expands" the thumbnail in-place.
const mtg::Card* BoardRenderer::commanderUnderCursor(float px, float py) const {
    if (!m_game) return nullptr;
    for (uint8_t pid : {uint8_t{0}, uint8_t{1}}) {
        auto slot = commanderThumbRect(pid);
        if (px < slot.left || px > slot.left + slot.width ||
            py < slot.top  || py > slot.top  + slot.height) continue;
        auto entries = gatherCommandZone(*m_game, pid);
        int n = static_cast<int>(entries.size());
        for (int i = 0; i < n; ++i) {
            auto r = cmdSubRect(slot, i, n);
            if (px >= r.left && px <= r.left + r.width &&
                py >= r.top  && py <= r.top  + r.height)
                return entries[i].card;
        }
    }
    return nullptr;
}

// ── Day/Night indicator ───────────────────────────────────────────────────────

void BoardRenderer::drawDayNightIndicator(sf::RenderTarget& t) const {
    if (!m_game || m_game->dayNightState == 0) return;  // 0 = neither

    bool isDay = (m_game->dayNightState == 1);
    float ix = SIDE_X + 4.f, iy = SIDE_PHASE_Y + 2.f;
    constexpr float r = 7.f;

    // Outer circle (sun = gold, moon = blue-grey)
    sf::CircleShape circle(r);
    circle.setOrigin(r, r);
    circle.setPosition(ix + r, iy + r);
    circle.setFillColor(isDay ? sf::Color(240, 193, 80) : sf::Color(120, 135, 170));
    circle.setOutlineColor(isDay ? sf::Color(200, 150, 40) : sf::Color(80, 100, 140));
    circle.setOutlineThickness(1.f);
    t.draw(circle);

    if (isDay) {
        // Sun rays — 8 short lines radiating out
        for (int i = 0; i < 8; ++i) {
            float angle = static_cast<float>(i) * 3.14159f / 4.f;
            float cx2 = ix + r + (r + 2.f) * std::cos(angle);
            float cy2 = iy + r + (r + 2.f) * std::sin(angle);
            float ex  = ix + r + (r + 5.f) * std::cos(angle);
            float ey  = iy + r + (r + 5.f) * std::sin(angle);
            sf::Vertex ray[2] = {
                {{cx2, cy2}, sf::Color(240, 193, 80, 200)},
                {{ex,  ey},  sf::Color(240, 193, 80, 80)}
            };
            t.draw(ray, 2, sf::Lines);
        }
    } else {
        // Crescent: inner dark circle offset slightly
        sf::CircleShape inner(r * 0.7f);
        inner.setOrigin(r * 0.7f, r * 0.7f);
        inner.setPosition(ix + r + 3.f, iy + r - 2.f);
        inner.setFillColor(sf::Color(11, 10, 9));  // background colour
        t.draw(inner);
    }

    // Label
    drawTxt(t, *m_font, isDay ? "DAY" : "NIGHT",
            ix + r * 2 + 4.f, iy + r - 6.f, 9,
            isDay ? sf::Color(240, 193, 80) : sf::Color(140, 155, 190), true);
}

// ── Sidebar ───────────────────────────────────────────────────────────────────

void BoardRenderer::drawSidebar(sf::RenderTarget& t, const RenderHints& hints) const {
    // Citadel sidebar: deepest void colour with warm gold right border
    sf::RectangleShape bg({SIDE_W, WIN_H});
    bg.setPosition(SIDE_X, 0.f);
    bg.setFillColor(sf::Color(11, 10, 9));    // --bg-0
    t.draw(bg);

    sf::RectangleShape border({1.f, WIN_H});
    border.setPosition(SIDE_X + SIDE_W - 1.f, 0.f);
    border.setFillColor(sf::Color(240, 220, 180, 28));  // subtle warm gold hairline
    t.draw(border);

    drawPlayerStatus(t, 1, SIDE_BOB_Y,   SIDE_BOB_H);    // Bob (opponent) at top
    drawDayNightIndicator(t);
    drawDungeonProgress  (t);
    drawPhaseTracker(t, hints);
    float ty = SIDE_STACK_Y;
    drawStackSection(t, ty);
    // drawCommandZoneThumbnail moved out of the sidebar — now lives in the
    // right preview panel (see drawPreviewPanel).
    drawPromptSection(t, hints);
    drawLogSection(t, hints);
    drawPlayerStatus(t, 0, SIDE_ALICE_Y, SIDE_ALICE_H);  // Alice (self) at bottom
}

void BoardRenderer::drawStackSection(sf::RenderTarget& t, float& ty) const {
    // The real stack (AbilityProcessor) holds spells AND activated/triggered
    // abilities. Build a display list with the TOP of the stack first so that
    // activated abilities (fetch lands, pingers, etc.) are visible too — not
    // just cast spells.
    struct StackItem { const Card* src; mtg::ScriptLine script; bool isAbility; bool hasScript; };
    std::vector<StackItem> items;
    if (m_abilities) {
        const auto& sa = m_abilities->stackAbilities();
        for (auto it = sa.rbegin(); it != sa.rend(); ++it)        // back() == top
            items.push_back({ m_game->findCard(it->sourceCardId), it->script,
                              it->isActivatedAbility, true });
    } else {
        for (const Card* c : m_game->stack().cards())
            items.push_back({ c, {}, false, false });
    }
    size_t stackSz = items.size();

    // When the stack is non-empty, draw a pulsing gold glow around the whole
    // section so it's impossible to miss that something is waiting to resolve.
    if (stackSz > 0) {
        int shown = std::min(static_cast<int>(stackSz), 5);
        float sectionH = 22.f + shown * 32.f + 6.f;
        static sf::Clock s_stackPulse;
        float p = 0.5f + 0.5f * std::sin(s_stackPulse.getElapsedTime().asSeconds() * 4.f);
        sf::RectangleShape glow({SIDE_W, sectionH});
        glow.setPosition(SIDE_X, ty - 3.f);
        glow.setFillColor(sf::Color(70, 52, 18, static_cast<sf::Uint8>(28 + 36 * p)));
        glow.setOutlineColor(sf::Color(232, 190, 92, static_cast<sf::Uint8>(150 + 100 * p)));
        glow.setOutlineThickness(2.5f);
        t.draw(glow);
    }

    // ── Header + "Resolve All" button ─────────────────────────────────────────
    std::string hdr = "STACK";
    if (stackSz) hdr += "  (" + std::to_string(stackSz) + ")";
    // Storm count: show alongside stack size when any spell has been cast this turn
    if (m_game->spellsCastThisTurn > 0) {
        hdr += "  Storm:" + std::to_string(m_game->spellsCastThisTurn);
    }
    drawSectionHeader(t, hdr, SIDE_X, ty, SIDE_W, 22.f);

    if (stackSz > 0) {
        constexpr float kBtnW = 68.f, kBtnH = 14.f;
        sf::RectangleShape btn({kBtnW, kBtnH});
        btn.setPosition(SIDE_X + SIDE_W - kBtnW - 3.f, ty + 4.f);
        btn.setFillColor(sf::Color(30, 80, 60));
        btn.setOutlineColor(sf::Color(60, 160, 100));
        btn.setOutlineThickness(1.f);
        t.draw(btn);
        drawTxt(t, *m_font, "Resolve All",
                SIDE_X + SIDE_W - kBtnW - 1.f, ty + 5.f, 8,
                sf::Color(160, 230, 180), true);
    }
    ty += 22.f;

    // ── Compact stack items (up to 5 at 30 px each) ───────────────────────────
    constexpr int   kMaxShow = 5;
    constexpr float kItemH   = 30.f;
    constexpr float kItemGap =  2.f;

    // Short effect summary from a resolved script line.
    auto summaryOf = [](const mtg::ScriptLine& sl) -> std::string {
        const auto& et = sl.effectType;
        if      (et == "DealDamage" || et == "Damage")
            return "deal " + std::string(sl.get("NumDmg", "?")) + " dmg";
        else if (et == "Destroy")       return "destroy target";
        else if (et == "ChangeZone") {
            auto dest = std::string(sl.get("Destination", ""));
            if      (dest == "Exile")       return "exile / search";
            else if (dest == "Hand")        return "bounce / tutor";
            else if (dest == "Battlefield") return "put onto battlefield";
            else if (dest == "Library")     return "to library";
            else                            return "move card";
        }
        else if (et == "ChangeZoneAll") return "board wipe";
        else if (et == "Draw")          return "draw " + std::string(sl.get("NumCards", "?"));
        else if (et == "Counter")       return "counter spell";
        else if (et == "GainLife")      return "gain life";
        else if (et == "LoseLife")      return "lose life";
        else if (et == "PutCounter")    return "add counters";
        else if (et == "Mana")          return "add mana";
        else if (!et.empty())           return et;
        return "";
    };

    for (int i = 0; i < static_cast<int>(stackSz) && i < kMaxShow; ++i) {
        const StackItem& item = items[static_cast<size_t>(i)];
        const Card* c = item.src;
        float iy = ty + static_cast<float>(i) * (kItemH + kItemGap);

        uint8_t ci = (c && c->rules) ? c->rules->manaCost.colorIdentity() : 0;
        sf::Color bgCol = cardBackground(ci, false);

        sf::RectangleShape row({SIDE_W - 4.f, kItemH});
        row.setPosition(SIDE_X + 2.f, iy);
        row.setFillColor(sf::Color(bgCol.r / 5, bgCol.g / 5, bgCol.b / 5 + 8));
        row.setOutlineColor(item.isAbility ? sf::Color(120, 90, 160, 200)   // ability tint
                                           : sf::Color(bgCol.r / 3, bgCol.g / 3, bgCol.b / 3 + 12));
        row.setOutlineThickness(1.f);
        t.draw(row);

        sf::RectangleShape accent({3.f, kItemH});
        accent.setPosition(SIDE_X + 2.f, iy);
        accent.setFillColor(item.isAbility ? sf::Color(150, 110, 200, 200)
                                           : sf::Color(bgCol.r, bgCol.g, bgCol.b, 180));
        t.draw(accent);

        std::string nm = (c && c->rules) ? c->rules->name : std::string("Ability");
        if (item.isAbility) nm += " \xE2\x80\xA2 ability";   // " • ability"
        if (nm.size() > 21) nm = nm.substr(0, 20) + ".";
        drawTxt(t, *m_font, nm, SIDE_X + 7.f, iy + 2.f, 9,
                item.isAbility ? sf::Color(214, 196, 240) : sf::Color(230, 230, 215), true);

        // Mana cost (spells only) — pip-less fallback when no skin.
        if (!item.isAbility && c && c->rules && !SkinAssets::ready()) {
            std::string mc = c->rules->manaCost.toString();
            if (!mc.empty()) {
                sf::Text tmp2(mc, *m_font, 8);
                float mw = tmp2.getLocalBounds().width;
                drawTxt(t, *m_font, mc, SIDE_X + SIDE_W - mw - 7.f, iy + 3.f, 8,
                        sf::Color(200, 185, 120));
            }
        }

        // Effect summary: prefer the actual stack script; fall back to the card's
        // first SP/DB line, then its type.
        std::string effectSummary;
        if (item.hasScript) effectSummary = summaryOf(item.script);
        if (effectSummary.empty() && c && c->rules)
            for (const auto& raw : c->rules->abilityLines) {
                auto sl = parseScriptLine(raw);
                if (sl.abilityType != "SP" && sl.abilityType != "DB" && sl.abilityType != "AB")
                    continue;
                effectSummary = summaryOf(sl);
                if (!effectSummary.empty()) break;
            }
        std::string line2 = effectSummary;
        if (line2.empty() && c && c->rules) line2 = c->rules->type.toString();
        if (line2.size() > 22) line2 = line2.substr(0, 21) + ".";
        drawTxt(t, *m_font, line2, SIDE_X + 7.f, iy + 17.f, 7,
                effectSummary.empty() ? sf::Color(130, 145, 135) : sf::Color(180, 200, 165));

        if (i == 0)
            drawTxt(t, *m_font, "TOP", SIDE_X + SIDE_W - 22.f, iy + 17.f, 7,
                    sf::Color(255, 215, 50), true);
    }

    if (stackSz > static_cast<size_t>(kMaxShow)) {
        float moreY = ty + kMaxShow * (kItemH + kItemGap);
        drawTxt(t, *m_font,
                "... +" + std::to_string(stackSz - kMaxShow) + " more",
                SIDE_X + 6.f, moreY, 8, sf::Color(110, 125, 115));
    }

    if (stackSz == 0)
        drawTxt(t, *m_font, "(empty)", SIDE_X + 6.f, ty + 10.f, 9,
                sf::Color(65, 85, 75));

    ty += static_cast<float>(std::min((int)stackSz, kMaxShow)) * (kItemH + kItemGap) + 6.f;

    // GY + Exile below stack if space allows (within the stack section)
    float maxTy = SIDE_X + SIDE_STACK_H;   // bottom of stack section

    for (uint8_t pid = 0; pid < 2 && ty + GY_LABEL_H < SIDE_STACK_H; ++pid) {
        const auto& gy = m_game->player(pid).graveyard().cards();
        if (gy.empty()) continue;

        std::string gyLabel = (pid == 0 ? "Alice" : "Bob");
        gyLabel += " GY (" + std::to_string(gy.size()) + ")";
        drawTxt(t, *m_font, gyLabel, SIDE_X + 4.f, ty, 9, sf::Color(140, 140, 145));
        ty += GY_LABEL_H;

        int shown = std::min(static_cast<int>(gy.size()), GY_MAX_SHOWN);
        for (int i = 0; i < shown && ty + GY_CARD_H <= SIDE_STACK_H; ++i) {
            const Card* c = gy[i];
            sf::RectangleShape bg2({SIDE_W - 4.f, GY_CARD_H});
            bg2.setPosition(SIDE_X + 2.f, ty);
            bg2.setFillColor(sf::Color(40, 38, 48));
            bg2.setOutlineColor(sf::Color(65, 58, 75));
            bg2.setOutlineThickness(1.f);
            t.draw(bg2);

            std::string nm = c->rules->name;
            if (nm.size() > 16) nm = nm.substr(0, 15) + ".";
            drawTxt(t, *m_font, nm, SIDE_X + 5.f, ty + 2.f, 9,
                    sf::Color(190, 178, 200), true);

            if (c->rules->hasPT()) {
                std::string pt = c->rules->power + "/" + c->rules->toughness;
                drawTxt(t, *m_font, pt, SIDE_X + 5.f, ty + 16.f, 9,
                        sf::Color(200, 195, 210));
            }

            ty += GY_PITCH;
        }
        ty += 4.f;
    }

    // Separator line
    sf::RectangleShape sep({SIDE_W, 1.f});
    sep.setPosition(SIDE_X, SIDE_STACK_H);
    sep.setFillColor(sf::Color(40, 60, 80));
    t.draw(sep);
}

void BoardRenderer::drawPromptSection(sf::RenderTarget& t, const RenderHints& hints) const {
    float y0 = SIDE_PROMPT_Y;

    // Priority indicator: glowing pill showing whose priority it is
    {
        uint8_t prioId = m_game->priorityPlayerId();
        const Player& pp = m_game->player(prioId);
        std::string prioTxt = pp.name() + " has priority";
        // Alice = gold, Bob = ember
        sf::Color pillCol = (prioId == 0) ? sf::Color(44, 34, 14) : sf::Color(48, 22, 8);
        sf::Color txtCol  = (prioId == 0) ? sf::Color(230, 193, 112) : sf::Color(239, 140, 82);
        sf::RectangleShape pill({SIDE_W - 4.f, 16.f});
        pill.setPosition(SIDE_X + 2.f, y0);
        pill.setFillColor(pillCol);
        pill.setOutlineColor(prioId == 0 ? sf::Color(203, 163, 90, 100) : sf::Color(217, 116, 63, 100));
        pill.setOutlineThickness(1.f);
        t.draw(pill);
        sf::Text pt(prioTxt, *m_font, 8);
        pt.setStyle(sf::Text::Bold);
        pt.setFillColor(txtCol);
        auto pb = pt.getLocalBounds();
        pt.setPosition(SIDE_X + (SIDE_W - pb.width) * 0.5f - pb.left, y0 + 3.f);
        applyTextScale(pt);
        t.draw(pt);
        y0 += 18.f;
    }

    // Trigger reorder overlay: show queue when multiple triggers pending
    if (hints.pendingTriggerNames.size() > 1) {
        drawSectionHeader(t, "TRIGGER ORDER  (Up/Down to reorder)", SIDE_X, y0, SIDE_W, 22.f);
        y0 += 22.f;
        for (size_t ti = 0; ti < hints.pendingTriggerNames.size() && y0 < WIN_H - 80.f; ++ti) {
            sf::Color rowBg = (ti == 0) ? sf::Color(34, 44, 28) : sf::Color(26, 30, 22);
            sf::RectangleShape row({SIDE_W - 4.f, 18.f});
            row.setPosition(SIDE_X + 2.f, y0);
            row.setFillColor(rowBg);
            t.draw(row);
            std::string lbl = std::to_string(ti + 1) + ". " + hints.pendingTriggerNames[ti];
            if (lbl.size() > 22) lbl = lbl.substr(0, 20) + "..";
            sf::Color col = (ti == 0) ? sf::Color(180, 220, 140) : sf::Color(130, 150, 120);
            drawTxt(t, *m_font, lbl, SIDE_X + 5.f, y0 + 3.f, 9, col, ti == 0);
            y0 += 20.f;
        }
        return;  // don't show regular prompt section while reordering
    }

    drawSectionHeader(t, "PROMPT", SIDE_X, y0, SIDE_W, 22.f);
    y0 += 22.f;

    // Combat damage preview bar
    if (hints.combatDamagePreview >= 0 && m_game) {
        int oppLife = m_game->player(1).life();
        int dmg = hints.combatDamagePreview;
        bool lethal = dmg >= oppLife && oppLife > 0;
        sf::Color barCol = lethal ? sf::Color(220, 60, 60) : sf::Color(203, 163, 90);
        float barW = (SIDE_W - 8.f) * std::min(1.f, oppLife > 0 ? (float)dmg / oppLife : 1.f);
        sf::RectangleShape barBg({SIDE_W - 8.f, 8.f});
        barBg.setPosition(SIDE_X + 4.f, y0);
        barBg.setFillColor(sf::Color(30, 25, 20));
        t.draw(barBg);
        if (barW > 0.f) {
            sf::RectangleShape bar({barW, 8.f});
            bar.setPosition(SIDE_X + 4.f, y0);
            bar.setFillColor(barCol);
            t.draw(bar);
        }
        std::string dmgLabel = std::to_string(dmg) + " dmg" + (lethal ? " — LETHAL" : "");
        drawTxt(t, *m_font, dmgLabel, SIDE_X + 6.f, y0, 8,
                lethal ? sf::Color(255, 120, 100) : sf::Color(230, 193, 112), lethal);
        y0 += 12.f;
    }

    // Prompt text (word-wrapped)
    float tx = SIDE_X + 6.f;
    float ty = y0;
    float textW = SIDE_W - 12.f;

    std::string msg = hints.instruction.empty()
                      ? "Waiting..."
                      : hints.instruction;

    // Simple word-wrap at ~24 chars
    while (!msg.empty() && ty < SIDE_PROMPT_Y + SIDE_PROMPT_H - 80.f) {
        std::string line;
        int wrapAt = static_cast<int>(textW / 7.f);
        if (wrapAt < 8) wrapAt = 8;
        if (static_cast<int>(msg.size()) <= wrapAt) {
            line = msg; msg.clear();
        } else {
            size_t sp = msg.rfind(' ', static_cast<size_t>(wrapAt));
            if (sp == std::string::npos) sp = static_cast<size_t>(wrapAt);
            line = msg.substr(0, sp);
            msg  = msg.substr(sp + 1);
        }
        drawTxt(t, *m_font, line, tx, ty, 11, sf::Color(220, 220, 190));
        ty += 15.f;
    }

    // OK / Cancel buttons at the bottom of the prompt section
    float btnY = SIDE_PROMPT_Y + SIDE_PROMPT_H - 76.f;
    float btnW = SIDE_W - 10.f;

    drawButton(t, "Pass / End Phase",
               SIDE_X + 5.f, btnY,
               btnW, 34.f,
               sf::Color(45, 75, 55),
               sf::Color(200, 230, 200));

    drawButton(t, "Confirm [Enter]",
               SIDE_X + 5.f, btnY + 38.f,
               btnW, 34.f,
               sf::Color(50, 65, 95),
               sf::Color(200, 215, 240));

    // Separator
    sf::RectangleShape sep({SIDE_W, 1.f});
    sep.setPosition(SIDE_X, SIDE_PROMPT_Y + SIDE_PROMPT_H);
    sep.setFillColor(sf::Color(40, 60, 80));
    t.draw(sep);
}

void BoardRenderer::drawLogSection(sf::RenderTarget& t, const RenderHints& hints) const {
    float y0 = SIDE_LOG_Y;

    // Header with filter indicator
    std::string hdr = "LOG";
    if (!hints.logFilter.empty()) hdr += "  [/" + hints.logFilter + "]";
    drawSectionHeader(t, hdr, SIDE_X, y0, SIDE_W, 20.f);
    y0 += 20.f;

    float ty = y0;
    constexpr float kLineH = 15.f;

    // Apply filter
    std::vector<const std::string*> visible;
    for (const auto& line : hints.logLines) {
        if (!hints.logFilter.empty()) {
            std::string lower = line;
            std::string filterLower = hints.logFilter;
            for (char& c : lower)       c = static_cast<char>(std::tolower((unsigned char)c));
            for (char& c : filterLower) c = static_cast<char>(std::tolower((unsigned char)c));
            if (lower.find(filterLower) == std::string::npos) continue;
        }
        visible.push_back(&line);
    }

    if (visible.empty()) {
        drawTxt(t, *m_font, hints.logFilter.empty() ? "(no events)" : "(no matches)",
                SIDE_X + 5.f, ty, 9, sf::Color(70, 85, 80));
        return;
    }

    // Draw newest entries last (most recent near the bottom). Cap the bottom
    // at the top of Alice's player chip (SIDE_ALICE_Y) so log entries don't
    // get covered by — or run into — the life-total panel beneath the log.
    const float logBottom = SIDE_ALICE_Y;
    int maxLines = static_cast<int>((logBottom - y0) / kLineH);
    int start = std::max(0, static_cast<int>(visible.size()) - maxLines);

    for (int i = start; i < static_cast<int>(visible.size())
                        && ty + kLineH <= logBottom; ++i) {
        const auto& line = *visible[i];
        bool isTurnLine  = line.find("Turn ") == 0;
        bool isWin       = line.find("wins") != std::string::npos;

        // Truncate if too wide
        std::string display = line;
        {
            sf::Text tmp(display, *m_font, 9);
            while (tmp.getLocalBounds().width > SIDE_W - 8.f && display.size() > 2) {
                display = display.substr(0, display.size() - 3) + "..";
                tmp.setString(display);
            }
        }
        sf::Color lineCol = isWin       ? sf::Color(255, 220, 50)
                          : isTurnLine  ? sf::Color(140, 200, 160)
                                        : sf::Color(150, 155, 148);
        drawTxt(t, *m_font, display, SIDE_X + 4.f, ty, 9, lineCol,
                isTurnLine || isWin);
        ty += kLineH;
    }

    // Log card-name hover popup: show oracle text snippet for a named card
    if (!hints.logHoverCardName.empty()) {
        const CardRules* hovRules = m_game ? m_game->findRules(hints.logHoverCardName) : nullptr;
        if (hovRules) {
            float popW = SIDE_W - 4.f, popH = 50.f;
            float popX = SIDE_X + 2.f, popY = WIN_H - popH - 4.f;
            sf::RectangleShape pop({popW, popH});
            pop.setPosition(popX, popY);
            pop.setFillColor(sf::Color(18, 17, 16, 245));
            pop.setOutlineColor(sf::Color(203, 163, 90, 120));
            pop.setOutlineThickness(1.f);
            t.draw(pop);
            drawTxt(t, *m_font, hovRules->name, popX + 4.f, popY + 2.f, 9,
                    sf::Color(230, 193, 112), true);
            std::string oracle = hovRules->oracleText;
            if (oracle.size() > 80) oracle = oracle.substr(0, 78) + "...";
            drawTxt(t, *m_font, oracle, popX + 4.f, popY + 14.f, 8,
                    sf::Color(170, 165, 155));
            if (hovRules->hasPT()) {
                drawTxt(t, *m_font,
                        hovRules->power + "/" + hovRules->toughness,
                        popX + 4.f, popY + popH - 12.f, 9, sf::Color(200, 200, 200), true);
            }
        }
    }
}

// ── Preview panel (card image + detail) ───────────────────────────────────────

void BoardRenderer::drawPreviewPanel(sf::RenderTarget& t, const RenderHints& hints) const {
    // Background
    sf::RectangleShape bg({PREV_W, WIN_H});
    bg.setPosition(PREV_X, 0.f);
    bg.setFillColor(sf::Color(14, 20, 28));
    t.draw(bg);

    // Left border
    sf::RectangleShape border({1.5f, WIN_H});
    border.setPosition(PREV_X, 0.f);
    border.setFillColor(sf::Color(45, 70, 60));
    t.draw(border);

    const mtg::Card* c = nullptr;
    if (hints.previewCardId != mtg::kInvalidId)
        c = m_game->findCard(hints.previewCardId);

    // Card comparison: show both cards side by side when Alt is held
    if (hints.compareCardId != mtg::kInvalidId && c) {
        const mtg::Card* cmp = m_game->findCard(hints.compareCardId);
        if (cmp && cmp != c) {
            // Draw both cards stacked vertically with a divider
            constexpr float halfH = WIN_H * 0.5f - 2.f;
            drawCardLarge(t, *m_font, c,   PREV_X, 0.f,      PREV_W, halfH, m_picsDir);
            sf::RectangleShape div({PREV_W, 2.f});
            div.setPosition(PREV_X, halfH);
            div.setFillColor(sf::Color(240, 220, 180, 60));
            t.draw(div);
            drawCardLarge(t, *m_font, cmp, PREV_X, halfH+2.f, PREV_W, halfH, m_picsDir);
            // Comparison header
            drawTxt(t, *m_font, "ALT: comparing", PREV_X + 4.f, 0.f, 7, sf::Color(100,95,85));
            return;
        }
    }

    if (!c) {
        sf::Text tmp("Hover a card", *m_font, 10);
        float pw = tmp.getLocalBounds().width;
        // Drop down a touch so it sits between the two commander thumbnails.
        drawTxt(t, *m_font, "Hover a card",
                PREV_X + (PREV_W - pw) * 0.5f, 300.f,
                10, sf::Color(55, 75, 70));
        drawCommandZoneThumbnail(t);
        return;
    }

    // Color-coded accent bar at top of panel
    {
        uint8_t ci = c->rules->manaCost.colorIdentity();
        sf::Color accent;
        int cbits = 0; for (int b = ci; b; b &= b-1) ++cbits;
        if      (cbits > 1)    accent = sf::Color(200, 175, 30);  // gold
        else if (ci & 0x01)                  accent = sf::Color(248, 245, 220); // W
        else if (ci & 0x02)                  accent = sf::Color(40,  130, 220); // U
        else if (ci & 0x04)                  accent = sf::Color(30,  25,  30);  // B
        else if (ci & 0x08)                  accent = sf::Color(210, 50,  30);  // R
        else if (ci & 0x10)                  accent = sf::Color(50,  155, 50);  // G
        else if (c->rules->type.isLand())    accent = sf::Color(100, 80,  60);  // land
        else                                 accent = sf::Color(160, 165, 175); // colorless
        sf::RectangleShape bar({PREV_W, 4.f});
        bar.setPosition(PREV_X, 0.f);
        bar.setFillColor(accent);
        t.draw(bar);
    }

    // Large card image — shifted down so the opponent commander thumbnail at
    // the top of this panel stays visible.
    float cardX = PREV_X + (PREV_W - PREV_CARD_W) * 0.5f;
    float cardY = 140.f;
    drawCardLarge(t, *m_font, c, cardX, cardY, PREV_CARD_W, PREV_CARD_H, m_picsDir);

    // Status badges below the image
    {
        float bx = PREV_X + 6.f;
        float by = cardY + PREV_CARD_H + 4.f;
        auto badge = [&](const char* lbl, sf::Color fill) {
            sf::Text bt(lbl, *m_font, 9);
            float bw = bt.getLocalBounds().width + 8.f;
            sf::RectangleShape bg({bw, 14.f});
            bg.setPosition(bx, by);
            bg.setFillColor(fill);
            bg.setOutlineColor(sf::Color(0, 0, 0, 120));
            bg.setOutlineThickness(1.f);
            t.draw(bg);
            drawTxt(t, *m_font, lbl, bx + 4.f, by + 1.f, 9, sf::Color(235, 235, 215));
            bx += bw + 4.f;
        };
        if (c->tapped)                badge("TAPPED",    sf::Color(90,  70,  30));
        if (hints.attackers.count(c->id)) badge("ATTACKING", sf::Color(160, 50,  30));
        if (hints.blockers.count(c->id))  badge("BLOCKING",  sf::Color(30,  80,  140));
        if (c->suspended)             badge("SUSPENDED", sf::Color(60,  40,  100));
        // Counters summary
        for (const auto& [key, val] : c->counters) {
            if (val <= 0) continue;
            std::string lbl2 = std::to_string(val) + " " + key;
            sf::Text tmp2(lbl2, *m_font, 9);
            float bw = tmp2.getLocalBounds().width + 8.f;
            sf::RectangleShape bg2({bw, 14.f});
            bg2.setPosition(bx, by);
            bg2.setFillColor(sf::Color(40, 60, 80));
            bg2.setOutlineColor(sf::Color(0, 0, 0, 120));
            bg2.setOutlineThickness(1.f);
            t.draw(bg2);
            drawTxt(t, *m_font, lbl2, bx + 4.f, by + 1.f, 9, sf::Color(180, 215, 240));
            bx += bw + 4.f;
        }
    }

    // Card text below image
    const auto& r = *c->rules;
    float ty = cardY + PREV_CARD_H + 22.f;
    float tx = PREV_X + 8.f;
    float textW = PREV_W - 16.f;
    // Stop text ABOVE the bottom commander thumbnail (≈682px) so long oracle
    // text doesn't flow into / overlap it and become unreadable.
    const float textBottom = commanderThumbRect(0).top - 6.f;

    auto drawLine = [&](const std::string& s, unsigned sz,
                        sf::Color col, bool bold = false) {
        std::string text = s;
        float charW = static_cast<float>(sz) * 0.58f;
        int wrapAt  = std::max(8, static_cast<int>(textW / charW));
        while (!text.empty() && ty < textBottom) {
            std::string line;
            if (static_cast<int>(text.size()) <= wrapAt) { line = text; text.clear(); }
            else {
                size_t sp = text.rfind(' ', static_cast<size_t>(wrapAt));
                if (sp == std::string::npos) sp = static_cast<size_t>(wrapAt);
                line = text.substr(0, sp);
                text = text.substr(sp + 1);
            }
            drawTxt(t, *m_font, line, tx, ty, sz, col, bold);
            ty += static_cast<float>(sz) + 3.f;
        }
    };

    drawLine(r.name, 13, sf::Color(235, 240, 215), true);
    ty += 2.f;

    // Mana cost symbols
    if (!r.manaCost.toString().empty() && SkinAssets::ready()) {
        SkinAssets::drawManaCost(t, r.manaCost.toString(), tx, ty, 14.f);
        ty += 18.f;
    }

    // Type line
    drawLine(r.type.toString(), 10, sf::Color(160, 175, 160));

    if (r.hasPT()) {
        int pw = mtg::effectivePower(*c);
        int tg = mtg::effectiveToughness(*c);
        int pp = c->counterCount("+1/+1"), mm = c->counterCount("-1/-1");
        std::string pt = std::to_string(pw) + "/" + std::to_string(tg);
        if (pp) pt += "  (+" + std::to_string(pp) + ")";
        if (mm) pt += "  (-" + std::to_string(mm) + ")";
        drawLine(pt, 12, sf::Color(210, 215, 205), true);
    }

    if (r.type.isPlaneswalker()) {
        int loyalty = c->counterCount("loyalty");
        drawLine("Loyalty: " + std::to_string(loyalty), 11,
                 sf::Color(130, 155, 220), true);
    }

    ty += 4.f;
    if (!r.oracleText.empty()) {
        // Render oracle text with inline mana symbols where possible.
        // Each line is scanned for {X} tokens; tokens are drawn as sprites
        // from sprite_manaicons.png; plain text segments use drawTxt.
        constexpr unsigned kOTSize = 10u;
        constexpr float kSymSz = 11.f;
        const sf::Color kOTCol(145, 153, 138);
        const float charW = kOTSize * 0.58f;
        int wrapAt = std::max(8, static_cast<int>(textW / charW));

        std::string remaining = r.oracleText;
        while (!remaining.empty() && ty < textBottom) {
            // Last line that fits but text remains → show an ellipsis instead of
            // letting it run into the bottom commander thumbnail.
            if (ty + static_cast<float>(kOTSize) + 3.f >= textBottom) {
                drawTxt(t, *m_font, "...", tx, ty, kOTSize, kOTCol);
                break;
            }
            // Extract one line (word-wrap)
            std::string line;
            if (static_cast<int>(remaining.size()) <= wrapAt) {
                line = remaining; remaining.clear();
            } else {
                size_t sp = remaining.rfind(' ', static_cast<size_t>(wrapAt));
                if (sp == std::string::npos) sp = static_cast<size_t>(wrapAt);
                line = remaining.substr(0, sp);
                remaining = remaining.substr(sp + 1);
            }

            // Render line segment by segment: split on {X} tokens
            float lx = tx;
            size_t pos = 0;
            while (pos < line.size()) {
                auto lb = line.find('{', pos);
                if (lb == std::string::npos) {
                    // Remaining plain text
                    if (pos < line.size())
                        lx += drawTxt(t, *m_font, line.substr(pos), lx, ty, kOTSize, kOTCol) + 1.f;
                    break;
                }
                // Draw plain text before the token
                if (lb > pos)
                    lx += drawTxt(t, *m_font, line.substr(pos, lb - pos), lx, ty, kOTSize, kOTCol) + 1.f;
                auto rb = line.find('}', lb + 1);
                if (rb == std::string::npos) { lb = pos; break; }
                std::string token = line.substr(lb + 1, rb - lb - 1);
                if (SkinAssets::ready()) {
                    SkinAssets::drawManaToken(t, token, lx, ty, kSymSz);
                    lx += kSymSz + 1.f;
                } else {
                    lx += drawTxt(t, *m_font, "{" + token + "}", lx, ty, kOTSize,
                                  sf::Color(200, 185, 110)) + 1.f;
                }
                pos = rb + 1;
            }
            ty += static_cast<float>(kOTSize) + 3.f;
        }
    }

    // Collection count
    if (hints.cardCollection) {
        auto it = hints.cardCollection->find(r.name);
        if (it != hints.cardCollection->end() && it->second > 0) {
            ty += 6.f;
            drawLine("Seen " + std::to_string(it->second) + "x in games", 9,
                     sf::Color(90, 110, 90));
        }
    }

    // Commander thumbnails always stay visible on top of the preview, so the
    // player can switch between previews by hovering the other commander.
    drawCommandZoneThumbnail(t);
}

// ── Tooltip (card hover) ──────────────────────────────────────────────────────

void BoardRenderer::drawTooltip(sf::RenderTarget& t, const RenderHints& hints) const {
    if (!m_font || !m_game || hints.mousePos.x < 0) return;

    auto hit = hitTest(hints.mousePos.x, hints.mousePos.y);
    if (hit.id == kInvalidId) return;
    const Card* c = m_game->findCard(hit.id);
    if (!c || c->rules == nullptr) return;
    const CardRules& r = *c->rules;

    std::vector<std::string> lines;
    lines.push_back(r.name);
    {
        std::string l2 = r.manaCost.toString();
        if (!l2.empty()) l2 += "  ";
        l2 += r.type.toString();
        if (r.hasPT()) l2 += "  " + r.power + "/" + r.toughness;
        lines.push_back(l2);
    }
    if (!r.oracleText.empty()) {
        std::string text = r.oracleText;
        while (!text.empty()) {
            size_t sp = text.rfind(' ', 40);
            if (sp == std::string::npos || text.size() <= 40) { lines.push_back(text); break; }
            lines.push_back(text.substr(0, sp));
            text = text.substr(sp + 1);
        }
    }

    // Activated abilities — show cost and short description
    if (!r.abilityLines.empty()) {
        bool addedHeader = false;
        for (const auto& raw : r.abilityLines) {
            auto s = parseScriptLine(raw);
            if (s.abilityType != "AB") continue;
            auto cost = s.get("Cost", "");
            auto desc = s.get("SpellDescription", s.get("Description", ""));
            if (cost.empty()) continue;
            if (!addedHeader) {
                lines.push_back("─ Abilities ─");
                addedHeader = true;
            }
            std::string line = "[" + std::string(cost) + "]";
            if (!desc.empty()) {
                std::string ds(desc);
                if (ds.size() > 26) ds = ds.substr(0, 25) + "…";
                line += " " + ds;
            }
            lines.push_back(line);
        }
    }

    const float pad = 7.f, lineH = 16.f, tw = 280.f;
    float th = pad * 2.f + static_cast<float>(lines.size()) * lineH;

    float tx = hints.mousePos.x + 14.f;
    float ty = hints.mousePos.y - th * 0.4f;
    if (tx + tw > PREV_X - 4.f) tx = hints.mousePos.x - tw - 14.f;
    if (ty < 4.f) ty = 4.f;
    if (ty + th > WIN_H - 4.f) ty = WIN_H - th - 4.f;

    sf::RectangleShape bg({tw, th});
    bg.setPosition(tx, ty);
    bg.setFillColor(sf::Color(10, 15, 20, 240));
    bg.setOutlineColor(sf::Color(80, 120, 100));
    bg.setOutlineThickness(1.5f);
    t.draw(bg);

    for (size_t i = 0; i < lines.size(); ++i) {
        drawTxt(t, *m_font, lines[i], tx + pad,
                ty + pad + static_cast<float>(i) * lineH - 1.f,
                (i == 0) ? 12u : 10u,
                i == 0 ? sf::Color(230, 235, 210) : sf::Color(170, 178, 162),
                i == 0);
    }

    // UI contextual tooltip (separate from card hover)
    if (!hints.uiTooltip.empty()) {
        constexpr float tipPad = 6.f;
        float tipX = std::min(hints.uiTooltipPos.x, WIN_W - 220.f);
        float tipY = std::max(4.f, hints.uiTooltipPos.y);
        // Split on newline
        std::vector<std::string> tipLines;
        std::string rem = hints.uiTooltip;
        while (!rem.empty()) {
            auto nl = rem.find('\n');
            tipLines.push_back(nl == std::string::npos ? rem : rem.substr(0, nl));
            if (nl == std::string::npos) break;
            rem = rem.substr(nl + 1);
        }
        float tipH = tipLines.size() * 13.f + tipPad * 2.f;
        sf::RectangleShape tipBg({200.f, tipH});
        tipBg.setPosition(tipX, tipY);
        tipBg.setFillColor(sf::Color(10, 9, 8, 230));
        tipBg.setOutlineColor(sf::Color(240, 220, 180, 40));
        tipBg.setOutlineThickness(1.f);
        t.draw(tipBg);
        for (size_t i = 0; i < tipLines.size(); ++i)
            drawTxt(t, *m_font, tipLines[i], tipX + tipPad,
                    tipY + tipPad + i * 13.f, 9, sf::Color(199, 189, 172));
    }
}

// ── Combat arrows ─────────────────────────────────────────────────────────────

namespace {
void drawArrow(sf::RenderTarget& t,
               sf::Vector2f from, sf::Vector2f to,
               sf::Color col) {
    sf::Vector2f d = to - from;
    float len = std::hypot(d.x, d.y);
    if (len < 4.f) return;
    d /= len;
    sf::Vector2f perp{-d.y, d.x};

    const float shaftW  =  4.f;
    const float headW   = 14.f;
    const float headLen = 18.f;

    sf::Vector2f base = to - d * headLen;

    sf::VertexArray shaft(sf::TrianglesStrip, 4);
    shaft[0].position = from + perp * (shaftW * 0.5f);
    shaft[1].position = from - perp * (shaftW * 0.5f);
    shaft[2].position = base + perp * (shaftW * 0.5f);
    shaft[3].position = base - perp * (shaftW * 0.5f);
    for (int i = 0; i < 4; ++i) shaft[i].color = col;
    t.draw(shaft);

    sf::VertexArray head(sf::Triangles, 3);
    head[0].position = to;
    head[1].position = base + perp * (headW * 0.5f);
    head[2].position = base - perp * (headW * 0.5f);
    for (int i = 0; i < 3; ++i) head[i].color = col;
    t.draw(head);
}
} // namespace

void BoardRenderer::drawCombatArrows(sf::RenderTarget& t,
                                      const RenderHints&) const {
    if (!m_tm || m_tm->combatState().empty()) return;

    static const sf::Color kAttackCol(230, 60, 40, 200);
    static const sf::Color kBlockCol (60, 180, 240, 160);

    for (const auto& atk : m_tm->combatState().attacks) {
        sf::Vector2f from = screenCenterOf(atk.attackerId);
        if (from.x < 1.f && from.y < 1.f) continue;

        // Target: centre of the defending player's info bar
        float targX = PLAY_W * 0.5f;
        float targY = (atk.defendingPlayerId == 1)
                        ? BOB_INFO_Y   + BOB_INFO_H   * 0.5f
                        : ALICE_INFO_Y + ALICE_INFO_H * 0.5f;
        drawArrow(t, from, {targX, targY}, kAttackCol);

        // Blocker arrows (blocker → attacker)
        for (ObjectId bid : atk.blockerIds) {
            sf::Vector2f bpos = screenCenterOf(bid);
            if (bpos.x < 1.f && bpos.y < 1.f) continue;
            drawArrow(t, bpos, from, kBlockCol);
        }
    }
}

// ── Library search overlay ────────────────────────────────────────────────────

namespace {
// Shared geometry for the library-search panel so draw and hit-test agree.
struct SearchPanelGeom {
    float pw, ph, px, py;
    float itemStartY, itemH, itemW, itemX;
};
SearchPanelGeom searchGeom(int count) {
    SearchPanelGeom g;
    g.pw         = 460.f;
    g.itemH      = 26.f;
    g.itemW      = g.pw - 20.f;
    int visible  = std::min(count, 18);
    g.ph         = 36.f + visible * g.itemH + 8.f;
    g.px         = (WIN_W - g.pw) * 0.5f;
    g.py         = (WIN_H - g.ph) * 0.5f;
    g.itemX      = g.px + 10.f;
    g.itemStartY = g.py + 34.f;
    return g;
}
} // namespace

void BoardRenderer::drawLibrarySearchOverlay(sf::RenderTarget& t,
                                              const RenderHints& hints) const {
    if (!hints.showLibrarySearch || hints.searchChoices.empty() || !m_font) return;

    // Dim whole screen
    sf::RectangleShape dim({WIN_W, WIN_H});
    dim.setFillColor(sf::Color(0, 0, 0, 160));
    t.draw(dim);

    auto g = searchGeom(static_cast<int>(hints.searchChoices.size()));

    sf::RectangleShape panel({g.pw, g.ph});
    panel.setPosition(g.px, g.py);
    panel.setFillColor(sf::Color(18, 26, 38, 245));
    panel.setOutlineColor(sf::Color(70, 110, 150));
    panel.setOutlineThickness(2.f);
    t.draw(panel);

    // Title
    const std::string title = hints.searchInstruction.empty()
                              ? "Choose a card from your library"
                              : hints.searchInstruction;
    drawTxt(t, *m_font, title, g.px + 10.f, g.py + 8.f, 13, sf::Color(160, 200, 240), true);

    float iy = g.itemStartY;
    int shown = 0;
    for (ObjectId id : hints.searchChoices) {
        if (shown >= 18) break;
        const Card* c = m_game->findCard(id);
        if (!c || !c->rules) { ++shown; iy += g.itemH; continue; }

        // Hover highlight
        bool hover = (hints.mousePos.x >= g.itemX &&
                      hints.mousePos.x <= g.itemX + g.itemW &&
                      hints.mousePos.y >= iy &&
                      hints.mousePos.y <= iy + g.itemH - 2.f);

        sf::RectangleShape row({g.itemW, g.itemH - 2.f});
        row.setPosition(g.itemX, iy);
        row.setFillColor(hover ? sf::Color(50, 80, 120, 230)
                               : sf::Color(30, 44, 62, 215));
        row.setOutlineColor(sf::Color(55, 85, 115, 180));
        row.setOutlineThickness(1.f);
        t.draw(row);

        drawTxt(t, *m_font, c->rules->name, g.itemX + 6.f, iy + 5.f,
                12, sf::Color(210, 215, 220));

        // Mana cost on the right
        if (!c->rules->manaCost.isNoCost()) {
            std::string mc = c->rules->manaCost.toString();
            sf::Text tmp(mc, *m_font, 11);
            float cx = g.itemX + g.itemW - tmp.getLocalBounds().width - 8.f;
            drawTxt(t, *m_font, mc, cx, iy + 6.f, 11, sf::Color(180, 170, 130));
        }

        iy += g.itemH;
        ++shown;
    }
    if ((int)hints.searchChoices.size() > 18) {
        drawTxt(t, *m_font,
                "... and " + std::to_string((int)hints.searchChoices.size() - 18) + " more",
                g.itemX + 4.f, iy + 2.f, 11, sf::Color(140, 140, 140));
    }
}

ObjectId BoardRenderer::hitSearchChoice(float px, float py,
                                         const RenderHints& hints) const {
    if (!hints.showLibrarySearch || hints.searchChoices.empty()) return kInvalidId;
    auto g = searchGeom(static_cast<int>(hints.searchChoices.size()));
    float iy = g.itemStartY;
    int shown = 0;
    for (ObjectId id : hints.searchChoices) {
        if (shown >= 18) break;
        if (px >= g.itemX && px <= g.itemX + g.itemW &&
            py >= iy       && py <= iy + g.itemH - 2.f)
            return id;
        iy += g.itemH;
        ++shown;
    }
    return kInvalidId;
}

// ── Main draw ─────────────────────────────────────────────────────────────────

void BoardRenderer::draw(sf::RenderTarget& t, const RenderHints& hints) const {
    // Full-window background (bg_match.jpg fills everything behind all panels)
    drawBackground(t);

    // Left sidebar (stack / prompt / log) — drawn directly at x = SIDE_X = 0
    drawSidebar     (t, hints);

    // Right preview panel — drawn directly at x = PREV_X
    drawPreviewPanel(t, hints);

    // Centre play area — rendered directly into the window with a shifted view so
    // all draw functions can keep using (0..PLAY_W) local coordinates.  This avoids
    // the intermediate RenderTexture that caused bilinear blur when the window was
    // resized or maximised.
    {
        sf::View savedView = t.getView();
        // Build a view that maps local x=0 → screen x=PLAY_X.
        // The full logical coordinate space (WIN_W×WIN_H) is visible; we just
        // shift the left edge of the world to -PLAY_X so the play area aligns.
        sf::View playView;
        playView.reset(sf::FloatRect(-PLAY_X, 0.f, WIN_W, WIN_H));
        playView.setViewport(savedView.getViewport());
        t.setView(playView);

        if (m_game->numPlayers() >= 4) {
            // 4-player layout: split the play area into quadrants.
            // Top row: player 1 (left) and player 2 (right); bottom: player 0 (left) and player 3 (right).
            // Each quadrant gets a scaled sub-view. This is a simplified 2x2 grid —
            // full interactive support still targets players 0/1; 2/3 are display-only.
            struct { float vx, vy, vw, vh; uint8_t pid; } quads[4] = {
                {0.5f, 0.f, 0.5f, 0.5f, 2},   // top-right   = player 2
                {0.f,  0.f, 0.5f, 0.5f, 1},   // top-left    = player 1 (opponent)
                {0.f,  0.5f,0.5f, 0.5f, 0},   // bottom-left = player 0 (you)
                {0.5f, 0.5f,0.5f, 0.5f, 3},   // bottom-right= player 3
            };
            auto vp = savedView.getViewport();
            for (const auto& q : quads) {
                sf::View qView;
                qView.reset(sf::FloatRect(-PLAY_X, 0.f, WIN_W, WIN_H));
                qView.setViewport({
                    vp.left + q.vx * vp.width,
                    vp.top  + q.vy * vp.height,
                    q.vw * vp.width,
                    q.vh * vp.height
                });
                t.setView(qView);
                drawBackground  (t);
                drawInfoBar     (t, q.pid);
                drawBattlefield (t, q.pid, hints);
                if (q.pid == 0) {
                    drawHand    (t, 0, hints);
                    drawDock    (t, hints);
                }
            }
            t.setView(savedView);
        } else {
            drawInfoBar      (t, 1);
            drawHand         (t, 1, hints);
            drawBattlefield  (t, 1, hints);
            drawBattlefield  (t, 0, hints);
            drawHand         (t, 0, hints);
            drawInfoBar      (t, 0);
            drawDock         (t, hints);
        }
        drawCombatArrows (t, hints);
        drawAnims        (t);

        t.setView(savedView);
    }

    // Tooltip on top of everything (uses window coordinates)
    drawTooltip(t, hints);

    // Library search modal overlay (drawn last so it covers the board)
    drawLibrarySearchOverlay(t, hints);

    // Zone (GY / Exile) browser overlay — drawn on top of everything
    if (hints.showZoneBrowse)
        drawZoneBrowserOverlay(t, hints);
}

// ── Hit testing ───────────────────────────────────────────────────────────────

mtg::ObjectId BoardRenderer::cardAt(float px, float py,
                                     uint8_t pid, ZoneType zone) const {
    float zx, zy, zh;
    zoneRect(pid, zone, zx, zy, zh);

    if (zone == ZoneType::Hand) {
        const auto& cards = m_game->player(pid).hand().cardsRaw();
        int idx = 0;
        for (const Card* c : cards) {
            auto pos = cardPos(idx++, zx, zy, zh);
            if (px >= pos.x && px <= pos.x + CARD_W &&
                py >= pos.y && py <= pos.y + CARD_H)
                return c->id;
        }
        return kInvalidId;
    }

    if (zone != ZoneType::Battlefield) return kInvalidId;

    // Battlefield: mirror the grouped 2-row layout in drawBattlefield() so the
    // hit-test agrees with what the player sees. Same category order, same
    // one-slot gap between groups, same top/bottom split.
    std::vector<const Card*> lands, creatures, pws, artifacts, enchants;
    for (const Card* c : m_game->battlefield().cards()) {
        if (c->controllerId != pid) continue;
        if (c->attachedTo != mtg::kInvalidId) continue;
        if      (c->rules->type.isLand())          lands.push_back(c);
        else if (c->rules->type.isCreature())      creatures.push_back(c);
        else if (c->rules->type.isPlaneswalker())  pws.push_back(c);
        else if (c->rules->type.isArtifact())      artifacts.push_back(c);
        else                                       enchants.push_back(c);
    }
    bool hasNonLand = !creatures.empty() || !pws.empty() ||
                      !artifacts.empty() || !enchants.empty();
    bool twoRows = !lands.empty() && hasNonLand;
    float rowH    = twoRows ? zh * 0.5f : zh;
    float topY    = zy;
    float botY    = twoRows ? zy + rowH : zy;
    float landRowH = twoRows ? rowH : zh;

    // Top row with group gaps
    const std::vector<const Card*>* topGroups[] = { &creatures, &pws, &artifacts, &enchants };
    int slot = 0;
    bool firstGroup = true;
    for (const auto* g : topGroups) {
        if (g->empty()) continue;
        if (!firstGroup) ++slot;
        for (const Card* c : *g) {
            auto pos = cardPos(slot++, zx, topY, rowH);
            if (px >= pos.x && px <= pos.x + CARD_W &&
                py >= pos.y && py <= pos.y + CARD_H)
                return c->id;
        }
        firstGroup = false;
    }
    // Bottom row (lands)
    int li = 0;
    for (const Card* c : lands) {
        auto pos = cardPos(li++, zx, botY, landRowH);
        if (px >= pos.x && px <= pos.x + CARD_W &&
            py >= pos.y && py <= pos.y + CARD_H)
            return c->id;
    }
    return kInvalidId;
}

BoardRenderer::HitResult BoardRenderer::hitTest(float px, float py) const {
    if (!m_game) return {};

    // Right preview panel: commander thumbnails. Hovering one "expands" it
    // into the main preview area (the existing preview logic handles the
    // full-size render once previewCardId is set).
    if (const mtg::Card* cmd = commanderUnderCursor(px, py))
        return { cmd->id, mtg::ZoneType::Command, cmd->ownerId };

    // Left sidebar: stack + GY (x = 0..SIDE_W)
    if (px >= SIDE_X && px < SIDE_X + SIDE_W) {
        const auto& stackCards = m_game->stack().cards();
        for (int i = 0; i < static_cast<int>(stackCards.size()) && i < STACK_MAX_SHOW; ++i) {
            auto pos = stackItemPos(i);
            if (py >= pos.y && py <= pos.y + STACK_ITEM_H)
                return { stackCards[i]->id, ZoneType::Stack, stackCards[i]->controllerId };
        }
        return {};
    }

    // Outside play area (e.g. right preview panel): no card hit
    if (px < PLAY_X || px >= PLAY_X + PLAY_W) return {};

    // Translate to play-area-local coordinates before testing
    float lx = px - PLAY_X;

    // Alice
    for (auto zone : {ZoneType::Hand, ZoneType::Battlefield}) {
        ObjectId id = cardAt(lx, py, 0, zone);
        if (id != kInvalidId) return {id, zone, 0};
    }
    // Bob's battlefield (targeting)
    ObjectId id = cardAt(lx, py, 1, ZoneType::Battlefield);
    if (id != kInvalidId) return {id, ZoneType::Battlefield, 1};

    return {};
}

BoardRenderer::DockBtn BoardRenderer::hitDock(float px, float py) const {
    // Dock removed — concede/undo moved to ESC pause menu, the rest are
    // keyboard-only now. Hit-test always misses.
    (void)px; (void)py;
    return DockBtn::None;
    // Unreachable but kept so the old layout constants don't go stale.
    if (py < DOCK_Y || py > DOCK_Y + DOCK_H)           return DockBtn::None;
    if (px < PLAY_X || px >= PLAY_X + PLAY_W)          return DockBtn::None;
    int btn = static_cast<int>((px - PLAY_X) / DOCK_BTN_W);
    switch (btn) {
        case 0: return DockBtn::EndPhase;
        case 1: return DockBtn::PassPriority;
        case 2: return DockBtn::AlphaStrike;
        case 3: return DockBtn::Concede;
        default: return DockBtn::None;
    }
}

BoardRenderer::PromptBtn BoardRenderer::hitPrompt(float px, float py) const {
    if (px < SIDE_X || px > SIDE_X + SIDE_W) return PromptBtn::None;
    float btnY = SIDE_PROMPT_Y + SIDE_PROMPT_H - 76.f;
    float btnW = SIDE_W - 10.f;
    float btnX = SIDE_X + 5.f;

    // "Pass / End Phase" (top button) → pass. NOTE: callers treat PromptBtn::Ok
    // as "confirm" (onConfirm) and PromptBtn::Cancel as "pass", so the top Pass
    // button must return Cancel and the bottom Confirm button must return Ok.
    // (These were previously swapped, so clicking "Confirm" to declare attackers
    // actually passed and skipped combat — making attacks impossible by mouse.)
    if (py >= btnY && py <= btnY + 34.f && px >= btnX && px <= btnX + btnW)
        return PromptBtn::Cancel;
    // "Confirm [Enter]" (bottom button) → confirm.
    if (py >= btnY + 38.f && py <= btnY + 72.f && px >= btnX && px <= btnX + btnW)
        return PromptBtn::Ok;
    return PromptBtn::None;
}

// ── Zone browser overlay ──────────────────────────────────────────────────────

namespace {
constexpr float kBrowserPanW  = 460.f;
constexpr float kBrowserPanH  = 480.f;
constexpr float kBrowserPanX  = (WIN_W - kBrowserPanW) * 0.5f;
constexpr float kBrowserPanY  = (WIN_H - kBrowserPanH) * 0.5f;
constexpr float kBrowserItemH = 28.f;
constexpr float kBrowserGap   =  2.f;
constexpr float kBrowserHdrH  = 32.f;
constexpr float kBrowserItemX = kBrowserPanX + 8.f;
constexpr float kBrowserItemW = kBrowserPanW - 16.f;
constexpr float kBrowserStartY = kBrowserPanY + kBrowserHdrH;
constexpr int   kBrowserMaxShow = 14;
} // namespace

void BoardRenderer::drawZoneBrowserOverlay(sf::RenderTarget& t,
                                            const RenderHints& hints) const {
    if (!hints.showZoneBrowse || !m_game || !m_font) return;

    if (hints.zoneBrowsePlayer >= 4) return;   // defensive: only valid seats

    const BrowseZone bz = hints.zoneBrowseZone;
    // Library is hidden information: only the viewing player (seat 0, the human)
    // knows cards that have been revealed to them. Everything else shows a back.
    const bool isLibrary = (bz == BrowseZone::Library);

    // Rebuild the list from the LIVE zones every frame. Caching raw Card*
    // across frames risked dangling pointers when a card left the zone while
    // the browser was open (the crash). The lists are tiny, so this is cheap.
    m_gyBrowserCache.clear();
    if (bz == BrowseZone::Exile) {
        for (const Card* c : m_game->exile().cards())
            if (c && c->rules && (c->ownerId & 1) == hints.zoneBrowsePlayer)
                m_gyBrowserCache.push_back(c);
    } else if (bz == BrowseZone::Library) {
        for (const Card* c : m_game->player(hints.zoneBrowsePlayer).library().cards())
            if (c && c->rules) m_gyBrowserCache.push_back(c);
    } else {
        for (const Card* c : m_game->player(hints.zoneBrowsePlayer).graveyard().cards())
            if (c && c->rules) m_gyBrowserCache.push_back(c);
    }
    // Graveyard/Exile are public piles → show newest on top. Library keeps its
    // real order (front() is the top of the library) so positions are accurate.
    if (!isLibrary)
        std::sort(m_gyBrowserCache.begin(), m_gyBrowserCache.end(),
            [](const Card* a, const Card* b) { return a->zoneChangeSeq > b->zoneChangeSeq; });
    m_gyBrowserCachedPlayer = hints.zoneBrowsePlayer;
    m_gyBrowserCachedZone   = bz;
    m_gyBrowserCacheDirty   = false;
    const std::vector<const Card*>& cards = m_gyBrowserCache;

    // A library card is shown face-up only when it is known to the human viewer
    // (revealed by a peek/scry/look effect). Public zones are always face-up.
    auto cardKnown = [&](const Card* c) -> bool {
        if (!isLibrary) return true;
        return c->revealedToOwner && hints.zoneBrowsePlayer == 0;
    };

    // Dim background
    sf::RectangleShape dim({WIN_W, WIN_H});
    dim.setFillColor(sf::Color(0, 0, 0, 170));
    t.draw(dim);

    // Panel
    sf::RectangleShape pan({kBrowserPanW, kBrowserPanH});
    pan.setPosition(kBrowserPanX, kBrowserPanY);
    pan.setFillColor(sf::Color(12, 18, 28, 250));
    pan.setOutlineColor(sf::Color(60, 100, 80));
    pan.setOutlineThickness(2.f);
    t.draw(pan);

    // Header
    std::string playerName = (hints.zoneBrowsePlayer == 0) ? "Alice" : "Bob";
    std::string zoneLabel  = (bz == BrowseZone::Exile)   ? "Exile"
                           : (bz == BrowseZone::Library) ? "Library (top first)"
                                                         : "Graveyard";
    std::string title = playerName + "'s " + zoneLabel
                        + "  (" + std::to_string(cards.size()) + " cards)";
    drawTxt(t, *m_font, title, kBrowserPanX + 10.f, kBrowserPanY + 8.f, 13,
            sf::Color(180, 220, 190), true);
    drawTxt(t, *m_font, "[Click outside to close]",
            kBrowserPanX + kBrowserPanW - 160.f, kBrowserPanY + 10.f, 9,
            sf::Color(100, 115, 105));

    // Divider
    sf::RectangleShape div({kBrowserPanW - 4.f, 1.f});
    div.setPosition(kBrowserPanX + 2.f, kBrowserPanY + kBrowserHdrH - 1.f);
    div.setFillColor(sf::Color(50, 80, 60));
    t.draw(div);

    // Clamp scroll offset
    int total = static_cast<int>(cards.size());
    int maxScroll = std::max(0, total - kBrowserMaxShow);
    m_gyBrowserScroll = std::clamp(m_gyBrowserScroll, 0, maxScroll);
    int startIdx = m_gyBrowserScroll;

    // Card rows (up to kBrowserMaxShow from scroll offset)
    int shown = std::min(kBrowserMaxShow, total - startIdx);
    for (int i = 0; i < shown; ++i) {
        const Card* c = cards[static_cast<size_t>(startIdx + i)];
        float iy = kBrowserStartY + i * (kBrowserItemH + kBrowserGap);

        // Unknown card (hidden library card) → render a uniform card back row
        // with just its position, never leaking the card's identity.
        if (!cardKnown(c)) {
            sf::RectangleShape back({kBrowserItemW, kBrowserItemH});
            back.setPosition(kBrowserItemX, iy);
            back.setFillColor(sf::Color(28, 30, 48));
            back.setOutlineColor(sf::Color(70, 75, 110));
            back.setOutlineThickness(1.f);
            t.draw(back);
            sf::RectangleShape thumb({36.f, kBrowserItemH - 4.f});
            thumb.setPosition(kBrowserItemX + 3.f, iy + 2.f);
            thumb.setFillColor(sf::Color(46, 50, 78));
            thumb.setOutlineColor(sf::Color(90, 96, 140));
            thumb.setOutlineThickness(1.f);
            t.draw(thumb);
            drawTxt(t, *m_font, "\x3F", kBrowserItemX + 17.f, iy + 9.f, 14,
                    sf::Color(150, 156, 200), true);
            std::string pos = "Hidden  (#" + std::to_string(startIdx + i + 1) + " from top)";
            drawTxt(t, *m_font, pos, kBrowserItemX + 46.f, iy + 9.f, 10,
                    sf::Color(150, 156, 195));
            continue;
        }

        uint8_t ci2 = c->rules->manaCost.colorIdentity();
        sf::Color bg2 = cardBackground(ci2, c->rules->type.isLand());

        sf::RectangleShape row2({kBrowserItemW, kBrowserItemH});
        row2.setPosition(kBrowserItemX, iy);
        row2.setFillColor(sf::Color(bg2.r / 5, bg2.g / 5, bg2.b / 5 + 6));
        row2.setOutlineColor(sf::Color(bg2.r / 3, bg2.g / 3, bg2.b / 3 + 10));
        row2.setOutlineThickness(1.f);
        t.draw(row2);

        sf::RectangleShape accent2({4.f, kBrowserItemH});
        accent2.setPosition(kBrowserItemX, iy);
        accent2.setFillColor(sf::Color(bg2.r, bg2.g, bg2.b, 160));
        t.draw(accent2);

        constexpr float kThW = 36.f, kThH = kBrowserItemH - 4.f;
        {
            auto imgPath = findCardImage(c->rules->name, m_picsDir);
            if (!imgPath.empty()) {
                if (const auto* tex = TextureCache::get(imgPath)) {
                    sf::Sprite spr2(*tex);
                    spr2.setScale(kThW / static_cast<float>(tex->getSize().x),
                                  kThH / static_cast<float>(tex->getSize().y));
                    spr2.setPosition(kBrowserItemX + 3.f, iy + 2.f);
                    t.draw(spr2);
                }
            }
        }
        float textX2 = kBrowserItemX + kThW + 7.f;

        std::string nm2 = c->rules->name;
        if (nm2.size() > 22) nm2 = nm2.substr(0, 21) + ".";
        drawTxt(t, *m_font, nm2, textX2, iy + 3.f, 10, sf::Color(225, 225, 210), true);

        std::string tp2 = c->rules->type.toString();
        if (tp2.size() > 20) tp2 = tp2.substr(0, 19) + ".";
        drawTxt(t, *m_font, tp2, textX2, iy + 17.f, 8, sf::Color(130, 142, 130));

        std::string mc2 = c->rules->manaCost.toString();
        if (!mc2.empty()) {
            sf::Text tmp4(mc2, *m_font, 9);
            float mw2 = tmp4.getLocalBounds().width;
            drawTxt(t, *m_font, mc2,
                    kBrowserItemX + kBrowserItemW - mw2 - 6.f, iy + 3.f, 9,
                    sf::Color(195, 178, 110));
        }
        if (c->rules->type.isCreature() && c->rules->hasPT()) {
            std::string pt2 = c->rules->power + "/" + c->rules->toughness;
            drawTxt(t, *m_font, pt2,
                    kBrowserItemX + kBrowserItemW - 28.f, iy + 17.f, 9,
                    sf::Color(200, 200, 200), true);
        }
    }

    // Scrollbar indicator
    if (total > kBrowserMaxShow) {
        float sbH = kBrowserPanH - kBrowserHdrH;
        float thumbH = std::max(20.f, sbH * kBrowserMaxShow / (float)total);
        float thumbY = kBrowserPanY + kBrowserHdrH +
                       (sbH - thumbH) * m_gyBrowserScroll / (float)maxScroll;
        sf::RectangleShape track({5.f, sbH});
        track.setPosition(kBrowserPanX + kBrowserPanW - 6.f, kBrowserPanY + kBrowserHdrH);
        track.setFillColor(sf::Color(30, 40, 35));
        t.draw(track);
        sf::RectangleShape thumb({5.f, thumbH});
        thumb.setPosition(kBrowserPanX + kBrowserPanW - 6.f, thumbY);
        thumb.setFillColor(sf::Color(70, 130, 90));
        t.draw(thumb);

        drawTxt(t, *m_font, "scroll: mouse wheel",
                kBrowserItemX, kBrowserPanY + kBrowserPanH - 14.f, 8,
                sf::Color(70, 90, 70));
    }
    if (cards.empty()) {
        drawTxt(t, *m_font, "(empty)", kBrowserItemX + 8.f, kBrowserStartY + 8.f,
                10, sf::Color(70, 85, 70));
    }
}

bool BoardRenderer::scrollZoneBrowser(int delta, const RenderHints& hints) noexcept {
    if (!hints.showZoneBrowse) return false;
    m_gyBrowserScroll = std::max(0, m_gyBrowserScroll + delta);
    return true;
}

void BoardRenderer::invalidateZoneBrowserCache() noexcept {
    m_gyBrowserCacheDirty = true;
    m_gyBrowserScroll = 0;
}

bool BoardRenderer::hitZoneBrowserClose(float px, float py,
                                         const RenderHints& hints) const noexcept {
    if (!hints.showZoneBrowse) return false;
    // Any click outside the panel closes it
    return !(px >= kBrowserPanX && px <= kBrowserPanX + kBrowserPanW &&
             py >= kBrowserPanY && py <= kBrowserPanY + kBrowserPanH);
}

mtg::ObjectId BoardRenderer::hitZoneBrowserCard(float px, float py,
                                                  const RenderHints& hints) const {
    if (!hints.showZoneBrowse || !m_game) return kInvalidId;
    if (hints.zoneBrowsePlayer >= 4) return kInvalidId;
    if (py < kBrowserStartY || px < kBrowserItemX ||
        px > kBrowserItemX + kBrowserItemW) return kInvalidId;

    // Mirror the draw exactly: same filter, same sort, same scroll offset —
    // otherwise a click maps to a different card than the one displayed.
    const BrowseZone bz = hints.zoneBrowseZone;
    const bool isLibrary = (bz == BrowseZone::Library);
    std::vector<const Card*> cards;
    if (bz == BrowseZone::Exile) {
        for (const Card* c : m_game->exile().cards())
            if (c && c->rules && (c->ownerId & 1) == hints.zoneBrowsePlayer) cards.push_back(c);
    } else if (bz == BrowseZone::Library) {
        for (const Card* c : m_game->player(hints.zoneBrowsePlayer).library().cards())
            if (c && c->rules) cards.push_back(c);
    } else {
        for (const Card* c : m_game->player(hints.zoneBrowsePlayer).graveyard().cards())
            if (c && c->rules) cards.push_back(c);
    }
    if (!isLibrary)
        std::sort(cards.begin(), cards.end(),
            [](const Card* a, const Card* b) { return a->zoneChangeSeq > b->zoneChangeSeq; });

    int total = static_cast<int>(cards.size());
    int startIdx = std::clamp(m_gyBrowserScroll, 0, std::max(0, total - kBrowserMaxShow));
    int shown = std::min(kBrowserMaxShow, total - startIdx);
    for (int i = 0; i < shown; ++i) {
        float iy = kBrowserStartY + i * (kBrowserItemH + kBrowserGap);
        if (py >= iy && py <= iy + kBrowserItemH) {
            const Card* c = cards[static_cast<size_t>(startIdx + i)];
            // Don't leak hidden library cards: only return known/public cards.
            if (isLibrary && !(c->revealedToOwner && hints.zoneBrowsePlayer == 0))
                return kInvalidId;
            return c->id;
        }
    }
    return kInvalidId;
}

// ── Stack hit-tests ───────────────────────────────────────────────────────────

bool BoardRenderer::hitResolveAll(float px, float py) const noexcept {
    if (!m_game || m_game->stack().empty()) return false;
    // Button is in the stack section header: right edge of SIDE_W at SIDE_STACK_Y
    constexpr float kBtnW = 68.f, kBtnH = 14.f;
    float bx = SIDE_X + SIDE_W - kBtnW - 3.f;
    float by = SIDE_STACK_Y + 4.f;
    return px >= bx && px <= bx + kBtnW && py >= by && py <= by + kBtnH;
}

// ── Phase tracker hit-test ────────────────────────────────────────────────────

int BoardRenderer::hitPhaseRow(float px, float py) const noexcept {
    constexpr int   kCount = 13;
    constexpr float kHdrH  = 14.f;
    constexpr float kRowH  = (SIDE_PHASE_H - kHdrH) / static_cast<float>(kCount);

    if (px < SIDE_X || px > SIDE_X + SIDE_W) return -1;
    float relY = py - (SIDE_PHASE_Y + kHdrH);
    if (relY < 0.f || relY >= SIDE_PHASE_H - kHdrH) return -1;
    int row = static_cast<int>(relY / kRowH);
    return (row >= 0 && row < kCount) ? row : -1;
}

// ── Animation system ──────────────────────────────────────────────────────────

void BoardRenderer::spawnDeathAnim(ObjectId id, sf::Vector2f topLeft, const Card* c) {
    CardAnim a;
    a.kind    = CardAnim::Kind::DeathFade;
    a.id      = id;
    a.from    = topLeft;
    a.to      = topLeft;  // stays in place; fade-out only
    a.t       = 0.f;
    a.dur     = 0.55f;
    a.cardRef = c;
    m_anims.push_back(a);
}

void BoardRenderer::update(float dt) {
    if (!m_game || !m_tm) return;

    // Advance all animations; discard completed ones.
    for (auto& a : m_anims) a.t = std::min(1.f, a.t + dt / a.dur);
    m_anims.erase(
        std::remove_if(m_anims.begin(), m_anims.end(),
                       [](const CardAnim& a){ return a.t >= 1.f; }),
        m_anims.end());

    // ── Snapshot current battlefield ──────────────────────────────────────────
    std::set<ObjectId>             curBf;
    std::map<ObjectId, sf::Vector2f> curPos;
    for (const Card* c : m_game->battlefield().cards()) {
        curBf.insert(c->id);
        sf::Vector2f ctr = screenCenterOf(c->id);
        curPos[c->id] = { ctr.x - CARD_W * 0.5f, ctr.y - CARD_H * 0.5f };
    }

    // ── Detect cards removed from the battlefield → spawn death anims ─────────
    for (auto id : m_lastBfSet) {
        if (curBf.count(id)) continue;

        // Skip commanders returning to the command zone — they are not destroyed
        bool isCommandReturn = false;
        for (const Card* c : m_game->command().cards())
            if (c->id == id) { isCommandReturn = true; break; }
        if (isCommandReturn) continue;

        // Avoid duplicate death anim
        bool alreadyAnimating = false;
        for (const auto& a : m_anims)
            if (a.id == id && a.kind == CardAnim::Kind::DeathFade)
                { alreadyAnimating = true; break; }
        if (alreadyAnimating) continue;

        auto posIt = m_lastCardPos.find(id);
        if (posIt == m_lastCardPos.end()) continue;

        // Find the card object in its new zone (GY or exile)
        const Card* dead = nullptr;
        for (uint8_t pid = 0; pid < 2 && !dead; ++pid)
            for (const Card* c : m_game->player(pid).graveyard().cards())
                if (c->id == id) { dead = c; break; }
        if (!dead)
            for (const Card* c : m_game->exile().cards())
                if (c->id == id) { dead = c; break; }

        if (dead) spawnDeathAnim(id, posIt->second, dead);
    }

    // ── Detect newly drawn cards → spawn draw-slide anims ────────────────────────
    for (uint8_t pid = 0; pid < 2; ++pid) {
        std::set<ObjectId> curHand;
        for (const Card* c : m_game->player(pid).hand().cards())
            curHand.insert(c->id);

        for (auto id : curHand) {
            if (m_lastHandSet[pid].count(id)) continue;
            // New card in hand — slide it in from the library corner
            bool dup = false;
            for (const auto& a : m_anims)
                if (a.id == id && a.kind == CardAnim::Kind::DrawSlide) { dup = true; break; }
            if (dup) continue;

            // Library position: top-right of the hand area (as a source)
            sf::Vector2f libPos = {
                static_cast<float>(pid == 0 ? PLAY_W - CARD_W - 4.f : PLAY_W - CARD_W - 4.f),
                static_cast<float>(pid == 0 ? ALICE_HAND_Y : BOB_HAND_Y)
            };
            // Destination: first hand slot (approximate — looks good enough)
            sf::Vector2f dest = libPos;
            dest.x -= CARD_W * 0.5f;

            CardAnim a;
            a.kind    = CardAnim::Kind::DrawSlide;
            a.id      = id;
            a.from    = libPos;
            a.to      = dest;
            a.t       = 0.f;
            a.dur     = 0.25f;
            a.cardRef = m_game->findCard(id);
            m_anims.push_back(a);
        }
        m_lastHandSet[pid] = std::move(curHand);
    }

    // ── Detect new attackers → spawn fly-attack arcs ──────────────────────────
    std::set<ObjectId> curAttackers;
    for (const auto& atk : m_tm->combatState().attacks)
        curAttackers.insert(atk.attackerId);

    for (auto id : curAttackers) {
        if (m_lastAttackerSet.count(id)) continue;

        bool hasAnim = false;
        for (const auto& a : m_anims)
            if (a.id == id && a.kind == CardAnim::Kind::FlyAttack)
                { hasAnim = true; break; }
        if (hasAnim) continue;

        sf::Vector2f ctr = screenCenterOf(id);
        if (ctr.x == 0.f && ctr.y == 0.f) continue;

        const Card* c = m_game->findCard(id);
        if (!c) continue;

        sf::Vector2f from = { ctr.x - CARD_W * 0.5f, ctr.y - CARD_H * 0.5f };
        // Fly toward opponent's info bar edge
        sf::Vector2f to;
        if (c->controllerId == 0) {
            // Alice attacks Bob — fly upward toward Bob's info bar
            to = { from.x, BOB_INFO_Y + BOB_INFO_H * 0.5f - CARD_H * 0.5f };
        } else {
            // Bob attacks Alice — fly downward toward Alice's info bar
            to = { from.x, ALICE_INFO_Y + ALICE_INFO_H * 0.5f - CARD_H * 0.5f };
        }

        CardAnim a;
        a.kind    = CardAnim::Kind::FlyAttack;
        a.id      = id;
        a.from    = from;
        a.to      = to;
        a.t       = 0.f;
        a.dur     = 0.40f;
        a.cardRef = c;
        m_anims.push_back(a);
    }

    // Clear attacker set when combat ends
    if (curAttackers.empty()) m_lastAttackerSet.clear();
    else                      m_lastAttackerSet = curAttackers;

    // ── Detect newly-tapped permanents → tap rotation + mana stream ───────────
    std::set<ObjectId> curTapped;
    for (const Card* c : m_game->battlefield().cards())
        if (c->tapped) curTapped.insert(c->id);

    for (ObjectId id : curTapped) {
        if (m_lastTappedSet.count(id)) continue;          // already tapped
        if (m_tapAnims.count(id)) continue;               // anim already running
        m_tapAnims[id] = 0.f;

        // If this card has an AB$ Mana ability AND it's controlled by Alice,
        // spawn a stream from the card to the right edge of her player chip
        // (where the mana pips live now).
        const Card* c = m_game->findCard(id);
        if (!c || c->controllerId != 0) continue;
        bool producesMana = false;
        sf::Color streamCol(255, 220, 80);  // default = generic yellow
        for (const auto& raw : c->rules->abilityLines) {
            auto sl = mtg::parseScriptLine(raw);
            if (sl.abilityType != "AB" || sl.effectType != "Mana") continue;
            producesMana = true;
            auto prod = sl.get("Produced", "C");
            // Pick a representative colour from the first non-comma char.
            for (char ch : prod) {
                if      (ch == 'W') { streamCol = sf::Color(245, 240, 200); break; }
                else if (ch == 'U') { streamCol = sf::Color( 90, 150, 230); break; }
                else if (ch == 'B') { streamCol = sf::Color(140,  90, 160); break; }
                else if (ch == 'R') { streamCol = sf::Color(230, 100,  90); break; }
                else if (ch == 'G') { streamCol = sf::Color(110, 200, 120); break; }
            }
            break;
        }
        if (!producesMana) continue;
        ManaStream stream;
        auto posIt = curPos.find(id);
        if (posIt == curPos.end()) continue;
        stream.from = { posIt->second.x + CARD_W * 0.5f,
                        posIt->second.y + CARD_H * 0.5f };
        // Target: Alice's mana-pool position. SIDE_ALICE_Y + ~30 lines up to
        // the pip strip; pull horizontally toward the avatar area.
        stream.to    = sf::Vector2f(SIDE_X + 110.f, SIDE_ALICE_Y + 30.f);
        stream.color = streamCol;
        m_manaStreams.push_back(stream);
    }
    // Drop tap anims for cards that became untapped or left the battlefield.
    for (auto it = m_tapAnims.begin(); it != m_tapAnims.end(); ) {
        if (!curTapped.count(it->first)) it = m_tapAnims.erase(it);
        else                              ++it;
    }
    // Advance tap rotations (250ms duration).
    for (auto& [id, prog] : m_tapAnims)
        prog = std::min(1.f, prog + dt / 0.25f);
    // Advance mana streams; drop any that finished.
    for (auto& s : m_manaStreams)
        s.t = std::min(1.f, s.t + dt / s.dur);
    m_manaStreams.erase(
        std::remove_if(m_manaStreams.begin(), m_manaStreams.end(),
                       [](const ManaStream& s){ return s.t >= 1.f; }),
        m_manaStreams.end());
    m_lastTappedSet = std::move(curTapped);

    m_lastBfSet  = curBf;
    m_lastCardPos = curPos;
}

void BoardRenderer::drawAnims(sf::RenderTarget& t) const {
    for (const auto& anim : m_anims) {
        if (!anim.cardRef) continue;

        if (anim.kind == CardAnim::Kind::FlyAttack) {
            // Interpolate position + parabolic upward arc
            float t01 = anim.t;
            float px  = anim.from.x + (anim.to.x - anim.from.x) * t01;
            float py  = anim.from.y + (anim.to.y - anim.from.y) * t01;
            float arcH = std::abs(anim.to.y - anim.from.y) * 0.35f;
            py -= arcH * std::sin(3.14159f * t01);

            CardDrawOptions opts;
            opts.attacking = true;
            drawCard(t, *m_font, anim.cardRef, px, py, opts, m_picsDir);

        } else if (anim.kind == CardAnim::Kind::DeathFade) {
            float progress = anim.t;
            float alpha    = 1.f - progress;
            CardDrawOptions opts;
            opts.alpha = alpha;
            drawCard(t, *m_font, anim.cardRef, anim.from.x, anim.from.y, opts, m_picsDir);
            if (progress > 0.05f) {
                float r = std::min(1.f, progress * 2.0f);
                sf::Color burn(255,
                    static_cast<uint8_t>(255.f * (1.f - r * 0.85f)),
                    static_cast<uint8_t>(255.f * (1.f - r)),
                    static_cast<uint8_t>(alpha * 180.f));
                sf::RectangleShape overlay({CARD_W, CARD_H});
                overlay.setPosition(anim.from.x, anim.from.y);
                overlay.setFillColor(burn);
                t.draw(overlay);
            }
        } else if (anim.kind == CardAnim::Kind::DrawSlide) {
            // Card slides in from the right edge of the hand — ease-out interpolation
            float ease = 1.f - (1.f - anim.t) * (1.f - anim.t);  // quadratic ease-out
            float px = anim.from.x + (anim.to.x - anim.from.x) * ease;
            float py = anim.from.y + (anim.to.y - anim.from.y) * ease;
            CardDrawOptions opts;
            opts.alpha = std::min(1.f, anim.t * 3.f);  // fade in quickly
            drawCard(t, *m_font, anim.cardRef, px, py, opts, m_picsDir);
        }
    }

    // Mana streams: small colored pip that fades + travels from the tapped
    // source to the mana-pool display. Cheap circle interpolation, no trail.
    for (const auto& s : m_manaStreams) {
        float t01 = s.t;
        // Slight ease so the streak slows as it lands at the pool.
        float ease = 1.f - (1.f - t01) * (1.f - t01);
        float px = s.from.x + (s.to.x - s.from.x) * ease;
        float py = s.from.y + (s.to.y - s.from.y) * ease;
        // Shrink the dot near the end to suggest it "arriving" into the pool.
        float r  = 7.f * (1.f - t01 * 0.6f);
        sf::CircleShape dot(r);
        dot.setOrigin(r, r);
        dot.setPosition(px, py);
        sf::Color c = s.color;
        c.a = static_cast<uint8_t>(255.f * (1.f - t01 * 0.8f));
        dot.setFillColor(c);
        // Soft halo
        sf::CircleShape halo(r * 1.8f);
        halo.setOrigin(r * 1.8f, r * 1.8f);
        halo.setPosition(px, py);
        sf::Color h = s.color;
        h.a = static_cast<uint8_t>(80.f * (1.f - t01));
        halo.setFillColor(h);
        t.draw(halo);
        t.draw(dot);
    }
}

} // namespace ui
