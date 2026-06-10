#pragma once
#include "../GameState.h"
#include "../TurnManager.h"
#include "../ability/AbilityProcessor.h"
#include "../ability/Target.h"
#include "ComboDatabase.h"
#include "MctsSearch.h"
#include "SaltDatabase.h"
#include "ValueNetInference.h"
#include <chrono>
#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>

namespace mtg {

struct CardRules;
struct ScriptLine;

// Heuristic AI that drives one player's decisions each turn.
//
// Priorities (in order):
//   Mana    — tap all available mana sources at start of main phase
//   Lands   — play one land per turn
//   Casting — cast the highest-CMC affordable spell; kill threats before
//              committing damage spells to face
//   Attack  — swing with every creature that can attack
//   Block   — block when we can kill the attacker (trade up) or must survive
// Set to true (via CITADEL_QUIET env var or at startup) to suppress verbose AI output.
inline bool g_aiQuiet = false;

class AiPlayer {
public:
    AiPlayer(uint8_t id, GameState& game, AbilityProcessor& abilities);

    // Run a complete turn (Untap … Cleanup).
    // Pass the opposing AiPlayer for blocking decisions; nullptr skips blocking.
    // Returns true if the game ended during this turn.
    bool takeTurn(TurnManager& tm, AiPlayer* defender = nullptr);

    // Declare blockers for the current combat (called by the attacker's takeTurn).
    void declareBlockers(TurnManager& tm);

    uint8_t id() const noexcept { return m_id; }

    // Rebind which seat this AiPlayer acts as. 1v1 only ever calls this
    // once (id stays fixed at 1), but 3/4-player games reuse a single
    // AiPlayer instance and re-bind it to the current activePlayerId
    // before each AI-controlled turn / combat. Heuristics that rely on
    // a single "the opponent" still work — they target the next seat,
    // which is fine for the way our AI evaluates positions.
    void setId(uint8_t id) noexcept { m_id = id; }

    // These are public so GameWindow can reuse them when driving Bob's turn
    // step-by-step (needed to pause at DeclareBlockers for Alice's input).
    bool tryPlayLand();
    bool tryCastBestSpell();
    // Once-per-game: bring the set-aside companion into hand for {3} when affordable.
    bool tryActivateCompanion();
    bool tryActivateAbilities();
    bool tryActivateGraveyardAbilities();
    bool tryActivateMonstrosity();
    void tryActivateOutlastAdapt();
    void tryUnlockRooms();
    bool tryForetell();
    bool tryPlayImpulseFromExile();  // play impulse-draw cards from exile (MayPlay)
    bool trySuspend();
    bool tryEquipEquipment();
    void doAttackers(TurnManager& tm);
    void tryActivatePlaneswalkers();

    // After blockers are declared, activate Ninjutsu for any affordable Ninja in hand
    // targeting an unblocked attacker. Returns true if a Ninja entered the battlefield.
    bool tryNinjutsu(TurnManager& tm);

    // Activate Level Up for a creature if affordable (sorcery speed).
    // Returns true if any creature leveled up.
    bool tryActivateLevelUp();

    // Cycle a card from hand if the cycling cost can be paid and the card
    // cannot be cast this turn.  Returns true if a card was cycled.
    bool tryCycle();

    // Turn face-up any face-down morph/megamorph creatures when affordable.
    // Returns true if any creature was turned face up.
    bool tryActivateMorph();

    // Cast the highest-value instant or Flash card from hand (or GY via Flashback)
    // as a response during the opponent's priority window.  Taps available mana
    // first; returns true if a spell was pushed onto the stack.
    bool tryCastInstant();

