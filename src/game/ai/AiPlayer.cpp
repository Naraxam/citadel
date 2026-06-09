#include "AiPlayer.h"
// Quiet-mode output helper: respects the CITADEL_QUIET env-var flag.
#define AIOUT if (!g_aiQuiet) std::cout
#include "ComboDatabase.h"
#include "SaltDatabase.h"
// encodeStateForNet is defined in GameRunner.cpp; declare here to avoid circular
// include (GameRunner.h → AiPlayer.h → circular).
namespace mtg { class GameState; }
namespace mtg { std::array<float,268> encodeStateForNet(const GameState&, uint8_t); }
#include "../CardStats.h"
#include "../TurnStep.h"
#include "../TurnManager.h"
#include "../KeywordAbility.h"
#include "../CardFilter.h"
#include "../StateBasedActions.h"
#include "../EquipSystem.h"
#include "../TriggerSystem.h"
#include "../ability/ScriptLine.h"
#include "../../core/mana/ManaCost.h"
#include <algorithm>
#include <charconv>
#include <unordered_set>
#include <climits>
#include <iostream>
#include <ostream>

namespace mtg {

// â”€â”€ Static combo database shared across all AiPlayer instances â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
ComboDatabase     AiPlayer::s_combos;
ValueNetInference AiPlayer::s_valueNet;
const ValueFn      AiPlayer::s_emptyValueFn{};
const PolicyFn     AiPlayer::s_emptyPolicyFn{};
const BatchValueFn AiPlayer::s_emptyBatchValueFn{};

void AiPlayer::loadCombos(const std::string& path) {
    s_combos.loadFromJson(path);
}

int AiPlayer::combosLoaded() noexcept {
    return s_combos.size();
}

int AiPlayer::downloadCombos(const std::string& path) {
    return s_combos.downloadFromSpellbook(path);
}

void AiPlayer::loadValueNet(const std::string& path) {
    s_valueNet.load(path);
    if (s_valueNet.loaded())
        std::cout << "[AiPlayer] Neural value net ready — MCTS will use learned evaluation.\n";
}

bool AiPlayer::hasValueNet() noexcept {
    return s_valueNet.loaded();
}

float AiPlayer::netEval() const {
    if (!s_valueNet.loaded()) {
        int raw = staticEval();
        return std::tanh(static_cast<float>(raw) / 5000.f);
    }
    auto stateVec = encodeStateForNet(m_game, m_id);
    return s_valueNet.predict(stateVec.data());
}

AiPlayer::AiPlayer(uint8_t id, GameState& game, AbilityProcessor& abilities)
    : m_id(id), m_game(game), m_abilities(abilities)
{}

bool AiPlayer::turnExpired() const noexcept {
    if (m_turnLimitMs <= 0) return false;
    return std::chrono::steady_clock::now() >= m_turnDeadline;
}

// â”€â”€ Main turn driver â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

bool AiPlayer::takeTurn(TurnManager& tm, AiPlayer* defender) {
    // Arm the per-turn deadline.
    if (m_turnLimitMs > 0)
        m_turnDeadline = std::chrono::steady_clock::now() +
                         std::chrono::milliseconds(m_turnLimitMs);

    // Advance through one step, running its automatic begin/end actions.
    auto autoStep = [&]() -> bool {
        tm.beginStep();
        tm.endStep();
        if (tm.isGameOver()) return true;
        tm.advanceStep();
        return false;
    };

    if (turnExpired()) {
        if (m_debugLog)
            *m_debugLog << "  WARN: turn expired before start p=" << (int)m_id << "\n";
        return true;
    }

    // Per-phase checkpoint logger â€” pinpoints which call crashes (only when debug log is on)
    auto phaseLog = [&](const char* tag) {
        if (!m_debugLog) return;
        *m_debugLog << "    [p" << (int)m_id << "-" << tag << "]\n";
        m_debugLog->flush();
    };

    // Untap â€” automatic, no triggers
    phaseLog("untap");
    if (autoStep()) return true;
    phaseLog("untap-done");

    // Upkeep â€” process suspend/rebound/echo/cumulative, then fire phase triggers
    phaseLog("upkeep");
    tm.beginStep();
    m_abilities.processSuspendUpkeep(m_game.activePlayerId());
    m_abilities.processReboundUpkeep(m_game.activePlayerId());
    m_abilities.processEchoUpkeep(m_game.activePlayerId());
    m_abilities.processCumulativeUpkeep(m_game.activePlayerId());

    // Forecast: reveal cards with K:Forecast from hand during upkeep for their effect
    {
        Player& p = m_game.player(m_id);
        ManaPool& pool = p.manaPool();
        for (const Card* c : p.hand().cards()) {
            if (!c->rules->hasForecast) continue;
            if (pool.total() < c->rules->forecastCost.cmc()) continue;
            // Simple AI: always activate Forecast if affordable
            m_abilities.activateAbility(c->id, 0, m_id, {});
            break; // only one Forecast per upkeep
        }
    }
    m_abilities.firePhaseTriggersAndDrain(TurnStep::Upkeep, m_game.activePlayerId());
    if (tm.isGameOver()) { tm.endStep(); return true; }
    tm.endStep(); if (tm.isGameOver()) return true; tm.advanceStep();
    phaseLog("upkeep-done");

    // Draw â€” fire triggers then draw
    phaseLog("draw");
    tm.beginStep();
    m_abilities.firePhaseTriggersAndDrain(TurnStep::Draw, m_game.activePlayerId());
    if (tm.isGameOver()) { tm.endStep(); return true; }
    tm.endStep(); if (tm.isGameOver()) return true; tm.advanceStep();
    phaseLog("draw-done");

    // Pre-Combat Main Phase
    phaseLog("main1");
    tm.beginStep();
    m_abilities.firePhaseTriggersAndDrain(TurnStep::PreCombatMain, m_game.activePlayerId());
    if (tm.isGameOver()) { tm.endStep(); return true; }
    {
        phaseLog("main1-land");
        // Play lands up to the per-turn limit (Exploration/Azusa grant extras).
        for (int i = 0; i < 8 && tryPlayLand(); ++i) {}
        tryActivateCompanion();   // bring companion to hand for {3} when affordable
        tryForetell();
        trySuspend();
        for (int i = 0; i < 8 && tryPlayImpulseFromExile(); ++i) {}
        phaseLog("main1-cast");
        {
            int castIter = 0;
            bool cast = true;
            while (cast && !turnExpired()) {
                if (tm.isGameOver()) break;
                if (++castIter > 200) {
                    if (m_debugLog)
                        *m_debugLog << "  WARN: pre-combat cast loop hit 200 iters"
                                    << " turn=" << m_game.turnNumber()
                                    << " p=" << (int)m_id << "\n";
                    break;
                }
                cast = tryCastBestSpell();
            }
        }
        phaseLog("main1-activate");
        {
            // Activate any non-mana abilities, then unconditionally drain the stack.
            // The drain must be unconditional: decks without activated abilities (e.g.
            // Enchantress) would otherwise leave every cast spell sitting on the stack
            // unresolved, causing mass-ETB explosions later and potential heap corruption.
            phaseLog("main1-act-try");
            tryActivateAbilities();
            phaseLog("main1-act-stack");
            int stackIter = 0;
            while (!m_abilities.stackEmpty() && !turnExpired()) {
                if (++stackIter > 200) {
                    if (m_debugLog)
                        *m_debugLog << "  WARN: pre-combat stack loop hit 200 iters"
                                    << " turn=" << m_game.turnNumber() << "\n";
                    break;
                }
                m_abilities.resolveTop();
                int sbaIter = 0;
                while (StateBasedActions::run(m_game) && !turnExpired()) {
                    if (++sbaIter > 500) {
                        if (m_debugLog)
                            *m_debugLog << "  WARN: pre-combat SBA loop hit 500 iters\n";
                        break;
                    }
                }
                if (tm.isGameOver()) break;
            }
            phaseLog("main1-act-drain");
            m_abilities.drainPendingTriggers();
            if (tm.isGameOver()) { tm.endStep(); return true; }
        }
        phaseLog("main1-misc");
        if (!turnExpired()) tryActivateLevelUp();
        tryActivateMonstrosity();
        tryActivateMorph();
        tryActivateGraveyardAbilities();
        tryEquipEquipment();
        tryActivateOutlastAdapt();
        tryUnlockRooms();
        tryCycle();
        phaseLog("main1-done");
    }
    tm.endStep();
    if (tm.isGameOver()) return true;
    tm.advanceStep();

    // Begin Combat
    phaseLog("combat");
    if (autoStep()) return true;

    // Declare Attackers
    phaseLog("attackers");
    tm.beginStep();
    doAttackers(tm);
    tm.endStep();
    // Drain attack triggers (includes Exalted buffs) before blockers are declared
    m_abilities.drainPendingTriggers();
    if (tm.isGameOver()) return true;
    tm.advanceStep();

    // Declare Blockers â€” ask the opposing AI
    phaseLog("blockers");
    tm.beginStep();
    if (defender && !tm.combatState().empty())
        defender->declareBlockers(tm);
    tm.endStep();
    if (tm.isGameOver()) return true;
    tm.advanceStep();

    // First Strike Damage
    phaseLog("firstStrike");
    tm.beginStep();
    if (tm.hasFirstStrikers()) {
        tm.dealCombatDamage(true);
        { int i = 0; while (StateBasedActions::run(m_game) && ++i < 500) {} }
        m_abilities.drainPendingTriggers();
    }
    tm.endStep();
    if (tm.isGameOver()) return true;
    tm.advanceStep();

    // Regular Combat Damage
    phaseLog("combatDmg");
    tm.beginStep();
    if (!tm.combatState().empty()) {
        tm.dealCombatDamage(false);
        { int i = 0; while (StateBasedActions::run(m_game) && ++i < 500) {} }
        m_abilities.drainPendingTriggers(); // drains DamageDone triggers
    }
    tm.endStep();
    if (tm.isGameOver()) return true;
    tm.advanceStep();

    // End of Combat
    phaseLog("endCombat");
    if (autoStep()) return true;

    // Post-Combat Main â€” cast any remaining spells, activate planeswalker abilities
    phaseLog("main2");
    tm.beginStep();
    m_abilities.firePhaseTriggersAndDrain(TurnStep::PostCombatMain, m_game.activePlayerId());
    if (tm.isGameOver()) { tm.endStep(); return true; }
    {
        {
            int castIter = 0;
            bool cast = true;
            while (cast && !turnExpired()) {
                if (tm.isGameOver()) break;
                if (++castIter > 200) {
                    if (m_debugLog)
                        *m_debugLog << "  WARN: post-combat cast loop hit 200 iters"
                                    << " turn=" << m_game.turnNumber()
                                    << " p=" << (int)m_id << "\n";
                    break;
                }
                cast = tryCastBestSpell();
            }
        }
        {
            phaseLog("main2-act-try");
            tryActivateAbilities();
            phaseLog("main2-act-stack");
            int stackIter = 0;
            while (!m_abilities.stackEmpty() && !turnExpired()) {
                if (++stackIter > 200) {
                    if (m_debugLog)
                        *m_debugLog << "  WARN: post-combat stack loop hit 200 iters"
                                    << " turn=" << m_game.turnNumber() << "\n";
                    break;
                }
                m_abilities.resolveTop();
                int sbaIter = 0;
                while (StateBasedActions::run(m_game) && !turnExpired()) {
                    if (++sbaIter > 500) {
                        if (m_debugLog)
                            *m_debugLog << "  WARN: post-combat SBA loop hit 500 iters\n";
                        break;
                    }
                }
                if (tm.isGameOver()) break;
            }
            phaseLog("main2-act-drain");
            m_abilities.drainPendingTriggers();
            if (tm.isGameOver()) { tm.endStep(); return true; }
        }
        tryActivateLevelUp();
        tryActivateMonstrosity();
        tryActivateMorph();
        tryActivateGraveyardAbilities();
        tryEquipEquipment();
        tryActivateOutlastAdapt();
        tryUnlockRooms();
        tryCycle();
        tryActivatePlaneswalkers();
        {
            int stackIter = 0;
            while (!m_abilities.stackEmpty() && !turnExpired()) {
                if (++stackIter > 200) {
                    if (m_debugLog)
                        *m_debugLog << "  WARN: post-planeswalker stack loop hit 200 iters"
                                    << " turn=" << m_game.turnNumber() << "\n";
                    break;
                }
                m_abilities.resolveTop();
                int sbaIter = 0;
                while (StateBasedActions::run(m_game) && !turnExpired()) {
                    if (++sbaIter > 500) {
                        if (m_debugLog)
                            *m_debugLog << "  WARN: post-planeswalker SBA loop hit 500 iters\n";
                        break;
                    }
                }
            }
        }
        m_abilities.drainPendingTriggers();
        tryForetell();
        trySuspend();
        for (int i = 0; i < 8 && tryPlayImpulseFromExile(); ++i) {}
    }
    tm.endStep();
    if (tm.isGameOver()) return true;
    tm.advanceStep();

    phaseLog("endStep");
    // End Step â€” fire end-step triggers
    tm.beginStep();
    m_abilities.firePhaseTriggersAndDrain(TurnStep::EndStep, m_game.activePlayerId());
    if (tm.isGameOver()) { tm.endStep(); return true; }
    tm.endStep();
    if (tm.isGameOver()) return true;
    tm.advanceStep();

    if (turnExpired()) {
        if (m_debugLog) {
            *m_debugLog << "  WARN: turn time limit hit, ending game as draw"
                        << " turn=" << m_game.turnNumber()
                        << " p=" << (int)m_id
                        << " bf=" << m_game.battlefield().size() << "\n";
            m_debugLog->flush();
        }
        return true;
    }

    phaseLog("cleanup");
    // Cleanup â€” automatic
    tm.beginStep(); // Cleanup
    tm.endStep();
    if (tm.isGameOver()) return true;
    tm.advanceStep(); // wraps to next player's Untap

    return tm.isGameOver();
}

// â”€â”€ Mana â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

void AiPlayer::tapAllMana() {
    // Tap every own untapped source for its CHEAPEST (no-life-cost) mana
    // line. We never pre-tap lines with PayLife / Sac extra costs because
    // the player might not actually need that mana — historically this used
    // to fire every mana line on every source, paying life for Shivan Reef-
    // style "{C}, or {coloured} for 1 life" lands even when generic was fine.
    for (Card* c : m_game.battlefield().cards()) {
        if (c->controllerId != m_id || c->tapped) continue;
        int idx = pickManaLineFor(*c, /*neededColors=*/0, /*genericOk=*/true);
        // idx == -1 → basic land (no AB$ lines): activateManaAbility(-1)
        // routes to the basic-land subtype path below.
        m_abilities.activateManaAbility(c->id, m_id, idx);
    }
}

// Pick the AB$ Mana line on `src` that best fits the controller's current
// needs. Higher score wins. Returns -1 if the card has no AB$ Mana lines
// (basic lands, etc.), which the caller forwards to activateManaAbility as
// the "use the engine default" sentinel.
//
// Scoring:
//   +10 per produced colour that appears in `neededColors`
//   +5  for {C} when `genericOk` (generic slot still unfilled)
//   +2  for {C} regardless (still useful, just less than colored we need)
//   −3  per PayLife<N> bracket on the cost (don't pay life for free)
//   −5  for Sac<…> on the cost (don't blow up a permanent for free)
//
// Effect: Shivan Reef gets line 0 ({C}, no extra cost) for generic; line 1
// (Combo U/R + 1 damage) only when the cost has a {U} or {R} requirement.
int AiPlayer::pickManaLineFor(const Card& src, uint8_t neededColors,
                                bool genericOk) const {
    if (!src.rules) return -1;
    int bestIdx   = -1;
    int bestScore = INT_MIN;
    int idx       = 0;
    for (const auto& raw : src.rules->abilityLines) {
        auto s = parseScriptLine(raw);
        if (s.abilityType != "AB" || s.effectType != "Mana") continue;

        // Extract producible colour letters (handles "Combo W U" too).
        auto prod = std::string(s.get("Produced", "C"));
        std::string colors;
        if (prod.size() > 6 && prod.substr(0, 6) == "Combo ")
            prod = prod.substr(6);
        for (char ch : prod)
            if (ch == 'W' || ch == 'U' || ch == 'B' ||
                ch == 'R' || ch == 'G' || ch == 'C')
                colors += ch;

        int score = 0;
        for (char ch : colors) {
            uint8_t bit = 0;
            switch (ch) {
                case 'W': bit = ManaAtom::WHITE; break;
                case 'U': bit = ManaAtom::BLUE;  break;
                case 'B': bit = ManaAtom::BLACK; break;
                case 'R': bit = ManaAtom::RED;   break;
                case 'G': bit = ManaAtom::GREEN; break;
                case 'C': score += (genericOk ? 5 : 2); continue;
            }
            if (bit & neededColors) score += 10;
            else                    score += 1;  // colour we don't need but could
        }
        // Penalise extra costs we don't want to pay speculatively.
        std::string cost = std::string(s.get("Cost", ""));
        if (cost.find("PayLife<") != std::string::npos) score -= 3;
        if (cost.find("Sac<")     != std::string::npos) score -= 5;

        if (score > bestScore) { bestScore = score; bestIdx = idx; }
        ++idx;
    }
    return bestIdx;
}

// Returns the CMC of the cheapest affordable instant/flash spell in hand (or GY w/ flashback).
// Used by tapForSpell to decide how much mana to reserve for the opponent's turn.
int AiPlayer::bestInstantCmcInHand() const {
    int best = INT_MAX;
    for (const Card* c : me().hand().cards()) {
        if (!c->rules->type.isInstant() && !c->hasKeyword(KeywordAbility::Flash)) continue;
        int cmc = c->rules->manaCost.cmc();
        if (cmc > 0 && cmc < best) best = cmc;
    }
    for (const Card* c : me().graveyard().cards()) {
        if (!c->rules->hasFlashback) continue;
        if (!c->rules->type.isInstant()) continue;
        int cmc = c->rules->flashbackCost.cmc();
        if (cmc > 0 && cmc < best) best = cmc;
    }
    return best == INT_MAX ? 0 : best;
}

// Tap mana up to neededTotal, but reserve reserveCmc lands untapped if they exist.
// Used during main-phase casting to leave mana open for instants.
void AiPlayer::tapForCostReserving(int neededTotal, int reserveCmc) {
    // Count total available lands
    int totalLands = 0;
    for (const Card* c : m_game.battlefield().cards()) {
        if (c->controllerId == m_id && !c->tapped && c->rules->type.isLand())
            ++totalLands;
    }
    int canTap = std::max(0, totalLands - reserveCmc);
    int tapped = 0;
    for (Card* c : m_game.battlefield().cards()) {
        if (me().manaPool().total() >= neededTotal) break;
        if (tapped >= canTap) break;
        if (c->controllerId != m_id || c->tapped) continue;
        if (!c->rules->type.isLand()) continue;
        int idx = pickManaLineFor(*c, /*neededColors=*/0, /*genericOk=*/true);
        m_abilities.activateManaAbility(c->id, m_id, idx);
        ++tapped;
    }
}

// ── Multi-player helpers ──────────────────────────────────────────────────────

// Returns the ID of the opponent most worth targeting (closest to winning).
// In 2-player always returns primaryOpponentId(); in 4-player picks the most dangerous seat.
uint8_t AiPlayer::primaryOpponentId() const {
    if (m_game.numPlayers() <= 2) return static_cast<uint8_t>(m_id ^ 1);
    uint8_t bestOpp = (m_id + 1) % m_game.numPlayers();
    int     bestThreat = -1;
    for (uint8_t pid = 0; pid < m_game.numPlayers(); ++pid) {
        if (pid == m_id) continue;
        // Threat = inverse of life + commander damage already dealt + board power
        int threat = (40 - m_game.player(pid).life()) * 2;
        for (const Card* c : m_game.battlefield().cards()) {
            if (c->controllerId != pid) continue;
            if (c->isCommander) threat += 15;
            if (c->isCreature())
                threat += effectivePower(*c) + effectiveToughness(*c);
            if (c->rules->type.isPlaneswalker())
                threat += c->counterCount("loyalty") * 3;
        }
        // Also weight by how much commander damage they've dealt to us
        threat += m_game.player(m_id).commanderDamageFrom(pid) * 2;
        if (threat > bestThreat) { bestThreat = threat; bestOpp = pid; }
    }
    return bestOpp;
}

// ── Threat modeling ──────────────────────────────────────────────────────────

// Estimate how threatening the opponent's hand is based on land count trajectory.
// Returns a multiplier [1.0, 3.0]: higher = opponent is ramping toward something big.
float AiPlayer::assessOpponentThreatLevel() const {
    const Player& oppPlayer = opp();
    int oppLands = 0;
    int oppCreatures = 0;
    int oppHandSize  = static_cast<int>(oppPlayer.hand().size());

    for (const Card* c : m_game.battlefield().cards()) {
        if (c->controllerId == (primaryOpponentId())) {
            if (c->isLand())     ++oppLands;
            if (c->isCreature()) ++oppCreatures;
        }
    }

    float threat = 1.0f;

    // Opponent playing many lands rapidly = ramp threat
    int turnNum = m_game.turnNumber();
    if (oppLands > turnNum + 1) threat += 0.5f;  // ahead on lands

    // Opponent has commander out = elevated threat
    for (const Card* c : m_game.battlefield().cards()) {
        if (c->controllerId == (primaryOpponentId()) && c->isCommander)
            threat += 0.8f;
    }

    // Large hand = opponent holding gas for something
    if (oppHandSize >= 6) threat += 0.4f;

    // Few permanents but many turns played = control player holding back
    if (oppCreatures == 0 && turnNum >= 5) threat += 0.3f;

    return std::min(3.0f, threat);
}

// ── Combo detection ───────────────────────────────────────────────────────────

// Check if the opponent appears to be assembling a known combo
// (simplified: detect graveyard recursion loops and infinite-mana setups).
bool AiPlayer::detectOpponentCombo() const {
    // Check if opponent has multiple combo pieces: Grave Titan + reanimator, etc.
    int oppGYCreatures = 0;
    int oppGYSpells    = 0;
    for (const Card* c : opp().graveyard().cards()) {
        if (c->isCreature()) ++oppGYCreatures;
        else                 ++oppGYSpells;
    }
    // Large GY with specific reanimation spells = combo threat
    if (oppGYCreatures >= 3 && oppGYSpells >= 2) return true;

    // Opponent has stax / lockdown pieces that prevent our actions
    if (m_game.cantPlayLand[m_id]) return true;  // we're locked out of playing lands
    for (const Card* c : m_game.battlefield().cards()) {
        if (c->rules->hasWard && c->isCreature() && c->controllerId == (primaryOpponentId())) {
            // Multiple ward creatures = opponent building a protected board — minor threat
        }
    }
    return false;
}

bool AiPlayer::canAfford(const ManaCost& cost) const {
    if (cost.isNoCost()) return true;
    // Use color-aware check on current pool
    return me().manaPool().canPay(cost);
}

bool AiPlayer::canAffordWithUntapped(const ManaCost& cost) const {
    if (cost.isNoCost()) return true;
    // X-cost: always castable with X=0 if fixed portion is affordable
    if (cost.hasX()) {
        // Create a zero-X version: just check generic + colored shards (X portion = 0)
        ManaPool simPool = me().manaPool();
        for (const Card* c : m_game.battlefield().cards()) {
            if (c->controllerId != m_id || c->tapped || !c->rules->type.isLand()) continue;
            uint8_t lc = 0;
            const auto& sub = c->rules->type;
            if      (sub.hasSubtype("Forest"))   lc = ManaAtom::GREEN;
            else if (sub.hasSubtype("Island"))   lc = ManaAtom::BLUE;
            else if (sub.hasSubtype("Mountain")) lc = ManaAtom::RED;
            else if (sub.hasSubtype("Plains"))   lc = ManaAtom::WHITE;
            else if (sub.hasSubtype("Swamp"))    lc = ManaAtom::BLACK;
            if (lc) simPool.add(ManaCostShard::fromAtoms(lc), 1);
            else    simPool.addGeneric(1);
        }
        // Build fixed cost (remove X shards)
        int fixedCmc = cost.genericAmount();
        for (const auto& s : cost.shards())
            if (!s.isX()) fixedCmc += s.cmc();
        return simPool.total() >= fixedCmc;
    }
    // Simulate tapping all untapped lands and check
    ManaPool simPool = me().manaPool();
    for (const Card* c : m_game.battlefield().cards()) {
        if (c->controllerId != m_id || c->tapped) continue;
        if (!c->rules->type.isLand()) continue;
        // Determine what color this land produces (basic land heuristic)
        uint8_t landColor = 0;
        const auto& sub = c->rules->type;
        if      (sub.hasSubtype("Forest"))   landColor = ManaAtom::GREEN;
        else if (sub.hasSubtype("Island"))   landColor = ManaAtom::BLUE;
        else if (sub.hasSubtype("Mountain")) landColor = ManaAtom::RED;
        else if (sub.hasSubtype("Plains"))   landColor = ManaAtom::WHITE;
        else if (sub.hasSubtype("Swamp"))    landColor = ManaAtom::BLACK;
        if (landColor)
            simPool.add(ManaCostShard::fromAtoms(landColor), 1);
        else
            simPool.addGeneric(1);  // non-basic: treat as generic for planning
    }
    if (cost.hasX()) return simPool.total() >= cost.cmc();
    return simPool.canPay(cost);
}

void AiPlayer::tapForCost(int neededTotal) {
    for (Card* c : m_game.battlefield().cards()) {
        if (me().manaPool().total() >= neededTotal) break;
        if (c->controllerId != m_id || c->tapped) continue;
        if (!c->rules->type.isLand()) continue;
        int idx = pickManaLineFor(*c, /*neededColors=*/0, /*genericOk=*/true);
        m_abilities.activateManaAbility(c->id, m_id, idx);
    }
}

// Tap colored lands first to satisfy colored requirements, then generics.
// Per-source: pick which AB$ Mana line to fire based on what's still needed —
// so Shivan Reef contributes {C} for a generic-only cost (no life paid) and
// only flips to its colored Combo line when {U} or {R} is actually required.
void AiPlayer::tapForManaCost(const ManaCost& cost) {
    ManaPool& pool = me().manaPool();
    if (pool.canPay(cost)) return;  // already have enough

    auto colorOfLand = [](const Card& c) -> uint8_t {
        const auto& sub = c.rules->type;
        if      (sub.hasSubtype("Forest"))   return ManaAtom::GREEN;
        else if (sub.hasSubtype("Island"))   return ManaAtom::BLUE;
        else if (sub.hasSubtype("Mountain")) return ManaAtom::RED;
        else if (sub.hasSubtype("Plains"))   return ManaAtom::WHITE;
        else if (sub.hasSubtype("Swamp"))    return ManaAtom::BLACK;
        return c.rules->manaCost.colorIdentity();
    };

    // Pass 1: needed colours — try sources that can produce them.
    uint8_t needed = cost.colorIdentity();
    if (needed) {
        for (Card* c : m_game.battlefield().cards()) {
            if (pool.canPay(cost)) break;
            if (c->controllerId != m_id || c->tapped || !c->rules->type.isLand()) continue;
            // For multi-line sources (Shivan Reef etc.) ask the picker; for
            // single-line basic lands a subtype check is faster.
            int idx = pickManaLineFor(*c, needed, /*genericOk=*/false);
            bool willHelp = false;
            if (idx >= 0) {
                // Score > 0 means the line produces something we need.
                // Re-check by mimicking the picker's keep-criteria: a line
                // that can produce a needed colour returns at least +10.
                // Easier: just check the produced field directly.
                int mi = 0;
                for (const auto& raw : c->rules->abilityLines) {
                    auto s = parseScriptLine(raw);
                    if (s.abilityType != "AB" || s.effectType != "Mana") continue;
                    if (mi++ != idx) continue;
                    auto prod = std::string(s.get("Produced", "C"));
                    if (prod.size() > 6 && prod.substr(0, 6) == "Combo ")
                        prod = prod.substr(6);
                    for (char ch : prod) {
                        uint8_t bit = 0;
                        switch (ch) {
                            case 'W': bit = ManaAtom::WHITE; break;
                            case 'U': bit = ManaAtom::BLUE;  break;
                            case 'B': bit = ManaAtom::BLACK; break;
                            case 'R': bit = ManaAtom::RED;   break;
                            case 'G': bit = ManaAtom::GREEN; break;
                        }
                        if (bit & needed) { willHelp = true; break; }
                    }
                    break;
                }
            } else {
                // Basic land: subtype tells us the colour.
                willHelp = (colorOfLand(*c) & needed) != 0;
            }
            if (!willHelp) continue;
            m_abilities.activateManaAbility(c->id, m_id, idx);
        }
    }
    // Pass 2: pad out generic with the cheapest line (no life paid for {C}).
    for (Card* c : m_game.battlefield().cards()) {
        if (pool.canPay(cost)) break;
        if (c->controllerId != m_id || c->tapped || !c->rules->type.isLand()) continue;
        int idx = pickManaLineFor(*c, /*neededColors=*/0, /*genericOk=*/true);
        m_abilities.activateManaAbility(c->id, m_id, idx);
    }
}

// â”€â”€ Board evaluator â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

// Non-linear life-score table â€” Commander format (0..40 HP).
// Preserves the same urgency curve at low life values; the upper range (20-40)
// scales more gently since being at 30/40 is comfortable, not critical.
// Values above kMaxLife (40) scale linearly at kLifeAboveMult per point.
static constexpr int kLifeScores[] = {
    //  0    1    2    3    4    5    6    7    8    9
        0,  40,  80, 120, 160, 188, 208, 228, 246, 263,
    // 10   11   12   13   14   15   16   17   18   19
      280, 295, 308, 320, 330, 340, 349, 357, 364, 371,
    // 20   21   22   23   24   25   26   27   28   29
      378, 385, 390, 395, 400, 404, 408, 411, 414, 417,
    // 30   31   32   33   34   35   36   37   38   39   40
      420, 422, 424, 426, 428, 430, 431, 432, 433, 434, 435
};
static constexpr int kMaxLife = 40;
static constexpr int kLifeAboveMult = 2;

static int lifeScore(int life) {
    if (life > kMaxLife) return kLifeScores[kMaxLife] + (life - kMaxLife) * kLifeAboveMult;
    if (life >= 0)       return kLifeScores[life];
    return 0;
}

int AiPlayer::keywordBonus(const Card& c) const {
    int s = 0;
    using K = KeywordAbility;
    if (c.hasKeyword(K::Flying))          s += 5;
    if (c.hasKeyword(K::Deathtouch))      s += 6;
    if (c.hasKeyword(K::Indestructible))  s += 15;
    if (c.hasKeyword(K::DoubleStrike))    s += 10;
    if (c.hasKeyword(K::FirstStrike))     s += 5;
    if (c.hasKeyword(K::Trample))         s += 3;
    if (c.hasKeyword(K::Hexproof))        s += 6;
    if (c.hasKeyword(K::Shroud))          s += 5;
    if (c.hasKeyword(K::Lifelink))        s += 3;
    if (c.hasKeyword(K::Menace))          s += 3;
    if (c.hasKeyword(K::Haste))           s += 2;
    if (c.hasKeyword(K::Vigilance))       s += 2;
    if (c.hasKeyword(K::Reach))           s += 2;
    if (c.hasKeyword(K::Infect))          s += 6;
    if (c.hasKeyword(K::Wither))          s += 3;
    if (c.hasKeyword(K::Undying))         s += 5;
    if (c.hasKeyword(K::Persist))         s += 4;
    if (c.hasKeyword(K::Fear))            s += 5;
    if (c.hasKeyword(K::Shadow))          s += 4;
    // Landwalk evasions
    uint32_t km = c.keywordMask;
    if (km & (static_cast<uint32_t>(K::Swampwalk) | static_cast<uint32_t>(K::Islandwalk) |
              static_cast<uint32_t>(K::Mountainwalk) | static_cast<uint32_t>(K::Forestwalk) |
              static_cast<uint32_t>(K::Plainswalk)))
        s += 2;
    // Protection (each color)
    if (km & (static_cast<uint32_t>(K::ProtectionWhite) | static_cast<uint32_t>(K::ProtectionBlue) |
              static_cast<uint32_t>(K::ProtectionBlack) | static_cast<uint32_t>(K::ProtectionRed)  |
              static_cast<uint32_t>(K::ProtectionGreen) | static_cast<uint32_t>(K::ProtectionAll)))
        s += 6;
    // Ward adds minor protection value
    if (c.hasKeyword(K::Ward))            s += 3;
    return s;
}

int AiPlayer::evaluatePermanent(const Card& c) const {
    int score = 5; // base: on the battlefield has value

    if (c.isCreature()) {
        int power    = std::max(0, effectivePower(c));
        int toughness= effectiveToughness(c);
        int kwBonus  = keywordBonus(c);
        score += power * 3 + std::max(0, toughness) + kwBonus * (power + 1) / 2;
        score -= c.markedDamage;
        if (c.tapped) score -= 4;
        if (c.summoningSickness && !c.hasKeyword(KeywordAbility::Haste)) score -= 3;

        // â”€â”€ Threat multipliers â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
        // Very large creatures are disproportionately dangerous
        if (power >= 7) score += (power - 6) * 4;
        // Indestructible creatures with evasion are major threats
        if (c.hasKeyword(KeywordAbility::Indestructible) &&
            (c.hasKeyword(KeywordAbility::Flying) || c.hasKeyword(KeywordAbility::Trample)))
            score += 12;
        // Deathtouch + First Strike = kills anything without dying
        if (c.hasKeyword(KeywordAbility::Deathtouch) &&
            (c.hasKeyword(KeywordAbility::FirstStrike) || c.hasKeyword(KeywordAbility::DoubleStrike)))
            score += 8;
        // Commander with significant power is a win-condition threat
        if (c.isCommander && power >= 5) score += 10;

    } else if (c.rules->type.isPlaneswalker()) {
        auto loyIt = c.counters.find("loyalty");
        int loyalty = (loyIt != c.counters.end()) ? loyIt->second : 0;
        // Base score scales with loyalty
        score += loyalty * 5;
        // Proximity-to-ultimate threat: if loyalty is close to the ult cost,
        // the planeswalker is extremely dangerous and must be dealt with.
        // Approximate ult cost from oracle text (look for "-N:" pattern)
        {
            int ultThreshold = 7;  // default assumption
            const std::string& ot = c.rules->oracleText;
            for (auto p = ot.find("âˆ’"); p != std::string::npos; p = ot.find("âˆ’", p + 1)) {
                int n = 0;
                auto q = p + 3;  // UTF-8 'âˆ’' is 3 bytes
                while (q < ot.size() && std::isdigit((unsigned char)ot[q])) {
                    n = n * 10 + (ot[q++] - '0');
                }
                if (n > ultThreshold) ultThreshold = n;
            }
            if (loyalty >= ultThreshold - 1)
                score += 25;  // about to ultimate â€” emergency threat
            else if (loyalty >= ultThreshold - 2)
                score += 12;
        }
        // Tapped planeswalkers just activated; slightly less urgent
        if (c.tapped) score -= 3;

    } else if (c.rules->type.hasSubtype("Equipment")) {
        // Equipped pieces have value proportional to the bonus they grant
        if (c.attachedTo != kInvalidId) score += 6;
        else score += 2;
    } else if (c.rules->type.isEnchantment()) {
        score += 4;  // enchantments are usually value-generating
    }

    // Counter-based value (charge, quest, experience, etc.)
    for (const auto& [type, cnt] : c.counters) {
        if      (type == "+1/+1") score += cnt;  // already reflected in P/T but extra value
        else if (type == "charge") score += cnt * 2;
        else if (type == "level")  score += cnt * 3;
        else if (type == "quest")  score += cnt * 2;
    }

    // Salt (heuristic: high-salt cards are staples; prioritise their removal)
    float salt = SaltDatabase::get(c.rules->name);
    if (salt > 0.f && c.controllerId != m_id)
        score += static_cast<int>(salt * 10.f);

    return score;
}

int AiPlayer::staticEval() const {
    const Player& myP  = me();
    const Player& oppP = opp();

    if (oppP.hasLost()) return  10000;
    if (myP.hasLost())  return -10000;

    int score = 0;

    // Non-linear life scoring: low life is disproportionately bad
    score += lifeScore(myP.life()) - lifeScore(oppP.life());

    // Poison counters (10 = death)
    score -= myP.poisonCounters()  * 40;
    score += oppP.poisonCounters() * 40;

    // Commander damage (21 = death). Use the same urgency curve as life: the closer
    // to 21 we/they are, the more each point matters.  effectiveLife(21-N) scores it
    // as if that remaining buffer were a life total.
    {
        uint8_t oppId = primaryOpponentId();
        int myDmgTaken  = myP.commanderDamageFrom(oppId);
        int oppDmgTaken = oppP.commanderDamageFrom(m_id);
        if (myDmgTaken > 0) {
            int bufRemaining = std::max(0, 21 - myDmgTaken);
            // Penalty: how bad it is to be N damage away from commander loss
            score -= (lifeScore(21) - lifeScore(bufRemaining));
        }
        if (oppDmgTaken > 0) {
            int bufRemaining = std::max(0, 21 - oppDmgTaken);
            // Bonus: how good it is that opponent is N damage away from commander loss
            score += (lifeScore(21) - lifeScore(bufRemaining));
        }
    }

    // Per-permanent evaluation
    for (const Card* c : m_game.battlefield().cards()) {
        int ps = evaluatePermanent(*c);
        if (c->controllerId == m_id) {
            score += ps;
        } else {
            score -= ps;
            // Salt threat: high-salt opponent cards make the position feel worse,
            // pushing the AI to remove them via lookahead.
            float salt = SaltDatabase::get(c->rules->name);
            if (salt > 0.f)
                score -= static_cast<int>(salt * 18.0f);
        }
    }

    // Card-in-hand advantage: weight by estimated average cost of opponent's hand.
    // More cards = more options, but high-CMC hands are slower to cast.
    {
        int myCards  = static_cast<int>(myP.hand().size());
        int oppCards = static_cast<int>(oppP.hand().size());
        score += (myCards - oppCards) * 3;

        // Additional penalty for opponent holding many cards late in game
        if (m_game.turnNumber() > 6 && oppCards > myCards)
            score -= (oppCards - myCards) * 2;
    }

    // â”€â”€ Combo-piece assembly bonus â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    // Count how many pieces of each known win-condition combo I have assembled
    // on the battlefield.  Partial assembly is rewarded proportionally; complete
    // assembly gets a large bonus (near-win).  Uses the ComboDatabase singleton.
    {
        // Build set of our battlefield card names for fast combo-piece lookup
        std::unordered_set<std::string> myNames;
        for (const Card* c : m_game.battlefield().cards())
            if (c->controllerId == m_id && c->rules) myNames.insert(c->rules->name);
        // Include hand cards â€” pieces we can still cast this turn
        for (const Card* c : myP.hand().cards())
            if (c->rules) myNames.insert(c->rules->name);

        int bestComboScore = 0;
        s_combos.forEach([&](const std::vector<std::string>& pieces,
                              const std::string& /*description*/) {
            if (pieces.empty()) return;
            int have = 0;
            for (const auto& p : pieces)
                if (myNames.count(p)) ++have;
            if (have == 0) return;
            // Score: proportional to fraction assembled; full combo = 60 bonus
            int cs = (have * 60) / static_cast<int>(pieces.size());
            if (have == static_cast<int>(pieces.size())) cs = 80; // complete
            if (cs > bestComboScore) bestComboScore = cs;
        });
        score += bestComboScore;

        // Penalty for opponent combo assembly
        std::unordered_set<std::string> oppNames;
        for (const Card* c : m_game.battlefield().cards())
            if (c->controllerId != m_id && c->rules) oppNames.insert(c->rules->name);
        int bestOppCombo = 0;
        s_combos.forEach([&](const std::vector<std::string>& pieces, const std::string&) {
            if (pieces.empty()) return;
            int have = 0;
            for (const auto& p : pieces)
                if (oppNames.count(p)) ++have;
            if (have == 0) return;
            int cs = (have * 60) / static_cast<int>(pieces.size());
            if (have == static_cast<int>(pieces.size())) cs = 80;
            if (cs > bestOppCombo) bestOppCombo = cs;
        });
        score -= bestOppCombo;
    }

    return score;
}

// â”€â”€ 1-ply lookahead â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

int AiPlayer::lookaheadScore(ObjectId id, const std::vector<Target>& targets,
                              const ManaCost& cost) const {
    const Card* card = m_game.findCard(id);
    if (!card || !card->rules) return 0;

    // Fast path: for non-targeted permanent spells (creatures, artifacts, enchantments)
    // without complex on-resolution effects, use rateSpell instead of cloning the game.
    // This cuts ~80% of clones for typical Commander games.
    if (targets.empty() && card->rules->type.isPermanent()) {
        // Only skip clone for "enter and stay" cards â€” skip spells with ETB removal/draw
        bool hasComplexETB = false;
        for (const auto& raw : card->rules->abilityLines) {
            auto s = parseScriptLine(raw);
            if (s.abilityType != "SP" && s.abilityType != "DB") continue;
            const auto& et = s.effectType;
            if (et == "Destroy" || et == "DealDamage" || et == "Counter" ||
                et == "Draw" || et == "ChangeZone" || et == "ChangeZoneAll") {
                hasComplexETB = true; break;
            }
        }
        if (!hasComplexETB) {
            int base = staticEval();
            return base + rateSpell(*card) + 5; // +5 for "cast happened" advantage
        }
    }

    // Full clone for spells with complex effects
    auto simPtr = std::make_unique<GameState>(m_game.clone());
    GameState& sim = *simPtr;
    AbilityProcessor ap  = m_abilities.cloneFor(sim);
    AiPlayer         simAi(m_id, sim, ap);
    simAi.tapForCost(cost.cmc());
    if (!ap.castSpell(id, m_id, targets)) return 0;
    ap.resolveTop();
    while (StateBasedActions::run(sim)) {}
    ap.drainPendingTriggers();
    return simAi.staticEval();
}

// â”€â”€ Instant-speed response casting â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

// Score a counterspell based on what it would counter (the opposing spell on the stack).
// Higher = more worth countering.  Returns -1 if nothing to counter.
static int rateCounterTarget(const mtg::GameState& game, uint8_t myId) {
    const auto& sv = game.stack().cards();
    for (int i = (int)sv.size() - 1; i >= 0; --i) {
        const Card* s = sv[i];
        if (!s || !s->rules || s->controllerId == myId) continue;
        // Base: CMC * 12 (a 5-CMC bomb is very worth countering)
        int val = s->rules->manaCost.cmc() * 12;
        // Board wipes and mass removal: extremely high value
        for (const auto& raw : s->rules->abilityLines) {
            auto sc = parseScriptLine(raw);
            if (sc.abilityType != "SP" && sc.abilityType != "DB") continue;
            auto dest = std::string(sc.get("Destination"));
            if (sc.effectType == "ChangeZoneAll" &&
                (dest == "Graveyard" || dest == "Exile"))
                val += 120;
            if (sc.effectType == "DealDamage") {
                int dmg = sc.getInt("NumDmg", 0);
                if (dmg >= game.player(myId).life()) val += 200; // lethal burn
                else val += dmg * 8;
            }
        }
        return val;
    }
    return -1; // nothing to counter
}

bool AiPlayer::tryNinjutsu(TurnManager& tm) {
    // Find any unblocked attacker we control
    ObjectId unblockedId = kInvalidId;
    for (const auto& atk : tm.combatState().attacks) {
        const Card* c = m_game.findCard(atk.attackerId);
        if (c && c->controllerId == m_id && atk.blockerIds.empty()) {
            unblockedId = atk.attackerId;
            break;
        }
    }
    if (unblockedId == kInvalidId) return false;

    // Find the best Ninja in hand we can afford
    for (Card* c : m_game.player(m_id).hand().cards()) {
        if (!c->rules->hasNinjutsu) continue;
        if (!canAfford(c->rules->ninjutsuCost)) continue;
        tapForCost(c->rules->ninjutsuCost.cmc());
        if (m_abilities.activateNinjutsu(c->id, unblockedId, m_id, tm)) {
            while (StateBasedActions::run(m_game)) {}
            m_abilities.drainPendingTriggers();
            return true;
        }
    }
    return false;
}

bool AiPlayer::tryCastInstant() {
    struct Candidate {
        ObjectId            id;
        int                 score;
        const CardRules*    rules;
        std::vector<Target> targets;
        bool                requiresTarget;
    };
    std::vector<Candidate> castable;

    // Don't pre-tap. canAfford() simulates mana from untapped lands without
    // touching the real pool, so we can evaluate candidates first and only
    // tap right before casting the one we pick — leaving everything else
    // untapped for future spells, blocking, or the opponent's turn.

    auto buildCandidate = [&](Card* c, const ManaCost& cost) {
        auto tgts = pickTargets(*c->rules);
        bool needsTgt = false;
        bool isCounter = false;
        for (const auto& raw : c->rules->abilityLines) {
            auto s = parseScriptLine(raw);
            if (!s.get("ValidTgts").empty()) needsTgt = true;
            if (s.effectType == "Counter") isCounter = true;
        }
        if (needsTgt && tgts.empty()) return;  // can't cast â€” no valid target
        // Auras need something to enchant even though they carry no ValidTgts line.
        if (c->rules->type.hasSubtype("Aura") && tgts.empty()) return;

        int score;
        if (isCounter) {
            score = rateCounterTarget(m_game, m_id);
            if (score < 0) return;  // nothing to counter â€” skip
        } else {
            // 1-ply lookahead for all other instants
            score = lookaheadScore(c->id, tgts, cost);
        }
        // Combat-trick bonus: pump spells are more valuable during active combat
        bool isCombatTrick = false;
        for (const auto& raw : c->rules->abilityLines) {
            auto sl = parseScriptLine(raw);
            if (sl.effectType == "Pump" || sl.effectType == "PutCounter") {
                auto vt = std::string(sl.get("ValidTgts", ""));
                if (vt.find("Creature") != std::string::npos) { isCombatTrick = true; break; }
            }
        }
        // Check if we're in combat via the game state's active combat pointer
        if (isCombatTrick && m_game.activeCombat && !m_game.activeCombat->empty())
            score += 35;

        castable.push_back({ c->id, score, c->rules, std::move(tgts), needsTgt });
    };

    // Hand: instants and Flash creatures/spells
    for (Card* c : me().hand().cards()) {
        if (!c->rules->type.isInstant() && !c->hasKeyword(KeywordAbility::Flash))
            continue;
        if (!canAfford(c->rules->manaCost)) continue;
        buildCandidate(c, c->rules->manaCost);
    }
    // Graveyard: Flashback instants
    for (Card* c : me().graveyard().cards()) {
        if (!c->rules->hasFlashback) continue;
        if (!c->rules->type.isInstant() && !c->hasKeyword(KeywordAbility::Flash))
            continue;
        if (!canAfford(c->rules->flashbackCost)) continue;
        buildCandidate(c, c->rules->flashbackCost);
    }
    if (castable.empty()) return false;

    // Only act if best candidate beats the baseline board score
    int baseline = staticEval();
    std::sort(castable.begin(), castable.end(),
              [](const Candidate& a, const Candidate& b){ return a.score > b.score; });

    for (const auto& cand : castable) {
        // Flash creatures need a significant swing to be worth flashing in
        if (cand.rules->type.isCreature() && !cand.rules->type.isInstant()
            && cand.score < baseline + 12) continue;
        // Non-counterspell instants: only cast if they improve the position
        bool isCounter = false;
        for (const auto& raw : cand.rules->abilityLines) {
            if (parseScriptLine(raw).effectType == "Counter") { isCounter = true; break; }
        }
        if (!isCounter && cand.score <= baseline) continue;

        const std::string spellName = cand.rules->name;
        if (!canAffordWithUntapped(cand.rules->manaCost)) continue;

        // Snapshot tap/pool/life so a FAILED cast rolls back instead of leaving
        // lands tapped (and pain-land life paid) "for nothing" — the source of
        // "the AI taps out at end of turn for no reason".
        std::vector<ObjectId> wasUntapped;
        for (Card* c : m_game.battlefield().cards())
            if (c->controllerId == m_id && !c->tapped) wasUntapped.push_back(c->id);
        ManaPool poolBefore = me().manaPool();
        int      lifeBefore = me().life();

        tapForManaCost(cand.rules->manaCost);
        if (m_abilities.castSpell(cand.id, m_id, cand.targets)) {
            AIOUT << "  [" << me().name() << "] responds with " << spellName << '\n';
            return true;
        }
        // Cast failed: undo the speculative taps, mana, and life.
        for (ObjectId id : wasUntapped)
            if (Card* c = m_game.findCard(id)) c->tapped = false;
        me().manaPool() = poolBefore;
        me().setLife(lifeBefore);
        // Stop after the first failed attempt — runner-ups usually fail for the
        // same reason; rolling back means nothing was wasted.
        return false;
    }
    return false;
}

// â”€â”€ Casting â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

// Opening book: early-game land play prioritizes color matching strictly
// because the AI has a specific color identity to hit (commander colors).
static bool isOpeningBookTurn(int turnNum) {
    return turnNum <= 4;
}

bool AiPlayer::tryActivateCompanion() {
    if (m_id >= 4) return false;
    if (m_game.companionId[m_id] == kInvalidId || m_game.companionUsed[m_id]) return false;
    ManaCost three = ManaCost::parse("3");
    if (!canAffordWithUntapped(three)) return false;   // only when {3} is available
    tapForCost(3);                                     // float the mana
    return m_abilities.activateCompanion(m_id);
}

bool AiPlayer::tryPlayLand() {
    // ControlPlayer: a controlled player takes no voluntary actions this turn.
    if (m_game.isTurnControlled(m_id)) return false;
    if (!m_game.canPlayLand(m_id)) return false;   // respects extra land plays

    // Compute which colors we need most from the hand's spells
    uint8_t neededColors = 0;
    int     neededCmc    = 0;
    for (const Card* hc : me().hand().cards()) {
        if (hc->rules->type.isLand()) continue;
        neededColors |= hc->rules->manaCost.colorIdentity();
        if (hc->rules->manaCost.cmc() > neededCmc)
            neededCmc = hc->rules->manaCost.cmc();
    }

    // Score each land in hand: prefer lands that produce needed colors
    auto landColor = [](const CardRules* r) -> uint8_t {
        const auto& sub = r->type;
        if (sub.hasSubtype("Plains"))   return ManaAtom::WHITE;
        if (sub.hasSubtype("Island"))   return ManaAtom::BLUE;
        if (sub.hasSubtype("Swamp"))    return ManaAtom::BLACK;
        if (sub.hasSubtype("Mountain")) return ManaAtom::RED;
        if (sub.hasSubtype("Forest"))   return ManaAtom::GREEN;
        return 0; // non-basic or any-color
    };

    Card* bestLand = nullptr;
    int bestScore = -1;
    for (Card* c : me().hand().cards()) {
        if (!c->rules->type.isLand()) continue;
        uint8_t col = landColor(c->rules);
        int score = 0;
        if (col == 0) score = 30;          // any-color land (Command Tower, etc.) â€” highest value
        else if (col & neededColors) score = 20; // produces a needed color
        else score = 5;                    // basic not matching current needs
        // Prefer lands not already represented (diversity bonus)
        for (const Card* bf : m_game.battlefield().cards()) {
            if (bf->controllerId == m_id && bf->isLand() &&
                landColor(bf->rules) == col && col != 0) { score -= 5; break; }
        }
        if (score > bestScore) { bestScore = score; bestLand = c; }
    }

    if (!bestLand) return false;

    const std::string name = bestLand->rules->name;
    ObjectId id = bestLand->id;
    Card* landed = m_game.moveToZone(id, ZoneType::Battlefield, m_id);
    me().incLandsPlayed();
    AIOUT << "  [" << me().name() << "] plays " << name << '\n';
    if (landed) {
        std::vector<PendingTrigger> t;
        TriggerSystem::onLandPlayed(*landed, m_id, m_game, t);
        m_game.queueTriggers(std::move(t));
        m_abilities.processHideaway(*landed);  // Mosswort Bridge, etc.
    }
    return true;
}

// â”€â”€ Spell rating â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

// Returns true if this instant is better saved for the opponent's turn
// (counterspells, combat tricks, bounce) rather than cast proactively now.
static bool shouldHoldForResponse(const mtg::CardRules& r) {
    if (!r.type.isInstant()) return false;
    for (const auto& raw : r.abilityLines) {
        auto s = mtg::parseScriptLine(raw);
        if (s.abilityType != "SP" && s.abilityType != "DB") continue;
        const auto& et = s.effectType;
        // Counterspells: always hold
        if (et == "Counter") return true;
        // Bounce targeting opponent's stuff: hold for their end step or combat
        if (et == "ChangeZone") {
            auto dest = std::string(s.get("Destination"));
            auto vt   = std::string(s.get("ValidTgts"));
            if ((dest == "Hand" || dest == "Library") &&
                (vt.find("Creature") != std::string::npos ||
                 vt.find("Permanent") != std::string::npos))
                return true;
        }
    }
    return false;
}

int AiPlayer::rateSpell(const Card& card) const {
    const CardRules& r = *card.rules;
    int score = r.cmc() * 3; // base tiebreaker: higher CMC usually more impactful

    // Instants that are better held for the opponent's turn get penalized
    // so the AI prefers sorceries/creatures/enchantments in its own main phase.
    if (shouldHoldForResponse(r)) score -= 30;

    // Board snapshot used for context
    bool hasOppCreature = false;
    int  oppTotalPower  = 0;
    int  myTotalPower   = 0;
    int  myCreatureCount  = 0;
    int  oppCreatureCount = 0;
    for (const Card* c : m_game.battlefield().cards()) {
        if (!c->isCreature()) continue;
        if (c->controllerId == m_id) {
            myTotalPower += effectivePower(*c);
            ++myCreatureCount;
        } else {
            hasOppCreature = true;
            oppTotalPower  += effectivePower(*c);
            ++oppCreatureCount;
        }
    }

    // Board mode: are we ahead or behind?
    // "Behind" = opponent has more creatures or significantly more power.
    // When behind: boost removal, wipes, and stabilizing plays.
    // When ahead: boost threats and finishers.
    bool isBehind = (oppTotalPower > myTotalPower + 4) ||
                    (oppCreatureCount > myCreatureCount + 1);
    bool isAhead  = (myTotalPower > oppTotalPower + 6) || (myCreatureCount >= 3 && oppCreatureCount == 0);
    if (isBehind) score += 5;
    (void)isAhead;

    // Threat modeling: urgency bonus when opponent appears dangerous
    float threatLevel = assessOpponentThreatLevel();
    bool  comboDetect = detectOpponentCombo();
    int   threatBonus = static_cast<int>((threatLevel - 1.f) * 15.f);  // 0..30
    if (comboDetect) threatBonus += 20;

    // Life-differential context
    const int oppLife = opp().life();
    const int myLife  = me().life();
    // Bonus multiplier for going face: high when opponent is at low life
    const int finishBonus = (oppLife <= 5)  ? 50
                          : (oppLife <= 10) ? 20
                          : 0;
    // Extra value for life-gain/defense when we're at low life
    const int surviveBonus = (myLife <= 5)  ? 30
                           : (myLife <= 10) ? 10
                           : 0;

    for (const auto& raw : r.abilityLines) {
        auto s = parseScriptLine(raw);
        if (s.abilityType != "SP" && s.abilityType != "DB") continue;
        const auto& et = s.effectType;

        if (et == "DealDamage" || et == "Damage") {
            int dmg = s.getInt("NumDmg", 0);
            if (dmg >= oppLife) return 1000; // lethal â€” always cast first
            auto vt = std::string(s.get("ValidTgts"));
            if (vt == "Any" || vt.find("Creature") != std::string::npos)
                score += hasOppCreature ? 80 : 30;
            else if (vt.find("Player") != std::string::npos)
                score += 25 + finishBonus;
            score += std::min(dmg * 5, 50);
        }
        else if (et == "Destroy") {
            auto vt = std::string(s.get("ValidTgts"));
            bool hitsCreatures = vt.find("Creature")  != std::string::npos ||
                                 vt.find("Permanent")  != std::string::npos;
            int threat = std::min(oppTotalPower, 30);
            int modeBonus = isBehind ? 30 : 0;
            // Don't rate removal highly if we can deal lethal this turn without it
            // (attacking face is better than wasting a removal)
            {
                int myTotalPow = 0;
                for (const Card* c : m_game.battlefield().cards())
                    if (c->controllerId == m_id && c->isCreature() && !c->tapped && !c->summoningSickness)
                        myTotalPow += effectivePower(*c);
                int curOppLife = opp().life();
                if (myTotalPow >= curOppLife && curOppLife > 0)
                    modeBonus -= 40;  // lethal face available — don't waste removal
            }
            score += hitsCreatures && hasOppCreature ? 110 + threat + modeBonus : 20;
        }
        else if (et == "ChangeZoneAll") {
            auto dest = std::string(s.get("Destination"));
            if (dest == "Graveyard" || dest == "Exile") {
                // Board wipe: strongly favoured when behind on board
                int behindBonus = std::max(0, (oppTotalPower - myTotalPower) * 2);
                if (isBehind) behindBonus += 40;
                score += 130 + behindBonus;
            } else {
                score += 40;
            }
        }
        else if (et == "ChangeZone") {
            auto dest = std::string(s.get("Destination"));
            auto vt   = std::string(s.get("ValidTgts"));
            bool hitsOpp = vt.find("Creature") != std::string::npos ||
                           vt.find("Permanent") != std::string::npos;
            if ((dest == "Exile" || dest == "Hand" || dest == "Library") && hitsOpp)
                score += hasOppCreature ? 95 : 15;
            else if (dest == "Battlefield")
                score += 50; // reanimate / tutor
            else
                score += 20;
        }
        else if (et == "Draw") {
            int n = s.getInt("NumCards", 1);
            // Card advantage scales with hand size: drawing is more valuable with fewer cards
            int handSz = static_cast<int>(me().hand().size());
            int cardAdvBonus = std::max(0, (4 - handSz) * 8);  // +8 per card below 4 in hand
            score += 15 + n * 12 + cardAdvBonus;
        }
        else if (et == "Dig" || et == "ChangeZone") {
            // Tutors / top-deck manipulation also count as card advantage
            auto dest = std::string(s.get("Destination", ""));
            if (dest == "Hand" || dest == "Battlefield") {
                int handSz = static_cast<int>(me().hand().size());
                int cardAdvBonus = std::max(0, (4 - handSz) * 6);
                score += 20 + cardAdvBonus;
            }
        }
        else if (et == "Counter") {
            // Don't cast counterspells proactively; only when there's a stack target
            // Threat level increases urgency of holding up countermagic
            int counterUrgency = threatBonus / 2;
            score += m_game.stack().empty() ? (-60 + counterUrgency) : (85 + threatBonus);
        }
        else if (et == "GainLife") {
            int n = s.getInt("LifeAmount", s.getInt("Num", 3));
            // At critically low life, lifegain is extremely valuable
            int desperationBonus = (myLife <= 5) ? 60 : (myLife <= 10) ? 20 : 0;
            score += 8 + surviveBonus + std::min(n * 2, 20) + desperationBonus;
        }
    }

    // Token economy: bonus for spells that create tokens when we need sacrifice fodder
    // or bodies. Extra value when we have sacrifice outlets on the field.
    {
        bool hasSacOutlet = false;
        for (const Card* c : m_game.battlefield().cards()) {
            if (c->controllerId != m_id) continue;
            for (const auto& raw : c->rules->abilityLines) {
                auto sl = parseScriptLine(raw);
                if (sl.abilityType == "AB" && std::string(sl.get("Cost","")).find("Sac<") != std::string::npos)
                    { hasSacOutlet = true; break; }
            }
            if (hasSacOutlet) break;
        }
        for (const auto& raw : r.abilityLines) {
            auto sl = parseScriptLine(raw);
            if (sl.effectType == "Token" || sl.effectType == "MakeCard") {
                score += hasSacOutlet ? 20 : 8;  // tokens are more valuable with a sac outlet
                break;
            }
        }
    }

    // Party mechanic: bonus for completing or extending your party
    if (r.type.isCreature()) {
        bool hasCl = false, hasRo = false, hasWa = false, hasWi = false;
        for (const Card* c : m_game.battlefield().cards()) {
            if (c->controllerId != m_id || !c->isCreature()) continue;
            if (c->rules->type.hasSubtype("Cleric"))  hasCl = true;
            if (c->rules->type.hasSubtype("Rogue"))   hasRo = true;
            if (c->rules->type.hasSubtype("Warrior")) hasWa = true;
            if (c->rules->type.hasSubtype("Wizard"))  hasWi = true;
        }
        int curParty = (int)hasCl + (int)hasRo + (int)hasWa + (int)hasWi;
        bool addsParty = (r.type.hasSubtype("Cleric") && !hasCl) ||
                         (r.type.hasSubtype("Rogue")  && !hasRo) ||
                         (r.type.hasSubtype("Warrior")&& !hasWa) ||
                         (r.type.hasSubtype("Wizard") && !hasWi);
        if (addsParty) score += 8 * (4 - curParty);  // more valuable when party is incomplete
    }

    // Creatures: score by P/T efficiency, adjusted for CMC
    if (r.type.isCreature()) {
        int p   = std::max(0, effectivePower(card));
        int t   = std::max(0, effectiveToughness(card));
        int cmc = std::max(1, r.cmc());
        score += (p * 10 + t * 6) / cmc + 20;
        for (const auto& kw : r.keywords) {
            if (kw.find("Flying")      != std::string::npos) score += 18;
            if (kw.find("Deathtouch")  != std::string::npos) score += 14;
            if (kw.find("Haste")       != std::string::npos) score += 12;
            if (kw.find("Lifelink")    != std::string::npos) score += 10;
            if (kw.find("Trample")     != std::string::npos) score +=  8;
            if (kw.find("Vigilance")   != std::string::npos) score +=  6;
        }
        if (oppTotalPower == 0) score += 10; // uncontested board: build up
    }
    else if (r.type.isPlaneswalker()) {
        score += 75;
    }
    else if (r.type.isEnchantment() || r.type.isArtifact()) {
        score += 18;
    }

    // "Hold-back" penalty: if we have a much higher-CMC spell in hand that we're
    // one or two turns away from casting, reduce the score of cheap spells to
    // avoid spending mana that would be better saved for the powerful play.
    {
        int bestHandCmc = 0;
        for (const Card* hc : me().hand().cards()) {
            if (hc->rules->type.isLand() || hc->id == card.id) continue;
            if (hc->rules->cmc() > bestHandCmc) bestHandCmc = hc->rules->cmc();
        }
        // Count our current lands
        int currentLands = 0;
        for (const Card* lc : m_game.battlefield().cards())
            if (lc->controllerId == m_id && lc->isLand()) ++currentLands;
        int nextTurnLands = currentLands + 1;  // we'll likely play another land next turn
        // If we could cast something way bigger next turn, small spells score less
        if (bestHandCmc > r.cmc() + 2 && nextTurnLands >= bestHandCmc) {
            // Spending mana now would leave us short for the bigger play
            // Penalise proportional to how much bigger the waiting spell is
            score -= (bestHandCmc - r.cmc()) * 3;
        }
    }

    // Opening book: turns 1-4, strongly prefer cheap ramp (Sol Ring, mana rocks, etc.)
    if (isOpeningBookTurn(m_game.turnNumber()) && r.cmc() <= 2) {
        // Check if the spell produces mana (ramp)
        bool isRamp = false;
        for (const auto& raw : r.abilityLines) {
            auto sl = parseScriptLine(raw);
            if (sl.effectType == "Mana" || sl.abilityType == "AB") isRamp = true;
        }
        for (const auto& raw : r.triggerLines) {
            auto sl = parseScriptLine(raw);
            if (std::string(sl.get("Mode","")).find("Land") != std::string::npos) isRamp = true;
        }
        if (isRamp || r.hasAffinity) score += 30;
        // Also boost any spell with CMC 1-2 slightly for curve considerations
        score += (3 - r.cmc()) * 5;
    }

    return score;
}

bool AiPlayer::tryCastBestSpell() {
    // ControlPlayer: a controlled player takes no voluntary actions this turn.
    if (m_game.isTurnControlled(m_id)) return false;
    // â”€â”€ MCTS spell sequencing â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    if (m_mctsEnabled) {
        std::vector<float>* dist = nullptr;
        if (m_epOut) {
            m_epOut->policy.emplace_back();
            dist = &m_epOut->policy.back();
        }
        // Reuse m_mctsSearch across calls so the TT accumulates knowledge
        MctsAction action = m_mctsSearch.searchMainPhase(m_game, m_abilities, m_id,
                                                         m_mctsConfig, dist,
                                                         *m_valueFn, *m_policyFn,
                                                         &m_mctsRoot,
                                                         *m_batchValueFn);
        if (action.kind == MctsAction::Kind::Pass) {
            if (m_epOut && dist && dist->empty())
                m_epOut->policy.pop_back();  // no-op decision, discard empty entry
            return false;
        }
        tapForCost(action.spellTapCmc);
        const std::string spellName = [&]() -> std::string {
            const Card* c = m_game.findCard(action.spellId);
            return c ? c->name() : "(unknown)";
        }();
        if (m_abilities.castSpell(action.spellId, m_id, action.spellTargets)) {
            AIOUT << "  [" << me().name() << "] casts " << spellName << '\n';
            return true;
        }
        return false;
    }

