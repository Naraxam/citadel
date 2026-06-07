#pragma once
#include <string_view>
#include <array>

namespace mtg {

enum class TurnStep {
    // Beginning Phase
    Untap,
    Upkeep,
    Draw,
    // Pre-Combat Main Phase
    PreCombatMain,
    // Combat Phase
    BeginCombat,
    DeclareAttackers,
    DeclareBlockers,
    FirstStrikeDamage, // only active when a first/double striker is in combat
    CombatDamage,
    EndCombat,
    // Post-Combat Main Phase
    PostCombatMain,
    // Ending Phase
    EndStep,
    Cleanup,

    COUNT
};

inline constexpr int kStepCount = static_cast<int>(TurnStep::COUNT);

// All steps in order — used by TurnManager to sequence a turn
inline constexpr std::array<TurnStep, kStepCount> kStepOrder = {
    TurnStep::Untap,
    TurnStep::Upkeep,
    TurnStep::Draw,
    TurnStep::PreCombatMain,
    TurnStep::BeginCombat,
    TurnStep::DeclareAttackers,
    TurnStep::DeclareBlockers,
    TurnStep::FirstStrikeDamage,
    TurnStep::CombatDamage,
    TurnStep::EndCombat,
    TurnStep::PostCombatMain,
    TurnStep::EndStep,
    TurnStep::Cleanup,
};

inline std::string_view stepName(TurnStep s) noexcept {
    switch (s) {
        case TurnStep::Untap:           return "Untap";
        case TurnStep::Upkeep:          return "Upkeep";
        case TurnStep::Draw:            return "Draw";
        case TurnStep::PreCombatMain:   return "Pre-Combat Main";
        case TurnStep::BeginCombat:     return "Begin Combat";
        case TurnStep::DeclareAttackers:return "Declare Attackers";
        case TurnStep::DeclareBlockers:    return "Declare Blockers";
        case TurnStep::FirstStrikeDamage: return "First Strike Damage";
        case TurnStep::CombatDamage:      return "Combat Damage";
        case TurnStep::EndCombat:       return "End of Combat";
        case TurnStep::PostCombatMain:  return "Post-Combat Main";
        case TurnStep::EndStep:         return "End Step";
        case TurnStep::Cleanup:         return "Cleanup";
        default:                        return "Unknown";
    }
}

// True if players receive priority during this step (all except Untap and Cleanup)
inline constexpr bool stepGivesPriority(TurnStep s) noexcept {
    return s != TurnStep::Untap && s != TurnStep::Cleanup;
}

// True if this is one of the two main phases (spells and abilities can be played freely)
inline constexpr bool isMainPhase(TurnStep s) noexcept {
    return s == TurnStep::PreCombatMain || s == TurnStep::PostCombatMain;
}

} // namespace mtg
