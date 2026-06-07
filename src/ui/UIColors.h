#pragma once
#include <SFML/Graphics/Color.hpp>
#include "../core/mana/ManaAtom.h"


namespace ui {

// ── Surface layers ────────────────────────────────────────────────────────────
inline const sf::Color kBg0      (11,  10,   9);       // --bg-0  deepest void
inline const sf::Color kBg1      (18,  17,  16);       // --bg-1  app base
inline const sf::Color kBg2      (26,  24,  22);       // --bg-2  panel
inline const sf::Color kBg3      (34,  31,  27);       // --bg-3  elevated panel
inline const sf::Color kBg4      (44,  40,  35);       // --bg-4  hover/raised
inline const sf::Color kBgGlass  (28,  25,  21, 184);  // --bg-glass 72% glass

// ── Hairlines ─────────────────────────────────────────────────────────────────
inline const sf::Color kLineSoft (240, 220, 180,  18); // 0.07 opacity
inline const sf::Color kLine     (240, 220, 180,  30); // 0.12
inline const sf::Color kLineStr  (240, 220, 180,  51); // 0.20

// ── Text ──────────────────────────────────────────────────────────────────────
inline const sf::Color kInk      (241, 234, 220);       // --ink  warm off-white
inline const sf::Color kInk2     (199, 189, 172);       // --ink-2
inline const sf::Color kInk3     (148, 139, 124);       // --ink-3 muted
inline const sf::Color kInk4     (107,  99,  87);       // --ink-4 faint

// ── Gold / ember ──────────────────────────────────────────────────────────────
inline const sf::Color kGold     (203, 163,  90);       // --gold
inline const sf::Color kGoldBrt  (230, 193, 112);       // --gold-bright
inline const sf::Color kGoldDeep (143, 111,  51);       // --gold-deep
inline const sf::Color kEmber    (217, 116,  63);       // --ember
inline const sf::Color kEmberBrt (239, 140,  82);       // --ember-bright

// ── Five-colour mana pips ─────────────────────────────────────────────────────
inline const sf::Color kManaW    (239, 231, 207);       // white
inline const sf::Color kManaU    ( 90, 160, 216);       // blue
inline const sf::Color kManaB    (157, 127, 182);       // black
inline const sf::Color kManaR    (223, 106,  68);       // red
inline const sf::Color kManaG    ( 95, 168, 115);       // green
inline const sf::Color kManaC    (185, 178, 164);       // colorless

// ── Status ────────────────────────────────────────────────────────────────────
inline const sf::Color kGood     (109, 185, 127);       // --good
inline const sf::Color kWarn     (217, 162,  63);       // --warn
inline const sf::Color kBad      (214,  91,  74);       // --bad

// ── Legacy aliases ────────────────────────────────────────────────────────────
// Keep for backward-compatibility with existing draw calls.
inline const sf::Color kPanelBg   = kBg2;
inline const sf::Color kZoneBg    = kBg2;
inline const sf::Color kBorderDark= kBg0;
inline const sf::Color kHighlight = kGoldBrt;
inline const sf::Color kTeal      = kGoldBrt;    // selection accent → gold
inline const sf::Color kTextBright= kInk;
inline const sf::Color kTextDim   = kInk3;
inline const sf::Color kHeaderBg  = kBg0;
inline const sf::Color kSidebarBg = kBg0;
inline const sf::Color kAccentRed = kBad;
inline const sf::Color kAccentGold= kGold;
inline const sf::Color kAttackTint(217, 116,  63, 170); // ember for attacking
inline const sf::Color kBlockTint ( 90, 160, 216, 160); // blue for blocking
inline const sf::Color kSelectTint(230, 193, 112,  60); // gold glow for selected
inline const sf::Color kBtnNormal = kBg3;
inline const sf::Color kBtnHover  = kBg4;
inline const sf::Color kBtnText   = kInk;

// ── Card frame colours ────────────────────────────────────────────────────────
inline sf::Color cardBackground(uint8_t colorMask, bool isLand) {
    using namespace mtg::ManaAtom;
    if (isLand) return sf::Color(44, 62, 40);          // muted forest green
    int bits = 0;
    for (uint8_t m = colorMask & 0x1F; m; m &= m-1) ++bits;
    if (bits == 0) return sf::Color(48, 44, 38);       // artifact – warm grey
    if (bits >  1) return sf::Color(62, 50, 22);       // multicolour – deep gold
    if (colorMask & WHITE) return sf::Color(66, 60, 44);
    if (colorMask & BLUE)  return sf::Color(24, 46, 72);
    if (colorMask & BLACK) return sf::Color(34, 24, 46);
    if (colorMask & RED)   return sf::Color(66, 26, 18);
    if (colorMask & GREEN) return sf::Color(24, 50, 32);
    return sf::Color(48, 44, 38);
}

inline sf::Color textColor(uint8_t /*colorMask*/) {
    return kInk;  
}

} // namespace ui