    // ── MCTS ──────────────────────────────────────────────────────────────────
    // Enable MCTS for spell sequencing, attacker selection, and blocker assignment.
    // When disabled (default), the existing heuristic logic is used.
    void enableMcts(bool on, const MctsConfig& cfg = {}) noexcept {
        m_mctsEnabled = on; m_mctsConfig = cfg;
    }
    // Set callbacks by reference — AiPlayer stores a raw pointer to the caller's
    // std::function. The caller (GameRunner) must outlive this AiPlayer (true by
    // construction: AiPlayers are stack-local to runEpisode/runGame).
    //
    // Why pointer rather than copy: each std::function holds a lambda capturing a
    // py::object (the Python predict callback). Copying the std::function copies
    // the captured py::object, which calls Py_INCREF. Worker threads run with the
    // GIL released for performance (py::gil_scoped_release in the bindings); a
    // copy in that thread fails the GIL-held assertion and aborts training.
    void setValueFn(const ValueFn& fn)         noexcept { m_valueFn      = &fn; }
    void setPolicyFn(const PolicyFn& fn)       noexcept { m_policyFn     = &fn; }
    void setBatchValueFn(const BatchValueFn& fn) noexcept { m_batchValueFn = &fn; }

    // Attach an episode output buffer; MCTS will append visit distributions as
    // policy targets for each decision step.
    struct EpisodeOut {
        std::vector<std::vector<float>> policy;
    };
    void setEpisodeOutput(EpisodeOut* ep) noexcept { m_epOut = ep; }

    // ── Debug logging ─────────────────────────────────────────────────────────
    // Attach a stream for loop-guard warnings and phase tracing.
    // Caller owns the stream lifetime; nullptr disables logging.
    void setDebugLog(std::ostream* log) { m_debugLog = log; }

    // ── Per-turn time limit ───────────────────────────────────────────────────
    // If a single takeTurn() call runs longer than limitMs wall-clock ms,
    // all inner loops abort and takeTurn() returns true (game ends as draw).
    // 0 = no limit (default).
    void setTurnTimeLimit(long long limitMs) { m_turnLimitMs = limitMs; }

    // Returns true if the turn deadline has passed (used by inner loop checks).
    bool turnExpired() const noexcept;


    // ── Combo system ──────────────────────────────────────────────────────────
    // Load combo definitions from a JSON file. Call once at startup; shared by
    // all AiPlayer instances for the lifetime of the process.
    static void loadCombos(const std::string& path);
    // Download combos from Commander Spellbook + cache to `path`, then load.
    // Returns the number of combos written (0 = failure / not on Windows).
    static int  downloadCombos(const std::string& path);
    // Process-wide combo entry count (0 if not loaded). Used by the main-menu
    // AI-data overlay to show "N combos loaded".
    static int  combosLoaded() noexcept;
    static void loadValueNet(const std::string& path);
    static bool hasValueNet() noexcept;
    float netEval() const;

    // Static board-position score from the perspective of player m_id.
    // Positive = favourable, negative = unfavourable.
    int staticEval() const;

    // 1-ply lookahead: clone the game, cast the spell in the sim, resolve it,
    // and return staticEval() of the resulting position.
    int lookaheadScore(ObjectId id, const std::vector<Target>& targets,
                       const ManaCost& cost) const;

    // Tap ALL untapped mana sources controlled by this player.
    // Public so GameWindow can tap before giving the AI an instant-response window.
    void tapAllMana();

    // Returns true if this creature should attack (not suicidal given opponents' blockers).
    bool shouldAttack(const Card& attacker) const;

private:
    uint8_t           m_id;
    GameState&        m_game;
    AbilityProcessor& m_abilities;
    std::ostream*     m_debugLog   = nullptr;
    long long         m_turnLimitMs = 0;
    std::chrono::steady_clock::time_point m_turnDeadline;

    static ComboDatabase         s_combos;
    static ValueNetInference     s_valueNet;
    // Salt database is process-wide; see SaltDatabase::load / fetchAndCache.

    bool       m_mctsEnabled = false;
    MctsConfig m_mctsConfig;
    // Raw pointers to GameRunner-owned std::functions (see setValueFn doc).
    // Default to a process-wide static empty std::function so MCTS code that takes
    // const ValueFn& can dereference unconditionally.
    static const ValueFn      s_emptyValueFn;
    static const PolicyFn     s_emptyPolicyFn;
    static const BatchValueFn s_emptyBatchValueFn;
    const ValueFn*    m_valueFn      = &s_emptyValueFn;
    const PolicyFn*   m_policyFn     = &s_emptyPolicyFn;
    const BatchValueFn* m_batchValueFn = &s_emptyBatchValueFn;
    EpisodeOut* m_epOut = nullptr;
    std::unique_ptr<MctsNode> m_mctsRoot;  // persisted tree root across main-phase calls
    MctsSearch                m_mctsSearch;    // persisted searcher (keeps transposition table warm)