    // â”€â”€ Combo check (highest priority) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    if (!s_combos.empty()) {
        if (const ComboEntry* combo = s_combos.findAssembled(m_id, m_game))
            if (executeCombo(*combo)) return true;
    }

    struct Candidate {
        ObjectId            id;
        int                 score;
        const CardRules*    rules;
        std::vector<Target> targets;
        int                 tapCmc    = 0;       // total cmc for tapForCost fallback
        const ManaCost*     costPtr   = nullptr; // actual cost for color-aware tapping
    };
    std::vector<Candidate> castable;

    // Helper: check whether a spell requires at least one target
    auto requiresTarget = [](const CardRules& r) {
        for (const auto& raw : r.abilityLines) {
            auto s = parseScriptLine(raw);
            if (!s.get("ValidTgts").empty()) return true;
        }
        return false;
    };

    // Hand cards â€” use canAffordWithUntapped so held-back lands count
    for (Card* c : me().hand().cards()) {
        if (c->rules->type.isLand()) continue;
        // Determine the cheapest affordable cost (base, Dash, Blitz, Evoke, Surge, Spectacle, Miracle)
        const ManaCost* effCost = &c->rules->manaCost;
        if (!canAffordWithUntapped(*effCost)) {
            if (c->rules->hasDash    && canAffordWithUntapped(c->rules->dashCost))
                effCost = &c->rules->dashCost;
            else if (c->rules->hasBlitz  && canAffordWithUntapped(c->rules->blitzCost))
                effCost = &c->rules->blitzCost;
            else if (c->rules->hasEvoke  && canAffordWithUntapped(c->rules->evokeCost))
                effCost = &c->rules->evokeCost;
            else if (c->rules->hasSurge  && m_game.spellsCastThisTurn > 0 &&
                     canAffordWithUntapped(c->rules->surgeCost))
                effCost = &c->rules->surgeCost;
            else if (c->rules->hasSpectacle &&
                     m_game.playerDamagedThisTurn[primaryOpponentId()] &&
                     canAffordWithUntapped(c->rules->spectacleCost))
                effCost = &c->rules->spectacleCost;
            else if (c->rules->hasMiracle && c->miracleEligible &&
                     canAffordWithUntapped(c->rules->miracleCost))
                effCost = &c->rules->miracleCost;
            else
                continue;
        }
        auto tgts = pickTargets(*c->rules);
        if (requiresTarget(*c->rules) && tgts.empty()) continue;
        int score = lookaheadScore(c->id, tgts, *effCost);
        castable.push_back({ c->id, score, c->rules, std::move(tgts), effCost->cmc(), effCost });
    }
    // Graveyard cards with Flashback, Retrace, or Unearth
    for (Card* c : me().graveyard().cards()) {
        const ManaCost* cost = nullptr;
        if (c->rules->hasFlashback && canAffordWithUntapped(c->rules->flashbackCost))
            cost = &c->rules->flashbackCost;
        else if (c->rules->hasRetrace && canAffordWithUntapped(c->rules->manaCost)) {
            bool hasLand = false;
            for (const Card* h : me().hand().cards()) if (h->isLand()) { hasLand = true; break; }
            if (hasLand) cost = &c->rules->manaCost;
        } else if (c->rules->hasUnearth && c->rules->type.isCreature() &&
                   canAffordWithUntapped(c->rules->unearthCost))
            cost = &c->rules->unearthCost;
        if (!cost) continue;
        auto tgts = pickTargets(*c->rules);
        if (requiresTarget(*c->rules) && tgts.empty()) continue;
        int score = lookaheadScore(c->id, tgts, *cost);
        // Graveyard recursion bonus: spending a GY resource is now-or-never,
        // so boost score slightly to prefer it over an equivalent hand spell.
        score += 8;
        castable.push_back({ c->id, score, c->rules, std::move(tgts), cost->cmc(), cost });
    }
    // Command zone commanders (with commander tax)
    for (Card* c : m_game.command().cards()) {
        if (c->controllerId != m_id || !c->isCommander) continue;
        if (c->rules->type.isLand()) continue;
        int tax = 2 * m_game.commanderCastCount[m_id];
        int totalCmc = c->rules->manaCost.cmc() + tax;
        // Check affordability with current pool + untapped lands
        int avail = me().manaPool().total();
        for (const Card* lc : m_game.battlefield().cards()) {
            if (lc->controllerId != m_id || lc->tapped) continue;
            if (lc->rules->type.isLand()) ++avail;
        }
        if (avail < totalCmc) continue;
        auto tgts = pickTargets(*c->rules);
        if (requiresTarget(*c->rules) && tgts.empty()) continue;
        // Use a generic-only cost covering base + tax for the lookahead simulation.
        ManaCost simCost = ManaCost::parse(std::to_string(totalCmc));
        int score = lookaheadScore(c->id, tgts, simCost);
        castable.push_back({ c->id, score, c->rules, std::move(tgts), totalCmc });
    }
    if (castable.empty()) return false;

    std::sort(castable.begin(), castable.end(),
              [](const Candidate& a, const Candidate& b){
                  if (a.score != b.score) return a.score > b.score;
                  // Tiebreak: cast more color-restrictive spells first to preserve generic mana
                  // for less restrictive spells later in the same turn.
                  auto colorBits = [](const ManaCost* c) -> int {
                      if (!c) return 0;
                      uint8_t ci = c->colorIdentity();
                      int n = 0; for (; ci; ci &= ci-1) ++n; return n;
                  };
                  return colorBits(a.costPtr) > colorBits(b.costPtr);
              });

    // Decide how much mana to hold up for instants after this sorcery-speed spell.
    // Only hold up if we have a good instant and enough lands to both cast the spell
    // and keep the reserve.
    int instantReserve = bestInstantCmcInHand();

    // Pass-the-turn evaluation: if the best spell is weak AND we have a much better
    // spell we'll be able to cast next turn, skip casting now.
    if (!castable.empty() && castable[0].score < 30) {
        // Check if there's a high-value spell in hand we're ONE land drop from casting
        int curLands = 0;
        for (const Card* lc : m_game.battlefield().cards())
            if (lc->controllerId == m_id && lc->isLand()) ++curLands;
        for (const Card* hc : me().hand().cards()) {
            if (hc->rules->type.isLand() || hc->id == castable[0].id) continue;
            int cmc = hc->rules->manaCost.cmc();
            if (cmc == curLands + 1 && rateSpell(*hc) > castable[0].score + 35) {
                return false;  // wait for next turn's land to cast the better spell
            }
        }
    }

    for (const auto& cand : castable) {
        // Re-check affordability against the CURRENT pool + untapped lands
        // in case an earlier candidate tapped sources that this one also
        // needed. Skip silently so a stale candidate doesn't trigger a tap.
        if (cand.costPtr && !canAffordWithUntapped(*cand.costPtr)) continue;

        // Snapshot what we tap so a cast that fails AFTER tapping (a targeting
        // edge case castSpell catches that canAffordWithUntapped doesn't) can be
        // fully rolled back — otherwise those lands stay tapped and the mana
        // floats for the rest of the turn, which looks like "the AI tapped all
        // its lands for no reason." Same rollback the instant path uses.
        std::vector<ObjectId> wasUntapped;
        for (Card* lc : m_game.battlefield().cards())
            if (lc->controllerId == m_id && !lc->tapped) wasUntapped.push_back(lc->id);
        ManaPool poolBefore = me().manaPool();
        int      lifeBefore = me().life();

        // Tap colored lands first, then fill in generics. Also try to
        // reserve some lands for instants if possible.
        if (cand.costPtr) {
            int totalLands = 0;
            for (const Card* lc : m_game.battlefield().cards())
                if (lc->controllerId == m_id && !lc->tapped && lc->rules->type.isLand())
                    ++totalLands;
            bool canReserve = (instantReserve > 0) &&
                              (totalLands >= cand.tapCmc + instantReserve);
            if (!canReserve)
                tapForManaCost(*cand.costPtr);
            else
                tapForCostReserving(cand.tapCmc, instantReserve);
        } else {
            tapForCost(cand.tapCmc);
        }

        const std::string spellName = cand.rules->name;
        if (m_abilities.castSpell(cand.id, m_id, cand.targets)) {
            AIOUT << "  [" << me().name() << "] casts " << spellName << '\n';
            return true;
        }
        // Cast failed after tapping — restore the lands we tapped and the mana
        // pool/life so nothing is wasted, then stop this pass so we don't burn
        // fresh lands cascading through runner-up candidates.
        for (ObjectId id : wasUntapped)
            if (Card* lc = m_game.findCard(id)) lc->tapped = false;
        me().manaPool() = poolBefore;
        me().setLife(lifeBefore);
        AIOUT << "  [" << me().name() << "] aborting cast pass (" << spellName
              << " failed after tapping); lands untapped.\n";
        return false;
    }
    return false;
}

