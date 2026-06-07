#pragma once
#include "Target.h"
#include <cstdint>
#include <vector>
#include <string>
#include <string_view>

namespace mtg {

class GameState;
struct Card;

// Runtime context passed to every effect handler.
struct EffectContext {
    GameState&          game;
    Card*               source;               // the card generating the effect (may be null)
    uint8_t             controller;           // player index who controls the effect
    std::vector<Target> targets;              // chosen targets for this effect
    int                 xValue = 0;           // value of X for variable-cost spells
    ObjectId            triggeredCardId = 0;  // card that caused a trigger (Defined$ TriggeredCard)
    uint8_t             triggerPlayer = 255;  // player involved in trigger (TriggeredTarget as player)
    int                 triggerAmount = 0;    // numeric trigger payload (TriggerCount$DamageAmount)
    bool                kicked  = false;
    bool                buyback = false;
    // Snapshot of the first target's controller at spell resolution time.
    // Used for "Controller$ TargetController" when the target may have moved zones.
    uint8_t             firstTargetController = 255; // 255 = not set

    // Cards "remembered" during this effect chain (used by RememberTargets$/RememberMilled$/
    // RememberChanged$ fields, resolved by Defined$ Remembered and Cleanup|ClearRemembered$).
    std::vector<ObjectId> remembered;

    // Look up a named SVar from the source card's script data.
    // Returns "" if source is null or the var isn't found.
    std::string_view svar(const std::string& name) const noexcept;
};

} // namespace mtg
