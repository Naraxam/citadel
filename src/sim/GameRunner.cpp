#include "GameRunner.h"
#include "../game/DeckLoader.h"
#include "../game/CardStats.h"
#include "../game/KeywordAbility.h"
#include "../game/StateBasedActions.h"
#include "../game/ZoneType.h"
#include <algorithm>
#include <memory>
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace mtg {

// ── Construction ──────────────────────────────────────────────────────────────

GameRunner::GameRunner(const CardDb& db,
                       const std::string& deck0Path,
                       const std::string& deck1Path)
    : m_db(db), m_deck0(deck0Path), m_deck1(deck1Path)
{}

// ── Game setup ────────────────────────────────────────────────────────────────

bool GameRunner::setupGame(GameState& game,
                            AbilityProcessor& abilities,
                            TurnManager& tm) const {
    (void)abilities; // constructed with game ref; reset is implicit via game.reset()
    (void)tm;

    game.reset();
    game.setCardDb(&m_db);
    game.player(0).setLife(40);
    game.player(1).setLife(40);

    int n0 = DeckLoader::loadAndBuild(m_deck0, m_db, game, 0, /*shuffle=*/true);
    int n1 = DeckLoader::loadAndBuild(m_deck1, m_db, game, 1, /*shuffle=*/true);

    if (n0 < 20 || n1 < 20) {
        std::cerr << "[GameRunner] decks too small (" << n0 << ", " << n1 << ")\n";
        return false;
    }

    // Draw opening hands (7 cards each; no mulligan for self-play)
    for (uint8_t pid = 0; pid < 2; ++pid)
        for (int i = 0; i < 7; ++i) {
            auto& lib = game.player(pid).library();
            if (lib.empty()) break;
            game.moveToZone(lib.front()->id, ZoneType::Hand, pid);
        }
    return true;
}

// ── State encoder ─────────────────────────────────────────────────────────────

GameRunner::StateVec GameRunner::encodeState(const GameState& game, uint8_t pov) {
    StateVec v{};
    const uint8_t opp = pov ^ 1;

    // ── Count lands and GY breakdown per player ───────────────────────────────
    int landCount[2]    = {0, 0};
    int gyCreature[2]   = {0, 0};
    int gySpell[2]      = {0, 0};
    int gyLand[2]       = {0, 0};

    for (const Card* c : game.battlefield().cards()) {
        if (c->rules->type.isLand())
            ++landCount[c->controllerId & 1];
    }
    for (uint8_t pid = 0; pid < 2; ++pid) {
        for (const Card* c : game.player(pid).graveyard().cards()) {
            if      (c->isCreature())           ++gyCreature[pid];
            else if (c->rules->type.isLand())   ++gyLand[pid];
            else                                ++gySpell[pid];
        }
    }

    // ── Global features [0..13] — Commander: life/40, library/100, turn/40 ─────
    v[0]  = static_cast<float>(game.player(pov).life())            / 40.0f;
    v[1]  = static_cast<float>(game.player(opp).life())            / 40.0f;
    v[2]  = static_cast<float>(game.player(pov).hand().size())     /  7.0f;
    v[3]  = static_cast<float>(game.player(opp).hand().size())     /  7.0f;
    v[4]  = static_cast<float>(game.player(pov).library().size())  / 100.0f;
    v[5]  = static_cast<float>(game.player(opp).library().size())  / 100.0f;
    v[6]  = static_cast<float>(landCount[pov])                     / 10.0f;
    v[7]  = static_cast<float>(landCount[opp])                     / 10.0f;
    v[8]  = static_cast<float>(game.player(pov).graveyard().size()) / 20.0f;
    v[9]  = static_cast<float>(game.player(opp).graveyard().size()) / 20.0f;
    v[10] = static_cast<float>(game.turnNumber())                  / 40.0f;
    v[11] = static_cast<float>(game.player(pov).poisonCounters())  / 10.0f;
    v[12] = static_cast<float>(game.player(opp).poisonCounters())  / 10.0f;
    v[13] = static_cast<float>(game.player(pov).manaPool().total()) / 10.0f;

    // ── Creature slots [14..85] own, [86..157] opp — 12 slots × 6 floats ─────
    // Floats per slot: power/10, toughness/10, keywords/128,
    //                  +1/+1 counters/5, tapped|sick flags, markedDamage/10
    constexpr int kCreatureSlots = 12;
    constexpr int kCreatureFloats = 6;
    constexpr int kOwnCreatureBase = 14;
    constexpr int kOppCreatureBase = 86;

    int povCreSlot = kOwnCreatureBase;
    int oppCreSlot = kOppCreatureBase;

    for (const Card* c : game.battlefield().cards()) {
        if (!c || !c->rules) continue;
        if (!c->isCreature()) continue;
        bool isPov = (c->controllerId == pov);
        int& slot  = isPov ? povCreSlot : oppCreSlot;
        int  limit = isPov ? (kOwnCreatureBase + kCreatureSlots * kCreatureFloats)
                           : (kOppCreatureBase + kCreatureSlots * kCreatureFloats);
        if (slot + kCreatureFloats > limit) continue;

        float tappedSick = (c->tapped ? 1.0f : 0.0f) + (c->summoningSickness ? 2.0f : 0.0f);
        v[slot++] = static_cast<float>(effectivePower(*c))       / 10.0f;
        v[slot++] = static_cast<float>(effectiveToughness(*c))   / 10.0f;
        v[slot++] = static_cast<float>(c->keywordMask)           / 128.0f;
        v[slot++] = static_cast<float>(c->counterCount("+1/+1")) /   5.0f;
        v[slot++] = tappedSick                                    /   3.0f;
        v[slot++] = static_cast<float>(c->markedDamage)          / 10.0f;
    }

    // ── Non-creature permanent slots [158..189] own, [190..221] opp — 8×4 ────
    // Floats per slot: cmc/10, type-bits/8, loyalty/5, tapped (0|1)
    constexpr int kPermSlots  = 8;
    constexpr int kPermFloats = 4;
    constexpr int kOwnPermBase = 158;
    constexpr int kOppPermBase = 190;

    int povPermSlot = kOwnPermBase;
    int oppPermSlot = kOppPermBase;

    for (const Card* c : game.battlefield().cards()) {
        if (!c || !c->rules) continue;
        if (c->isCreature()) continue;
        bool isPov = (c->controllerId == pov);
        int& slot  = isPov ? povPermSlot : oppPermSlot;
        int  limit = isPov ? (kOwnPermBase + kPermSlots * kPermFloats)
                           : (kOppPermBase + kPermSlots * kPermFloats);
        if (slot + kPermFloats > limit) continue;

        float typeBits = 0;
        if (c->rules->type.isLand())         typeBits += 1;
        if (c->rules->type.isArtifact())     typeBits += 2;
        if (c->rules->type.isEnchantment())  typeBits += 4;
        if (c->rules->type.isPlaneswalker()) typeBits += 8;

        int loyalty = c->counterCount("loyalty");

        v[slot++] = static_cast<float>(c->rules->manaCost.cmc()) / 10.0f;
        v[slot++] = typeBits                                      /  8.0f;
        v[slot++] = static_cast<float>(loyalty)                  /  5.0f;
        v[slot++] = c->tapped ? 1.0f : 0.0f;
    }

    // ── GY breakdown [222..229] ───────────────────────────────────────────────
    int exileCount[2] = {0, 0};
    for (const Card* c : game.exile().cards()) exileCount[c->ownerId & 1]++;

    v[222] = static_cast<float>(gyCreature[pov])   / 10.0f;
    v[223] = static_cast<float>(gySpell[pov])      / 10.0f;
    v[224] = static_cast<float>(gyCreature[opp])   / 10.0f;
    v[225] = static_cast<float>(gySpell[opp])      / 10.0f;
    v[226] = static_cast<float>(exileCount[pov])   / 10.0f;
    v[227] = static_cast<float>(exileCount[opp])   / 10.0f;
    v[228] = static_cast<float>(gyLand[pov])       /  5.0f;
    v[229] = static_cast<float>(gyLand[opp])       /  5.0f;

    // ── Hand CMC [230..236] — own hand cards, up to 7, sorted CMC descending ─────
    {
        constexpr int kHandSlots = 7;
        std::vector<float> cmcs;
        cmcs.reserve(kHandSlots);
        for (const Card* c : game.player(pov).hand().cards())
            if (c && c->rules)
                cmcs.push_back(static_cast<float>(c->rules->manaCost.cmc()) / 10.0f);
        std::sort(cmcs.begin(), cmcs.end(), std::greater<float>());
        for (int i = 0; i < kHandSlots && i < static_cast<int>(cmcs.size()); ++i)
            v[230 + i] = cmcs[i];
    }

    // ── WUBRG land counts [237..241] own, [242..246] opp ─────────────────────
    // Subtypes: Plains=W, Island=U, Swamp=B, Mountain=R, Forest=G
    {
        constexpr int kWUBRG = 5;
        static constexpr const char* kSubtypes[kWUBRG] = {
            "Plains", "Island", "Swamp", "Mountain", "Forest"
        };
        for (const Card* c : game.battlefield().cards()) {
            if (!c || !c->rules) continue;
            if (!c->rules->type.isLand()) continue;
            uint8_t ctrl = c->controllerId & 1;
            int base = (ctrl == pov) ? 237 : 242;
            for (int ci = 0; ci < kWUBRG; ++ci)
                if (c->rules->type.hasSubtype(kSubtypes[ci]))
                    v[base + ci] += 1.0f / 10.0f;
        }
    }

    // ── Stack [247..254] — up to 4 spells × (cmc/10, is_pov_controller) ──────
    {
        constexpr int kStackSlots  = 4;
        constexpr int kStackFloats = 2;
        int stackSlot = 247;
        for (const Card* c : game.stack().cards()) {
            if (stackSlot >= 247 + kStackSlots * kStackFloats) break;
            if (!c || !c->rules) { stackSlot += 2; continue; }
            v[stackSlot++] = static_cast<float>(c->rules->manaCost.cmc()) / 10.0f;
            v[stackSlot++] = (c->controllerId == pov) ? 1.0f : 0.0f;
        }
    }
    // v[255] remains 0 (padding)

    // ── Commander features [256..263] ─────────────────────────────────────────
    // Damage taken from each opponent's commander (threshold 21 = loss)
    v[256] = static_cast<float>(game.player(pov).commanderDamageFrom(opp)) / 21.0f;
    v[257] = static_cast<float>(game.player(opp).commanderDamageFrom(pov)) / 21.0f;

    // Commander tax cast counts (each cast = +2 mana tax)
    v[258] = static_cast<float>(game.commanderCastCount[pov]) / 5.0f;
    v[259] = static_cast<float>(game.commanderCastCount[opp]) / 5.0f;

    // Is each player's commander currently in the command zone?
    // Also collect commander CMC for features [262..263].
    float cmdCmc[2] = {0.f, 0.f};
    bool  cmdInZone[2] = {false, false};
    for (const Card* c : game.command().cards()) {
        if (!c->isCommander) continue;
        uint8_t owner = c->ownerId & 1;
        cmdInZone[owner] = true;
        cmdCmc[owner] = static_cast<float>(c->rules->manaCost.cmc()) / 10.0f;
    }
    // Also check battlefield for commander CMC (when it's in play)
    for (const Card* c : game.battlefield().cards()) {
        if (!c->isCommander) continue;
        uint8_t owner = c->ownerId & 1;
        if (cmdCmc[owner] == 0.f)
            cmdCmc[owner] = static_cast<float>(c->rules->manaCost.cmc()) / 10.0f;
    }
    v[260] = cmdInZone[pov] ? 1.0f : 0.0f;
    v[261] = cmdInZone[opp] ? 1.0f : 0.0f;
    v[262] = cmdCmc[pov];
    v[263] = cmdCmc[opp];

    // ── Hand type breakdown [255, 264..265] ───────────────────────────────────
    // v[255] was formerly unused padding; v[264..265] are new slots.
    {
        int handCrea = 0, handSpell = 0, handLand = 0;
        for (const Card* c : game.player(pov).hand().cards()) {
            if (!c || !c->rules) continue;
            if      (c->rules->type.isLand())    ++handLand;
            else if (c->isCreature())            ++handCrea;
            else                                 ++handSpell;
        }
        v[255] = static_cast<float>(handLand)  / 7.0f;
        v[264] = static_cast<float>(handCrea)  / 7.0f;
        v[265] = static_cast<float>(handSpell) / 7.0f;
    }

    // ── Own commander on-battlefield P/T [266..267] ───────────────────────────
    {
        float cmdPow = 0.f, cmdTgh = 0.f;
        for (const Card* c : game.battlefield().cards()) {
            if (!c->isCommander || (c->ownerId & 1) != pov) continue;
            cmdPow = static_cast<float>(effectivePower(*c))     / 10.0f;
            cmdTgh = static_cast<float>(effectiveToughness(*c)) / 10.0f;
            break;
        }
        v[266] = cmdPow;
        v[267] = cmdTgh;
    }

    return v;
}

// ── Single game ───────────────────────────────────────────────────────────────

int GameRunner::runGame(int maxTurns) {
    auto game_up      = std::make_unique<GameState>();
    auto abilities_up = std::make_unique<AbilityProcessor>(*game_up);
    auto tm_up        = std::make_unique<TurnManager>(*game_up);

    GameState&        game      = *game_up;
    AbilityProcessor& abilities = *abilities_up;
    TurnManager&      tm        = *tm_up;

    if (!setupGame(game, abilities, tm)) return -1;

    auto ai0_up = std::make_unique<AiPlayer>(uint8_t{0}, game, abilities);
    auto ai1_up = std::make_unique<AiPlayer>(uint8_t{1}, game, abilities);
    AiPlayer& ai0 = *ai0_up;
    AiPlayer& ai1 = *ai1_up;
    ai0.setDebugLog(m_debugLog);
    ai1.setDebugLog(m_debugLog);
    ai0.setTurnTimeLimit(30000); // 30-second hard cap per half-turn
    ai1.setTurnTimeLimit(30000);
    if (m_mctsEnabled) {
        ai0.enableMcts(true, m_mctsConfig);
        ai1.enableMcts(true, m_mctsConfig);
        // Pass by reference, never copy: callbacks wrap Python objects whose
        // refcount can't be safely bumped from this thread (GIL is released for
        // the duration of the game). AiPlayer stores raw pointers into this
        // GameRunner's std::functions, which live for the whole call.
        const ValueFn& fn0 = m_valueFnP0 ? m_valueFnP0 : m_valueFn;
        const ValueFn& fn1 = m_valueFnP1 ? m_valueFnP1 : m_valueFn;
        if (fn0) ai0.setValueFn(fn0);
        if (fn1) ai1.setValueFn(fn1);
        if (m_policyFn) { ai0.setPolicyFn(m_policyFn); ai1.setPolicyFn(m_policyFn); }
        if (m_batchValueFn) { ai0.setBatchValueFn(m_batchValueFn); ai1.setBatchValueFn(m_batchValueFn); }
    }

    using Clock = std::chrono::steady_clock;
    using Ms    = std::chrono::milliseconds;

    // Returns current epoch-milliseconds as a string prefix for log lines.
    auto tsMs = [&]() -> long long {
        return std::chrono::duration_cast<Ms>(Clock::now().time_since_epoch()).count();
    };

    auto logHalfTurn = [&](int player, long long startMs) {
        if (!m_debugLog) return;
        long long elapsedMs = tsMs() - startMs;
        *m_debugLog
            << "  P" << player << " turn done in "
            << elapsedMs << "ms"
            << " | P0: " << game.player(0).life() << "hp"
            << " " << game.player(0).hand().size() << "h"
            << " | P1: " << game.player(1).life() << "hp"
            << " " << game.player(1).hand().size() << "h"
            << " | BF: " << game.battlefield().size()
            << "\n";
        m_debugLog->flush();
    };

    int turns = 0;
    while (turns < maxTurns) {
        if (m_debugLog) {
            *m_debugLog << "[" << tsMs() << "ms] Turn " << (turns + 1)
                        << " P0 starting"
                        << " | P0: " << game.player(0).life() << "hp "
                        << game.player(0).hand().size() << "h"
                        << " P1: " << game.player(1).life() << "hp "
                        << game.player(1).hand().size() << "h\n";
            m_debugLog->flush();
        }

        // Safety net: force SBAs and game-over check even if takeTurn()
        // didn't return true (catches life going negative mid-phase without
        // the isGameOver check firing at the right moment).
        auto forceCheck = [&](bool done) -> bool {
            if (done) return true;
            if (game.player(0).life() <= 0 || game.player(1).life() <= 0 ||
                game.player(0).hasLost() || game.player(1).hasLost()) {
                while (StateBasedActions::runBasic(game)) {}
                if (tm.isGameOver()) {
                    if (m_debugLog) {
                        *m_debugLog << "  [safety-net] game over detected outside takeTurn()"
                                    << " P0=" << game.player(0).life() << "hp"
                                    << " P1=" << game.player(1).life() << "hp\n";
                        m_debugLog->flush();
                    }
                    return true;
                }
            }
            return false;
        };

        auto dbgMark = [&](const char* tag) {
            if (!m_debugLog) return;
            *m_debugLog << "  [" << tag << "@" << tsMs() << "ms]\n";
            m_debugLog->flush();
        };

        long long t0ms = tsMs();
        auto t0 = Clock::now();
        bool done = ai0.takeTurn(tm, &ai1);
        dbgMark("P0-postTurn");
        done = forceCheck(done);
        dbgMark("P0-postForce");
        ++turns;
        logHalfTurn(0, t0ms);
        dbgMark("P0-postLog");
        if (m_turnCb) m_turnCb(turns);
        dbgMark("P0-postCb");
        if (m_turnMinMs > 0) {
            auto elapsed = std::chrono::duration_cast<Ms>(Clock::now() - t0).count();
            if (elapsed < m_turnMinMs) {
                dbgMark("P0-sleep");
                std::this_thread::sleep_for(Ms(m_turnMinMs - elapsed));
            }
        }
        dbgMark("P0-doneCheck");
        if (done) { m_lastWinner = game.player(1).hasLost() ? 0 : 1; break; }

        if (m_debugLog) {
            *m_debugLog << "[" << tsMs() << "ms] Turn " << (turns + 1)
                        << " P1 starting"
                        << " | P0: " << game.player(0).life() << "hp "
                        << game.player(0).hand().size() << "h"
                        << " P1: " << game.player(1).life() << "hp "
                        << game.player(1).hand().size() << "h\n";
            m_debugLog->flush();
        }

        t0ms = tsMs();
        t0 = Clock::now();
        done = ai1.takeTurn(tm, &ai0);
        dbgMark("P1-postTurn");
        done = forceCheck(done);
        dbgMark("P1-postForce");
        ++turns;
        logHalfTurn(1, t0ms);
        dbgMark("P1-postLog");
        if (m_turnCb) m_turnCb(turns);
        dbgMark("P1-postCb");
        if (m_turnMinMs > 0) {
            auto elapsed = std::chrono::duration_cast<Ms>(Clock::now() - t0).count();
            if (elapsed < m_turnMinMs) {
                dbgMark("P1-sleep");
                std::this_thread::sleep_for(Ms(m_turnMinMs - elapsed));
            }
        }
        dbgMark("P1-doneCheck");
        if (done) { m_lastWinner = game.player(0).hasLost() ? 1 : 0; break; }
    }

    m_lastTurns   = turns;
    m_lastWinner  = (turns >= maxTurns) ? -1 : m_lastWinner;
    if (m_debugLog) {
        *m_debugLog << "[pre-return] winner=" << m_lastWinner << " turns=" << m_lastTurns << "\n";
        m_debugLog->flush();
    }

    auto dlog = [&](const char* tag) {
        if (!m_debugLog) return;
        *m_debugLog << "[" << tag << "]\n";
        m_debugLog->flush();
    };

    ai1_up.reset(); dlog("dtor-ai1");
    ai0_up.reset(); dlog("dtor-ai0");
    tm_up.reset();  dlog("dtor-tm");
    abilities_up.reset(); dlog("dtor-abilities");
    game_up.reset(); dlog("dtor-game");

    return m_lastWinner;
}

// ── Episode recording ─────────────────────────────────────────────────────────

GameRunner::Episode GameRunner::runEpisode(int maxTurns) {
    Episode ep;

    GameState        game;
    AbilityProcessor abilities(game);
    TurnManager      tm(game);

    if (!setupGame(game, abilities, tm)) return ep;

    AiPlayer ai0(0, game, abilities);
    AiPlayer ai1(1, game, abilities);

    // Attach MCTS and episode policy output buffers if enabled
    AiPlayer::EpisodeOut ep0out, ep1out;
    if (m_mctsEnabled) {
        ai0.enableMcts(true, m_mctsConfig);
        ai1.enableMcts(true, m_mctsConfig);
        // Pass by reference, never copy — see runGame() for the GIL-safety reason.
        const ValueFn& fn0 = m_valueFnP0 ? m_valueFnP0 : m_valueFn;
        const ValueFn& fn1 = m_valueFnP1 ? m_valueFnP1 : m_valueFn;
        if (fn0) ai0.setValueFn(fn0);
        if (fn1) ai1.setValueFn(fn1);
        if (m_policyFn) { ai0.setPolicyFn(m_policyFn); ai1.setPolicyFn(m_policyFn); }
        if (m_batchValueFn) { ai0.setBatchValueFn(m_batchValueFn); ai1.setBatchValueFn(m_batchValueFn); }
        ai0.setEpisodeOutput(&ep0out);
        ai1.setEpisodeOutput(&ep1out);
    }

    // povSeq[i] records which player's perspective state[i] belongs to,
    // so we can assign the correct reward sign at the end.
    std::vector<uint8_t> povSeq;

    int turns = 0;
    while (turns < maxTurns) {
        // Capture both perspectives at each decision point (opponent modeling:
        // 2× training data at zero extra game cost).
        ep.states.push_back(encodeState(game, 0)); povSeq.push_back(0);
        ep.states.push_back(encodeState(game, 1)); povSeq.push_back(1);
        ep.actions.push_back(0);
        ep.actions.push_back(0);

        bool done = ai0.takeTurn(tm, &ai1);
        ++turns;
        if (done) { ep.winner = game.player(1).hasLost() ? 0 : 1; break; }

        ep.states.push_back(encodeState(game, 0)); povSeq.push_back(0);
        ep.states.push_back(encodeState(game, 1)); povSeq.push_back(1);
        ep.actions.push_back(0);
        ep.actions.push_back(0);

        done = ai1.takeTurn(tm, &ai0);
        ++turns;
        if (done) { ep.winner = game.player(0).hasLost() ? 1 : 0; break; }
    }

    // Merge MCTS policy outputs from both players into the episode
    if (m_mctsEnabled) {
        for (auto& v : ep0out.policy) ep.policy.push_back(std::move(v));
        for (auto& v : ep1out.policy) ep.policy.push_back(std::move(v));
    }

    ep.turns = turns;
    if (turns >= maxTurns) ep.winner = -1;

    // Assign terminal rewards: +1 if the snapshot's pov player won, -1 otherwise
    float r0 = (ep.winner == 0) ? 1.0f : (ep.winner == 1) ? -1.0f : 0.0f;
    for (uint8_t pov : povSeq)
        ep.rewards.push_back(pov == 0 ? r0 : -r0);

    m_lastWinner = ep.winner;
    m_lastTurns  = ep.turns;
    return ep;
}

// ── Batch runner ──────────────────────────────────────────────────────────────

void GameRunner::runBatch(int games, const std::string& outPath, int maxTurns) {
    std::ofstream out(outPath);
    if (!out.is_open()) {
        std::cerr << "[GameRunner] cannot open output: " << outPath << '\n';
        return;
    }
    out << "winner,turns\n";

    int p0wins = 0, p1wins = 0, draws = 0;
    for (int g = 0; g < games; ++g) {
        int w = runGame(maxTurns);
        out << w << ',' << m_lastTurns << '\n';
        if      (w == 0) ++p0wins;
        else if (w == 1) ++p1wins;
        else             ++draws;

        if ((g + 1) % 10 == 0)
            std::cout << "[GameRunner] " << (g + 1) << '/' << games
                      << "  P0=" << p0wins << " P1=" << p1wins
                      << " Draw=" << draws << '\n';
    }
    std::cout << "[GameRunner] done. results → " << outPath << '\n';
}

// ── Summary ───────────────────────────────────────────────────────────────────

void GameRunner::printLastResult() const {
    std::cout << "[GameRunner] winner=" << m_lastWinner
              << " turns=" << m_lastTurns << '\n';
}

// ── Free-function wrapper for AiPlayer (avoids circular include) ──────────────
// AiPlayer.h declares this in ValueNetInference.h; GameRunner provides the body.
std::array<float, 268> encodeStateForNet(const GameState& g, uint8_t pov) {
    auto sv = GameRunner::encodeState(g, pov);
    // GameRunner::StateVec is std::array<float,268>; identical to our return type.
    return sv;
}

} // namespace mtg
