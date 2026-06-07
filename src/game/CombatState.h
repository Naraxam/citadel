#pragma once
#include "ObjectId.h"
#include <vector>
#include <algorithm>

namespace mtg {

// Tracks declared attackers and blockers for the current combat phase.
// Cleared at End of Combat.
struct CombatState {
    struct Attack {
        ObjectId attackerId;
        uint8_t  defendingPlayerId;
        ObjectId defendingPlaneswalker = kInvalidId; // non-kInvalidId when attacking a PW
        std::vector<ObjectId> blockerIds;
        std::vector<ObjectId> bandIds;  // other attackers in this band (Banding)
        bool damageDone = false;
    };

    std::vector<Attack> attacks;

    bool empty() const noexcept { return attacks.empty(); }

    bool isAttacking(ObjectId id) const noexcept {
        for (const auto& a : attacks)
            if (a.attackerId == id) return true;
        return false;
    }

    bool isBlocking(ObjectId id) const noexcept {
        for (const auto& a : attacks)
            for (auto bid : a.blockerIds)
                if (bid == id) return true;
        return false;
    }

    Attack* findAttack(ObjectId attackerId) noexcept {
        for (auto& a : attacks)
            if (a.attackerId == attackerId) return &a;
        return nullptr;
    }

    const Attack* findAttack(ObjectId attackerId) const noexcept {
        for (const auto& a : attacks)
            if (a.attackerId == attackerId) return &a;
        return nullptr;
    }

    // Which attacker is this blocker assigned to?
    ObjectId attackerOf(ObjectId blockerId) const noexcept {
        for (const auto& a : attacks)
            for (auto bid : a.blockerIds)
                if (bid == blockerId) return a.attackerId;
        return kInvalidId;
    }

    void clear() noexcept { attacks.clear(); }
};

} // namespace mtg
