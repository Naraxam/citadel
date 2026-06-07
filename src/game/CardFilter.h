#pragma once
#include "Card.h"
#include "ObjectId.h"
#include <string_view>
#include <vector>

namespace mtg {

class GameState;

// Match a card against a Forge ValidCards$ / ValidTgts$ filter string.
//
// Supported type tokens (case-sensitive):
//   Creature, Artifact, Enchantment, Land, Planeswalker, Permanent, Card, Any, Basic
//
// Qualifier tokens (after '.', multiple joined by '+'):
//   All, YouCtrl, OppCtrl, NotYouCtrl
//   Other        — not the same object as selfId (kInvalidId = no exclusion)
//   Tapped / Untapped
//   Token / nonToken
//   nonCreature / nonLand / nonLegendary
//   Legendary
//   MultiColor / Historic / ExiledWithSource / EffectSource
//   IsRemembered / IsTriggerRemembered / RememberedPlayerCtrl
//   ChosenType / ChosenColor / TopLibrary
//   Sub-type strings (Human, Elf, Forest …) — accepted if the card has that subtype
//   Unknown qualifiers are accepted (conservative).
//
// Multiple comma-separated sub-filters are OR-ed by cardMatchesAnyFilter.
//
// activeController : the player performing the action (0 or 1).
// selfId           : id of the "source" card for Other checks; defaults to kInvalidId.
// sourceCard       : optional pointer to the card that owns the static ability being evaluated.
//                    Used by Card.NamedCard, ExiledWithSource, EffectSource, IsImprinted.
// game             : optional pointer to GameState for ChosenType, ChosenColor, TopLibrary.
// remembered       : optional pointer to the current effect's remembered-card list (IsRemembered).
//                    When null, IsRemembered / IsTriggerRemembered accept conservatively.
bool cardMatchesFilter(const Card& c, std::string_view filter,
                       uint8_t activeController,
                       ObjectId selfId = kInvalidId,
                       const Card* sourceCard = nullptr,
                       const GameState* game = nullptr,
                       const std::vector<ObjectId>* remembered = nullptr) noexcept;

bool cardMatchesAnyFilter(const Card& c, std::string_view filter,
                          uint8_t activeController,
                          ObjectId selfId = kInvalidId,
                          const Card* sourceCard = nullptr,
                          const GameState* game = nullptr,
                          const std::vector<ObjectId>* remembered = nullptr) noexcept;

} // namespace mtg
