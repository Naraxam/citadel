#pragma once
#include "../ObjectId.h"
#include <cstdint>

namespace mtg {

// A single target for a spell or ability — either a player or a game object.
struct Target {
    enum class Kind { None, Player, Card };

    Kind     kind     = Kind::None;
    uint8_t  playerId = 0;
    ObjectId cardId   = kInvalidId;

    static Target forPlayer(uint8_t id) noexcept {
        Target t;
        t.kind     = Kind::Player;
        t.playerId = id;
        return t;
    }

    static Target forCard(ObjectId id) noexcept {
        Target t;
        t.kind   = Kind::Card;
        t.cardId = id;
        return t;
    }

    bool isValid()  const noexcept { return kind != Kind::None; }
    bool isPlayer() const noexcept { return kind == Kind::Player; }
    bool isCard()   const noexcept { return kind == Kind::Card; }
};

} // namespace mtg