std::vector<Target> AiPlayer::pickTargets(const CardRules& rules) const {
    // Aura spells (Enchantment — Aura type): prefer own creatures as targets.
    // These are almost always beneficial buffs and should not target opponent creatures.
    bool isAura = rules.type.isEnchantment() && rules.type.hasSubtype("Aura");

    // Most auras have NO SP$ ValidTgts line — their target is the "Enchant X"
    // restriction (almost always a creature). Pick a creature directly here so
    // the aura actually attaches: curse auras (AttachAILogic$ Curse) go on an
    // opponent's creature, beneficial ones on our own best creature.
    if (isAura) {
        bool curse = false;
        auto it = rules.svars.find("AttachAILogic");
        if (it != rules.svars.end() && it->second == "Curse") curse = true;
        auto pick = [&](bool wantOwn) -> Card* {
            Card* best = nullptr; int bestScore = -1;
            for (Card* c : m_game.battlefield().cards()) {
                if (!c->isCreature()) continue;
                if ((c->controllerId == m_id) != wantOwn) continue;
                if (c->cantBeTargeted || c->hasKeyword(KeywordAbility::Shroud)) continue;
                if (c->hasKeyword(KeywordAbility::Hexproof) && c->controllerId != m_id) continue;
                int score = effectivePower(*c) + effectiveToughness(*c);
                if (score > bestScore) { bestScore = score; best = c; }
            }
            return best;
        };
        Card* best = pick(!curse);          // pump→own, curse→opponent
        if (!best) best = pick(curse);      // fall back to the other side
        if (best) return { Target::forCard(best->id) };
        return {};
    }

    for (const auto& rawLine : rules.abilityLines) {
        auto script = parseScriptLine(rawLine);
        if (script.abilityType != "SP" && script.abilityType != "AB") continue;

        std::string validTgts = std::string(script.get("ValidTgts"));
        if (validTgts.empty()) return {};

        // Aura with a creature/permanent filter: prefer own creature with highest P/T.
        if (isAura && (validTgts.find("Creature") != std::string::npos ||
                       validTgts.find("Permanent") != std::string::npos)) {
            Card* best = nullptr;
            int bestScore = -1;
            for (Card* c : m_game.battlefield().cards()) {
                if (c->controllerId != m_id || !c->isCreature()) continue;
                if (!cardMatchesAnyFilter(*c, validTgts, m_id)) continue;
                if (c->cantBeTargeted) continue;
                if (c->hasKeyword(KeywordAbility::Shroud)) continue;
                int score = effectivePower(*c) + effectiveToughness(*c);
                if (score > bestScore) { bestScore = score; best = c; }
            }
            if (best) return { Target::forCard(best->id) };
            return {};
        }

        // Multi-target: NumTgts$ or MaxTgts$ specifies how many targets
        int numTgts = script.getInt("NumTgts", script.getInt("MaxTgts", 1));
        if (numTgts > 1) {
            std::vector<Target> result;
            // Collect up to numTgts distinct targets
            for (const Card* c : m_game.battlefield().cards()) {
                if (static_cast<int>(result.size()) >= numTgts) break;
                if (!cardMatchesAnyFilter(*c, validTgts, m_id)) continue;
                if (c->cantBeTargeted) continue;
                if (c->hasKeyword(KeywordAbility::Shroud)) continue;
                if (c->hasKeyword(KeywordAbility::Hexproof) && c->controllerId == m_id) continue;
                result.push_back(Target::forCard(c->id));
            }
            if (!result.empty()) return result;
        }

        // "Any" â€” can target creature or player; prefer to kill a threat
        if (validTgts == "Any") {
            auto t = pickDamageTarget(script.getInt("NumDmg", 0));
            return t.isValid() ? std::vector<Target>{t} : std::vector<Target>{};
        }

        // "Player" or "Each Player" â€” always target opponent
        if (validTgts == "Player" || validTgts.find("Player") != std::string::npos) {
            return { Target::forPlayer(primaryOpponentId()) };
        }

        // Spell on Stack (Counterspell, Cancel, Remand, etc.)
        // Must come before the Creature check â€” "Spell.NonCreature" contains "Creature"
        // as a substring and would otherwise be misrouted to battlefield targeting.
        if (validTgts == "Spell"
            || validTgts.find("Spell") != std::string::npos
            || validTgts == "Instant,Sorcery"
            || validTgts.find("Instant") != std::string::npos) {
            const auto& sv = m_game.stack().cards();
            bool wantCreature    = validTgts.find("NonCreature") == std::string::npos
                                   && validTgts.find("Creature") != std::string::npos;
            bool excludeCreature = validTgts.find("NonCreature") != std::string::npos;
            for (int i = (int)sv.size() - 1; i >= 0; --i) {
                const Card* s = sv[i];
                if (!s) continue;
                if (s->controllerId == m_id) continue;  // don't counter own spells
                bool isCreature = s->rules && s->rules->type.isCreature();
                if (excludeCreature && isCreature) continue;
                if (wantCreature  && !isCreature) continue;
                return { Target::forCard(s->id) };
            }
            return {};
        }

        // Anything involving creatures.
        if (validTgts.find("Creature") != std::string::npos ||
            validTgts.find("Permanent") != std::string::npos) {
            // YouCtrl filter ⇒ this is a beneficial effect (pump/counter/protect):
            // pick our highest-value creature, not just the first match.
            if (validTgts.find("YouCtrl") != std::string::npos) {
                Card* best = nullptr; int bestScore = -1;
                for (Card* c : m_game.battlefield().cards()) {
                    if (c->controllerId != m_id) continue;
                    if (!cardMatchesAnyFilter(*c, validTgts, m_id)) continue;
                    if (c->cantBeTargeted || c->hasKeyword(KeywordAbility::Shroud)) continue;
                    int sc = std::max(0, effectivePower(*c)) + std::max(0, effectiveToughness(*c));
                    if (sc > bestScore) { bestScore = sc; best = c; }
                }
                if (best) return { Target::forCard(best->id) };
                return {};
            }
            // Otherwise it's removal/damage: pick the most threatening opponent permanent.
            auto t = pickBestTarget(validTgts);
            return t.isValid() ? std::vector<Target>{t} : std::vector<Target>{};
        }

        // Generic card on Stack
        if (validTgts.find("Card") != std::string::npos) {
            const auto& sv = m_game.stack().cards();
            for (int i = (int)sv.size() - 1; i >= 0; --i) {
                if (sv[i]) return { Target::forCard(sv[i]->id) };
            }
            return {};
        }
        break;
    }
    return {};
}

