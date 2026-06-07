#pragma once
#include <string>
#include <string_view>
#include <vector>

namespace mtg {

enum class CardSuperType {
    Basic,
    Legendary,
    Snow,
    World,
    Elite,
};

enum class CardMainType {
    Creature,
    Instant,
    Sorcery,
    Artifact,
    Enchantment,
    Land,
    Planeswalker,
    Battle,
    Dungeon,
    Tribal,
    Kindred,
};

struct CardType {
    std::vector<CardSuperType> supertypes;
    std::vector<CardMainType>  types;
    std::vector<std::string>   subtypes;

    // Parse from Forge script format: "Basic Land Forest", "Creature Human Wizard"
    static CardType parse(std::string_view text);

    bool has(CardMainType t)  const noexcept;
    bool has(CardSuperType t) const noexcept;
    bool hasSubtype(std::string_view sub) const noexcept;

    bool isCreature()     const noexcept { return has(CardMainType::Creature); }
    bool isLand()         const noexcept { return has(CardMainType::Land); }
    bool isInstant()      const noexcept { return has(CardMainType::Instant); }
    bool isSorcery()      const noexcept { return has(CardMainType::Sorcery); }
    bool isArtifact()     const noexcept { return has(CardMainType::Artifact); }
    bool isEnchantment()  const noexcept { return has(CardMainType::Enchantment); }
    bool isPlaneswalker() const noexcept { return has(CardMainType::Planeswalker); }
    bool isBattle()       const noexcept { return has(CardMainType::Battle); }
    bool isBasic()        const noexcept { return has(CardSuperType::Basic); }
    bool isLegendary()    const noexcept { return has(CardSuperType::Legendary); }
    bool isSnow()         const noexcept { return has(CardSuperType::Snow); }

    // Permanents stay on the battlefield: creatures, artifacts, enchantments, lands,
    // planeswalkers, battles.
    bool isPermanent() const noexcept;

    // Human-readable: "Legendary Creature — Human Wizard"
    std::string toString() const;

    static std::string_view superTypeName(CardSuperType t) noexcept;
    static std::string_view mainTypeName(CardMainType t) noexcept;
};

} // namespace mtg
