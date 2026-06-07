#pragma once
#include "Zone.h"
#include "ManaPool.h"
#include <string>
#include <unordered_map>

namespace mtg {

class Player {
public:
    explicit Player(uint8_t id, std::string name = "")
        : m_id(id)
        , m_name(name.empty() ? "Player " + std::to_string(id + 1) : std::move(name))
        , m_library(ZoneType::Library)
        , m_hand(ZoneType::Hand)
        , m_graveyard(ZoneType::Graveyard)
    {}

    // ── Identity ──────────────────────────────────────────────────────────
    uint8_t            id()   const noexcept { return m_id; }
    const std::string& name() const noexcept { return m_name; }

    // ── Life ──────────────────────────────────────────────────────────────
    int  life()         const noexcept { return m_life; }
    void setLife(int v)       noexcept { m_life = v; }
    void gainLife(int n)      noexcept { m_life += n; }
    void loseLife(int n)      noexcept { m_life -= n; }

    // ── Loss condition ────────────────────────────────────────────────────
    bool hasLost() const noexcept { return m_lost; }
    void lose()          noexcept { m_lost = true; }

    // ── Zones ─────────────────────────────────────────────────────────────
    Zone& library()         noexcept { return m_library; }
    Zone& hand()            noexcept { return m_hand; }
    Zone& graveyard()       noexcept { return m_graveyard; }

    const Zone& library()   const noexcept { return m_library; }
    const Zone& hand()      const noexcept { return m_hand; }
    const Zone& graveyard() const noexcept { return m_graveyard; }

    // Returns the player's zone of the given type, or nullptr for shared zones.
    Zone* zoneByType(ZoneType t) noexcept {
        switch (t) {
            case ZoneType::Library:   return &m_library;
            case ZoneType::Hand:      return &m_hand;
            case ZoneType::Graveyard: return &m_graveyard;
            default:                  return nullptr;
        }
    }

    // ── Mana ──────────────────────────────────────────────────────────────
    ManaPool&       manaPool()       noexcept { return m_manaPool; }
    const ManaPool& manaPool() const noexcept { return m_manaPool; }

    // ── Turn flags ────────────────────────────────────────────────────────
    int  maxHandSize()      const noexcept { return m_maxHandSize; }
    void setMaxHandSize(int n)    noexcept { m_maxHandSize = n; }

    // Number of lands played this turn (the limit is base 1 + AdjustLandPlays
    // static bonuses; see GameState::landPlayLimit / canPlayLand).
    int  landsPlayedThisTurn() const noexcept { return m_landsPlayedThisTurn; }
    void incLandsPlayed()            noexcept { ++m_landsPlayedThisTurn; }
    // Back-compat: "has played at least one land" (kept for callers that only
    // care whether the land drop was used).
    bool landPlayedThisTurn()  const noexcept { return m_landsPlayedThisTurn > 0; }

    // Extra turns queued (by Time Walk, etc.)
    int  extraTurns()        const noexcept { return m_extraTurns; }
    void addExtraTurn(int n = 1)   noexcept { m_extraTurns += n; }
    int  consumeExtraTurn()        noexcept {
        if (m_extraTurns > 0) { --m_extraTurns; return 1; } return 0;
    }

    // Turns to skip (Stasis, Fatigue effects, etc.)
    int  skipTurns()            const noexcept { return m_skipTurns; }
    void addSkipTurn(int n = 1)       noexcept { m_skipTurns += n; }
    bool consumeSkipTurn()            noexcept {
        if (m_skipTurns > 0) { --m_skipTurns; return true; } return false;
    }

    // Call at start of each of this player's turns to reset per-turn state.
    void resetTurnState() noexcept {
        m_landsPlayedThisTurn = 0;
        m_manaPool.empty();
    }

    // Copy all non-zone player state and zone card lists into dst.
    // Zone pointers in dst will still reference this player's Card objects;
    // call Zone::rebuildPointers() on each zone after cloning m_objects.
    void cloneStateTo(Player& dst) const noexcept {
        dst.m_life               = m_life;
        dst.m_lost               = m_lost;
        dst.m_maxHandSize        = m_maxHandSize;
        dst.m_landsPlayedThisTurn = m_landsPlayedThisTurn;
        dst.m_poison             = m_poison;
        dst.m_damageShield            = m_damageShield;
        dst.m_extraTurns              = m_extraTurns;
        dst.m_skipTurns               = m_skipTurns;
        dst.m_commanderDamageFrom[0]  = m_commanderDamageFrom[0];
        dst.m_commanderDamageFrom[1]  = m_commanderDamageFrom[1];
        dst.m_counters                = m_counters;
        dst.m_manaPool           = m_manaPool;
        dst.m_library.copyCardsFrom(m_library);
        dst.m_hand.copyCardsFrom(m_hand);
        dst.m_graveyard.copyCardsFrom(m_graveyard);
    }

    // ── Poison / Infect ───────────────────────────────────────────────────
    int  poisonCounters() const noexcept { return m_poison; }
    void addPoison(int n = 1)   noexcept { m_poison += n; }

    // ── General player counters (energy, experience, RAD, etc.) ──────────
    int  counterCount(const std::string& type) const noexcept {
        auto it = m_counters.find(type);
        return it != m_counters.end() ? it->second : 0;
    }
    void addCounter(const std::string& type, int n = 1) { m_counters[type] += n; }
    void removeCounter(const std::string& type, int n = 1) {
        auto it = m_counters.find(type);
        if (it != m_counters.end()) {
            it->second -= n;
            if (it->second <= 0) m_counters.erase(it);
        }
    }

    // ── Damage prevention shield ──────────────────────────────────────────
    int  damageShield()           const noexcept { return m_damageShield; }
    void addDamageShield(int n)         noexcept { m_damageShield += n; }
    void clearDamageShield()            noexcept { m_damageShield = 0; }

    // ── Commander damage ──────────────────────────────────────────────────────
    // commanderDamageFrom(src): total combat damage dealt to this player by player src's commander.
    int  commanderDamageFrom(uint8_t src) const noexcept { return m_commanderDamageFrom[src & 1]; }
    void addCommanderDamage(uint8_t src, int n)   noexcept { if (n > 0) m_commanderDamageFrom[src & 1] += n; }

private:
    uint8_t     m_id;
    std::string m_name;
    int         m_life           = 40;
    bool        m_lost           = false;
    int         m_maxHandSize    = 7;
    int         m_landsPlayedThisTurn = 0;
    int         m_poison         = 0;
    int         m_damageShield  = 0;
    int         m_extraTurns    = 0;
    int         m_skipTurns     = 0;
    int         m_commanderDamageFrom[2] = {0, 0};
    std::unordered_map<std::string, int> m_counters; // energy, experience, etc.

    Zone        m_library;
    Zone        m_hand;
    Zone        m_graveyard;
    ManaPool    m_manaPool;
};

} // namespace mtg