Target AiPlayer::pickDamageTarget(int damage) const {
    Card* best = nullptr;
    int bestPower = -1;
    for (Card* c : m_game.battlefield().cards()) {
        if (c->controllerId == m_id || !c->isCreature()) continue;
        if (c->cantBeTargeted) continue;
        if (c->hasKeyword(KeywordAbility::Shroud)) continue;
        if (c->hasKeyword(KeywordAbility::Hexproof) && c->controllerId != m_id) continue;
        if (effectiveToughness(*c) - c->markedDamage > damage) continue;
        int p = effectivePower(*c);
        if (p > bestPower) { bestPower = p; best = c; }
    }
    return best ? Target::forCard(best->id) : Target::forPlayer(primaryOpponentId());
}

void AiPlayer::tryActivatePlaneswalkers() {
    // Try to activate the first available planeswalker ability (prefer +N, then 0)
    // Snapshot IDs: activateAbility may drain triggers that cause zone changes.
    std::vector<ObjectId> ids;
    for (const Card* c : m_game.battlefield().cards())
        ids.push_back(c->id);
    for (ObjectId cid : ids) {
        Card* c = m_game.findCard(cid);
        if (!c || !c->isOnBattlefield()) continue;
        if (c->controllerId != m_id) continue;
        if (!c->rules->type.isPlaneswalker()) continue;
        if (c->tapped) continue; // "tapped" = already used this turn

        // â”€â”€ Planeswalker ability selection â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
        // Strategy:
        //   1. Use the ultimate (-N ability) if loyalty is high enough to survive
        //      (i.e. the remaining loyalty after activating would be > 0).
        //   2. Otherwise prefer the largest + ability to build toward ultimate.
        //   3. Use a 0-cost (flat) ability if no + is available.
        //   4. Use a - ability only if no + or 0 is available and the game is close.

        int curLoyalty = c->counterCount("loyalty");

        // Parse all loyalty abilities with their change amounts
        struct LoyaltyAbil { int idx; int delta; bool isUlt; };
        std::vector<LoyaltyAbil> loyalAbils;
        for (int i = 0; i < static_cast<int>(c->rules->abilityLines.size()); ++i) {
            auto s = parseScriptLine(c->rules->abilityLines[i]);
            if (s.abilityType != "AB") continue;
            auto cost = std::string(s.get("Cost", ""));
            int delta = 0;
            if (cost.find("AddCounter") != std::string::npos &&
                cost.find("LOYALTY")    != std::string::npos) {
                auto lt = cost.find('<'), sl = cost.find('/');
                if (lt != std::string::npos && sl != std::string::npos)
                    std::from_chars(cost.data()+lt+1, cost.data()+sl, delta);
            } else if (cost.find("SubCounter") != std::string::npos &&
                       cost.find("LOYALTY")    != std::string::npos) {
                auto lt = cost.find('<'), sl = cost.find('/');
                if (lt != std::string::npos && sl != std::string::npos) {
                    int n = 0;
                    std::from_chars(cost.data()+lt+1, cost.data()+sl, n);
                    delta = -n;
                }
            } else continue;
            int resultLoyalty = curLoyalty + delta;
            if (resultLoyalty <= 0 && delta < 0) continue; // would die
            // Mark as ultimate if it's the most-negative ability (biggest -)
            loyalAbils.push_back({i, delta, false});
        }

        // Mark the most-negative ability as the "ultimate"
        if (!loyalAbils.empty()) {
            int minDelta = 0;
            for (auto& a : loyalAbils) if (a.delta < minDelta) minDelta = a.delta;
            for (auto& a : loyalAbils) if (a.delta == minDelta) a.isUlt = true;
        }

        // Detect game-winning ultimates: abilities that create an emblem, win the game,
        // or generate overwhelming tokens/resources. Identified by their oracle text.
        auto isGameWinningUlt = [&](int abilIdx) -> bool {
            const auto& raw = c->rules->abilityLines[abilIdx];
            auto sl = parseScriptLine(raw);
            const auto& et = sl.effectType;
            // Emblem, WinsGame, or "each opponent loses" are clearly game-winning
            if (et == "Emblem" || et == "WinsGame" || et == "LosesGame") return true;
            // Large token generation (>= 5 tokens) at the ult level
            if (et == "MakeCard" || et == "Token") {
                int n = sl.getInt("Amount", sl.getInt("NumTokens", 0));
                if (n >= 5) return true;
            }
            return false;
        };

        // Choose: ult if affordable and loyal high; else largest +; else 0; else -
        int bestAbilityIdx = -1;
        auto selectBest = [&]() {
            // 0) Game-winning ultimates: always activate if loyalty permits
            for (auto& a : loyalAbils)
                if (a.isUlt && curLoyalty + a.delta >= 0 && isGameWinningUlt(a.idx))
                    { bestAbilityIdx = a.idx; return; }
            // 1) Regular ultimate if loyalty won't drop below 1
            for (auto& a : loyalAbils)
                if (a.isUlt && curLoyalty + a.delta >= 1) { bestAbilityIdx = a.idx; return; }
            // 2) Largest + ability
            int bestDelta = 0;
            for (auto& a : loyalAbils)
                if (a.delta > bestDelta) { bestDelta = a.delta; bestAbilityIdx = a.idx; }
            if (bestAbilityIdx >= 0) return;
            // 3) Any 0-delta ability
            for (auto& a : loyalAbils)
                if (a.delta == 0) { bestAbilityIdx = a.idx; return; }
            // 4) Least-costly - ability (closest to 0)
            for (auto& a : loyalAbils)
                if (a.delta < 0 && !a.isUlt) { bestAbilityIdx = a.idx; return; }
        };
        selectBest();

        if (bestAbilityIdx >= 0) {
            auto tgts = pickTargets(*c->rules);
            if (m_abilities.activateAbility(c->id, bestAbilityIdx, m_id, tgts)) {
                c->tapped = true;
                AIOUT << "  [" << me().name() << "] activates " << c->name() << '\n';
            }
        }
    }
}

