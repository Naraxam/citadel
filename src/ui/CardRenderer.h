#pragma once
#include "UIColors.h"
#include "BoardLayout.h"
#include "../game/Card.h"
#include <SFML/Graphics.hpp>
#include <atomic>
#include <list>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace ui {

// Simple text render cache: reuses sf::Text objects to avoid per-frame allocation.
// Key is (string, size, bold); value is a pre-built sf::Text.
// Thread-local so there's no locking needed.
struct TextCache {
    struct Key {
        std::string str;
        unsigned    size;
        bool        bold;
        bool operator==(const Key& o) const noexcept {
            return str == o.str && size == o.size && bold == o.bold;
        }
    };
    struct KeyHash {
        size_t operator()(const Key& k) const noexcept {
            size_t h = std::hash<std::string>{}(k.str);
            h ^= std::hash<unsigned>{}(k.size) + 0x9e3779b9 + (h<<6) + (h>>2);
            h ^= static_cast<size_t>(k.bold) + 0x9e3779b9 + (h<<6) + (h>>2);
            return h;
        }
    };
    std::unordered_map<Key, sf::Text, KeyHash> cache;
    // Returns a reference to a sf::Text; caller sets position/color before drawing.
    sf::Text& get(const std::string& s, const sf::Font& font, unsigned sz, bool bold) {
        Key k{s, sz, bold};
        auto it = cache.find(k);
        if (it != cache.end()) return it->second;
        sf::Text& t = cache[k];
        t.setFont(font);
        t.setString(s);
        t.setCharacterSize(sz);
        if (bold) t.setStyle(sf::Text::Bold);
        return t;
    }
    void clear() { cache.clear(); }
};
inline thread_local TextCache g_textCache;

// Caches sf::Texture objects by file path. Textures are large and can't be
// copied, so this singleton avoids repeated disk loads.
// Textures are loaded asynchronously on a background thread — get() returns
// nullptr on the first call (triggers a load), then the texture on subsequent
// frames once the load completes.
class TextureCache {
public:
    // Returns the cached texture, or nullptr if not yet loaded.
    // On first call for a path, queues an async load; returns nullptr until done.
    static const sf::Texture* get(const std::string& path);

    // Like get(), but loads + uploads the texture synchronously on the calling
    // (render) thread when it isn't cached yet, so it's available THIS frame.
    // Used for the large hover/preview card where a one-frame async gap would
    // otherwise flash the text fallback. Returns nullptr only if the file can't
    // be decoded. Must be called from the render thread.
    static const sf::Texture* getSync(const std::string& path);

    // Must be called once per frame from the main thread to promote completed
    // background loads into the SFML-usable cache (SFML textures must be
    // created on the main/render thread).
    static void flushPending();

    // Discard all cached textures (call before recreating the SFML window).
    static void clear();

private:
    static std::unordered_map<std::string, sf::Texture> s_cache;
    // LRU recency list; head = most recently used, tail = eviction candidate.
    // Touched on every successful get() so frequently-shown cards stay hot.
    static std::list<std::string>                       s_lru;
    static std::unordered_map<std::string,
        std::list<std::string>::iterator>               s_lruIter;
    // Hard cap on cached textures. At ~488×680 RGBA each that's roughly
    // 250 MB of VRAM for 200 textures, which is well within any modern GPU
    // and keeps a heavy mid-game board (~80 unique cards) fully cached
    // with plenty of headroom for hover previews.
    static constexpr size_t kMaxCachedTextures = 200;
    static void touchLRU(const std::string& path);
    static void evictIfFull();

    // Async load infrastructure
    struct PendingLoad {
        std::string            path;
        std::vector<sf::Uint8> pixels;
        unsigned               width = 0, height = 0;
        bool                   ok    = false;
    };
    static std::mutex                        s_mutex;
    static std::queue<PendingLoad>           s_ready;      // loaded, waiting for main thread
    static std::unordered_set<std::string>   s_inFlight;   // paths currently being loaded
    static void loadAsync(const std::string& path);
};

