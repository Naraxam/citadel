#pragma once
#include <SFML/System/Vector2.hpp>

namespace ui::Layout {

// ── Window ────────────────────────────────────────────────────────────────────
constexpr float WIN_W = 1560.f;
constexpr float WIN_H = 800.f;

// ── Card dimensions ───────────────────────────────────────────────────────────
constexpr float CARD_W     = 82.f;
constexpr float CARD_H     = 112.f;
constexpr float CARD_GAP   =  8.f;
constexpr float CARD_PITCH = CARD_W + CARD_GAP;
constexpr float ZONE_PAD   =  8.f;

// ── Horizontal split: sidebar | play area | preview ──────────────────────────
constexpr float SIDE_X = 0.f;
constexpr float SIDE_W = 220.f;

constexpr float PLAY_X = SIDE_W;           // 220
constexpr float PLAY_W = 1075.f;

constexpr float PREV_X = PLAY_X + PLAY_W;  // 1295
constexpr float PREV_W = WIN_W - PREV_X;   // 265

constexpr float PREV_CARD_W = PREV_W - 30.f;
constexpr float PREV_CARD_H = PREV_CARD_W * CARD_H / CARD_W;  // ~321

// ── Play area row heights (sum = WIN_H = 800) ─────────────────────────────────
// Phase strip removed — those 24px redistributed to battlefields.
// Bottom dock removed — its 39px went to Alice's hand for readability.
constexpr float BOB_INFO_H     =  36.f;
constexpr float BOB_HAND_H     = 115.f;
constexpr float BOB_BF_H       = 232.f;
constexpr float ALICE_BF_H     = 232.f;
constexpr float ALICE_HAND_H   = 149.f;   // was 110, +39 from removed dock
constexpr float ALICE_INFO_H   =  36.f;
constexpr float DOCK_H         =   0.f;   // removed; concede+undo now in ESC menu
// 36+115+232+232+149+36+0 = 800 ✓

// ── Play area row Y positions ─────────────────────────────────────────────────
constexpr float BOB_INFO_Y     =  0.f;
constexpr float BOB_HAND_Y     = BOB_INFO_Y    + BOB_INFO_H;    //  36
constexpr float BOB_BF_Y       = BOB_HAND_Y    + BOB_HAND_H;    // 151
constexpr float ALICE_BF_Y     = BOB_BF_Y      + BOB_BF_H;      // 383 (was 395)
constexpr float ALICE_HAND_Y   = ALICE_BF_Y    + ALICE_BF_H;    // 615
constexpr float ALICE_INFO_Y   = ALICE_HAND_Y  + ALICE_HAND_H;  // 725
constexpr float DOCK_Y         = ALICE_INFO_Y  + ALICE_INFO_H;  // 761

// ── Sidebar sections ──────────────────────────────────────────────────────────
// Both player strips are now 80px for visual symmetry.
constexpr float SIDE_BOB_Y     =   0.f;
constexpr float SIDE_BOB_H     =  80.f;   // was 64
// Phase tracker (replaces the center horizontal phase strip)
constexpr float SIDE_PHASE_Y   = SIDE_BOB_H;                          //  80
constexpr float SIDE_PHASE_H   = 196.f;   // 14px header + 13×14px rows
// Stack section
constexpr float SIDE_STACK_Y   = SIDE_PHASE_Y + SIDE_PHASE_H;         // 276
constexpr float SIDE_STACK_H   = 180.f;   // was 310
// Prompt section
constexpr float SIDE_PROMPT_Y  = SIDE_STACK_Y + SIDE_STACK_H;         // 456
constexpr float SIDE_PROMPT_H  = 145.f;   // was 180
// Log section
constexpr float SIDE_LOG_Y     = SIDE_PROMPT_Y + SIDE_PROMPT_H;       // 601
constexpr float SIDE_LOG_H     = WIN_H - SIDE_LOG_Y - SIDE_BOB_H;     // 119
// Alice status at very bottom (same height as Bob for symmetry)
constexpr float SIDE_ALICE_Y   = SIDE_LOG_Y + SIDE_LOG_H;             // 720
constexpr float SIDE_ALICE_H   = WIN_H - SIDE_ALICE_Y;                //  80
// Sanity: 80+196+180+145+119+80 = 800 ✓

// Stack items inside the sidebar stack section
constexpr float STACK_ITEM_Y   = SIDE_STACK_Y + 22.f;  // below "STACK" header
constexpr float STACK_ITEM_H   = 66.f;
constexpr float STACK_PITCH    = STACK_ITEM_H + 3.f;
constexpr int   STACK_MAX_SHOW = 2;   // tighter section: show 2 at most

// Graveyard (below stack items when stack is short)
constexpr float GY_LABEL_H     = 16.f;
constexpr float GY_CARD_H      = 38.f;
constexpr float GY_PITCH       = GY_CARD_H + 2.f;
constexpr int   GY_MAX_SHOWN   = 2;

// Exile (below GY)
constexpr float EX_LABEL_H     = 16.f;
constexpr float EX_CARD_H      = 28.f;
constexpr float EX_PITCH       = EX_CARD_H + 2.f;
constexpr int   EX_MAX_SHOWN   = 1;

// ── Dock buttons (4 equal-width buttons across PLAY_W) ───────────────────────
constexpr float DOCK_BTN_W = PLAY_W / 4.f;  // 268.75

// ── Inline helpers ────────────────────────────────────────────────────────────

inline sf::Vector2f cardPos(int index, float zoneX, float zoneY, float zoneH) {
    float x = zoneX + ZONE_PAD + static_cast<float>(index) * CARD_PITCH;
    float y = zoneY + (zoneH - CARD_H) * 0.5f;
    return {x, y};
}

inline sf::Vector2f stackItemPos(int i) {
    return { SIDE_X + 2.f, STACK_ITEM_Y + static_cast<float>(i) * STACK_PITCH };
}

} // namespace ui::Layout
