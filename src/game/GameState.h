#pragma once
#include "Player.h"
#include "Zone.h"
#include "Card.h"
#include "CombatState.h"
#include "ContinuousEffect.h"
#include "TriggerSystem.h"
#include "../core/db/CardDb.h"
#include <algorithm>
#include <array>
#include <list>
#include <memory>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

namespace mtg {

// Riot ETB choice pending for the human player (Haste vs +1/+1 counter).
struct PendingRiotChoice {
    bool     active = false;
    ObjectId cardId = kInvalidId;
};

// "As CARDNAME enters, choose a creature type" (Herald's Horn) — pending for the
// human player. `options` is a focused, clickable list of candidate creature
// types (those present across the player's zones); picking one stores it on the
// card's chosenType.
struct PendingChooseTypeChoice {
    bool                     active = false;
    ObjectId                 cardId = kInvalidId;
    std::vector<std::string> options;
};

// "Pay N life or it enters tapped" (shock lands) — pending for the human player.
// On accept: pay `amount` life. On decline: tap `cardId`.
struct PendingPayLifeChoice {
    bool     active = false;
    ObjectId cardId = kInvalidId;
    int      amount = 0;
    uint8_t  payer  = 0;
};

// Fabricate ETB choice pending for the human player (+N/+N counters vs N Servo tokens).
struct PendingFabricateChoice {
    bool     active = false;
    ObjectId cardId = kInvalidId;
    int      amount = 0;
};

// Charm mode selection pending for the human player.
// Stores the resolved SVar bodies so they can be executed after the source
// card has moved to the graveyard.
struct PendingCharmChoice {
    bool                     active     = false;
    std::vector<std::string> labels;   // mode names shown in the UI
    std::vector<std::string> bodies;   // SVar body strings, one per mode
    uint8_t                  controller = 0;
    std::vector<Target>      targets;  // original resolution targets
    int                      xValue    = 0;
    ObjectId                 sourceId  = kInvalidId;
};

// Forced discard waiting for the human player to choose which card(s) to discard.
struct PendingDiscardChoice {
    bool active   = false;
    int  numCards = 0;   // remaining cards still to discard
};

// Madness cast-or-GY decision pending for the human player.
struct PendingMadnessCast {
    bool        active    = false;
    ObjectId    cardId    = kInvalidId;
    uint8_t     ctrl      = 0;
    std::string cardName;
};

// Represents a tutor/search effect waiting for the human player to pick a card.
struct PendingLibrarySearch {
    bool        active    = false;
    uint8_t     libPlayer = 0;
    ZoneType    dest      = ZoneType::Hand;
    uint8_t     destCtrl  = 0;
    std::string filter;   // Forge ValidCards$ filter string (e.g. "Basic Land")
};

class GameState {
public:
    GameState();

    // ── Players ───────────────────────────────────────────────────────────
    // ── Multi-player support ──────────────────────────────────────────────
    // numPlayers() returns 2 (default) or 4 (Commander pods).
    // All existing code using `pid ^ 1` still works correctly for 2-player;
    // for 4-player the engine cycles through seats 0→1→2→3→0.
    uint8_t numPlayers() const noexcept { return m_numPlayers; }
    void    setNumPlayers(uint8_t n) noexcept { m_numPlayers = std::min(n, uint8_t{4}); }

    Player&       player(uint8_t id)       { return m_players[id]; }
    const Player& player(uint8_t id) const { return m_players[id]; }

    Player&       activePlayer()          { return m_players[m_activePlayerId]; }
    const Player& activePlayer()    const { return m_players[m_activePlayerId]; }
    // nonActivePlayer: in 2-player returns the opponent; in 4-player returns the next seat.
    Player&       nonActivePlayer()       { return m_players[nextPlayerAfter(m_activePlayerId)]; }
    const Player& nonActivePlayer() const { return m_players[nextPlayerAfter(m_activePlayerId)]; }

    // Returns the ID of the "next" player in seat order (handles 2 and 4 player).
    uint8_t nextPlayerAfter(uint8_t pid) const noexcept {
        return (pid + 1) % m_numPlayers;
    }

    uint8_t activePlayerId()   const noexcept { return m_activePlayerId; }
    uint8_t priorityPlayerId() const noexcept { return m_priorityPlayerId; }
    int     turnNumber()       const noexcept { return m_turnNumber; }

    void setActivePlayer(uint8_t id)   noexcept { m_activePlayerId   = id; }
    void setPriorityPlayer(uint8_t id) noexcept { m_priorityPlayerId = id; }
    void incrementTurnNumber()         noexcept { ++m_turnNumber; }
    void advanceTurn()                 noexcept {
        ++m_turnNumber;
        m_activePlayerId   = nextPlayerAfter(m_activePlayerId);
        m_priorityPlayerId = m_activePlayerId;
        activePlayer().resetTurnState();
    }

    // ── Shared zones ──────────────────────────────────────────────────────
    Zone& battlefield() noexcept { return m_battlefield; }
    Zone& exile()       noexcept { return m_exile; }
    Zone& stack()       noexcept { return m_stack; }
    Zone& command()     noexcept { return m_command; }

    const Zone& battlefield() const noexcept { return m_battlefield; }
    const Zone& exile()       const noexcept { return m_exile; }
    const Zone& stack()       const noexcept { return m_stack; }
    const Zone& command()     const noexcept { return m_command; }

