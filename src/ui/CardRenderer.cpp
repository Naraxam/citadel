#include "CardRenderer.h"
#include "CardImageDownloader.h"
#include "UiScale.h"
#include "../core/mana/ManaAtom.h"
#include "../game/CardStats.h"
#include "../game/KeywordAbility.h"
#include <filesystem>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <thread>
#include <unordered_map>
#include <vector>

namespace ui {

// ── TextureCache ──────────────────────────────────────────────────────────────

std::unordered_map<std::string, sf::Texture>  TextureCache::s_cache;
std::list<std::string>                         TextureCache::s_lru;
std::unordered_map<std::string,
    std::list<std::string>::iterator>          TextureCache::s_lruIter;
std::mutex                                     TextureCache::s_mutex;
std::queue<TextureCache::PendingLoad>          TextureCache::s_ready;
std::unordered_set<std::string>                TextureCache::s_inFlight;

void TextureCache::touchLRU(const std::string& path) {
    auto it = s_lruIter.find(path);
    if (it != s_lruIter.end())
        s_lru.erase(it->second);
    s_lru.push_front(path);
    s_lruIter[path] = s_lru.begin();
}

void TextureCache::evictIfFull() {
    while (s_cache.size() >= kMaxCachedTextures && !s_lru.empty()) {
        const std::string& victim = s_lru.back();
        s_cache.erase(victim);
        s_lruIter.erase(victim);
        s_lru.pop_back();
    }
}

void TextureCache::loadAsync(const std::string& path) {
    // Load image pixels on a detached thread; promote to sf::Texture on main thread.
    std::thread([path]() {
        sf::Image img;
        if (!img.loadFromFile(path)) {
            std::lock_guard<std::mutex> lk(s_mutex);
            s_inFlight.erase(path);
            return;
        }
        PendingLoad pl;
        pl.path   = path;
        pl.width  = img.getSize().x;
        pl.height = img.getSize().y;
        pl.ok     = true;
        // Copy raw pixel data (sf::Image is not move-constructible into a cross-thread safe struct)
        const sf::Uint8* px = img.getPixelsPtr();
        pl.pixels.assign(px, px + static_cast<size_t>(pl.width) * pl.height * 4);
        std::lock_guard<std::mutex> lk(s_mutex);
        s_ready.push(std::move(pl));
        s_inFlight.erase(path);
    }).detach();
}

void TextureCache::flushPending() {
    std::vector<PendingLoad> batch;
    {
        std::lock_guard<std::mutex> lk(s_mutex);
        while (!s_ready.empty()) {
            batch.push_back(std::move(s_ready.front()));
            s_ready.pop();
        }
    }
    for (auto& pl : batch) {
        if (!pl.ok || pl.pixels.empty()) continue;
        if (s_cache.count(pl.path)) continue;  // already promoted by another path
        // pl.pixels is RAW RGBA decoded by sf::Image on the worker thread —
        // create the texture and upload pixels directly. (Texture::loadFromMemory
        // is for ENCODED image bytes; using it here was silently failing for
        // every card image.)
        evictIfFull();
        sf::Texture tex;
        if (tex.create(pl.width, pl.height)) {
            tex.update(pl.pixels.data());
            tex.setSmooth(true);
            s_cache.emplace(pl.path, std::move(tex));
            touchLRU(pl.path);
        }
    }
}

const sf::Texture* TextureCache::get(const std::string& path) {
    auto it = s_cache.find(path);
    if (it != s_cache.end()) {
        touchLRU(path);                  // mark recently used
        return &it->second;
    }

    // Not yet cached — queue an async load and return nullptr this frame.
    {
        std::lock_guard<std::mutex> lk(s_mutex);
        if (!s_inFlight.count(path)) {
            s_inFlight.insert(path);
            loadAsync(path);
        }
    }
    return nullptr;
}

const sf::Texture* TextureCache::getSync(const std::string& path) {
    auto it = s_cache.find(path);
    if (it != s_cache.end()) { touchLRU(path); return &it->second; }

    // Decode + upload right now so the caller can draw it this frame.
    sf::Image img;
    if (!img.loadFromFile(path)) return nullptr;
    evictIfFull();
    sf::Texture tex;
    if (!tex.create(img.getSize().x, img.getSize().y)) return nullptr;
    tex.update(img);
    tex.setSmooth(true);
    auto [ins, ok] = s_cache.emplace(path, std::move(tex));
    touchLRU(path);
    // Don't double-load in the background if an async request was already queued.
    { std::lock_guard<std::mutex> lk(s_mutex); s_inFlight.erase(path); }
    return &ins->second;
}

void TextureCache::clear() {
    s_cache.clear();
    s_lru.clear();
    s_lruIter.clear();
    std::lock_guard<std::mutex> lk(s_mutex);
    while (!s_ready.empty()) s_ready.pop();
    s_inFlight.clear();
}

// ── SkinAssets ────────────────────────────────────────────────────────────────

std::string SkinAssets::s_skinDir;

void SkinAssets::init(const std::string& skinDir) {
    s_skinDir = skinDir;
}

const sf::Texture* SkinAssets::manaSprite() {
    if (s_skinDir.empty()) return nullptr;
    namespace fs = std::filesystem;
    return TextureCache::get((fs::path(s_skinDir) / "sprite_manaicons.png").string());
}

const sf::Texture* SkinAssets::iconsSprite() {
    if (s_skinDir.empty()) return nullptr;
    namespace fs = std::filesystem;
    return TextureCache::get((fs::path(s_skinDir) / "sprite_icons.png").string());
}

const sf::Texture* SkinAssets::zoneSprite() {
    if (s_skinDir.empty()) return nullptr;
    namespace fs = std::filesystem;
    return TextureCache::get((fs::path(s_skinDir) / "sprite_zone.png").string());
}

const sf::Texture* SkinAssets::bgMatch() {
    if (s_skinDir.empty()) return nullptr;
    namespace fs = std::filesystem;
    return TextureCache::get((fs::path(s_skinDir) / "bg_match.jpg").string());
}

const sf::Texture* SkinAssets::cardBack() {
    if (s_skinDir.empty()) return nullptr;
    namespace fs = std::filesystem;
    // no_card.jpg lives in res/defaults/ (sibling of skins/)
    auto p = fs::path(s_skinDir).parent_path().parent_path() / "defaults" / "no_card.jpg";
    return TextureCache::get(p.string());
}

void SkinAssets::drawSpriteRegion(sf::RenderTarget& t, const sf::Texture& tex,
                                   sf::IntRect src,
                                   float x, float y, float w, float h) {
    sf::Sprite spr(tex, src);
    spr.setScale(w / static_cast<float>(src.width),
                 h / static_cast<float>(src.height));
    spr.setPosition(x, y);
    t.draw(spr);
}

sf::IntRect SkinAssets::manaRect(const std::string& token) {
    // Coordinates from FSkinProp.java (PropType.MANAICONS, all 80×80)
    if (token == "W")  return {412,  84, 80, 80};
    if (token == "U")  return {330,  84, 80, 80};
    if (token == "B")  return {166,   2, 80, 80};
    if (token == "R")  return {330,   2, 80, 80};
    if (token == "G")  return {166,  84, 80, 80};
    if (token == "C")  return {248,   2, 80, 80};
    if (token == "X")  return {248, 576, 80, 80};
    if (token == "Y")  return {330, 576, 80, 80};
    if (token == "Z")  return {412, 576, 80, 80};
    if (token == "S")  return {412,   2, 80, 80};
    if (token == "0")  return {  2,   2, 80, 80};
    if (token == "1")  return { 84,   2, 80, 80};
    if (token == "2")  return {  2,  84, 80, 80};
    if (token == "3")  return { 84,  84, 80, 80};
    if (token == "4")  return {  2, 166, 80, 80};
    if (token == "5")  return { 84, 166, 80, 80};
    if (token == "6")  return {  2, 248, 80, 80};
    if (token == "7")  return { 84, 248, 80, 80};
    if (token == "8")  return {  2, 330, 80, 80};
    if (token == "9")  return { 84, 330, 80, 80};
    if (token == "10") return {  2, 412, 80, 80};
    if (token == "11") return { 84, 412, 80, 80};
    if (token == "12") return {  2, 494, 80, 80};
    if (token == "13") return { 84, 494, 80, 80};
    if (token == "14") return {  2, 576, 80, 80};
    if (token == "15") return { 84, 576, 80, 80};
    if (token == "16") return {  2, 658, 80, 80};
    if (token == "17") return { 84, 658, 80, 80};
    if (token == "18") return {166, 658, 80, 80};
    if (token == "19") return {248, 658, 80, 80};
    if (token == "20") return {330, 658, 80, 80};
    return {0, 0, 0, 0};
}

bool SkinAssets::hasManaSprite(const std::string& token) {
    return manaSprite() != nullptr && manaRect(token).width != 0;
}

bool SkinAssets::drawManaToken(sf::RenderTarget& t, const std::string& token,
                                float x, float y, float sz) {
    const sf::Texture* tex = manaSprite();
    if (!tex) return false;
    sf::IntRect rect = manaRect(token);
    if (rect.width == 0) return false;   // no sprite (tap/hybrid/etc.) — caller draws text
    drawSpriteRegion(t, *tex, rect, x, y, sz, sz);
    return true;
}

float SkinAssets::drawManaCost(sf::RenderTarget& t, const std::string& costStr,
                                float x, float y, float sz) {
    if (!ready() || costStr.empty()) return 0.f;
    float cx  = x;
    float gap = 1.f;
    size_t i  = 0;
    while (i < costStr.size()) {
        if (costStr[i] == '{') {
            // "{TOKEN}" format — ManaCost::toString() and ManaPool::toString()
            size_t j = costStr.find('}', i + 1);
            if (j == std::string::npos) break;
            drawManaToken(t, costStr.substr(i + 1, j - i - 1), cx, y, sz);
            cx += sz + gap;
            i = j + 1;
        } else if (costStr[i] == '(') {
            // "(n)" format — ManaPool generic floating mana
            size_t j = costStr.find(')', i + 1);
            if (j == std::string::npos) break;
            drawManaToken(t, costStr.substr(i + 1, j - i - 1), cx, y, sz);
            cx += sz + gap;
            i = j + 1;
        } else {
            ++i;
        }
    }
    return cx - x;
}

void SkinAssets::drawZoneIcon(sf::RenderTarget& t, const std::string& key,
                               float x, float y, float sz) {
    const sf::Texture* tex = iconsSprite();
    if (!tex) return;
    // Coordinates from FSkinProp.java (PropType.IMAGE, all 40×40 in sprite_zone.png)
    sf::IntRect rect{0, 0, 0, 0};
    if      (key == "HAND")      rect = {280, 40, 40, 40};
    else if (key == "LIBRARY")   rect = {280,  0, 40, 40};
    else if (key == "GRAVEYARD") rect = {320,  0, 40, 40};
    else if (key == "EXILE")     rect = {320, 40, 40, 40};
    else if (key == "SIDEBOARD") rect = {360, 40, 40, 40};
    else return;
    drawSpriteRegion(t, *tex, rect, x, y, sz, sz);
}

void SkinAssets::drawAvatar(sf::RenderTarget& t, float x, float y, float sz) {
    // IMG_ZONE_AVATAR: (0, 256, 128, 128) in sprite_zone.png (PropType.ZONES)
    const sf::Texture* tex = zoneSprite();
    if (!tex) return;
    drawSpriteRegion(t, *tex, {0, 256, 128, 128}, x, y, sz, sz);
}

// ── Image path helpers ────────────────────────────────────────────────────────

std::string normalizeCardName(const std::string& name) {
    std::string out;
    out.reserve(name.size());
    for (char c : name) {
        if (c == ' ' || c == '-') out += '_';
        else if (std::isalnum(static_cast<unsigned char>(c)))
            out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

std::string findCardImage(const std::string& cardName, const std::string& /*picsDir*/) {
    if (cardName.empty()) return "";

    static std::unordered_map<std::string, std::string> s_found;
    static std::unordered_map<std::string, std::chrono::steady_clock::time_point> s_missing;

    auto fit = s_found.find(cardName);
    if (fit != s_found.end()) return fit->second;

    auto mit = s_missing.find(cardName);
    if (mit != s_missing.end()) {
        if (std::chrono::steady_clock::now() - mit->second < std::chrono::seconds(3))
            return "";
        s_missing.erase(mit);
    }

    std::string p = CardImageDownloader::imagePath(cardName);
    if (!p.empty()) { s_found[cardName] = p; return p; }

    s_missing[cardName] = std::chrono::steady_clock::now();
    return "";
}

// ── Drawing helpers ───────────────────────────────────────────────────────────

namespace {

void drawRoundedRect(sf::RenderTarget& t, float x, float y, float w, float h,
                     sf::Color fill, sf::Color border, float borderThick = 2.f) {
    // Border
    sf::RectangleShape b({w, h});
    b.setPosition(x, y);
    b.setFillColor(border);
    t.draw(b);
    // Fill inset
    sf::RectangleShape f({w - borderThick*2, h - borderThick*2});
    f.setPosition(x + borderThick, y + borderThick);
    f.setFillColor(fill);
    t.draw(f);
}

void drawLabel(sf::RenderTarget& t, const sf::Font& font, const std::string& str,
               float x, float y, unsigned size, sf::Color col,
               bool bold = false) {
    // Decode as UTF-8 so accented card names (Æther Vial, Lim-Dûl's Vault…)
    // render correctly rather than as Latin-1 mojibake.
    sf::Text txt(sf::String::fromUtf8(str.begin(), str.end()), font, size);
    if (bold) txt.setStyle(sf::Text::Bold);
    txt.setFillColor(col);
    txt.setPosition(x, y);
    applyTextScale(txt);
    t.draw(txt);
}

} // namespace

// ── Public draw functions ─────────────────────────────────────────────────────

void drawCardBack(sf::RenderTarget& target, float x, float y) {
    using namespace Layout;
    // NOTE: Forge ships res/defaults/no_card.jpg, but that's a blank FRONT-face
    // placeholder, not a card back. We always draw the procedural MTG-style
    // back here so the opponent's hand looks like real face-down cards.
    // (If we ever ship an actual cardback.png, gate this on its existence.)

    // Procedural card back that resembles the classic MTG card back.
    // Layer 1: outermost border (very dark, near-black)
    sf::RectangleShape outer({CARD_W, CARD_H});
    outer.setPosition(x, y);
    outer.setFillColor(sf::Color(12, 9, 7));
    target.draw(outer);

    // Layer 2: golden/tan frame border
    constexpr float b1 = 3.f;
    sf::RectangleShape frame({CARD_W - b1*2, CARD_H - b1*2});
    frame.setPosition(x + b1, y + b1);
    frame.setFillColor(sf::Color(158, 122, 58));
    target.draw(frame);

    // Layer 3: blue field interior
    constexpr float b2 = b1 + 4.f;
    sf::RectangleShape field({CARD_W - b2*2, CARD_H - b2*2});
    field.setPosition(x + b2, y + b2);
    field.setFillColor(sf::Color(18, 30, 72));
    target.draw(field);

    // Layer 4: darker blue oval in the centre (approximated as a scaled circle)
    {
        float cx   = x + CARD_W * 0.5f;
        float cy   = y + CARD_H * 0.5f;
        float rw   = CARD_W * 0.30f;
        float rh   = CARD_H * 0.30f;
        float r    = std::min(rw, rh);
        sf::CircleShape oval(r, 48);
        oval.setScale(rw / r, rh / r);
        oval.setFillColor(sf::Color(10, 18, 52));
        oval.setPosition(cx - rw, cy - rh);
        target.draw(oval);
    }

    // Layer 5: thin inner gold ring echoing the outer frame
    constexpr float b3 = b2 + 3.f;
    sf::RectangleShape inner({CARD_W - b3*2, CARD_H - b3*2});
    inner.setPosition(x + b3, y + b3);
    inner.setFillColor(sf::Color::Transparent);
    inner.setOutlineColor(sf::Color(158, 122, 58, 120));
    inner.setOutlineThickness(1.f);
    target.draw(inner);

    // Layer 6: tiny corner decorations (small gold squares at each corner of inner ring)
    constexpr float cornSz = 4.f;
    const sf::Color cornCol(180, 145, 70, 200);
    const float ci = b3;
    for (int cx2 = 0; cx2 < 2; ++cx2) {
        for (int cy2 = 0; cy2 < 2; ++cy2) {
            float cx3 = cx2 == 0 ? x + ci - 0.5f : x + CARD_W - ci - cornSz + 0.5f;
            float cy3 = cy2 == 0 ? y + ci - 0.5f : y + CARD_H - ci - cornSz + 0.5f;
            sf::RectangleShape corn({cornSz, cornSz});
            corn.setPosition(cx3, cy3);
            corn.setFillColor(cornCol);
            target.draw(corn);
        }
    }
}

void drawCard(sf::RenderTarget& target, const sf::Font& font,
              const mtg::Card* card, float x, float y,
              const CardDrawOptions& opts, const std::string& picsDir) {
    using namespace Layout;

    if (opts.faceDown || !card) {
        drawCardBack(target, x, y);
        return;
    }

    // Tapped permanents (or anyone mid-rotation) render rotated about the
    // card's center. We render the un-rotated face into a shared offscreen
    // RenderTexture, then blit it as a rotated sprite. The rotation angle
    // defaults to 90° for tapped cards; the caller can override with
    // rotationOverride to animate the 0→90 sweep.
    {
        float angle = opts.rotationOverride;
        if (angle < 0.f && opts.tapped) angle = 90.f;
        if (angle > 0.5f) {
            // Render the un-rotated face into a SUPERSAMPLED offscreen texture
            // (ss× the logical card size) so the rotated, view-magnified result
            // stays crisp when the window is maximized — previously the RT was
            // logical-size and got blurred by both the rotation and the scale.
            const float ss = std::max(2.f, std::ceil(ui::g_uiScale));
            const unsigned w = static_cast<unsigned>(std::ceil(CARD_W * ss));
            const unsigned h = static_cast<unsigned>(std::ceil(CARD_H * ss));
            static thread_local sf::RenderTexture s_tapRT;
            if (s_tapRT.getSize().x != w || s_tapRT.getSize().y != h) {
                s_tapRT.create(w, h);
                s_tapRT.setSmooth(true);
            }
            // Map logical card coords → the full hi-res texture.
            s_tapRT.setView(sf::View(sf::FloatRect(0.f, 0.f, CARD_W, CARD_H)));
            s_tapRT.clear(sf::Color::Transparent);
            CardDrawOptions untapped = opts;
            untapped.tapped = false;
            untapped.rotationOverride = -1.f;
            // Rasterize the card's own text at the supersample density too.
            float savedScale = ui::g_uiScale;
            ui::g_uiScale = ss;
            drawCard(s_tapRT, font, card, 0.f, 0.f, untapped, picsDir);
            ui::g_uiScale = savedScale;
            s_tapRT.display();
            sf::Sprite spr(s_tapRT.getTexture());
            spr.setOrigin(w * 0.5f, h * 0.5f);
            spr.setScale(1.f / ss, 1.f / ss);   // display at logical card size
            spr.setRotation(angle);
            spr.setPosition(x + CARD_W * 0.5f, y + CARD_H * 0.5f);
            target.draw(spr);
            return;
        }
    }

    const mtg::CardRules* rules = card->rules;

    // Determine card color
    uint8_t ci = rules->manaCost.colorIdentity();
    bool isLand = rules->type.isLand();
    sf::Color bg = cardBackground(ci, isLand);
    sf::Color fg = textColor(ci);

    // Try loading card image (queue a background download if not yet on disk)
    bool drewImage = false;
    auto imgPath = findCardImage(rules->name, picsDir);
    {
        if (!imgPath.empty()) {
            if (const auto* tex = TextureCache::get(imgPath)) {
                sf::Sprite spr(*tex);
                float scaleX = CARD_W / static_cast<float>(tex->getSize().x);
                float scaleY = CARD_H / static_cast<float>(tex->getSize().y);
                spr.setScale(scaleX, scaleY);
                spr.setPosition(x, y);
                if (opts.alpha < 1.f)
                    spr.setColor(sf::Color(255, 255, 255,
                        static_cast<uint8_t>(opts.alpha * 255)));
                target.draw(spr);
                // Still draw border over the image
                sf::RectangleShape border({CARD_W, CARD_H});
                border.setPosition(x, y);
                border.setFillColor(sf::Color::Transparent);
                border.setOutlineColor(kBorderDark);
                border.setOutlineThickness(2.f);
                target.draw(border);

                // Hand cards get a name + mana cost bar at top for readability
                if (opts.inHand) {
                    constexpr float kBarH = 16.f;
                    sf::RectangleShape bar({CARD_W - 2.f, kBarH});
                    bar.setPosition(x + 1.f, y + 1.f);
                    bar.setFillColor(sf::Color(0, 0, 0, 175));
                    target.draw(bar);

                    // Mana cost (right-aligned) — measure it FIRST so the name can
                    // be trimmed to the space left of it instead of overlapping it
                    // (the bug that showed "Treachery{3}{U}{U}" run together).
                    std::string cost = rules->manaCost.toString();
                    float costW = 0.f;
                    if (!cost.empty()) {
                        sf::Text tmp(cost, font, 8);
                        costW = tmp.getLocalBounds().width;
                    }

                    // Card name — truncate to whatever width remains before the cost.
                    float nameMaxW = CARD_W - 6.f - (costW > 0.f ? costW + 5.f : 0.f);
                    auto nameWidth = [&](const std::string& s) {
                        sf::Text tt(sf::String::fromUtf8(s.begin(), s.end()), font, 9);
                        tt.setStyle(sf::Text::Bold);
                        return tt.getLocalBounds().width;
                    };
                    std::string nm = rules->name;
                    if (nameWidth(nm) > nameMaxW) {
                        while (nm.size() > 1 && nameWidth(nm + ".") > nameMaxW)
                            nm.pop_back();
                        nm += ".";
                    }
                    drawLabel(target, font, nm, x + 3.f, y + 2.f, 9,
                              sf::Color(235, 235, 215), true);

                    if (!cost.empty()) {
                        float costX = x + CARD_W - costW - 4.f;
                        drawLabel(target, font, cost, costX, y + 3.f, 8,
                                  sf::Color(220, 196, 110));
                    }
                }

                drewImage = true;
            }
        }
    }

    if (!drewImage) {
        // Text-only card rendering
        drawRoundedRect(target, x, y, CARD_W, CARD_H, bg, kBorderDark);

        // If the art exists on disk but the texture is still loading, overlay a
        // gentle pulse so the card reads as "loading" instead of looking final.
        if (!imgPath.empty()) {
            static sf::Clock s_shimmer;
            float ph = s_shimmer.getElapsedTime().asSeconds() * 2.2f;
            float pulse = 0.10f + 0.10f * std::sin(ph);   // 0.0..0.20
            sf::RectangleShape ov({CARD_W, CARD_H});
            ov.setPosition(x, y);
            ov.setFillColor(sf::Color(255, 255, 255,
                                      static_cast<uint8_t>(pulse * 255.f)));
            target.draw(ov);
        }

        float tx = x + 4.f;
        float ty = y + 3.f;

        // Card name (truncated if long)
        std::string nameStr = rules->name;
        if (nameStr.size() > 12) nameStr = nameStr.substr(0, 11) + ".";
        drawLabel(target, font, nameStr, tx, ty, 9, fg, true);

        // Mana cost
        std::string costStr = rules->manaCost.toString();
        if (costStr.size() > 8) costStr = costStr.substr(0, 8);
        drawLabel(target, font, costStr, tx, ty + 12.f, 8, fg);

        // Divider
        sf::RectangleShape div({CARD_W - 8.f, 1.f});
        div.setPosition(x + 4.f, y + 26.f);
        div.setFillColor(sf::Color(kBorderDark.r, kBorderDark.g,
                                   kBorderDark.b, 120));
        target.draw(div);

        // Type line
        std::string typeStr;
        for (auto mt : rules->type.types)
            typeStr += std::string(mtg::CardType::mainTypeName(mt)) + " ";
        if (typeStr.size() > 10) typeStr = typeStr.substr(0, 9) + ".";
        drawLabel(target, font, typeStr, tx, y + 29.f, 8, fg);

        // Oracle text (first line only)
        if (!rules->oracleText.empty()) {
            auto nl = rules->oracleText.find('\n');
            std::string oracle = (nl != std::string::npos)
                                 ? rules->oracleText.substr(0, nl)
                                 : rules->oracleText;
            if (oracle.size() > 40) oracle = oracle.substr(0, 38) + "…";
            // Wrap into ~2 lines of ~13 chars
            if (oracle.size() > 13) {
                drawLabel(target, font, oracle.substr(0, 13), tx, y + 43.f, 7, fg);
                drawLabel(target, font, oracle.substr(13, 13), tx, y + 53.f, 7, fg);
            } else {
                drawLabel(target, font, oracle, tx, y + 48.f, 7, fg);
            }
        }

        // P/T for creatures
        if (rules->type.isCreature() && card->rules->hasPT()) {
            int power = mtg::effectivePower(*card);
            int tgh   = mtg::effectiveToughness(*card);
            std::string pt = std::to_string(power) + "/" + std::to_string(tgh);
            float ptX = x + CARD_W - 4.f - static_cast<float>(pt.size()) * 6.f;
            drawLabel(target, font, pt, ptX, y + CARD_H - 14.f, 9, fg, true);
        }
        // Loyalty for planeswalkers
        if (rules->type.isPlaneswalker()) {
            int loyalty = card->counterCount("loyalty");
            std::string lStr = std::to_string(loyalty);
            sf::RectangleShape lbox({22.f, 16.f});
            lbox.setPosition(x + CARD_W - 24.f, y + CARD_H - 18.f);
            lbox.setFillColor(sf::Color(40, 80, 160));
            target.draw(lbox);
            drawLabel(target, font, lStr,
                      x + CARD_W - 22.f + (2 - static_cast<float>(lStr.size())) * 3.f,
                      y + CARD_H - 17.f, 10, sf::Color::White, true);
        }
    }

    // Blood/scratch marks proportional to damage taken this combat
    if (card->markedDamage > 0 && rules->type.isCreature() && card->rules->hasPT()) {
        int tgh = std::max(1, mtg::effectiveToughness(*card));
        float ratio = std::min(1.f, static_cast<float>(card->markedDamage) / static_cast<float>(tgh));
        uint8_t sa = static_cast<uint8_t>(50.f + ratio * 170.f);
        sf::Color sc(210, 35, 35, sa);
        // Three diagonal gashes across the card face
        for (int s = 0; s < 3; ++s) {
            float midY = y + CARD_H * (0.30f + s * 0.14f);
            float len  = CARD_W * (0.35f + s * 0.09f);
            float sx   = x + (CARD_W - len) * 0.5f;
            sf::Vertex line[2] = {
                {{sx,       midY - 5.f}, sc},
                {{sx + len, midY + 5.f}, sc},
            };
            target.draw(line, 2, sf::Lines);
            // Parallel thin shadow for depth
            sf::Color ss(0, 0, 0, sa / 2);
            sf::Vertex shadow[2] = {
                {{sx + 1.f,       midY - 4.f}, ss},
                {{sx + len + 1.f, midY + 6.f}, ss},
            };
            target.draw(shadow, 2, sf::Lines);
        }
    }

    // ── Current-stats badge (bottom-right): creature P/T or planeswalker
    //    loyalty. Larger, outlined and colour-coded so it reads over busy art.
    if (rules->type.isCreature() && card->rules->hasPT()) {
        int power = mtg::effectivePower(*card);
        int tgh   = mtg::effectiveToughness(*card);
        std::string pt = std::to_string(power) + "/" + std::to_string(tgh);
        bool damaged = card->markedDamage > 0;
        int  buff    = card->counterCount("+1/+1") - card->counterCount("-1/-1");
        sf::Color bg(12, 12, 18, 220), txt(236, 236, 246);
        if      (damaged)   { bg = sf::Color(150, 28, 28, 225); txt = sf::Color(255, 205, 205); }
        else if (buff > 0)  { txt = sf::Color(150, 240, 150); }   // pumped → green
        else if (buff < 0)  { txt = sf::Color(255, 170, 150); }   // shrunk → red
        constexpr float bh = 15.f;
        float bw = static_cast<float>(pt.size()) * 7.f + 8.f;
        sf::RectangleShape badge({bw, bh});
        badge.setPosition(x + CARD_W - bw - 1.f, y + CARD_H - bh - 1.f);
        badge.setFillColor(bg);
        badge.setOutlineColor(sf::Color(0, 0, 0, 190));
        badge.setOutlineThickness(1.f);
        target.draw(badge);
        drawLabel(target, font, pt, x + CARD_W - bw + 3.f, y + CARD_H - bh + 1.f, 11, txt, true);
    } else if (rules->type.isPlaneswalker()) {
        int loy = card->counterCount("loyalty");
        std::string ls = std::to_string(loy);
        constexpr float bh = 16.f;
        float bw = std::max(16.f, static_cast<float>(ls.size()) * 8.f + 9.f);
        sf::RectangleShape badge({bw, bh});
        badge.setPosition(x + CARD_W - bw - 1.f, y + CARD_H - bh - 1.f);
        badge.setFillColor(sf::Color(36, 26, 50, 230));          // planeswalker purple
        badge.setOutlineColor(sf::Color(210, 180, 90, 220));     // gold loyalty rim
        badge.setOutlineThickness(1.5f);
        target.draw(badge);
        drawLabel(target, font, ls, x + CARD_W - bw + 4.f, y + CARD_H - bh + 1.f, 11,
                  sf::Color(245, 220, 140), true);
    }

    // Tapped indicator — amber diagonal stripe at bottom
    if (opts.tapped) {
        sf::RectangleShape tapLine({CARD_W - 4.f, 3.f});
        tapLine.setPosition(x + 2.f, y + CARD_H - 5.f);
        tapLine.setFillColor(sf::Color(220, 160, 0, 220));
        target.draw(tapLine);
        if (!drewImage) {
            drawLabel(target, font, "[T]", x + 3.f, y + CARD_H - 25.f, 8,
                      sf::Color(220, 160, 0));
        }
    }

    // Selection / attack overlays — glow border instead of fill tint
    auto drawGlowBorder = [&](sf::Color glowCol, float thick) {
        // Slightly tinted fill
        sf::RectangleShape ov({CARD_W, CARD_H});
        ov.setPosition(x, y);
        ov.setFillColor(sf::Color(glowCol.r, glowCol.g, glowCol.b, 45));
        target.draw(ov);
        // Bright outline border
        sf::RectangleShape border({CARD_W - thick * 2.f, CARD_H - thick * 2.f});
        border.setPosition(x + thick, y + thick);
        border.setFillColor(sf::Color::Transparent);
        border.setOutlineColor(glowCol);
        border.setOutlineThickness(thick);
        target.draw(border);
    };
    if (opts.colorBlind) {
        // Color-blind: use thick white outline for selected, dashed pattern for attacking
        if (opts.selected) {
            // Double-thick white border
            drawGlowBorder(sf::Color(255, 255, 255, 180), 3.f);
            // Inner dotted pattern (3 horizontal bars)
            for (int bi = 0; bi < 3; ++bi) {
                sf::RectangleShape bar({CARD_W - 8.f, 2.f});
                bar.setPosition(x + 4.f, y + CARD_H * (0.25f + bi * 0.25f));
                bar.setFillColor(sf::Color(255, 255, 255, 90));
                target.draw(bar);
            }
        }
        if (opts.attacking) {
            // Diagonal cross pattern (X shape) for attacking
            drawGlowBorder(sf::Color(255, 255, 255, 200), 3.f);
            sf::Vertex diag1[2] = {{{x+4.f, y+4.f},{255,255,255,160}},
                                   {{x+CARD_W-4.f, y+CARD_H-4.f},{255,255,255,160}}};
            sf::Vertex diag2[2] = {{{x+CARD_W-4.f, y+4.f},{255,255,255,160}},
                                   {{x+4.f, y+CARD_H-4.f},{255,255,255,160}}};
            target.draw(diag1, 2, sf::Lines);
            target.draw(diag2, 2, sf::Lines);
        }
    } else {
        if (opts.selected)  drawGlowBorder(kTeal,       2.5f);
        if (opts.attacking) drawGlowBorder(kAccentRed,  2.5f);
    }

    // Damage counter (top-left)
    if (card->markedDamage > 0 && rules->type.isCreature()) {
        sf::RectangleShape dmgBadge({24.f, 13.f});
        dmgBadge.setPosition(x + 1.f, y + 1.f);
        dmgBadge.setFillColor(sf::Color(180, 30, 30, 200));
        target.draw(dmgBadge);
        drawLabel(target, font, std::to_string(card->markedDamage),
                  x + 4.f, y + 1.f, 9, sf::Color(255, 200, 200), true);
    }

    // ── Counter overlays (bottom-left): EVERY counter type on the card, each as
    //    a labelled pip. +1/+1 and -1/-1 read as "+N/+N" / "-N/-N"; others show a
    //    short tag + count. loyalty/LEVEL are omitted (shown as dedicated badges).
    {
        float cx = x + 2.f;
        float cy = y + CARD_H - 28.f;  // a row above the stats badge
        auto drawPip = [&](const std::string& lbl, sf::Color bg, sf::Color fg) {
            sf::Text tmp(lbl, font, 8);
            float pw = tmp.getLocalBounds().width + 6.f;
            if (cx + pw > x + CARD_W - 2.f) { cx = x + 2.f; cy -= 12.f; }  // wrap upward
            sf::RectangleShape pip({pw, 11.f});
            pip.setPosition(cx, cy);
            pip.setFillColor(bg);
            pip.setOutlineColor(sf::Color(0, 0, 0, 160));
            pip.setOutlineThickness(1.f);
            target.draw(pip);
            drawLabel(target, font, lbl, cx + 3.f, cy + 1.f, 8, fg, true);
            cx += pw + 2.f;
        };
        // Short, readable tag for an arbitrary counter type.
        auto tagOf = [](const std::string& t) -> std::string {
            if (t == "charge") return "CHG";
            if (t == "TIME")   return "TIME";
            if (t == "lore")   return "LORE";
            if (t == "page")   return "PAGE";
            if (t == "stun")   return "STUN";
            if (t == "oil")    return "OIL";
            if (t == "shield") return "SHLD";
            if (t == "age")    return "AGE";
            if (t == "fade")   return "FADE";
            if (t == "ki")     return "KI";
            std::string up;
            for (size_t i = 0; i < t.size() && i < 4; ++i)
                up += static_cast<char>(std::toupper(static_cast<unsigned char>(t[i])));
            return up.empty() ? std::string("CTR") : up;
        };
        // +1/+1 and -1/-1 first (most common), then everything else.
        auto pipFor = [&](const std::string& type, int n) {
            if (n <= 0 || type == "loyalty" || type == "LEVEL") return;
            if (type == "+1/+1")
                drawPip("+" + std::to_string(n) + "/+" + std::to_string(n),
                        sf::Color(40, 120, 55, 220), sf::Color(225, 255, 225));
            else if (type == "-1/-1")
                drawPip("-" + std::to_string(n) + "/-" + std::to_string(n),
                        sf::Color(150, 40, 40, 220), sf::Color(255, 220, 220));
            else
                drawPip(tagOf(type) + " " + std::to_string(n),
                        sf::Color(64, 56, 120, 220), sf::Color(228, 224, 250));
        };
        pipFor("+1/+1", card->counterCount("+1/+1"));
        pipFor("-1/-1", card->counterCount("-1/-1"));
        for (const auto& [type, n] : card->counters)
            if (type != "+1/+1" && type != "-1/-1") pipFor(type, n);
    }

    // Class enchantment tier indicator — show "Lv.N" badge
    if (rules->type.isEnchantment() && !rules->levelBands.empty()) {
        int lv = card->counterCount("LEVEL");
        if (lv > 0) {
            std::string lvStr = "Lv." + std::to_string(lv);
            sf::RectangleShape badge({28.f, 11.f});
            badge.setPosition(x + 2.f, y + 2.f);
            badge.setFillColor(sf::Color(44, 34, 14, 220));
            badge.setOutlineColor(sf::Color(203, 163, 90, 180));
            badge.setOutlineThickness(1.f);
            target.draw(badge);
            drawLabel(target, font, lvStr, x + 4.f, y + 2.f, 7, sf::Color(230, 193, 112), true);
        }
    }

    // Case solved indicator — gold "SOLVED" badge
    if (rules->hasCase && card->caseSolved) {
        sf::RectangleShape badge({36.f, 11.f});
        badge.setPosition(x + CARD_W - 38.f, y + 2.f);
        badge.setFillColor(sf::Color(44, 34, 14, 220));
        badge.setOutlineColor(sf::Color(203, 163, 90, 200));
        badge.setOutlineThickness(1.f);
        target.draw(badge);
        drawLabel(target, font, "SOLVED", x + CARD_W - 36.f, y + 2.f, 7,
                  sf::Color(230, 193, 112), true);
    }

    // Saga chapter indicator — show I / II / III along the left edge
    if (rules->saga.has_value()) {
        int lore = card->counterCount("lore");
        int maxCh = rules->saga->maxChapter;
        if (lore > 0 && maxCh > 0) {
            // Draw chapter pip boxes along left edge
            constexpr float boxH = 10.f, boxW = 14.f, gap = 2.f;
            float startY = y + 8.f;
            for (int i = 1; i <= maxCh; ++i) {
                bool active = (i == lore);
                bool done   = (i < lore);
                sf::Color bg = active ? sf::Color(203, 163, 90, 220)
                             : done   ? sf::Color(60, 50, 30, 180)
                                      : sf::Color(30, 28, 24, 180);
                sf::RectangleShape box({boxW, boxH});
                box.setPosition(x, startY + (i - 1) * (boxH + gap));
                box.setFillColor(bg);
                target.draw(box);
                static const char* kRoman[] = {"I","II","III","IV","V"};
                if (i <= 5)
                    drawLabel(target, font, kRoman[i-1],
                              x + 1.f, startY + (i-1)*(boxH+gap), 7,
                              active ? sf::Color(20, 12, 4) : sf::Color(160, 145, 110), active);
            }
        }
    }

    // Summoning-sickness pip: creatures that entered this turn (and lack Haste)
    // can't attack or use {T} abilities yet. Small "Zz" badge, top-left.
    if (card->isOnBattlefield() && card->isCreature() && card->summoningSickness &&
        !card->hasKeyword(mtg::KeywordAbility::Haste)) {
        constexpr float r = 8.f;
        sf::CircleShape badge(r);
        badge.setPosition(x + 3.f, y + 3.f);
        badge.setFillColor(sf::Color(24, 20, 16, 220));
        badge.setOutlineColor(sf::Color(150, 140, 110, 230));
        badge.setOutlineThickness(1.f);
        target.draw(badge);
        drawLabel(target, font, "Zz", x + 4.f, y + 4.f, 9,
                  sf::Color(225, 205, 155), true);
    }
}

// ── Large card preview ────────────────────────────────────────────────────────

void drawCardLarge(sf::RenderTarget& target, const sf::Font& font,
                   const mtg::Card* card, float x, float y, float w, float h,
                   const std::string& picsDir, bool syncImage) {
    if (!card) return;
    const mtg::CardRules* rules = card->rules;

    // Image mode: scale the texture to fill w×h
    {
        auto imgPath = findCardImage(rules->name, picsDir);
        if (!imgPath.empty()) {
            // The large preview loads synchronously so its art is up THIS frame
            // rather than flashing the text fallback during the async gap.
            const auto* tex = syncImage ? TextureCache::getSync(imgPath)
                                        : TextureCache::get(imgPath);
            if (tex) {
                sf::Sprite spr(*tex);
                float scaleX = w / static_cast<float>(tex->getSize().x);
                float scaleY = h / static_cast<float>(tex->getSize().y);
                spr.setScale(scaleX, scaleY);
                spr.setPosition(x, y);
                target.draw(spr);
                // Border
                sf::RectangleShape border({w, h});
                border.setPosition(x, y);
                border.setFillColor(sf::Color::Transparent);
                border.setOutlineColor(kBorderDark);
                border.setOutlineThickness(2.f);
                target.draw(border);
                return;
            }
        }
    }

    // Text-only fallback — scaled-up layout matching drawCard proportions
    uint8_t ci = rules->manaCost.colorIdentity();
    bool isLand = rules->type.isLand();
    sf::Color bg = cardBackground(ci, isLand);
    sf::Color fg = textColor(ci);

    drawRoundedRect(target, x, y, w, h, bg, kBorderDark, 3.f);

    float tx = x + 10.f;
    float ty = y + 8.f;
    float scale = w / Layout::CARD_W;  // ~3.1 at PREV_CARD_W=255

    // Name
    auto nameSize = static_cast<unsigned>(std::max(10.f, 9.f * scale));
    std::string nameStr = rules->name;
    // Truncate to ~18 chars at large size
    if (nameStr.size() > 20) nameStr = nameStr.substr(0, 19) + "...";
    drawLabel(target, font, nameStr, tx, ty, nameSize, fg, true);
    ty += static_cast<float>(nameSize) + 6.f;

    // Mana cost
    auto costSize = static_cast<unsigned>(std::max(9.f, 8.f * scale));
    drawLabel(target, font, rules->manaCost.toString(), tx, ty, costSize, fg);
    ty += static_cast<float>(costSize) + 4.f;

    // Divider
    sf::RectangleShape div({w - 20.f, 1.5f});
    div.setPosition(x + 10.f, ty);
    div.setFillColor(sf::Color(kBorderDark.r, kBorderDark.g, kBorderDark.b, 140));
    target.draw(div);
    ty += 6.f;

    // Type line
    std::string typeStr;
    for (auto mt : rules->type.types)
        typeStr += std::string(mtg::CardType::mainTypeName(mt)) + " ";
    auto typeSize = static_cast<unsigned>(std::max(9.f, 8.f * scale));
    drawLabel(target, font, typeStr, tx, ty, typeSize, fg);
    ty += static_cast<float>(typeSize) + 6.f;

    // Divider
    sf::RectangleShape div2({w - 20.f, 1.5f});
    div2.setPosition(x + 10.f, ty);
    div2.setFillColor(sf::Color(kBorderDark.r, kBorderDark.g, kBorderDark.b, 80));
    target.draw(div2);
    ty += 6.f;

    // Oracle text — word-wrapped
    if (!rules->oracleText.empty()) {
        auto oracleSize = static_cast<unsigned>(std::max(8.f, 7.f * scale));
        // Approximate chars per line: content_width / (charWidth estimate)
        float contentW = w - 20.f;
        int charsPerLine = std::max(8, static_cast<int>(contentW / (static_cast<float>(oracleSize) * 0.58f)));
        std::string text = rules->oracleText;
        float lineH = static_cast<float>(oracleSize) + 3.f;
        while (!text.empty() && ty + lineH < y + h - 25.f) {
            // Break hard at an embedded newline (oracle text uses real '\n' between
            // abilities); otherwise word-wrap. Without this the '\n'-containing
            // substring renders two physical lines but ty only advances once,
            // overlapping the next line.
            std::string line;
            size_t nl = text.find('\n');
            if (nl != std::string::npos && static_cast<int>(nl) <= charsPerLine) {
                line = text.substr(0, nl);
                text = text.substr(nl + 1);
            } else if (static_cast<int>(text.size()) <= charsPerLine) {
                line = text;
                text.clear();
            } else {
                size_t sp = text.rfind(' ', static_cast<size_t>(charsPerLine));
                if (sp == std::string::npos || sp == 0) sp = static_cast<size_t>(charsPerLine);
                line = text.substr(0, sp);
                text = text.substr(sp + 1);
            }
            drawLabel(target, font, line, tx, ty, oracleSize, fg);
            ty += lineH;
        }
    }

    // P/T for creatures
    if (rules->type.isCreature() && rules->hasPT()) {
        int power = mtg::effectivePower(*card);
        int tgh   = mtg::effectiveToughness(*card);
        std::string pt = std::to_string(power) + "/" + std::to_string(tgh);
        auto ptSize = static_cast<unsigned>(std::max(10.f, 9.f * scale));
        float ptX = x + w - 10.f - static_cast<float>(pt.size()) * (static_cast<float>(ptSize) * 0.6f);
        drawLabel(target, font, pt, ptX, y + h - static_cast<float>(ptSize) - 8.f, ptSize, fg, true);
    }

    // Loyalty for planeswalkers
    if (rules->type.isPlaneswalker()) {
        int loyalty = card->counterCount("loyalty");
        std::string lStr = std::to_string(loyalty);
        auto lSize = static_cast<unsigned>(std::max(10.f, 10.f * scale));
        float boxW = 28.f * scale, boxH = 22.f * scale;
        sf::RectangleShape lbox({boxW, boxH});
        lbox.setPosition(x + w - boxW - 6.f, y + h - boxH - 6.f);
        lbox.setFillColor(sf::Color(40, 80, 160));
        target.draw(lbox);
        drawLabel(target, font, lStr,
                  x + w - boxW - 6.f + (boxW - static_cast<float>(lStr.size()) * lSize * 0.6f) / 2.f,
                  y + h - boxH - 6.f + (boxH - static_cast<float>(lSize)) / 2.f,
                  lSize, sf::Color::White, true);
    }
}

} // namespace ui