bool AiPlayer::tryActivateAbilities() {
    // ControlPlayer: a controlled player takes no voluntary actions this turn.
    if (m_game.isTurnControlled(m_id)) return false;
    // Activate non-mana, non-planeswalker abilities for all controlled battlefield cards.
    // Iterates once (no loop) to avoid cascading effects mid-phase.
    // Snapshot IDs first: payActivationCost may sacrifice a card and call moveToZone,
    // which erases from battlefield().cards() and invalidates range-for iterators.
    bool activated = false;
    std::vector<ObjectId> ids;
    ids.reserve(m_game.battlefield().size());
    for (const Card* c : m_game.battlefield().cards())
        ids.push_back(c->id);

    for (ObjectId cid : ids) {
        Card* c = m_game.findCard(cid);
        if (!c || !c->isOnBattlefield()) continue;
        if (c->controllerId != m_id) continue;
        if (c->rules->type.isPlaneswalker()) continue; // handled by tryActivatePlaneswalkers

        for (int i = 0; i < static_cast<int>(c->rules->abilityLines.size()); ++i) {
            auto s = parseScriptLine(c->rules->abilityLines[i]);
            if (s.abilityType != "AB" || s.effectType == "Mana") continue;

            auto costStr = std::string(s.get("Cost", ""));

            // Parse cost tokens to check feasibility without paying yet
            bool hasTap = false;
            std::string manaCostTokens;
            {
                std::string_view rem = costStr;
                while (!rem.empty()) {
                    auto sp = rem.find(' ');
                    std::string_view tok = (sp == std::string_view::npos) ? rem : rem.substr(0, sp);
                    rem = (sp == std::string_view::npos) ? std::string_view{} : rem.substr(sp + 1);
                    if (tok == "T") {
                        hasTap = true;
                    } else if (tok.substr(0, 4) != "Sac<" &&
                               tok.substr(0, 8) != "Discard<" &&
                               tok.find("AddCounter") == std::string_view::npos &&
                               tok.find("SubCounter") == std::string_view::npos) {
                        if (!manaCostTokens.empty()) manaCostTokens += ' ';
                        manaCostTokens += std::string(tok);
                    }
                }
            }

            if (hasTap && (c->tapped || c->summoningSickness)) continue;

            // Sacrifice evaluation: only activate sac-for-value abilities when beneficial.
            // Detect "Sac<Self>" or "Sac<Token>" in cost with draw/benefit in effect.
            bool hasSacSelf = (costStr.find("Sac<Self>") != std::string::npos ||
                               costStr.find("SacrificeSource") != std::string::npos);
            if (hasSacSelf) {
                // Only sac tokens or if the effect draws cards / gains significant value
                bool isToken = c->isToken;
                bool hasDraw = (s.effectType == "Draw" || s.effectType == "GainLife");
                bool isDyingAnyway = (c->markedDamage > 0 &&
                                      c->markedDamage >= effectiveToughness(*c));
                if (!isToken && !hasDraw && !isDyingAnyway) continue;
            }

            if (!manaCostTokens.empty()) {
                auto cost = ManaCost::parse(manaCostTokens);
                if (!canAffordWithUntapped(cost)) continue;
                tapForCost(cost.cmc());  // lazy tap â€” only what the ability needs
            }

            // Pick targets if required
            auto validTgts = std::string(s.get("ValidTgts", ""));
            std::vector<Target> targets;
            if (!validTgts.empty()) {
                targets = pickTargets(*c->rules);
                if (targets.empty()) continue;
            }

            const std::string cardName = c->name();
            if (m_abilities.activateAbility(cid, i, m_id, targets)) {
                AIOUT << "  [" << me().name() << "] activates " << cardName << '\n';
                activated = true;
            }
            break; // one ability per card per pass
        }
    }
    return activated;
}