    // Returns the zone of the given type, using controllerId for player zones.
    Zone&       zoneOf(ZoneType type, uint8_t controllerId = 0);
    const Zone& zoneOf(ZoneType type, uint8_t controllerId = 0) const;

    // ── Object management ─────────────────────────────────────────────────

    // Clear all game state and return to a fresh game (resets all zones, objects,
    // turn number, and player states). Call before re-using a GameState for a new game.
    void reset();

    // Create a new card object and place it in the owner's library by default.
    // Returns a non-owning pointer valid for the lifetime of this GameState.
    Card* createCard(const CardRules* rules, uint8_t ownerId);

    // Create a token creature and place it directly on the battlefield.
    // The GameState owns the rules object; the pointer remains valid for the
    // game's lifetime.
    Card* createToken(const std::string& name,
                      const std::string& types,      // e.g. "Creature Saproling"
                      uint8_t           colorMask,   // ManaAtom color bits
                      const std::string& power,
                      const std::string& toughness,
                      uint8_t           controllerId,
                      const std::vector<std::string>& keywords = {});

    // Find a card by id across all zones.
    Card*       findCard(ObjectId id)       noexcept;
    const Card* findCard(ObjectId id) const noexcept;

    // Move a card to a destination zone.
    // Per MTG rules a zone change creates a NEW object (new id); the old id
    // becomes invalid. Returns a pointer to the new object.
    Card* moveToZone(ObjectId id, ZoneType destination, uint8_t controllerId);

    // Recompute all S:Mode$ Continuous static ability bonuses on battlefield cards.
    // Called automatically by moveToZone and createToken.  Skips the full recompute
    // when m_staticBonusDirty is false (no battlefield changes since last call).
    void recomputeStaticBonuses();
    void markStaticBonusDirty() noexcept { m_staticBonusDirty = true; }

    // ── Continuous effects (Rule 613 layer engine) ────────────────────────
    // Add a dynamic continuous effect (e.g. "until EOT" pump from Effects.cpp).
    void addContinuousEffect(ContinuousEffect e) {
        e.timestamp = m_effectTimestamp++;
        m_continuousEffects.push_back(std::move(e));
    }

    // Remove all UntilEOT effects — called by TurnManager at the Cleanup step.
    void clearUntilEOTEffects() {
        auto& v = m_continuousEffects;
        v.erase(std::remove_if(v.begin(), v.end(), [](const ContinuousEffect& e) {
            return e.duration == ContinuousEffect::Duration::UntilEOT;
        }), v.end());
    }

    // Mutable access used by LayerEngine::apply() to sort in place.
    std::vector<ContinuousEffect>& continuousEffects() noexcept { return m_continuousEffects; }

    // Per-player commander cast count (commander tax: +{2} per prior cast from command zone).
    // Not reset between turns; reset only by reset()/new game.
    int commanderCastCount[4] = {0, 0, 0, 0};   // indexed by owner player ID

    // Record commander combat damage: damagedPlayer took 'amount' from commanderOwner's commander.
    void recordCommanderDamage(uint8_t damagedPlayer, uint8_t commanderOwner, int amount) noexcept {
        if (amount > 0) m_players[damagedPlayer].addCommanderDamage(commanderOwner, amount);
    }

    // Per-turn spell cast counter — used by Storm.  Incremented by AbilityProcessor::castSpell.
    int  spellsCastThisTurn       = 0;
    // Per-player per-turn spell cast count — used by ActivatorThisTurnCast$ checks.
    int  spellsCastByPlayer[2]    = {0, 0};
    // Per-turn creature death counter — used for Morbid condition.  Incremented by moveToZone.
    int  creaturesDiedThisTurn    = 0;

    // X value hint for ETB counter evaluation (set by AbilityProcessor before resolving).
    int  etbXHint                 = 0;

    // Damage prevention flags (cleared each Cleanup step)
    bool preventAllCombatDamage   = false;
    bool preventAllDamageToPlayer = false;

    // Bloodthirst: tracks whether each player was dealt damage this turn.
    // Index = the player who received damage. Cleared each Cleanup step.
    bool playerDamagedThisTurn[4] = {false, false, false, false};

    // Life gained by each player this turn. Cleared each Cleanup step.
    // Index = player who gained life. Used by Count$LifeYouGainedThisTurn.
    int lifeGainedThisTurn[4] = {0, 0, 0, 0};

    // Life lost by each player this turn. Cleared each Cleanup step.
    // Index = player who lost life. Used by Count$LifeOppsLostThisTurn / Count$LifeYouLostThisTurn.
    int lifeLostThisTurn[4] = {0, 0, 0, 0};

    // Hint for Count$Kicked evaluations: true while a kicked spell is resolving.
    // Set by AbilityProcessor before executing effects; cleared after.
    bool kickedHint    = false;
    // Hint for Bargain: true while a bargained spell is resolving.
    bool bargainedHint = false;

    // Dynamic SVar overrides — set by DB$ StoreSVar at resolution time.
    // Checked first in evaluateSVar() before looking up card rules.
    // Cleared after the outermost effect chain resolves.
    std::unordered_map<std::string, int> dynamicSVars;