    Player&       me()        { return m_game.player(m_id); }
    Player&       opp()       { return m_game.player(m_id ^ 1); }
    const Player& me()  const { return m_game.player(m_id); }
    const Player& opp() const { return m_game.player(m_id ^ 1); }

    // ── Mana ──────────────────────────────────────────────────────────────
    bool canAfford(const ManaCost& cost) const;
    // True when (manaPool + untapped land count) >= cost.cmc().
    // Used by tryCastInstant() to avoid calling tapAllMana() unnecessarily.
    // `spell` (optional) is the creature/spell being paid for, so restricted-mana
    // producers (Secluded Courtyard…) count their colours only when the spell
    // actually matches their RestrictValid clause.
    bool canAffordWithUntapped(const ManaCost& cost, const mtg::Card* spell = nullptr) const;
    // Tap just enough mana sources to reach neededTotal in the pool.
    void tapForCost(int neededTotal);
    // Color-aware: tap colored lands first, then generics, to satisfy the cost.
    // `spell` lets restricted-mana lines be tapped (and counted) for a match.
    void tapForManaCost(const ManaCost& cost, const mtg::Card* spell = nullptr);
    // Like tapForCost but leaves reserveCmc lands untapped (hold-up for instants).
    void tapForCostReserving(int neededTotal, int reserveCmc);
    // Returns CMC of cheapest instant/flash in hand, 0 if none.
    int  bestInstantCmcInHand() const;
    // Pick which AB$ Mana line on a multi-line source to fire given the
    // current colored needs. See AiPlayer.cpp for the scoring rules. `spell`
    // makes a RestrictValid line attractive only when the spell matches it.
    int  pickManaLineFor(const mtg::Card& src, uint8_t neededColors,
                          bool genericOk, const mtg::Card* spell = nullptr) const;

    // ── Combo execution ──────────────────────────────────────────────────
    // Try to execute a detected combo. Returns true if at least one step fired.
    bool executeCombo(const ComboEntry& combo);

    // Find the first card with the given name in own hand (nullptr if missing).
    Card* findInHandByName(std::string_view name) const;
    // Find the first card with the given name on the battlefield that we control.
    Card* findOnBattlefieldByName(std::string_view name) const;
    // Find the best own creature to equip to (highest P/T, not already equipped
    // with this equipment, not protected by Hexproof/Shroud, not tapped).
    Card* findBestEquipTarget(ObjectId equipId) const;

    // ── Casting (public for testing) ──────────────────────────────────────
public:
    int    rateSpell   (const Card& c)              const; // casting priority score
private:
    std::vector<Target> pickTargets(const CardRules& rules) const;
    Target pickDamageTarget(int damage)             const;
    Target pickDestroyTarget()                      const;
    Target pickBestTarget(std::string_view filter)  const; // respects ValidTgts filter

    // ── Multi-player ─────────────────────────────────────────────────────
    uint8_t primaryOpponentId() const;  // most-threatening opponent in 2/4-player

    // ── Threat modeling ───────────────────────────────────────────────────
    float assessOpponentThreatLevel() const;  // [1.0, 3.0] how dangerous the opp looks
    bool  detectOpponentCombo() const;         // true if opp seems to be assembling a combo

    // ── Evaluation helpers ────────────────────────────────────────────────
    // Score a single permanent from the perspective of its controller.
    // Used by staticEval(); factored out to allow reuse in lookahead paths.
    int  evaluatePermanent(const Card& c) const;
    // Bonus score for combat keywords on a creature (Flying, Deathtouch, etc.).
    int  keywordBonus(const Card& c)      const;
};

} // namespace mtg
