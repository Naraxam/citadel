#pragma once
#include <SFML/Graphics/Text.hpp>
#include <algorithm>
#include <cmath>

namespace ui {

inline float g_uiScale = 1.f;

// logical (view-space) area.
inline void applyTextScale(sf::Text& t) {
    if (g_uiScale <= 1.f) return;          // nothing to do at 1x
    unsigned base  = t.getCharacterSize();
    unsigned hiRes = static_cast<unsigned>(std::lround(base * g_uiScale));
    if (hiRes < 1) hiRes = 1;
    t.setCharacterSize(hiRes);
    float inv = 1.f / g_uiScale;
    t.setScale(inv, inv);
}

} // namespace ui