    // Hints for Count$RememberedSize / Count$RememberedNumber.
    // Set by Effects.cpp before SVar evaluation; cleared after.
    int rememberedSizeHint   = 0;
    int rememberedNumberHint = 0;

    // Hint for Count$ChosenNumber — set by DB$ ChooseNumber effects.
    int chosenNumberHint = 0;

    // CantGainLife: set by S:Mode$ CantGainLife static ability. Index = player who can't gain life.
    // Cleared in recomputeStaticBonuses and reset.
    bool cantGainLife[4] = {false, false, false, false};

    // "Can't gain life this turn" from a DB$ Effect (Atarka's Command, Sulfuric Vortex
    // variants). Recompute-safe (unlike cantGainLife which is rebuilt); cleared at EOT.
    bool tempCantGainLife[2] = {false, false};

    // "Can't activate non-mana abilities this turn" from a DB$ Effect (Abeyance).
    // Checked in activateAbility; cleared at EOT. (Per-permanent restrictions like
    // Braided Net use Card::tempCantActivate instead.)
    bool tempCantActivate[2] = {false, false};

    // CantPlayLand: set by S:Mode$ CantPlayLand. Index = player forbidden from playing lands.
    // Cleared in recomputeStaticBonuses each turn.
    bool cantPlayLand[4] = {false, false, false, false};

    // Extra land plays granted by S:Mode$ Continuous | AdjustLandPlays$ N
    // (Exploration, Azusa, Oracle of Mul Daya, Dryad of the Ilysian Grove, …).
    // Recomputed every recomputeStaticBonuses(). The per-turn limit is 1 + this.
    int extraLandPlays[4] = {0, 0, 0, 0};

    // Extra land plays granted by a DB$ Effect "play an additional land this turn"
    // (Explore, Summer Bloom). Recompute-safe (extraLandPlays is rebuilt); EOT-cleared.
    int tempExtraLandPlays[2] = {0, 0};

    // "No maximum hand size" override from a DB$ Effect (-1 = use the player's normal
    // max). Read at the controller's Cleanup discard; EOT-cleared.
    int tempMaxHandSize[2] = {-1, -1};

    // Land-play limit / eligibility for a player (base 1 + AdjustLandPlays).
    int landPlayLimit(uint8_t pid) const noexcept {
        return 1 + (pid < 4 ? extraLandPlays[pid] : 0)
                 + (pid < 2 ? tempExtraLandPlays[pid] : 0);
    }
    bool canPlayLand(uint8_t pid) const noexcept {
        if (pid >= 4 || cantPlayLand[pid]) return false;
        return player(pid).landsPlayedThisTurn() < landPlayLimit(pid);
    }

    // Player chosen by DB$ ChoosePlayer. 255 = not set.
    // Cleared by DB$ Cleanup | ClearChosenPlayer$ True.
    uint8_t chosenPlayerHint = 255;

    // Card type chosen by DB$ ChooseType (e.g. "Creature", "Instant", etc.).
    // Used by protection effects. Cleared by DB$ Cleanup.
    std::string chosenTypeName;

    // Color chosen by DB$ ChooseColor (e.g. "White", "Blue", "Black", "Red", "Green").
    // Used by Card.ChosenColor filter. Cleared by DB$ Cleanup | ClearChosenColor$ True.
    std::string chosenColorName;

    // Pending stack modifications applied when the affected spell resolves (resolveTop).
    // Keyed by the spell's stack card id. Set by DB$ ChangeTargets / DB$ ControlSpell.
    //  - pendingRetarget:      value = the redirector's player id (targets aimed at their opponent)
    //  - pendingControlChange: value = the new controller for the spell
    std::unordered_map<ObjectId, uint8_t> pendingRetarget;
    std::unordered_map<ObjectId, uint8_t> pendingControlChange;

    // Hint for Count$DamageAmount — the amount from the most recent damage trigger.
    // Set by AbilityProcessor before executing DamageDone trigger effects; cleared after.
    int triggerAmountHint = 0;

    // Wrapper that gains life AND tracks the amount for Count$LifeYouGainedThisTurn.
    // Returns false if life gain was prevented by S:Mode$ CantGainLife.
    bool gainLife(uint8_t pid, int amount) noexcept {
        if (amount <= 0) return false;
        if (cantGainLife[pid] || tempCantGainLife[pid]) return false;
        m_players[pid].gainLife(amount);
        lifeGainedThisTurn[pid] += amount;
        return true;
    }

    // Wrapper that loses life AND tracks the amount for Count$LifeOppsLostThisTurn.
    void loseLife(uint8_t pid, int amount) noexcept {
        if (amount <= 0) return;
        m_players[pid].loseLife(amount);
        lifeLostThisTurn[pid] += amount;
    }

    // Experience counters: persistent per-player counters that power some commanders.
    // They survive between turns and are never reset (only removed by specific card effects).
    int experienceCounters[4] = {0, 0, 0, 0};
    void addExperience(uint8_t pid, int n = 1) noexcept {
        if (pid < 4) experienceCounters[pid] += n;
    }
    int getExperience(uint8_t pid) const noexcept {
        return (pid < 4) ? experienceCounters[pid] : 0;
    }