bool AiPlayer::tryCycle() {
    for (Card* c : me().hand().cards()) {
        // Transmute: use when card is uncastable
        if (c->rules->hasTransmute && !canAffordWithUntapped(c->rules->manaCost)
                && canAffordWithUntapped(c->rules->transmuteCost)) {
            tapForCost(c->rules->transmuteCost.cmc());
            std::string nm = c->rules->name;
            if (m_abilities.activateTransmute(c->id, m_id)) {
                while (StateBasedActions::run(m_game)) {}
                m_abilities.drainPendingTriggers();
                AIOUT << "  [" << me().name() << "] transmutes " << nm << '\n';
                return true;
            }
        }

        // Reinforce: pay cost to put counters on a creature
        if (c->rules->hasReinforce && !canAffordWithUntapped(c->rules->manaCost)
                && canAffordWithUntapped(c->rules->reinforceCost)) {
            // Find best creature to reinforce
            Card* target = nullptr; int bestVal = -1;
            for (Card* bf : m_game.battlefield().cards()) {
                if (bf->controllerId != m_id || !bf->isCreature()) continue;
                int val = effectivePower(*bf) + effectiveToughness(*bf);
                if (val > bestVal) { bestVal = val; target = bf; }
            }
            if (target) {
                tapForCost(c->rules->reinforceCost.cmc());
                // Discard and put counters
                std::string nm = c->rules->name;
                m_game.moveToZone(c->id, ZoneType::Graveyard, m_id);
                target->addCounter("+1/+1", c->rules->reinforceAmount);
                while (StateBasedActions::run(m_game)) {}
                m_abilities.drainPendingTriggers();
                AIOUT << "  [" << me().name() << "] reinforces with " << nm << '\n';
                return true;
            }
        }

        bool canCycle = c->rules->hasCycling || c->rules->hasTypeCycling;
        if (!canCycle) continue;
        // Only cycle cards that can't be cast this turn (avoid cycling castable spells)
        if (canAffordWithUntapped(c->rules->manaCost)) continue;
        const ManaCost& cost = c->rules->hasTypeCycling
                               ? c->rules->typeCyclingCost : c->rules->cyclingCost;
        if (!canAffordWithUntapped(cost)) continue;
        tapForCost(cost.cmc());
        const std::string cycleName = c->rules->name;
        if (m_abilities.activateCycling(c->id, m_id)) {
            while (StateBasedActions::run(m_game)) {}
            m_abilities.drainPendingTriggers();
            AIOUT << "  [" << me().name() << "] cycles " << cycleName << '\n';
            return true;
        }
    }
    return false;
}

bool AiPlayer::tryActivateLevelUp() {
    for (Card* c : m_game.battlefield().cards()) {
        if (c->controllerId != m_id) continue;
        if (!c->rules->hasLevelUp) continue;
        if (!canAfford(c->rules->levelUpCost)) continue;
        tapForCost(c->rules->levelUpCost.cmc());
        if (m_abilities.activateLevelUp(c->id, m_id)) {
            while (StateBasedActions::run(m_game)) {}
            m_abilities.drainPendingTriggers();
            return true;
        }
    }
    return false;
}

bool AiPlayer::tryEquipEquipment() {
    bool equipped = false;
    for (Card* eq : m_game.battlefield().cards()) {
        if (eq->controllerId != m_id) continue;

        // Identify equip cost from K:Equip:N keyword
        int equipCost = -1;
        for (const auto& kw : eq->rules->keywords) {
            equipCost = parseEquipCost(kw);
            if (equipCost >= 0) break;
        }
        if (equipCost < 0) continue;
        if (!canAfford(ManaCost::parse(std::to_string(equipCost)))) continue;

        // Pick our creature with the highest power+toughness
        Card* target = nullptr;
        int bestScore = -1;
        for (Card* c : m_game.battlefield().cards()) {
            if (c->controllerId != m_id || !c->isCreature()) continue;
            int score = effectivePower(*c) + effectiveToughness(*c);
            if (score > bestScore) { bestScore = score; target = c; }
        }
        if (!target) continue;
        // Skip if already equipped to the best target
        if (eq->attachedTo == target->id) continue;

        if (m_abilities.activateEquip(eq->id, target->id, m_id)) {
            AIOUT << "  [" << me().name() << "] equips " << eq->name()
                      << " onto " << target->name() << '\n';
            equipped = true;
        }
    }
    return equipped;
}

Target AiPlayer::pickBestTarget(std::string_view filter) const {
    // Skip cards with CantBeTargeted, Shroud, or Hexproof from our perspective
    auto canTarget = [&](const Card* c) {
        if (c->cantBeTargeted) return false;
        if (c->hasKeyword(KeywordAbility::Shroud)) return false;
        if (c->hasKeyword(KeywordAbility::Hexproof) && c->controllerId != m_id)
            return false;
        return true;
    };

    Card* best = nullptr;
    int bestScore = -1;
    for (Card* c : m_game.battlefield().cards()) {
        if (!cardMatchesAnyFilter(*c, filter, m_id)) continue;
        if (c->controllerId == m_id) continue;
        if (!canTarget(c)) continue;
        float salt = SaltDatabase::get(c->rules->name);
        int saltBonus = static_cast<int>(salt * 15.0f);
        int base = c->isCreature() ? effectivePower(*c) + effectiveToughness(*c) : 1;
        // Bonus for immediate threats
        if (c->isCommander) base += 12;                                  // kill their commander
        if (c->attacking)   base += 6;                                   // currently attacking us
        if (c->hasKeyword(KeywordAbility::Flying))  base += 3;
        if (c->hasKeyword(KeywordAbility::Lifelink)) base += 2;
        if (c->hasKeyword(KeywordAbility::Deathtouch)) base += 3;
        if (c->rules->type.isPlaneswalker()) base += 8;                  // planeswalkers snowball
        // Commander damage urgency: if they've dealt 10+ commander damage to us already
        if (c->isCommander) {
            int cmdDmg = m_game.player(m_id).commanderDamageFrom(c->ownerId);
            if (cmdDmg >= 10) base += 15;  // getting close to 21 â€” remove immediately
        }
        // Threat prioritization: combo pieces score much higher than raw power
        // Detect combo potential: cards with low P/T but powerful activated abilities
        // (e.g. Thassa's Oracle, Thopter Foundry, Walking Ballista)
        bool isCombo = false;
        for (const auto& raw : c->rules->abilityLines) {
            auto sl = parseScriptLine(raw);
            if (sl.effectType == "WinsGame" || sl.effectType == "LosesGame") isCombo = true;
            if (sl.effectType == "Mill" || sl.effectType == "Draw") {
                auto amt = sl.getInt("NumCards", sl.getInt("Amount", 0));
                if (amt >= 5) isCombo = true;  // mass draw/mill = potential combo
            }
        }
        if (isCombo) base += 25;  // always prioritize combo pieces

        int score = base + saltBonus;
        if (score > bestScore) { bestScore = score; best = c; }
    }
    if (best) return Target::forCard(best->id);
    for (Card* c : m_game.battlefield().cards()) {
        if (!cardMatchesAnyFilter(*c, filter, m_id)) continue;
        if (!canTarget(c)) continue;
        return Target::forCard(c->id);
    }
    return Target{};
}

Target AiPlayer::pickDestroyTarget() const {
    Card* best = nullptr;
    int bestScore = -1;
    for (Card* c : m_game.battlefield().cards()) {
        if (c->controllerId == m_id || !c->isCreature()) continue;
        float salt = SaltDatabase::get(c->rules->name);
        int saltBonus = static_cast<int>(salt * 20.0f);
        int score = effectivePower(*c) + effectiveToughness(*c) + saltBonus;
        if (score > bestScore) { bestScore = score; best = c; }
    }
    return best ? Target::forCard(best->id) : Target{};
}

// â”€â”€ Combat â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

bool AiPlayer::shouldAttack(const Card& attacker) const {
    int myPow = effectivePower(attacker);
    int myTgh = effectiveToughness(attacker);

    // 0-power creatures deal no damage â€” never worth attacking
    if (myPow <= 0) return false;

    // Always attack if we can deal lethal to opponent (win condition)
    if (myPow >= opp().life()) return true;

    // Commander: always attack â€” it returns to the command zone on death rather
    // than being permanently lost. The tax cost (2 mana per recast) makes reckless
    // attacks somewhat costly but it's almost never wrong to swing with the commander.
    // Exception: skip if 0-power (handled above) or if the opponent can profit from us
    // spending commander tax by engineering a forced bad trade.
    if (attacker.isCommander) {
        // Check if this attack would let the commander reach lethal commander damage
        int cmdDmgDealt = opp().commanderDamageFrom(m_id);
        if (cmdDmgDealt + myPow >= 21) return true;  // lethal commander damage swing!
        // Still attack unless every legal blocker both kills the commander AND we
        // can't kill them (very bad trade when tax is already high).
        int tax = m_game.commanderCastCount[m_id];
        if (tax >= 4) {
            // High tax: be more conservative â€” only attack if we have a good chance
            // of connecting or making a favorable trade.
            bool allBlockersKillUs = true;
            for (const Card* oppC : m_game.battlefield().cards()) {
                if (oppC->controllerId == m_id || !oppC->isCreature() || oppC->tapped) continue;
                int oppPow = effectivePower(*oppC);
                bool kills = (oppPow >= myTgh) || oppC->hasKeyword(KeywordAbility::Deathtouch);
                bool weKill = (myPow >= effectiveToughness(*oppC)) ||
                              attacker.hasKeyword(KeywordAbility::Deathtouch);
                if (!kills || weKill) { allBlockersKillUs = false; break; }
            }
            if (allBlockersKillUs && tax >= 4) return false;
        }
        return true;  // attack with commander by default
    }

    // Indestructible creatures can always attack safely (can't die in combat)
    if (attacker.hasKeyword(KeywordAbility::Indestructible)) return true;

    // Always attack with evasion (flying, shadow, fear, etc.) since it may go unblocked
    if (attacker.hasKeyword(KeywordAbility::Flying)  ||
        attacker.hasKeyword(KeywordAbility::Shadow)  ||
        attacker.hasKeyword(KeywordAbility::Fear))
        return true;

    // Check if any opponent creature would block us and kill us without us killing them
    int myValue = myPow * 3 + myTgh + keywordBonus(attacker);
    for (const Card* oppC : m_game.battlefield().cards()) {
        if (oppC->controllerId == m_id || !oppC->isCreature() || oppC->tapped) continue;

        // Can this opponent creature actually block us? (flying restriction)
        if (attacker.hasKeyword(KeywordAbility::Flying)) {
            if (!oppC->hasKeyword(KeywordAbility::Flying) &&
                !oppC->hasKeyword(KeywordAbility::Reach))
                continue; // can't block flyer
        }
        // Shadow
        if (attacker.hasKeyword(KeywordAbility::Shadow) !=
            oppC->hasKeyword(KeywordAbility::Shadow))
            continue;

        int oppPow = effectivePower(*oppC);
        int oppTgh = effectiveToughness(*oppC);

        bool killsMe  = (oppPow >= myTgh) ||
                        (oppC->hasKeyword(KeywordAbility::Deathtouch) && oppPow > 0);
        bool killsOpp = (myPow >= oppTgh) ||
                        (attacker.hasKeyword(KeywordAbility::Deathtouch) && myPow > 0);

        if (killsMe && !killsOpp) {
            return false; // profitable block for opponent â€” skip this attack
        }

        // Mutual kill: only accept if we're trading even or up in value
        if (killsMe && killsOpp) {
            int oppValue = oppPow * 3 + oppTgh + keywordBonus(*oppC);
            if (oppValue > myValue) return false; // trading down â€” skip
        }
    }
    return true;
}


