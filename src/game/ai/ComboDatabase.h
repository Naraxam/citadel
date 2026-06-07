#pragma once
#include <string>
#include <vector>
#include <cstdint>

namespace mtg {

class GameState;

// One step in a combo sequence.
struct ComboStep {
    enum class Type { Cast, Activate, Equip };
    Type        type         = Type::Cast;
    std::string cardName;           // source card for this step
    std::string targetCardName;     // Cast: target another named combo piece
    std::string targetFilter;       // Cast: fallback ValidTgts-style filter
    int         abilityIndex = 0;   // Activate: 0-indexed non-mana AB$ ability
};

// A declarative two-or-three card combo: required pieces + action sequence.
struct ComboEntry {
    std::string              id;
    std::string              description;
    std::vector<std::string> required;   // card names; each must be in hand OR on BF
    std::string              condition;  // "" = always; see evalCondition()
    std::vector<ComboStep>   sequence;
};

// Loads combo definitions from combos.json and exposes them to AiPlayer.
class ComboDatabase {
public:
    // Parse combos.json.  Silently skips malformed entries.
    void loadFromJson(const std::string& path);

    // Download combos from Commander Spellbook's public API, convert each
    // variant to our internal {required, description} schema, and write the
    // result to cachePath as a JSON array (overwriting any existing file).
    // Re-loads m_combos from the freshly-written cache on success.
    // Returns the number of combos written (0 on failure / non-Windows).
    // maxCombos <= 0 means "download everything" — paginate until the API
    // reports no next page (the full Spellbook DB is ~30k+ variants).
    int downloadFromSpellbook(const std::string& cachePath,
                              int maxCombos = 0);

    int  size()  const noexcept { return static_cast<int>(m_combos.size()); }
    bool empty() const noexcept { return m_combos.empty(); }

    // Return the first combo whose required pieces are all present and whose
    // condition passes, or nullptr if none is ready.
    const ComboEntry* findAssembled(uint8_t playerId,
                                    const GameState& game) const;

    // Iterate all combo entries — used by staticEval() for partial-assembly scoring.
    template <typename Fn>
    void forEach(Fn&& fn) const {
        for (const auto& e : m_combos) fn(e.required, e.description);
    }

private:
    std::vector<ComboEntry> m_combos;

    // True if cardName appears in the player's hand or on the battlefield.
    static bool hasCard(const std::string& name,
                        uint8_t playerId,
                        const GameState& game);

    // Evaluate a simple condition string.
    // Supported tokens: "" / "always", "own_life >= N", "opp_life <= N",
    //                   "own_creatures >= N", "opp_creatures >= N"
    static bool evalCondition(const std::string& cond,
                               uint8_t playerId,
                               const GameState& game);
};

} // namespace mtg