    // Prowl tracking: set to true when a creature with Prowl (or any creature sharing
    // a creature type with a Prowl card) deals combat damage to a player this turn.
    // Cleared at end of turn. Per-player: prowlActive[caster].
    bool prowlActive[4] = {false, false, false, false};

    // City's Blessing: granted when a player has had 10+ permanents on the battlefield
    // simultaneously at any point this game. Never revoked once granted.
    bool hasCityBlessing[4] = {false, false, false, false};
    void checkCityBlessing(uint8_t pid) noexcept {
        if (!hasCityBlessing[pid]) {
            int cnt = 0;
            for (const Card* c : m_battlefield.cards())
                if (c->controllerId == pid) ++cnt;
            if (cnt >= 10) hasCityBlessing[pid] = true;
        }
    }

    // Returns true if any spell with Split Second is currently on the stack.
    // While true, players may not cast spells or activate non-mana abilities.
    bool splitSecondOnStack() const noexcept {
        for (const Card* c : m_stack.cards())
            if (c && c->rules && c->rules->hasSplitSecond) return true;
        return false;
    }

    // Companion: a card set aside outside the game that can be put into hand for {3}.
    ObjectId companionId[4] = {kInvalidId, kInvalidId, kInvalidId, kInvalidId};
    bool     companionUsed[4] = {false, false, false, false};  // once per game

    // Epic: once an Epic spell resolves for a player, they can no longer cast spells.
    // The Epic spell's effect copies itself at each upkeep via a T: trigger line.
    bool epicRestriction[4] = {false, false, false, false};

    // Day/Night (Innistrad daybound/nightbound mechanic).
    // 0 = neither day nor night yet  1 = Day  2 = Night
    uint8_t dayNightState = 0;

    // Initiative / Dungeon state (Adventures in the Forgotten Realms and later sets).
    // initiativeHolder: -1 = nobody has the initiative, 0/1 = that player holds it.
    // initiativeRoom: current room index in the Undercity dungeon (0 = start).
    int8_t initiativeHolder = -1;
    int    initiativeRoom   = 0;

    // Monarch: the player who is currently the Monarch (gains a card at EOT, loses it on combat damage).
    // 255 = no one is the Monarch.
    uint8_t monarchPlayer = 255;

    // ControlPlayer (Mindslaver, Sorin Markov, Worst Fears): turnControllerOf[V] = C
    // means player C controls player V's next turn (255 = uncontrolled). A controlled
    // player takes no voluntary actions that turn (enforced in AiPlayer). Consumed at
    // the controlled player's Cleanup step.
    uint8_t turnControllerOf[2] = { 255, 255 };
    bool isTurnControlled(uint8_t pid) const noexcept { return turnControllerOf[pid] != 255; }
    void setTurnController(uint8_t victim, uint8_t controller) noexcept { turnControllerOf[victim] = controller; }
    void clearTurnController(uint8_t pid) noexcept { turnControllerOf[pid] = 255; }

    // Temporary "can't cast" restrictions from DB$ Effect (Silence, Abeyance, Azor):
    // each entry is (restricted player id, ValidCard$ filter). Checked in castSpell;
    // cleared at end of turn.
    std::vector<std::pair<uint8_t, std::string>> tempCantCast;

    // Temporary generic-cost reductions from DB$ Effect (Ballad of the Black Flag,
    // "spells you cast this turn cost less"). Summed by genericReductionFor; cleared
    // at end of turn. (activator 255 = any; validCard "" = all spells.)
    struct TempCostMod { uint8_t activator; std::string validCard; int amount; };
    std::vector<TempCostMod> tempCostMods;

    // Active combat state pointer — set by TurnManager while in a combat phase,
    // nullptr otherwise. Allows Effects to remove creatures from combat.
    CombatState* activeCombat = nullptr;

    // Revolt: tracks whether a permanent a player controlled left the battlefield this turn.
    // Index = the player whose permanent left. Cleared each Cleanup step.
    bool permanentLeftBattlefieldThisTurn[4] = {false, false, false, false};

    // Raid: tracks whether a player attacked this turn.
    // Index = the attacking player. Cleared each Cleanup step.
    bool attackedThisTurn[4] = {false, false, false, false};

    // Dash: cards that entered the battlefield via dash cost and must return to
    // hand at the beginning of the next end step. Cleared after processing.
    std::vector<ObjectId> dashedCards;

    // Miracle: how many cards each player has drawn this turn (reset at Cleanup).
    // Index = the player who drew. Used to mark the first drawn card as miracleEligible.
    int cardsDrawnThisTurn[4] = {0, 0, 0, 0};

    // Unearth: cards that entered via Unearth and must be exiled at the next end step.
    std::vector<ObjectId> unearthedCards;

    // Blitz: cards cast via Blitz cost; sacrificed at the next end step (draw-a-card fires via Dies trigger).
    std::vector<ObjectId> blitzedCards;

    // Connive: pending draw-discard-counter effect for the human player.
    struct PendingConnive {
        bool      active     = false;
        ObjectId  creatureId = kInvalidId;   // the creature gaining counters
        int       amount     = 1;
        int       discardsDone = 0;
        std::vector<ObjectId> nonlandDiscarded;  // filled as player discards
    };
    PendingConnive pendingConnive;
    bool hasPendingConnive() const noexcept { return pendingConnive.active; }