void AiPlayer::doAttackers(TurnManager& tm) {
    // ControlPlayer: a controlled player declares no attackers (denies them the turn).
    if (m_game.isTurnControlled(m_id)) return;
    // MCTS path: let MctsSearch select the attacker subset
    if (m_mctsEnabled) {
        std::vector<float>* dist = nullptr;
        if (m_epOut) {
            m_epOut->policy.emplace_back();
            dist = &m_epOut->policy.back();
        }
        // Reuse m_mctsSearch so its transposition table stays warm across decisions
        MctsAction action = m_mctsSearch.searchAttackers(m_game, m_abilities, m_id,
                                                          m_mctsConfig, dist, *m_valueFn, *m_policyFn);
        std::vector<std::string> names;
        for (ObjectId id : action.attackerIds) {
            if (tm.declareAttacker(id, primaryOpponentId())) {
                if (const Card* c = m_game.findCard(id)) names.push_back(c->name());
            }
        }
        if (!names.empty()) {
            AIOUT << "  [" << me().name() << "] attacks with:";
            for (const auto& n : names) AIOUT << ' ' << n;
            AIOUT << '\n';
        }
        return;
    }

    // â”€â”€ Lethal swing check â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    // If we can deal lethal damage to the opponent by attacking with ALL eligible
    // creatures (ignoring trading concerns), do so immediately â€” it's game over.
    {
        int oppLife = opp().life();
        int totalPower = 0;
        std::vector<Card*> swingAll;
        for (Card* c : m_game.battlefield().cards()) {
            if (c->controllerId != m_id || !c->isCreature() ||
                c->tapped || c->summoningSickness || c->cantAttack) continue;
            if (c->hasKeyword(KeywordAbility::Flying)) {
                // Only count flyers if opponent has no flying blockers
                bool hasAirBlocker = false;
                for (const Card* oC : m_game.battlefield().cards()) {
                    if (oC->controllerId != m_id && oC->isCreature() &&
                        (oC->hasKeyword(KeywordAbility::Flying) ||
                         oC->hasKeyword(KeywordAbility::Reach)))
                        { hasAirBlocker = true; break; }
                }
                if (hasAirBlocker) continue;
            }
            totalPower += effectivePower(*c);
            swingAll.push_back(c);
        }
        // Account for opponent blockers reducing damage
        int blockerAbsorb = 0;
        for (const Card* oC : m_game.battlefield().cards()) {
            if (oC->controllerId == m_id || !oC->isCreature() || oC->tapped) continue;
            blockerAbsorb += effectiveToughness(*oC);
        }
        // Conservative: assume each blocker absorbs its toughness in damage
        // If even after full blocking we deal lethal, swing with everything
        int lifelink = 0;
        for (Card* c : swingAll)
            if (c->hasKeyword(KeywordAbility::Lifelink)) lifelink += effectivePower(*c);
        int netDamage = totalPower - blockerAbsorb;
        if (netDamage >= oppLife) {
            // LETHAL SWING â€” attack with all eligible creatures
            std::vector<std::string> lethalNames;
            for (Card* c : swingAll) {
                if (tm.declareAttacker(c->id, primaryOpponentId()))
                    lethalNames.push_back(c->name());
            }
            if (!lethalNames.empty()) {
                AIOUT << "  [" << me().name() << "] LETHAL SWING:";
                for (const auto& n : lethalNames) AIOUT << ' ' << n;
                AIOUT << '\n';
                return;
            }
        }
        // Commander damage lethal check: commander can win by commander damage
        for (Card* c : m_game.battlefield().cards()) {
            if (c->controllerId != m_id || !c->isCommander) continue;
            if (c->tapped || c->summoningSickness) continue;
            int cmdDmgDealt = opp().commanderDamageFrom(m_id);
            if (cmdDmgDealt + effectivePower(*c) >= 21) {
                // This attack alone wins via commander damage â€” always make it
                if (!tm.combatState().isAttacking(c->id))
                    tm.declareAttacker(c->id, primaryOpponentId());
            }
        }
    }

    // Heuristic path
    std::vector<std::string> names;
    for (Card* c : m_game.battlefield().cards()) {
        if (c->controllerId != m_id || !c->isCreature() ||
            c->tapped || c->summoningSickness) continue;
        // MustAttack: force this creature to attack (if it can), skipping the
        // shouldAttack heuristic that would normally prevent bad attacks.
        if (c->mustAttack) {
            uint8_t target = (c->mustAttackTarget != 255) ? c->mustAttackTarget
                                                          : (primaryOpponentId());
            if (tm.declareAttacker(c->id, target))
                names.push_back(c->name());
            continue;
        }
        // Goaded: must attack if able, and can't attack the goading player.
        if (c->goaded) {
            // In 2-player, goadedBy is the opponent, so we attack that same opponent anyway.
            // Goad forces attack but the target must NOT be the goading player.
            uint8_t target = (c->goadedBy != 255 && c->goadedBy != (primaryOpponentId()))
                             ? (primaryOpponentId())   // opponent isn't the goad source, attack normally
                             : (primaryOpponentId());  // in 2-player goad, still attack the only opponent
            if (tm.declareAttacker(c->id, target))
                names.push_back(c->name());
            continue;
        }
        if (!shouldAttack(*c)) continue; // skip suicidal attacks

        // Check if attacking a threatening planeswalker is better than face
        bool attackedPw = false;
        for (const Card* pw : m_game.battlefield().cards()) {
            if (pw->controllerId != (primaryOpponentId())) continue;
            if (!pw->rules->type.isPlaneswalker()) continue;
            int loy = pw->counterCount("loyalty");
            // Attack the PW if it has â‰¥ 4 loyalty (threatening) and we can kill it in â‰¤2 hits
            if (loy >= 4 && effectivePower(*c) >= loy / 2) {
                if (tm.declareAttackerVsPlaneswalker(c->id, pw->id)) {
                    names.push_back(c->name() + "â†’" + pw->rules->name);
                    attackedPw = true;
                }
                break;
            }
        }
        if (attackedPw) continue;

        if (tm.declareAttacker(c->id, primaryOpponentId()))
            names.push_back(c->name());
    }
    if (!names.empty()) {
        AIOUT << "  [" << me().name() << "] attacks with:";
        for (const auto& n : names) AIOUT << ' ' << n;
        AIOUT << '\n';
    }
}

void AiPlayer::declareBlockers(TurnManager& tm) {
    // MCTS path: let MctsSearch assign blockers
    if (m_mctsEnabled) {
        std::vector<float>* dist = nullptr;
        if (m_epOut) {
            m_epOut->policy.emplace_back();
            dist = &m_epOut->policy.back();
        }
        MctsAction action = m_mctsSearch.searchBlockers(m_game, m_abilities, tm, m_id,
                                                         m_mctsConfig, dist, *m_valueFn, *m_policyFn);
        for (const auto& [blockerId, attackerId] : action.blockerAssignments) {
            if (tm.declareBlocker(blockerId, attackerId)) {
                const Card* blk = m_game.findCard(blockerId);
                const Card* atk = m_game.findCard(attackerId);
                if (blk && atk)
                    AIOUT << "  [" << me().name() << "] blocks "
                              << atk->name() << " with " << blk->name() << '\n';
            }
        }
        return;
    }

    // Heuristic path
    // MustBlock: creatures with a forced block target must block that attacker first.
    for (Card* c : m_game.battlefield().cards()) {
        if (c->controllerId != m_id || !c->isCreature() || c->tapped) continue;
        if (c->mustBlockTarget == kInvalidId) continue;
        if (tm.combatState().isBlocking(c->id)) continue;
        if (tm.combatState().isAttacking(c->mustBlockTarget))
            tm.declareBlocker(c->id, c->mustBlockTarget);
    }
    // MustBlock "if able" (any attacker) — block the first attacker it legally can.
    for (Card* c : m_game.battlefield().cards()) {
        if (c->controllerId != m_id || !c->isCreature() || c->tapped) continue;
        if (!c->mustBlockAny || tm.combatState().isBlocking(c->id)) continue;
        for (const auto& a : tm.combatState().attacks)
            if (tm.declareBlocker(c->id, a.attackerId)) break;
    }

    // Planeswalker defense: block attackers heading for our planeswalkers
    // (attacks targeting a PW are in atk.defendingPlaneswalker)
    for (const auto& atk : tm.combatState().attacks) {
        if (atk.defendingPlaneswalker == kInvalidId) continue;
        if (!atk.blockerIds.empty()) continue;
        const Card* pw = m_game.findCard(atk.defendingPlaneswalker);
        const Card* attacker = m_game.findCard(atk.attackerId);
        if (!pw || !attacker) continue;
        int pwLoy = pw->counterCount("loyalty");
        if (pwLoy <= 0) continue;
        int atkPow = effectivePower(*attacker);
        // Block if attacker would kill the PW or PW has >= 5 loyalty (valuable)
        if (atkPow >= pwLoy || pwLoy >= 5) {
            // Find best blocker for this attacker
            Card* defender = nullptr; int bestScore = -1;
            for (Card* c : m_game.battlefield().cards()) {
                if (c->controllerId != m_id || !c->isCreature() || c->tapped) continue;
                if (tm.combatState().isBlocking(c->id)) continue;
                if (attacker->hasKeyword(KeywordAbility::Flying) &&
                    !c->hasKeyword(KeywordAbility::Flying) &&
                    !c->hasKeyword(KeywordAbility::Reach)) continue;
                bool kills   = effectivePower(*c) >= effectiveToughness(*attacker) ||
                               c->hasKeyword(KeywordAbility::Deathtouch);
                bool survives = effectiveToughness(*c) > atkPow;
                int score = effectivePower(*c) + effectiveToughness(*c) +
                            (kills ? 20 : 0) + (survives ? 10 : 0);
                if (score > bestScore) { bestScore = score; defender = c; }
            }
            if (defender) {
                tm.declareBlocker(defender->id, atk.attackerId);
                AIOUT << "  [" << me().name() << "] defends " << pw->rules->name << '\n';
            }
        }
    }

    // Desperation mode: at ≤5 life, block EVERYTHING regardless of trade quality.
    bool desperationMode = (me().life() <= 5);

    // Check if total incoming damage is lethal (forces chump blocks)
    int incomingDamage = 0;
    for (const auto& atk : tm.combatState().attacks)
        if (const Card* c = m_game.findCard(atk.attackerId))
            if (atk.blockerIds.empty())
                incomingDamage += effectivePower(*c);

    // Protect-the-win: when we're heavily ahead (opponent at ≤5 life), hold back
    // our strongest attackers as potential blockers to prevent opponent comebacks.
    bool protectWin = (opp().life() <= 5 && me().life() >= 10);

    bool lifeThreaten = (incomingDamage >= me().life()) || desperationMode;

    // Also treat commander damage as threatening: if an unblocked commander attack
    // would reach 21 commander damage, treat it as lethal (force blocks).
    if (!lifeThreaten) {
        for (const auto& atk : tm.combatState().attacks) {
            if (!atk.blockerIds.empty()) continue;
            const Card* c = m_game.findCard(atk.attackerId);
            if (!c || !c->isCommander) continue;
            int cmdDmgSoFar = me().commanderDamageFrom(c->ownerId);
            if (cmdDmgSoFar + effectivePower(*c) >= 21) { lifeThreaten = true; break; }
        }
    }

    for (const auto& atk : tm.combatState().attacks) {
        const Card* attacker = m_game.findCard(atk.attackerId);
        if (!attacker) continue;

        int atkPow = effectivePower(*attacker);
        int atkTgh = effectiveToughness(*attacker);

        // Categorise candidate blockers (ported from Mage's CombatUtil.blockWithGoodTrade2):
        //   killerSurvivor â€” kills attacker AND blocker lives (best)
        //   killerDies     â€” kills attacker but blocker dies (trade)
        //   surviveOnly    â€” blocker lives but can't kill attacker (absorb damage)
        //   chump          â€” everything else
        Card* killerSurvivor = nullptr;  int ksTgh = INT_MAX;
        Card* killerDies     = nullptr;  int kdTgh = INT_MAX;
        Card* surviveOnly    = nullptr;  int soTgh = INT_MAX;
        Card* chumpBlock     = nullptr;

        for (Card* c : m_game.battlefield().cards()) {
            if (c->controllerId != m_id || !c->isCreature() || c->tapped) continue;
            if (tm.combatState().isBlocking(c->id)) continue;
            if (attacker->hasKeyword(KeywordAbility::Flying)) {
                if (!c->hasKeyword(KeywordAbility::Flying) &&
                    !c->hasKeyword(KeywordAbility::Reach)) continue;
            }
            // Protect-the-win: don't block with our best attackers when ahead
            if (protectWin && !lifeThreaten) {
                int myPow = effectivePower(*c);
                // Skip blocking with creatures that contribute significantly to our lethal swing
                if (myPow >= 3 && !c->isCommander) continue;
            }

            int blkPow = effectivePower(*c);
            int blkTgh = effectiveToughness(*c);
            bool kills    = (blkPow >= atkTgh) || c->hasKeyword(KeywordAbility::Deathtouch);
            bool survives = blkTgh > atkPow && !attacker->hasKeyword(KeywordAbility::Deathtouch);

            // Prefer flying blockers for flying attackers (save non-flyers for ground)
            int blocker_score = blkTgh;
            if (attacker->hasKeyword(KeywordAbility::Flying) &&
                c->hasKeyword(KeywordAbility::Flying) &&
                !c->hasKeyword(KeywordAbility::Reach))
                blocker_score -= 10;  // strongly prefer the actual flyer

            if (kills && survives) {
                if (!killerSurvivor || blocker_score < ksTgh)
                    { killerSurvivor = c; ksTgh = blocker_score; }
            } else if (kills) {
                if (!killerDies || blocker_score < kdTgh)
                    { killerDies = c; kdTgh = blocker_score; }
            } else if (survives) {
                if (!surviveOnly || blkTgh < soTgh)
                    { surviveOnly = c; soTgh = blkTgh; }
            } else if (!chumpBlock) {
                chumpBlock = c;
            }
        }

        // Priority 1: kill attacker, blocker survives
        Card* bestBlocker = killerSurvivor;

        // Priority 2: trade (kill attacker, blocker dies) â€” only when trade is favorable
        if (!bestBlocker && killerDies) {
            // Trade is worth it if the attacker is stronger than our blocker
            int atkVal = atkPow * 3 + atkTgh;
            int blkVal = effectivePower(*killerDies) * 3 + effectiveToughness(*killerDies);
            if (atkVal >= blkVal)
                bestBlocker = killerDies;
        }

        // Priority 3: blocker survives but can't kill attacker â€” absorb damage
        // Commander attacks are treated more urgently: block if we've accumulated
        // any commander damage (10+) or if the attack would push us past threshold.
        if (!bestBlocker && surviveOnly && atkPow >= 3) {
            int cmdDmgTaken = attacker->isCommander
                              ? me().commanderDamageFrom(attacker->ownerId) : 0;
            bool cmdUrgent  = attacker->isCommander && cmdDmgTaken >= 10;
            if (me().life() <= 20 || atkPow >= 5 || cmdUrgent)
                bestBlocker = surviveOnly;
        }

        // Priority 4: chump block only if total damage is lethal
        if (!bestBlocker && lifeThreaten && chumpBlock)
            bestBlocker = chumpBlock;

        // Priority 4b: if chump is the only option and life is critical, chump with any available
        if (!bestBlocker && lifeThreaten) {
            for (Card* c : m_game.battlefield().cards()) {
                if (c->controllerId != m_id || !c->isCreature() || c->tapped) continue;
                if (tm.combatState().isBlocking(c->id)) continue;
                if (attacker->hasKeyword(KeywordAbility::Flying)) {
                    if (!c->hasKeyword(KeywordAbility::Flying) &&
                        !c->hasKeyword(KeywordAbility::Reach)) continue;
                }
                bestBlocker = c;
                break;
            }
        }

        if (bestBlocker) {
            tm.declareBlocker(bestBlocker->id, atk.attackerId);
            AIOUT << "  [" << me().name() << "] blocks "
                      << attacker->name() << " with " << bestBlocker->name() << '\n';
        }

        // Gang-block: if the attacker is still alive after one blocker and is high-power,
        // assign a second blocker to finish it off (trade with one, kill with the other).
        if (bestBlocker && atkPow >= 5) {
            int bestBlkPow = effectivePower(*bestBlocker);
            bool alreadyKills = (bestBlkPow >= atkTgh) ||
                                bestBlocker->hasKeyword(KeywordAbility::Deathtouch);
            if (!alreadyKills) {
                // Find a second creature that together would kill the attacker
                for (Card* c : m_game.battlefield().cards()) {
                    if (c->controllerId != m_id || !c->isCreature() || c->tapped) continue;
                    if (tm.combatState().isBlocking(c->id)) continue;
                    if (c->id == bestBlocker->id) continue;
                    if (attacker->hasKeyword(KeywordAbility::Flying)) {
                        if (!c->hasKeyword(KeywordAbility::Flying) &&
                            !c->hasKeyword(KeywordAbility::Reach)) continue;
                    }
                    int combinedPow = bestBlkPow + effectivePower(*c);
                    if (combinedPow >= atkTgh) {
                        tm.declareBlocker(c->id, atk.attackerId);
                        AIOUT << "  [" << me().name() << "] gang-blocks "
                                  << attacker->name() << " with " << c->name() << '\n';
                        break;
                    }
                }
            }
        }
    }
}

