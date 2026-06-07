#pragma once
#include <cstdint>

namespace mtg {

// Every game object (card, token, copy on stack) gets a unique id.
// When a permanent changes zones it becomes a new object with a new id —
// that rule is enforced in GameState::moveToZone.
using ObjectId = uint32_t;
inline constexpr ObjectId kInvalidId = 0;

} // namespace mtg