    // Permanents that entered the battlefield this turn (new ObjectId after zone change).
    // Cleared at Cleanup. Used by Count$ThisTurnEntered_Battlefield_<filter>.
    std::vector<ObjectId> permanentsEnteredThisTurn;

    // Evaluate a named SVar from a CardRules. If the SVar body starts with
    // "Count$..." the result is a board-state count; otherwise returns 0.
    // controller is the player who owns the card.
    // selfId: optional source card ID for filters that exclude the source (e.g. Count$Xpower).
    int evaluateSVar(const std::string& svarName, uint8_t controller,
                     const CardRules* rules, ObjectId selfId = kInvalidId) const;

    // Evaluate a raw "Count$..." expression body against the current board state.
    // Used by effect handlers that compute values without going through a named SVar.
    int evaluateCountExpr(std::string_view countExpr, uint8_t controller) const;

    // Overload with an explicit selfId for filters that exclude the source card
    // (e.g. Count$Xpower, Count$OtherCreatureYou with Card.Self exclusion).
    int evaluateCountExpr(std::string_view countExpr, uint8_t controller, ObjectId selfId) const;

    // Saga: add one lore counter and execute the corresponding chapter.
    // Sacrifices the Saga after the last chapter. Pointer may become invalid.
    void triggerSagaChapter(Card* saga);

    // ── Card database (optional; enables MakeCard effects) ───────────────
    void setCardDb(const CardDb* db) noexcept { m_cardDb = db; }
    const CardRules* findRules(std::string_view name) const noexcept {
        return m_cardDb ? m_cardDb->find(name) : nullptr;
    }

    // Returns true if any battlefield static ability (S:Mode$ CastWithFlash) grants
    // Flash to the given card for the purposes of when it may be cast.
    bool hasFlashGrant(const Card& c) const noexcept;

    // ── Emblems ───────────────────────────────────────────────────────────
    // Lightweight emblem: a named source of continuous triggers/static effects
    // owned by a player, placed in the command zone conceptually.
    struct Emblem {
        std::string  sourceName;      // planeswalker that created it (for display)
        uint8_t      controllerId = 0;
        std::vector<std::string> triggerLines;  // T: lines granting ongoing effects
        std::vector<std::string> staticLines;   // S: lines
    };
    const std::vector<Emblem>& emblems() const noexcept { return m_emblems; }
    void addEmblem(Emblem e) { m_emblems.push_back(std::move(e)); }

    // ── Randomness ────────────────────────────────────────────────────────
    std::mt19937& rng() noexcept { return m_rng; }
    void seedRng(uint32_t seed)  { m_rng.seed(seed); }

    // ── Clone (for AI simulation / Monte Carlo) ───────────────────────────
    // Returns a deep copy of this game state.  All Card objects are cloned;
    // zones are rebuilt to point at the new copies.  Token rules are also
    // deep-copied and raw CardRules* pointers are remapped to the new list.
    // The clone has isHumanInteractive = false (simulation mode).
    GameState clone() const;

    // Copy and move semantics — delegates to clone() so all deep-copy logic
    // stays in one place.  Move operators are explicitly defaulted because
    // declaring any copy member suppresses compiler-generated moves.
    GameState(const GameState& o) : GameState(o.clone()) {}
    GameState& operator=(const GameState& o) { if (this != &o) *this = o.clone(); return *this; }
    GameState(GameState&&)            = default;
    GameState& operator=(GameState&&) = default;

    // ── Debug ─────────────────────────────────────────────────────────────
    void printState() const;

private:
    uint8_t               m_numPlayers = 2;   // 2 = 1v1; 4 = Commander pod
    std::array<Player, 4> m_players;          // seats 0-3; seats 2-3 unused in 2-player

    Zone m_battlefield{ ZoneType::Battlefield };
    Zone m_exile      { ZoneType::Exile       };
    Zone m_stack      { ZoneType::Stack       };
    Zone m_command    { ZoneType::Command     };

    int     m_turnNumber       = 1;
    uint8_t m_activePlayerId   = 0;
    uint8_t m_priorityPlayerId = 0;

    std::unordered_map<ObjectId, std::unique_ptr<Card>> m_objects;

    // Card objects that have left their last zone (moved to a new ObjectId or removed).
    // We keep these alive instead of freeing immediately so any in-flight iteration
    // holding a raw Card* (e.g. a snapshot of m_battlefield) cannot dereference freed
    // memory. Cleared by reset(); accumulates harmlessly over the course of a game.
    std::vector<std::unique_ptr<Card>> m_dyingObjects;

    ObjectId m_nextId = 1;
    uint32_t m_zoneSeq = 0;  // monotonically increasing, stamped on every zone change

    // Dirty flag: recomputeStaticBonuses() is a no-op until this is true.
    // Set by moveToZone(), createToken(), and markStaticBonusDirty().
    // Cleared at the end of recomputeStaticBonuses().
    mutable bool m_staticBonusDirty = true;

    // Owns CardRules for tokens (must outlive the Card objects that point to them)
    std::list<CardRules> m_tokenRules;

    // Pool of cloned CardRules (from Clone/Transform/Copy effects).
    // Stored here so they persist for the entire game — individual Card objects
    // that clone rules get freed as cards change zones, but the rules themselves
    // must remain alive as long as ANY card or pending effect references them.
    // This prevents use-after-free when ctx.source moves mid-effect-chain.
    std::vector<std::shared_ptr<CardRules>> m_ownedRulesPool;