bool AiPlayer::tryActivateMonstrosity() {
    bool activated = false;
    std::vector<ObjectId> ids;
    for (const Card* c : m_game.battlefield().cards())
        ids.push_back(c->id);
    for (ObjectId cid : ids) {
        Card* c = m_game.findCard(cid);
        if (!c || !c->isOnBattlefield()) continue;
        if (c->controllerId != m_id) continue;
        if (!c->rules->hasMonstrosity || c->monstrous) continue;
        if (!canAffordWithUntapped(c->rules->monstrosityCost)) continue;
        tapForCost(c->rules->monstrosityCost.cmc());
        const std::string cname = c->rules->name;
        if (m_abilities.activateMonstrosity(cid, m_id)) {
            AIOUT << "  [" << me().name() << "] activates Monstrosity on " << cname << '\n';
            while (StateBasedActions::run(m_game)) {}
            activated = true;
        }
    }
    return activated;
}

void AiPlayer::tryActivateOutlastAdapt() {
    // Snapshot IDs first — activateOutlast/Adapt drains triggers which may
    // modify the battlefield, invalidating a direct iteration pointer.
    std::vector<ObjectId> ids;
    for (const Card* c : m_game.battlefield().cards()) ids.push_back(c->id);

    for (ObjectId cid : ids) {
        Card* c = m_game.findCard(cid);
        if (!c || !c->isOnBattlefield() || c->controllerId != m_id || !c->isCreature()) continue;

        if (c->rules->hasOutlast && !c->tapped && !c->summoningSickness &&
            canAffordWithUntapped(c->rules->outlastCost)) {
            tapForCost(c->rules->outlastCost.cmc());
            m_abilities.activateOutlast(cid, m_id);
        }
        // Re-fetch after potential trigger drain
        c = m_game.findCard(cid);
        if (!c || !c->isOnBattlefield()) continue;

        if (c->rules->hasAdapt && c->counterCount("+1/+1") == 0 &&
            canAffordWithUntapped(c->rules->adaptCost)) {
            tapForCost(c->rules->adaptCost.cmc());
            m_abilities.activateAdapt(cid, m_id);
        }
    }
}

void AiPlayer::tryUnlockRooms() {
    static const std::vector<Target> kNoTargets;
    // Snapshot IDs first — activateAbility can drain triggers and modify the battlefield.
    std::vector<ObjectId> ids;
    for (const Card* c : m_game.battlefield().cards()) ids.push_back(c->id);

    for (ObjectId cid : ids) {
        Card* c = m_game.findCard(cid);
        if (!c || !c->isOnBattlefield() || c->controllerId != m_id) continue;
        if (!c->rules->hasRoom) continue;

        if (!c->doorAUnlocked && c->rules->doorACost.cmc() > 0 &&
            canAffordWithUntapped(c->rules->doorACost)) {
            tapForCost(c->rules->doorACost.cmc());
            m_abilities.activateAbility(cid, 0, m_id, kNoTargets);
        }
        c = m_game.findCard(cid);
        if (!c || !c->isOnBattlefield()) continue;

        if (!c->doorBUnlocked && c->rules->doorBCost.cmc() > 0 &&
            canAffordWithUntapped(c->rules->doorBCost)) {
            tapForCost(c->rules->doorBCost.cmc());
            m_abilities.activateAbility(cid, 1, m_id, kNoTargets);
        }
    }
}

bool AiPlayer::tryActivateGraveyardAbilities() {
    bool activated = false;

    // Snapshot GY card IDs first â€” activations may move cards
    std::vector<ObjectId> gyIds;
    for (Card* c : me().graveyard().cards()) gyIds.push_back(c->id);

    for (ObjectId cid : gyIds) {
        Card* c = m_game.findCard(cid);
        if (!c || c->zone != ZoneType::Graveyard || c->controllerId != m_id) continue;

        // Scavenge
        if (c->rules->hasScavenge && c->rules->type.isCreature() &&
            canAffordWithUntapped(c->rules->scavengeCost)) {
            Card* target = nullptr;
            int bestScore = -1;
            for (Card* bf : m_game.battlefield().cards()) {
                if (bf->controllerId != m_id || !bf->isCreature()) continue;
                int score = effectivePower(*bf) + effectiveToughness(*bf);
                if (score > bestScore) { bestScore = score; target = bf; }
            }
            if (target) {
                tapForCost(c->rules->scavengeCost.cmc());
                const std::string srcName = c->rules->name;
                if (m_abilities.activateScavenge(cid, target->id, m_id)) {
                    AIOUT << "  [" << me().name() << "] scavenges " << srcName
                              << " onto " << target->name() << '\n';
                    activated = true;
                    continue;
                }
            }
        }

        // Embalm â€” use if we have a creature slot to fill and can afford it
        if (c->rules->hasEmbalm && c->rules->type.isCreature() &&
            canAffordWithUntapped(c->rules->embalmCost)) {
            tapForCost(c->rules->embalmCost.cmc());
            const std::string srcName = c->rules->name;
            if (m_abilities.activateEmbalm(cid, m_id)) {
                AIOUT << "  [" << me().name() << "] embalms " << srcName << '\n';
                activated = true;
                continue;
            }
        }

        // Eternalize
        if (c->rules->hasEternalize && c->rules->type.isCreature() &&
            canAffordWithUntapped(c->rules->eternalizeCost)) {
            tapForCost(c->rules->eternalizeCost.cmc());
            const std::string srcName = c->rules->name;
            if (m_abilities.activateEternalize(cid, m_id)) {
                AIOUT << "  [" << me().name() << "] eternalizes " << srcName << '\n';
                activated = true;
                continue;
            }
        }
    }
    return activated;
}

bool AiPlayer::trySuspend() {
    // Suspend expensive cards when we have spare mana and can't cast them normally
    for (Card* c : me().hand().cards()) {
        if (!c->rules->hasSuspend) continue;
        // Don't suspend if we could cast it now (counting untapped lands)
        if (canAffordWithUntapped(c->rules->manaCost)) continue;
        // For Suspend X: pick X=3 (card ready in 3 upkeeps â€” decent tempo tradeoff)
        int xVal = c->rules->suspendCountIsX ? 3 : 0;
        const std::string srcName = c->rules->name;
        if (m_abilities.activateSuspend(c->id, m_id, xVal)) {
            AIOUT << "  [" << me().name() << "] suspends " << srcName
                      << (c->rules->suspendCountIsX ? " (X=3)" : "") << '\n';
            return true;
        }
    }
    return false;
}

bool AiPlayer::tryForetell() {
    // Foretell expensive spells when we have â‰¥3 mana available (pool + untapped lands)
    int avail = me().manaPool().total();
    for (const Card* c : m_game.battlefield().cards()) {
        if (c->controllerId == m_id && !c->tapped && c->rules->type.isLand()) ++avail;
    }
    if (avail < 3) return false; // keep at least 1 mana after paying {2}

    for (Card* c : me().hand().cards()) {
        if (!c->rules->hasForetell) continue;
        if (c->rules->cmc() < 3) continue;
        // Don't foretell if castable right now
        if (canAffordWithUntapped(c->rules->manaCost)) continue;

        tapForCost(2); // foretell cost is always {2}
        const std::string srcName = c->rules->name;
        if (m_abilities.activateForetell(c->id, m_id)) {
            AIOUT << "  [" << me().name() << "] foretells " << srcName << '\n';
            return true;
        }
    }

    // Also try casting foretold cards from Exile
    for (Card* c : m_game.exile().cards()) {
        if (c->controllerId != m_id) continue;
        if (!c->foretold) continue;
        if (m_game.turnNumber() <= c->foretoldOnTurn) continue;
        if (!canAffordWithUntapped(c->rules->foretellCost)) continue;
        tapForCost(c->rules->foretellCost.cmc());
        const std::string srcName = c->rules->name;
        auto tgts = pickTargets(*c->rules);
        if (m_abilities.castSpell(c->id, m_id, tgts)) {
            AIOUT << "  [" << me().name() << "] casts foretold " << srcName << '\n';
            return true;
        }
    }
    return false;
}

// Impulse draw: play cards exiled with "you may play those cards until end of [next]
// turn" (Light Up the Stage, Reckless Impulse). Lands are played as a land drop;
// nonland cards are cast paying their mana cost.
bool AiPlayer::tryPlayImpulseFromExile() {
    if (m_game.isTurnControlled(m_id)) return false;   // controlled player takes no actions
    for (Card* c : m_game.exile().cards()) {
        if (!c->mayPlayFromExile || c->mayPlayController != m_id) continue;
        if (m_game.turnNumber() > c->mayPlayUntilTurn) continue;

        if (c->rules->type.isLand()) {
            if (!m_game.canPlayLand(m_id)) continue;
            const std::string nm = c->rules->name;
            Card* landed = m_game.moveToZone(c->id, ZoneType::Battlefield, m_id);
            me().incLandsPlayed();
            AIOUT << "  [" << me().name() << "] plays exiled land " << nm << '\n';
            if (landed) {
                std::vector<PendingTrigger> t;
                TriggerSystem::onLandPlayed(*landed, m_id, m_game, t);
                m_game.queueTriggers(std::move(t));
            }
            return true;
        }

        if (!canAffordWithUntapped(c->rules->manaCost)) continue;
        tapForCost(c->rules->manaCost.cmc());
        const std::string nm = c->rules->name;
        auto tgts = pickTargets(*c->rules);
        if (m_abilities.castSpell(c->id, m_id, tgts)) {
            AIOUT << "  [" << me().name() << "] plays exiled " << nm << '\n';
            return true;
        }
    }
    return false;
}

bool AiPlayer::tryActivateMorph() {
    bool activated = false;
    // Snapshot IDs first â€” activating may recompute bonuses
    std::vector<ObjectId> faceDownIds;
    for (const Card* c : m_game.battlefield().cards()) {
        if (c->controllerId == m_id && c->isFaceDown)
            faceDownIds.push_back(c->id);
    }
    for (ObjectId cid : faceDownIds) {
        Card* c = m_game.findCard(cid);
        if (!c || !c->isFaceDown || c->controllerId != m_id) continue;
        bool isMegamorph = c->rules->hasMegamorph;
        const ManaCost& cost = isMegamorph ? c->rules->megamorphCost : c->rules->morphCost;
        if (!canAffordWithUntapped(cost)) continue;
        tapForCost(cost.cmc());
        const std::string cname = c->rules->name;
        if (m_abilities.activateMorph(cid, m_id)) {
            AIOUT << "  [" << me().name() << "] turns " << cname
                      << " face up" << (isMegamorph ? " (Megamorph)" : "") << '\n';
            while (StateBasedActions::run(m_game)) {}
            activated = true;
        }
    }
    return activated;
}

// â”€â”€ Combo helpers â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

Card* AiPlayer::findInHandByName(std::string_view name) const {
    for (Card* c : me().hand().cards())
        if (c->rules->name == name) return c;
    return nullptr;
}

Card* AiPlayer::findOnBattlefieldByName(std::string_view name) const {
    for (Card* c : m_game.battlefield().cards())
        if (c->controllerId == m_id && c->rules->name == name) return c;
    return nullptr;
}

Card* AiPlayer::findBestEquipTarget(ObjectId equipId) const {
    Card* best = nullptr;
    int   bestScore = -1;
    for (Card* c : m_game.battlefield().cards()) {
        if (c->controllerId != m_id) continue;
        if (!c->isCreature())        continue;
        if (c->id == equipId)        continue;
        if (c->attachedTo != kInvalidId && c->attachedTo == equipId) continue;
        if (c->cantBeTargeted)       continue;
        if (c->hasKeyword(KeywordAbility::Shroud)) continue;
        if (c->hasKeyword(KeywordAbility::Hexproof)) continue;
        int score = effectivePower(*c) + effectiveToughness(*c) + keywordBonus(*c);
        // Bonus for attackers that are likely to connect (evasion)
        if (c->hasKeyword(KeywordAbility::Flying))     score += 8;
        if (c->hasKeyword(KeywordAbility::Haste))      score += 5;
        if (c->hasKeyword(KeywordAbility::Trample))    score += 4;
        if (c->isCommander)                            score += 6;
        // Penalty for creatures already carrying another equipment
        if (c->attachedTo != kInvalidId)               score -= 4;
        if (score > bestScore) { bestScore = score; best = c; }
    }
    return best;
}

bool AiPlayer::executeCombo(const ComboEntry& combo) {
    bool anyFired = false;

    for (const ComboStep& step : combo.sequence) {
        switch (step.type) {

        case ComboStep::Type::Cast: {
            Card* src = findInHandByName(step.cardName);
            if (!src) break;
            if (!canAffordWithUntapped(src->rules->manaCost)) break;

            std::vector<Target> tgts;
            // Prefer named target card (another combo piece already on BF)
            if (!step.targetCardName.empty()) {
                Card* tgt = findOnBattlefieldByName(step.targetCardName);
                if (tgt) tgts = { Target::forCard(tgt->id) };
            }
            // Fallback: use filter-based picker
            if (tgts.empty() && !step.targetFilter.empty())
                tgts = { pickBestTarget(step.targetFilter) };

            tapForCost(src->rules->manaCost.cmc());
            if (m_abilities.castSpell(src->id, m_id, tgts)) {
                AIOUT << "  [" << me().name() << "] combo-casts " << step.cardName << '\n';
                anyFired = true;
                while (StateBasedActions::run(m_game)) {}
                m_abilities.drainPendingTriggers();
            }
            break;
        }

        case ComboStep::Type::Activate: {
            Card* src = findOnBattlefieldByName(step.cardName);
            if (!src || src->tapped) break;
            if (m_abilities.activateAbility(src->id, step.abilityIndex, m_id)) {
                AIOUT << "  [" << me().name() << "] combo-activates " << step.cardName << '\n';
                anyFired = true;
                while (!m_abilities.stackEmpty()) {
                    m_abilities.resolveTop();
                    while (StateBasedActions::run(m_game)) {}
                }
                m_abilities.drainPendingTriggers();
            }
            break;
        }

        case ComboStep::Type::Equip: {
            Card* eq  = findOnBattlefieldByName(step.cardName);
            if (!eq) break;
            Card* tgt = findBestEquipTarget(eq->id);
            if (!tgt) break;
            if (m_abilities.activateEquip(eq->id, tgt->id, m_id)) {
                AIOUT << "  [" << me().name() << "] combo-equips " << step.cardName
                          << " â†’ " << tgt->rules->name << '\n';
                anyFired = true;
                while (StateBasedActions::run(m_game)) {}
                m_abilities.drainPendingTriggers();
            }
            break;
        }
        }
    }

    return anyFired;
}

} // namespace mtg




