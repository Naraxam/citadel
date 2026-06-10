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

// Evaluate a RestrictValid$ mana-spend spec (the clause on AB$ Mana lines such as
// Secluded Courtyard / Cavern of Souls / Pillar of Origins). Restricted mana may
// only pay for things matching the spec. The spec is a comma-separated list of
// context-gated sub-filters, e.g.:
//   "Spell.Creature+ChosenType,Activated.Creature+ChosenType+inZoneBattlefield"
// The leading token gates the context (Spell = casting a spell, Activated =
// activating an ability; Triggered/Static are not modelled for a mana spend); the
// remaining '+'-joined tokens form a card filter the payee must satisfy.
//   payee       — the spell card (isSpell) or the ability's source (isActivated)
//   producer    — the mana-producing permanent (resolves ChosenType); may be null
// Returns true if restricted mana with this spec may be spent on `payee`.
bool manaRestrictionAllows(std::string_view spec, const Card& payee,
                           bool isSpell, bool isActivated,
                           uint8_t activeController,
                           const Card* producer = nullptr,
                           const GameState* game = nullptr) noexcept;

} // namespace mtg