    // Register a cloned CardRules and return the raw pointer. The pool keeps
    // the object alive for the lifetime of the GameState.
    const CardRules* internOwnedRules(std::shared_ptr<CardRules> rules) noexcept {
        const CardRules* ptr = rules.get();
        m_ownedRulesPool.push_back(std::move(rules));
        return ptr;
    }

    // Active emblems (copied with the game state; controller index is preserved)
    std::vector<Emblem> m_emblems;

    std::string m_pendingReveal;

    // Optional pointer to the card database (set via setCardDb; not owned).
    // Used by MakeCard effects and similar "create from nowhere" effects.
    const CardDb* m_cardDb = nullptr;

    // Rule 613 continuous effects active in this game state.
    // Populated by addContinuousEffect(); drained by clearUntilEOTEffects() at Cleanup.
    // Plain-value vector — no pointers inside, so clone() copies it directly.
    std::vector<ContinuousEffect> m_continuousEffects;
    uint32_t m_effectTimestamp = 0;

    // Triggers that have fired and are waiting to be drained by AbilityProcessor
    std::vector<PendingTrigger> m_pendingTriggers;

public:
    // Drain all queued triggers — called by AbilityProcessor before giving priority
    std::vector<PendingTrigger> drainTriggers() {
        return std::move(m_pendingTriggers);
    }

    // Returns the name of a morph revealed this step, then clears it.
    std::string drainPendingReveal() noexcept {
        return std::move(m_pendingReveal);
    }

    // Exploit: when a creature with Exploit ETBs, the controller may sacrifice a creature.
    // If a creature was sacrificed, the card's bonus T: trigger fires (exploited flag on card).
    struct PendingExploitChoice {
        bool     active      = false;
        ObjectId exploiterId = kInvalidId;
        uint8_t  controller  = 0;
    };
    PendingExploitChoice m_pendingExploit;
    bool hasPendingExploit() const noexcept { return m_pendingExploit.active; }
    const PendingExploitChoice& pendingExploit() const noexcept { return m_pendingExploit; }
    void setPendingExploit(ObjectId id, uint8_t ctrl) noexcept {
        m_pendingExploit = {true, id, ctrl};
    }
    void clearPendingExploit() noexcept { m_pendingExploit.active = false; }

    // Tribute: when a creature with Tribute ETBs, the opponent may put N +1/+1 counters on it.
    // If they do (tributed=true), the "not paid" bonus effect doesn't fire.
    struct PendingTributeChoice {
        bool     active  = false;
        ObjectId cardId  = kInvalidId;
        int      amount  = 0;
        uint8_t  chooser = 1;   // the player making the choice (opponent of the controller)
    };
    PendingTributeChoice m_pendingTribute;
    bool hasPendingTribute() const noexcept { return m_pendingTribute.active; }
    const PendingTributeChoice& pendingTribute() const noexcept { return m_pendingTribute; }
    void setPendingTribute(ObjectId id, int amt, uint8_t chooser) noexcept {
        m_pendingTribute = {true, id, amt, chooser};
    }
    void clearPendingTribute() noexcept { m_pendingTribute.active = false; }

    // Phyrexian mana payment choice: human chooses per-shard whether to pay life or mana.
    // Set by payCost when a Phyrexian shard is encountered and controller == 0 (human).
    struct PendingPhyrexianChoice {
        bool    active     = false;
        int     numShards  = 0;   // how many Phyrexian shards need a decision
        int     lifePayCount = 0; // how many the human chose to pay with life (set by UI)
    };
    PendingPhyrexianChoice m_pendingPhyrexian;
    bool hasPendingPhyrexian() const noexcept { return m_pendingPhyrexian.active; }
    const PendingPhyrexianChoice& pendingPhyrexian() const noexcept { return m_pendingPhyrexian; }
    void setPendingPhyrexian(int shards) noexcept { m_pendingPhyrexian = {true, shards, 0}; }
    void clearPendingPhyrexian() noexcept { m_pendingPhyrexian.active = false; }

    // Miracle: human may cast the revealed card at its miracle cost immediately.
    struct PendingMiracleChoice {
        bool     active = false;
        ObjectId cardId = kInvalidId;
        uint8_t  controller = 0;
    };
    PendingMiracleChoice m_pendingMiracle;
    bool hasPendingMiracle() const noexcept { return m_pendingMiracle.active; }
    const PendingMiracleChoice& pendingMiracle() const noexcept { return m_pendingMiracle; }
    void setPendingMiracle(ObjectId id, uint8_t ctrl) noexcept {
        m_pendingMiracle = {true, id, ctrl};
    }
    void clearPendingMiracle() noexcept { m_pendingMiracle.active = false; }