// Loads and draws Java Forge skin assets: sprite_manaicons.png, sprite_zone.png,
// bg_match.jpg, no_card.jpg. All methods are no-ops when not initialized.
class SkinAssets {
public:
    // Point at the Java Forge skin directory (…/forge-gui/res/skins/default).
    // Must be called before any draw method.
    static void init(const std::string& skinDir);
    static bool ready() noexcept { return !s_skinDir.empty(); }

    // Parse a ManaCost::toString() string ("{3}{W}{W}", "{X}{R}", etc.) or a
    // ManaPool::toString() string ("{W}(2)") and draw each symbol as a sz×sz
    // sprite. Returns the total pixel width drawn (symbols + gaps).
    static float drawManaCost(sf::RenderTarget& t, const std::string& costStr,
                               float x, float y, float sz);

    // Draw a single mana token by name ("W","U","B","R","G","X","0"–"20","C","S").
    // Returns false when the token has no sprite in the atlas (e.g. "{T}", hybrids),
    // so callers can fall back to drawing the token as text.
    static bool drawManaToken(sf::RenderTarget& t, const std::string& token,
                               float x, float y, float sz);
    // True when drawManaToken would render a sprite for this token.
    static bool hasManaSprite(const std::string& token);

    // Draw a zone icon by key ("HAND","LIBRARY","GRAVEYARD","EXILE").
    static void drawZoneIcon(sf::RenderTarget& t, const std::string& key,
                              float x, float y, float sz);

    // Draw the generic player avatar (IMG_ZONE_AVATAR from sprite_zone.png).
    static void drawAvatar(sf::RenderTarget& t, float x, float y, float sz);

    // bg_match.jpg — match background.
    static const sf::Texture* bgMatch();

    // no_card.jpg — card back placeholder.
    static const sf::Texture* cardBack();

private:
    static std::string s_skinDir;

    static const sf::Texture* manaSprite();   // sprite_manaicons.png (PropType.MANAICONS)
    static const sf::Texture* iconsSprite();  // sprite_icons.png     (PropType.IMAGE)
    static const sf::Texture* zoneSprite();   // sprite_zone.png      (PropType.ZONES)

    // Sprite sub-rectangle for a given mana token in sprite_manaicons.png.
    // Returns a zero-size rect for unknown tokens.
    static sf::IntRect manaRect(const std::string& token);

    static void drawSpriteRegion(sf::RenderTarget& t, const sf::Texture& tex,
                                  sf::IntRect src,
                                  float x, float y, float w, float h);
};

// Attempts to build a path to a card's image given the Forge pics directory.
// Returns "" if picsDir is empty or the file doesn't exist.
std::string findCardImage(const std::string& cardName, const std::string& picsDir);

// Normalize a card name to a filename-safe form (lowercase, spaces→underscores).
std::string normalizeCardName(const std::string& name);

// ── Card drawing ──────────────────────────────────────────────────────────────

struct CardDrawOptions {
    bool  selected      = false; // surrounded by selection glow
    bool  tapped        = false; // shown with tap indicator
    bool  attacking     = false; // red overlay
    bool  faceDown      = false; // draw card back
    bool  inHand        = false; // show name+cost overlay bar at top of art
    float alpha         = 1.0f;  // 0–1 opacity
    bool  colorBlind    = false; // use shape overlays instead of color tints
    // Custom rotation override in degrees. -1 (default) means "use natural
    // orientation": 0° untapped, 90° tapped. Used by the tap animation to
    // interpolate from 0 → 90 over a short fade-in.
    float rotationOverride = -1.f;
};

// Draw a card at pixel position (x, y) using font for text.
// picsDir is the path to Forge's pics/cards directory (may be empty).
void drawCard(sf::RenderTarget& target,
              const sf::Font&       font,
              const mtg::Card*      card,
              float x, float y,
              const CardDrawOptions& opts = {},
              const std::string& picsDir = "");

// Draw a face-down card back.
void drawCardBack(sf::RenderTarget& target, float x, float y);

// Draw a card at an arbitrary size (w × h pixels) — used for the large preview panel.
// Same image/text-fallback logic as drawCard but scaled to the given dimensions.
void drawCardLarge(sf::RenderTarget& target,
                   const sf::Font&       font,
                   const mtg::Card*      card,
                   float x, float y, float w, float h,
                   const std::string& picsDir = "",
                   bool syncImage = false);

} // namespace ui
