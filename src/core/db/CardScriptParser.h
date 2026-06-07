#pragma once
#include "../card/CardRules.h"
#include <filesystem>
#include <optional>
#include <vector>
#include <string>

namespace mtg {

// Parses Forge's card definition .txt files into CardRules objects.
//
// File format (one key:value per line):
//   Name:Lightning Bolt
//   ManaCost:R
//   Types:Instant
//   A:SP$ DealDamage | ...
//   Oracle:Lightning Bolt deals 3 damage to any target.
//
// Multi-value keys (K, A, T, R) produce one entry per line.
// SVar lines use the form: SVar:VarName:body
class CardScriptParser {
public:
    static std::optional<CardRules> parseFile(const std::filesystem::path& path);
    static std::optional<CardRules> parseText(std::string_view text);

    // Returns 1 rule for single-faced cards, 2 rules for DFCs (front + back).
    // Both faces have their altName set to each other's name for wireBackFaces().
    static std::vector<CardRules> parseBothFaces(const std::filesystem::path& path);
    static std::vector<CardRules> parseBothFacesText(std::string_view text);

private:
    static std::optional<CardRules> parseLines(const std::vector<std::string>& lines);
    static std::vector<CardRules>   parseBothFacesLines(const std::vector<std::string>& lines);
    static std::string unescape(std::string_view s); // converts literal \n → newline
};

} // namespace mtg