    // Proliferate: human chooses which permanents/players to give an extra counter.
    struct PendingProliferateChoice {
        bool     active    = false;
        int      times     = 0;    // how many proliferate applications remain
        // Selected IDs (permanents to proliferate); empty = not yet chosen
        std::vector<ObjectId> selectedIds;
        bool includePoison[4] = {false,false,false,false};
    };
    PendingProliferateChoice m_pendingProliferate;
    bool hasPendingProliferate() const noexcept { return m_pendingProliferate.active; }
    PendingProliferateChoice& pendingProliferate() noexcept { return m_pendingProliferate; }
    const PendingProliferateChoice& pendingProliferate() const noexcept { return m_pendingProliferate; }
    void setPendingProliferate(int times) noexcept {
        m_pendingProliferate = {true, times, {}, {}};
    }
    void clearPendingProliferate() noexcept { m_pendingProliferate.active = false; }
    // Apply the chosen proliferate selections
    void resolveProliferateChoice() {
        auto& pc = m_pendingProliferate;
        for (ObjectId id : pc.selectedIds) {
            Card* c = findCard(id);
            if (!c || c->counters.empty()) continue;
            for (auto& [type, cnt] : c->counters)
                if (cnt > 0) ++cnt;
        }
        for (uint8_t i = 0; i < m_numPlayers; ++i) {
            if (pc.includePoison[i] && m_players[i].poisonCounters() > 0)
                m_players[i].addPoison(1);
        }
        --pc.times;
        pc.selectedIds.clear();
        for (auto& b : pc.includePoison) b = false;
        if (pc.times <= 0) pc.active = false;
    }

    // Cascade choice: human player must decide whether to cast the revealed card.
    struct PendingCascadeChoice {
        bool     active     = false;
        ObjectId cardId     = kInvalidId;
        uint8_t  controller = 0;
        int      maxCmc     = 0;
    };
    PendingCascadeChoice m_pendingCascade;  // lives here so struct is in scope
    bool hasPendingCascade() const noexcept { return m_pendingCascade.active; }
    const PendingCascadeChoice& pendingCascade() const noexcept { return m_pendingCascade; }
    void setPendingCascade(ObjectId id, uint8_t ctrl, int maxCmc) noexcept {
        m_pendingCascade = {true, id, ctrl, maxCmc};
    }
    void clearPendingCascade() noexcept { m_pendingCascade.active = false; }
    bool hasPendingTriggers() const noexcept { return !m_pendingTriggers.empty(); }

    // True if any pending trigger was queued from the given source card.
    // Used by checkAlwaysTriggers to avoid double-queuing.
    bool hasPendingTriggerFrom(ObjectId id) const noexcept {
        for (const auto& t : m_pendingTriggers)
            if (t.sourceCardId == id) return true;
        return false;
    }

    // Queue a batch of triggers (called by TriggerSystem on attacks etc.)
    void queueTriggers(std::vector<PendingTrigger>&& v) {
        for (auto& t : v) m_pendingTriggers.push_back(std::move(t));
    }

    // Riot: ETB choice pending for human player (Haste vs +1/+1 counter)
    bool     hasPendingRiot()    const noexcept { return m_pendingRiot.active; }
    ObjectId pendingRiotCardId() const noexcept { return m_pendingRiot.cardId; }
    void setPendingRiot(ObjectId id) noexcept   { m_pendingRiot = {true, id}; }
    void clearPendingRiot()      noexcept       { m_pendingRiot = {}; }

    // ChooseType: ETB "choose a creature type" pending for the human player.
    bool     hasPendingChooseType() const noexcept { return m_pendingChooseType.active; }
    const PendingChooseTypeChoice& pendingChooseType() const noexcept { return m_pendingChooseType; }
    void setPendingChooseType(ObjectId id, std::vector<std::string> opts) {
        m_pendingChooseType = {true, id, std::move(opts)};
    }
    void clearPendingChooseType() noexcept { m_pendingChooseType = {}; }

    // "Pay N life or enters tapped" (shock lands): ETB choice pending for human.
    bool     hasPendingPayLife()    const noexcept { return m_pendingPayLife.active; }
    ObjectId pendingPayLifeCardId() const noexcept { return m_pendingPayLife.cardId; }
    int      pendingPayLifeAmount() const noexcept { return m_pendingPayLife.amount; }
    uint8_t  pendingPayLifePayer()  const noexcept { return m_pendingPayLife.payer; }
    void setPendingPayLife(ObjectId id, int amount, uint8_t payer) noexcept {
        m_pendingPayLife = {true, id, amount, payer};
    }
    void clearPendingPayLife() noexcept { m_pendingPayLife = {}; }

    // Fabricate: ETB choice pending for human player (+1/+1 counters vs Servo tokens)
    bool     hasPendingFabricate()      const noexcept { return m_pendingFabricate.active; }
    ObjectId pendingFabricateCardId()   const noexcept { return m_pendingFabricate.cardId; }
    int      pendingFabricateAmount()   const noexcept { return m_pendingFabricate.amount; }
    void setPendingFabricate(ObjectId id, int n) noexcept { m_pendingFabricate = {true, id, n}; }
    void clearPendingFabricate()        noexcept       { m_pendingFabricate = {}; }

    // Charm: mode selection pending for human player
    bool                      hasPendingCharm() const noexcept { return m_pendingCharm.active; }
    const PendingCharmChoice& pendingCharm()    const noexcept { return m_pendingCharm; }
    void setPendingCharm(PendingCharmChoice c)        noexcept { m_pendingCharm = std::move(c); }
    void clearPendingCharm()                          noexcept { m_pendingCharm = {}; }

    // Madness: cards that went to Exile via the Madness replacement effect and need a
    // cast-or-GY decision. { cardId in exile, controller }.
    void queueMadnessCast(ObjectId id, uint8_t ctrl) {
        m_pendingMadnessCasts.push_back({id, ctrl});
    }
    bool hasPendingMadnessCasts() const noexcept { return !m_pendingMadnessCasts.empty(); }
    std::vector<std::pair<ObjectId,uint8_t>> drainMadnessCasts() {
        return std::move(m_pendingMadnessCasts);
    }

    // Human-player madness decision overlay (one at a time).
    bool                       hasPendingMadnessCast()     const noexcept { return m_pendingMadnessCast.active; }
    const PendingMadnessCast&  pendingMadnessCast()        const noexcept { return m_pendingMadnessCast; }
    void setPendingMadnessCast(ObjectId id, uint8_t ctrl, const std::string& name) noexcept {
        m_pendingMadnessCast = {true, id, ctrl, name};
    }
    void clearPendingMadnessCast() noexcept { m_pendingMadnessCast = {}; }

    // Library search (tutor) — deferred for human player to pick interactively
    bool hasPendingSearch() const noexcept { return m_pendingSearch.active; }
    const PendingLibrarySearch& pendingSearch() const noexcept { return m_pendingSearch; }
    void setPendingSearch(uint8_t libPlayer, ZoneType dest,
                          uint8_t destCtrl, const std::string& filter) noexcept {
        m_pendingSearch = {true, libPlayer, dest, destCtrl, filter};
    }
    void clearPendingSearch() noexcept { m_pendingSearch = {}; }

    // Discard: forced discard waiting for human to choose card(s)
    bool hasPendingDiscard()    const noexcept { return m_pendingDiscard.active; }
    int  pendingDiscardCount()  const noexcept { return m_pendingDiscard.numCards; }
    void setPendingDiscard(int n) noexcept {
        m_pendingDiscard = {true, n};
    }
    void clearPendingDiscard() noexcept { m_pendingDiscard = {}; }
    void decrementPendingDiscard() noexcept {
        if (m_pendingDiscard.numCards > 0) --m_pendingDiscard.numCards;
        if (m_pendingDiscard.numCards <= 0) m_pendingDiscard.active = false;
    }

    // True when running with a UI (GameWindow sets this).
    // When false, library searches auto-pick the first match (tests / AI mode).
    bool isHumanInteractive() const noexcept { return m_humanInteractive; }
    void setHumanInteractive(bool v) noexcept { m_humanInteractive = v; }

    // Combo mana choice (Temple-style "Add {B} or {G}") — human selects which
    // colour to add to the pool. AI side resolves synchronously inside
    // effectMana so this overlay never fires for them.
    struct PendingManaChoice {
        bool        active     = false;
        uint8_t     controller = 0;
        std::string colors;        // e.g. "BG" → buttons for {B} and {G}
        int         amount     = 1;
    };
    bool hasPendingManaChoice() const noexcept { return m_pendingManaChoice.active; }
    const PendingManaChoice& pendingManaChoice() const noexcept { return m_pendingManaChoice; }
    void setPendingManaChoice(uint8_t ctrl, const std::string& colors, int amt) noexcept {
        m_pendingManaChoice = {true, ctrl, colors, amt};
    }
    void clearPendingManaChoice() noexcept { m_pendingManaChoice = {}; }

    // Scry overlay — human reviews top-N cards one at a time and chooses
    // Top (keep on top, in original order) or Bottom for each. After all are
    // decided, kept cards return to the top and bottomed cards go to the
    // bottom. AI keeps the existing in-line heuristic.
    struct PendingScry {
        bool                   active     = false;
        uint8_t                controller = 0;
        std::vector<ObjectId>  looking;     // cards still awaiting decision
        std::vector<ObjectId>  keepTop;     // chosen to stay on top
        std::vector<ObjectId>  putBottom;   // chosen to go to bottom
    };
    bool hasPendingScry() const noexcept { return m_pendingScry.active; }
    const PendingScry& pendingScry() const noexcept { return m_pendingScry; }
    PendingScry&       pendingScry()       noexcept { return m_pendingScry; }
    void setPendingScry(uint8_t ctrl, std::vector<ObjectId> ids) noexcept {
        m_pendingScry = {true, ctrl, std::move(ids), {}, {}};
    }
    void clearPendingScry() noexcept { m_pendingScry = {}; }

private:
    std::vector<std::pair<ObjectId,uint8_t>> m_pendingMadnessCasts;
    PendingMadnessCast     m_pendingMadnessCast;
    PendingLibrarySearch m_pendingSearch;
    PendingRiotChoice      m_pendingRiot;
    PendingChooseTypeChoice m_pendingChooseType;
    PendingPayLifeChoice   m_pendingPayLife;
    PendingFabricateChoice m_pendingFabricate;
    PendingCharmChoice     m_pendingCharm;
    PendingDiscardChoice m_pendingDiscard;
    PendingManaChoice    m_pendingManaChoice;
    PendingScry          m_pendingScry;
    bool m_humanInteractive = false;

    std::mt19937 m_rng{ std::random_device{}() };

    ObjectId allocId() noexcept { return m_nextId++; }

    // Remove a card from whatever zone it currently occupies.
    void removeFromCurrentZone(const Card& card);
};

} // namespace mtg
