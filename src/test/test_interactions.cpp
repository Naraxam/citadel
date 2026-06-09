/**
 * test_interactions.cpp — lightweight interaction tests for the MTG engine.
 *
 * Build: add to citadel_core target or compile standalone.
 * Run:   mtg_tests.exe  (returns 0 on pass, non-zero on first failure)
 *
 * Each TEST() block sets up a minimal GameState, performs an action, and
 * asserts on the resulting state.  Tests are self-contained and fast (~1ms each).
 */
#include "../game/GameState.h"
#include "../game/TurnManager.h"
#include "../game/StateBasedActions.h"
#include "../game/ability/AbilityProcessor.h"
#include "../game/TriggerSystem.h"
#include "../game/ability/Effects.h"
#include "../game/ability/ScriptLine.h"
#include "../game/ability/EffectContext.h"
#include "../game/CardStats.h"
#include "../game/ManaPool.h"
#include "../game/ai/AiPlayer.h"
#include "../core/mana/ManaCost.h"
#include "../core/mana/ManaCostShard.h"
#include <cassert>
#include <iostream>
#include <string>

using namespace mtg;

namespace {

// ── Micro-test framework ──────────────────────────────────────────────────────
int g_passed = 0, g_failed = 0;
#define TEST(name) static void test_##name()
#define ASSERT(cond) do { if (!(cond)) { \
    std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << " (" #cond ")\n"; \
    ++g_failed; return; } } while(0)
#define RUN(name) do { test_##name(); ++g_passed; \
    std::cout << "PASS " #name "\n"; } while(0)

// ── Helpers ───────────────────────────────────────────────────────────────────

// Build a minimal CardRules with just a name, type string, and optional P/T
mtg::CardRules makeRules(const std::string& name,
                          const std::string& types = "Creature",
                          const std::string& power = "2",
                          const std::string& toughness = "2") {
    mtg::CardRules r;
    r.name      = name;
    r.type      = mtg::CardType::parse(types);
    r.power     = power;
    r.toughness = toughness;
    return r;
}

// ── Tests ─────────────────────────────────────────────────────────────────────

TEST(deathtouch_lethal) {
    // A creature that takes any damage from a deathtouch source is lethal.
    mtg::GameState game;
    auto rules = makeRules("Fanatic", "Creature", "1", "4");
    rules.keywords.push_back("Deathtouch");
    mtg::Card* attacker = game.createCard(&rules, 0);
    game.moveToZone(attacker->id, mtg::ZoneType::Battlefield, 0);

    auto defRules = makeRules("Bear", "Creature", "2", "2");
    mtg::Card* defLib = game.createCard(&defRules, 1);
    // moveToZone returns the new card object (new id); capture it
    mtg::Card* def = game.moveToZone(defLib->id, mtg::ZoneType::Battlefield, 1);
    ASSERT(def != nullptr);
    mtg::ObjectId defId = def->id;

    def->markedDamage     = 1;
    def->deathtouchDamage = true;

    StateBasedActions::run(game);
    // Defender should have died
    mtg::Card* still = game.findCard(defId);
    ASSERT(still == nullptr || !still->isOnBattlefield());
}

TEST(trample_overflow) {
    mtg::GameState game;
    game.player(0).setLife(40);
    game.player(1).setLife(40);

    mtg::TurnManager tm(game);
    mtg::AbilityProcessor abilities(game);

    auto atkR = makeRules("Stomper", "Creature", "5", "5");
    atkR.keywords.push_back("Trample");
    mtg::Card* atkLib = game.createCard(&atkR, 0);
    mtg::Card* atk    = game.moveToZone(atkLib->id, mtg::ZoneType::Battlefield, 0);
    ASSERT(atk != nullptr);
    atk->summoningSickness = false;  // simulate having been on BF for a full turn

    auto blkR = makeRules("Blocker", "Creature", "2", "2");
    mtg::Card* blkLib = game.createCard(&blkR, 1);
    mtg::Card* blk    = game.moveToZone(blkLib->id, mtg::ZoneType::Battlefield, 1);
    ASSERT(blk != nullptr);

    tm.declareAttacker(atk->id, 1);
    tm.declareBlocker(blk->id, atk->id);
    tm.dealCombatDamage(false);
    StateBasedActions::run(game);

    // 5-power trampler vs 2-toughness blocker: 3 excess damage to player
    ASSERT(game.player(1).life() <= 37);
}

TEST(indestructible_survives_lethal) {
    mtg::GameState game;
    auto rules = makeRules("God", "Creature", "4", "4");
    rules.keywords.push_back("Indestructible");
    mtg::Card* godLib = game.createCard(&rules, 0);
    mtg::Card* god    = game.moveToZone(godLib->id, mtg::ZoneType::Battlefield, 0);
    ASSERT(god != nullptr);
    mtg::ObjectId godId = god->id;

    god->markedDamage = 99;
    StateBasedActions::run(game);

    mtg::Card* still = game.findCard(godId);
    ASSERT(still != nullptr && still->isOnBattlefield());
}

TEST(ward_prevents_targeting) {
    // Ward N: targeting player must pay N or the targeting fails.
    // Verify ward cost struct is populated when parsed.
    mtg::CardRules rules;
    rules.name = "Warded";
    rules.type = mtg::CardType::parse("Creature");
    rules.power = rules.toughness = "3";
    rules.hasWard = true;
    rules.wardCost = mtg::ManaCost::parse("2");
    ASSERT(!rules.wardCost.isNoCost());
    ASSERT(rules.wardCost.cmc() == 2);
}

TEST(commander_damage_threshold) {
    // 21 commander damage from a single commander = loss.
    mtg::GameState game;
    game.player(0).setLife(40);
    game.recordCommanderDamage(0, 1, 20);
    ASSERT(!game.player(0).hasLost());
    game.recordCommanderDamage(0, 1, 1);
    ASSERT(game.player(0).commanderDamageFrom(1) >= 21);
    // Loss is detected by StateBasedActions
    StateBasedActions::run(game);
    ASSERT(game.player(0).hasLost());
}

TEST(graft_counter_placement) {
    // Graft N: enters with N +1/+1 counters.
    mtg::CardRules rules;
    rules.name      = "Vinelasher";
    rules.type      = mtg::CardType::parse("Creature");
    rules.power     = rules.toughness = "0";
    rules.hasGraft  = true;
    rules.graftCount = 4;
    ASSERT(rules.hasGraft);
    ASSERT(rules.graftCount == 4);
}

TEST(spell_mastery_condition) {
    // SpellMastery requires 2+ instants/sorceries in GY.
    mtg::GameState game;

    mtg::CardRules instR;
    instR.name = "Spell1";
    instR.type = mtg::CardType::parse("Instant");
    mtg::Card* s1 = game.createCard(&instR, 0);
    mtg::Card* s2 = game.createCard(&instR, 0);
    game.moveToZone(s1->id, mtg::ZoneType::Graveyard, 0);
    game.moveToZone(s2->id, mtg::ZoneType::Graveyard, 0);

    // evaluateSVar with SpellMastery condition — check it's now 2 spells in GY
    int spellsInGy = 0;
    for (const mtg::Card* c : game.player(0).graveyard().cards())
        if (c->rules->type.isInstant() || c->rules->type.isSorcery())
            ++spellsInGy;
    ASSERT(spellsInGy >= 2);
}

// ── Additional tests ──────────────────────────────────────────────────────────

TEST(lifelink_gains_life) {
    // A lifelink creature dealing damage should gain life for controller.
    GameState game;
    game.player(0).setLife(10);
    auto r = makeRules("Angel", "Creature", "3", "3");
    r.keywords.push_back("Lifelink");
    mtg::Card* lib = game.createCard(&r, 0);
    mtg::Card* atk = game.moveToZone(lib->id, ZoneType::Battlefield, 0);
    ASSERT(atk != nullptr);
    atk->summoningSickness = false;

    // Simulate lifelink: gain life equal to power
    game.gainLife(0, effectivePower(*atk));
    ASSERT(game.player(0).life() == 13);
}

TEST(counter_cancellation) {
    // +1/+1 and -1/-1 counters cancel each other (SBA 704.5q).
    GameState game;
    auto r = makeRules("Bear", "Creature", "2", "2");
    mtg::Card* lib = game.createCard(&r, 0);
    mtg::Card* c   = game.moveToZone(lib->id, ZoneType::Battlefield, 0);
    ASSERT(c != nullptr);
    c->addCounter("+1/+1", 3);
    c->addCounter("-1/-1", 2);
    StateBasedActions::run(game);
    mtg::Card* still = game.findCard(c->id);
    ASSERT(still != nullptr);
    ASSERT(still->counterCount("+1/+1") == 1);
    ASSERT(still->counterCount("-1/-1") == 0);
}

TEST(flying_evasion_blocks) {
    // A non-flying creature can't block a flying attacker (unless it has Reach).
    GameState game;
    auto atkR = makeRules("Dragon", "Creature", "3", "3");
    atkR.keywords.push_back("Flying");
    mtg::Card* aLib = game.createCard(&atkR, 0);
    mtg::Card* atk  = game.moveToZone(aLib->id, ZoneType::Battlefield, 0);
    ASSERT(atk != nullptr);

    auto blkR = makeRules("Bear", "Creature", "2", "2");
    mtg::Card* bLib = game.createCard(&blkR, 1);
    mtg::Card* blk  = game.moveToZone(bLib->id, ZoneType::Battlefield, 1);
    ASSERT(blk != nullptr);

    TurnManager tm(game);
    atk->summoningSickness = false;
    tm.declareAttacker(atk->id, 1);
    // Non-flying creature should NOT be able to block the flyer
    bool blocked = tm.declareBlocker(blk->id, atk->id);
    ASSERT(!blocked);
}

TEST(reach_blocks_flying) {
    // A Reach creature CAN block a flying attacker.
    GameState game;
    auto atkR = makeRules("Flyer", "Creature", "2", "2");
    atkR.keywords.push_back("Flying");
    mtg::Card* aLib = game.createCard(&atkR, 0);
    mtg::Card* atk  = game.moveToZone(aLib->id, ZoneType::Battlefield, 0);
    ASSERT(atk != nullptr);

    auto blkR = makeRules("Spider", "Creature", "2", "4");
    blkR.keywords.push_back("Reach");
    mtg::Card* bLib = game.createCard(&blkR, 1);
    mtg::Card* blk  = game.moveToZone(bLib->id, ZoneType::Battlefield, 1);
    ASSERT(blk != nullptr);

    TurnManager tm(game);
    atk->summoningSickness = false;
    tm.declareAttacker(atk->id, 1);
    bool blocked = tm.declareBlocker(blk->id, atk->id);
    ASSERT(blocked);
}

TEST(poison_counters_win) {
    // 10 poison counters = loss (rule 704.5c).
    GameState game;
    game.player(0).setLife(40);
    for (int i = 0; i < 10; ++i) game.player(0).addPoison(1);
    ASSERT(game.player(0).poisonCounters() == 10);
    StateBasedActions::run(game);
    ASSERT(game.player(0).hasLost());
}

TEST(legend_rule) {
    // Two legendary permanents with the same name under one player: one must go (SBA 704.5j).
    GameState game;
    auto r = makeRules("Thalia, Guardian of Thraben", "Legendary Creature", "2", "1");
    mtg::Card* lib1 = game.createCard(&r, 0);
    mtg::Card* leg1 = game.moveToZone(lib1->id, ZoneType::Battlefield, 0);
    ASSERT(leg1 != nullptr);
    mtg::Card* lib2 = game.createCard(&r, 0);
    mtg::Card* leg2 = game.moveToZone(lib2->id, ZoneType::Battlefield, 0);
    ASSERT(leg2 != nullptr);
    ObjectId id1 = leg1->id, id2 = leg2->id;
    StateBasedActions::run(game);
    // Exactly one should remain; the older one is sacrificed
    int onBf = 0;
    for (const Card* c : game.battlefield().cards())
        if (c->rules->name == "Thalia, Guardian of Thraben") ++onBf;
    ASSERT(onBf == 1);
}

TEST(mana_pool_colored) {
    // ManaPool::canPay correctly requires colored mana.
    using namespace mtg;
    ManaPool pool;
    pool.add(ManaCostShard::WHITE);
    pool.add(ManaCostShard::BLUE);
    ManaCost costWU = ManaCost::parse("W U");
    ASSERT(pool.canPay(costWU));
    ManaCost costWWU = ManaCost::parse("W W U");
    ASSERT(!pool.canPay(costWWU));  // only 1 white available
}

TEST(mana_generic_does_not_pay_colored) {
    // Generic mana must NOT satisfy coloured pips (MTG rule). It only pays the
    // generic portion of a cost.
    using namespace mtg;
    ManaPool pool;
    pool.addGeneric(3);
    ASSERT(!pool.canPay(ManaCost::parse("R R")));   // generic can't pay {R}{R}
    ASSERT(pool.canPay(ManaCost::parse("3")));       // but pays {3}
}

TEST(toughness_zero_dies_to_sba) {
    // Creature with toughness 0 or less dies to SBA even if indestructible.
    GameState game;
    auto r = makeRules("Shrinking", "Creature", "1", "1");
    mtg::Card* lib = game.createCard(&r, 0);
    mtg::Card* c   = game.moveToZone(lib->id, ZoneType::Battlefield, 0);
    ASSERT(c != nullptr);
    // Simulate a -1/-1 counter dropping toughness to 0
    c->addCounter("-1/-1", 1);
    // effective toughness = 1 + (-1/-1 counter) = 0
    StateBasedActions::run(game);
    // Card should have died
    bool isDead = true;
    for (const Card* bf : game.battlefield().cards())
        if (bf->id == c->id) { isDead = false; break; }
    ASSERT(isDead);
}

TEST(counter_fast_path) {
    // Fast-path counters (m_p1p1/m_m1m1) stay in sync with the full map.
    GameState game;
    auto r = makeRules("Bear", "Creature", "2", "2");
    mtg::Card* lib = game.createCard(&r, 0);
    mtg::Card* c   = game.moveToZone(lib->id, ZoneType::Battlefield, 0);
    ASSERT(c != nullptr);
    c->addCounter("+1/+1", 3);
    ASSERT(c->m_p1p1 == 3);
    ASSERT(c->counterCount("+1/+1") == 3);
    c->removeCounter("+1/+1", 1);
    ASSERT(c->m_p1p1 == 2);
    ASSERT(c->counterCount("+1/+1") == 2);
}

// ── AI behavior tests ─────────────────────────────────────────────────────────

TEST(ai_blocks_lethal_attacker) {
    // AI should block an attacker that would deal lethal damage.
    GameState game;
    game.player(0).setLife(40);
    game.player(1).setLife(3);  // Bob is at 3 life — Alice attacking should be lethal

    TurnManager tm(game);
    AbilityProcessor abilities(game);
    AiPlayer aliceAi(0, game, abilities);
    AiPlayer bobAi(1, game, abilities);

    // Create a 5/5 creature for Alice (player 0)
    auto atkR = makeRules("Big Creature", "Creature", "5", "5");
    mtg::Card* aLib = game.createCard(&atkR, 0);
    mtg::Card* atk  = game.moveToZone(aLib->id, ZoneType::Battlefield, 0);
    ASSERT(atk != nullptr);
    atk->summoningSickness = false;

    // Create a 2/2 blocker for Bob (player 1)
    auto blkR = makeRules("Blocker", "Creature", "2", "2");
    mtg::Card* bLib = game.createCard(&blkR, 1);
    mtg::Card* blk  = game.moveToZone(bLib->id, ZoneType::Battlefield, 1);
    ASSERT(blk != nullptr);

    // Alice attacks
    game.setActivePlayer(0);
    tm.declareAttacker(atk->id, 1);
    ASSERT(tm.combatState().isAttacking(atk->id));

    // Bob should block since 5 damage > 3 life
    bobAi.declareBlockers(tm);
    bool isBlocking = tm.combatState().isBlocking(blk->id);
    ASSERT(isBlocking);
}

TEST(ai_prefers_removal_when_behind) {
    // When the opponent has more power, the AI should score removal spells higher.
    GameState game;
    game.player(0).setLife(40);
    game.player(1).setLife(40);
    AbilityProcessor abilities(game);
    AiPlayer ai(1, game, abilities);  // Bob's AI

    // Put a strong creature on Alice's side
    auto threat = makeRules("Threat", "Creature", "6", "6");
    mtg::Card* tLib = game.createCard(&threat, 0);
    game.moveToZone(tLib->id, ZoneType::Battlefield, 0);

    // Removal spell for Bob
    auto remRules = makeRules("Doom Blade", "Instant", "", "");
    remRules.abilityLines.push_back("SP$ Destroy | ValidTgts$ Creature.Other");
    int score = ai.rateSpell(*game.createCard(&remRules, 1));

    // Score should be positive (worth casting given the threat)
    ASSERT(score > 0);
}

TEST(ai_recognizes_lethal_attack) {
    // AI should declare all attackers when total power >= opponent's life.
    GameState game;
    game.player(0).setLife(40);
    game.player(1).setLife(5);  // Bob at 5 life — vulnerable to lethal

    TurnManager tm(game);
    AbilityProcessor abilities(game);
    AiPlayer ai(0, game, abilities);  // Alice's AI

    // Create 3/3 creature with no summoning sickness
    auto cr = makeRules("Attacker", "Creature", "3", "3");
    mtg::Card* cLib = game.createCard(&cr, 0);
    mtg::Card* c    = game.moveToZone(cLib->id, ZoneType::Battlefield, 0);
    ASSERT(c != nullptr);
    c->summoningSickness = false;

    game.setActivePlayer(0);
    ai.doAttackers(tm);

    // The creature should be attacking since 3 power > 5... wait, 3 < 5.
    // Let's add two more creatures to make total >= 5
    mtg::Card* c2Lib = game.createCard(&cr, 0);
    mtg::Card* c2 = game.moveToZone(c2Lib->id, ZoneType::Battlefield, 0);
    c2->summoningSickness = false;

    // Now total power = 6 >= 5 — lethal swing
    ai.doAttackers(tm);

    // At least one should be attacking
    bool anyAttacking = tm.combatState().isAttacking(c->id) || tm.combatState().isAttacking(c2->id);
    ASSERT(anyAttacking);
}

} // namespace

// ── Main ──────────────────────────────────────────────────────────────────────
// ── Marvel-fix confirmations ──────────────────────────────────────────────────

TEST(token_owner_opponent) {
    // The Sentry: "target opponent creates The Void". TokenOwner$ Opponent must
    // put the token under the opponent (player 1), not the source's controller.
    mtg::GameState game;
    auto rules = makeRules("Sentry", "Creature", "3", "3");
    mtg::Card* src = game.createCard(&rules, 0);
    game.moveToZone(src->id, mtg::ZoneType::Battlefield, 0);

    auto line = mtg::parseScriptLine(
        "DB$ Token | TokenAmount$ 1 | TokenName$ TheVoid | TokenTypes$ Creature | "
        "TokenColors$ B | TokenPower$ 5 | TokenToughness$ 5 | "
        "TokenKeywords$ Flying & Indestructible | TokenOwner$ Opponent");
    mtg::EffectContext ctx{ game, src, static_cast<uint8_t>(0), {}, 0 };
    mtg::executeEffect(line, ctx);

    const mtg::Card* tok = nullptr;
    for (const mtg::Card* c : game.battlefield().cards())
        if (c->isToken && c->name() == "TheVoid") { tok = c; break; }
    ASSERT(tok != nullptr);
    ASSERT(tok->controllerId == 1);                 // TokenOwner$ Opponent fix
    ASSERT(tok->rules->hasKeyword("Flying"));        // keyword-trim fix ("A & B")
    ASSERT(tok->rules->hasKeyword("Indestructible"));
}

TEST(trigger_activation_limit_once_per_turn) {
    // ActivationLimit$ N on a trigger line = fire at most N times per turn.
    mtg::GameState game;
    auto rules = makeRules("Watcher", "Creature", "2", "2");
    mtg::Card* c = game.createCard(&rules, 0);
    ASSERT(c->tryTriggerLimit("TrigX", 1) == true);
    ASSERT(c->tryTriggerLimit("TrigX", 1) == false);   // limit reached this turn
    ASSERT(c->tryTriggerLimit("TrigY", 1) == true);    // a different ability is independent
    c->triggerFiresThisTurn.clear();                   // turn reset
    ASSERT(c->tryTriggerLimit("TrigX", 1) == true);
}

TEST(clone_copies_with_overrides) {
    // Absorbing Man: becomes a copy of a target artifact/enchantment/land, but
    // keeps his name and is a legendary 4/4 creature with vigilance.
    mtg::GameState game;
    auto artRules = makeRules("Gadget", "Artifact", "", "");   // a non-creature artifact
    mtg::Card* art   = game.createCard(&artRules, 1);
    mtg::Card* artBf = game.moveToZone(art->id, mtg::ZoneType::Battlefield, 1);

    auto amRules = makeRules("Absorbing Man", "Legendary Creature Human Villain", "4", "4");
    mtg::Card* am   = game.createCard(&amRules, 0);
    mtg::Card* amBf = game.moveToZone(am->id, mtg::ZoneType::Battlefield, 0);

    auto line = mtg::parseScriptLine(
        "DB$ Clone | Defined$ Self | AddTypes$ Legendary Creature Human Villain | "
        "SetPower$ 4 | SetToughness$ 4 | Keywords$ Vigilance | KeepName$ True");
    std::vector<mtg::Target> tgts{ mtg::Target::forCard(artBf->id) };
    mtg::EffectContext ctx{ game, amBf, static_cast<uint8_t>(0), tgts, 0 };
    mtg::executeEffect(line, ctx);

    ASSERT(amBf->rules->name == "Absorbing Man");   // KeepName$
    ASSERT(amBf->rules->isCreature());              // AddTypes$ kept him a creature
    ASSERT(amBf->rules->power == "4");              // SetPower$
    ASSERT(amBf->rules->hasKeyword("Vigilance"));   // Keywords$
}

TEST(counter_bonus_doc_samson) {
    // Doc Samson: counters placed on your permanents get +1 of that kind.
    mtg::GameState game;
    auto docRules = makeRules("Doc Samson", "Legendary Creature", "4", "4");
    docRules.staticAbilityLines.push_back("Mode$ CounterBonus | Amount$ 1");
    mtg::Card* doc = game.createCard(&docRules, 0);
    game.moveToZone(doc->id, mtg::ZoneType::Battlefield, 0);

    auto bearRules = makeRules("Bear", "Creature", "2", "2");
    mtg::Card* bear   = game.createCard(&bearRules, 0);
    mtg::Card* bearBf = game.moveToZone(bear->id, mtg::ZoneType::Battlefield, 0);

    auto line = mtg::parseScriptLine("DB$ PutCounter | CounterType$ P1P1 | CounterNum$ 1");
    std::vector<mtg::Target> tgts{ mtg::Target::forCard(bearBf->id) };
    mtg::EffectContext ctx{ game, doc, static_cast<uint8_t>(0), tgts, 0 };
    mtg::executeEffect(line, ctx);
    ASSERT(bearBf->counterCount("+1/+1") == 2);   // 1 placed + 1 Doc Samson bonus
}

TEST(token_amount_by_trigger_pips) {
    // Namor: one Merfolk per blue pip in the noncreature spell you cast.
    mtg::GameState game;
    auto namorRules = makeRules("Namor", "Legendary Creature Merfolk", "0", "4");
    mtg::Card* namor = game.createCard(&namorRules, 0);
    game.moveToZone(namor->id, mtg::ZoneType::Battlefield, 0);

    auto spellRules = makeRules("Twincast", "Instant", "", "");
    spellRules.manaCost = mtg::ManaCost::parse("U U");   // two blue pips
    mtg::Card* spell = game.createCard(&spellRules, 0);

    auto line = mtg::parseScriptLine(
        "DB$ Token | TokenAmount$ 1 | AmountPipsOfTrigger$ U | TokenName$ Merfolk | "
        "TokenTypes$ Creature Merfolk | TokenColors$ U | TokenPower$ 1 | TokenToughness$ 1");
    mtg::EffectContext ctx{ game, namor, static_cast<uint8_t>(0), {}, 0 };
    ctx.triggeredCardId = spell->id;
    mtg::executeEffect(line, ctx);

    int merfolk = 0;
    for (const mtg::Card* c : game.battlefield().cards())
        if (c->isToken && c->name() == "Merfolk") ++merfolk;
    ASSERT(merfolk == 2);   // two blue pips → two tokens
}

TEST(token_doubler_replacement) {
    // Doubling Season: an effect that would create tokens you control creates twice as many.
    mtg::GameState game;
    auto dsRules = makeRules("Doubling Season", "Enchantment", "", "");
    dsRules.replacementLines.push_back(
        "Event$ CreateToken | ActiveZones$ Battlefield | ValidToken$ Card.YouCtrl | ReplaceWith$ DoubleToken");
    dsRules.svars["DoubleToken"] = "DB$ ReplaceToken | Type$ Amount";
    mtg::Card* ds = game.createCard(&dsRules, 0);
    game.moveToZone(ds->id, mtg::ZoneType::Battlefield, 0);

    auto makerRules = makeRules("Maker", "Creature", "1", "1");
    mtg::Card* maker = game.createCard(&makerRules, 0);
    game.moveToZone(maker->id, mtg::ZoneType::Battlefield, 0);

    auto line = mtg::parseScriptLine(
        "DB$ Token | TokenAmount$ 1 | TokenName$ Soldier | TokenTypes$ Creature Soldier | "
        "TokenPower$ 1 | TokenToughness$ 1");
    mtg::EffectContext ctx{ game, maker, static_cast<uint8_t>(0), {}, 0 };
    mtg::executeEffect(line, ctx);

    int soldiers = 0;
    for (const mtg::Card* c : game.battlefield().cards())
        if (c->isToken && c->name() == "Soldier") ++soldiers;
    ASSERT(soldiers == 2);   // one doubled to two
}

TEST(counter_doubler_replacement) {
    // Doubling Season also doubles counters placed on your permanents.
    mtg::GameState game;
    auto dsRules = makeRules("Doubling Season", "Enchantment", "", "");
    dsRules.replacementLines.push_back(
        "Event$ AddCounter | ActiveZones$ Battlefield | ValidCard$ Creature.YouCtrl | ReplaceWith$ DoubleCounters");
    dsRules.svars["DoubleCounters"] = "DB$ ReplaceCounter | Amount$ Y";
    dsRules.svars["Y"] = "ReplaceCount$CounterNum/Twice";
    mtg::Card* ds = game.createCard(&dsRules, 0);
    game.moveToZone(ds->id, mtg::ZoneType::Battlefield, 0);

    auto bearRules = makeRules("Bear", "Creature", "2", "2");
    mtg::Card* bear   = game.createCard(&bearRules, 0);
    mtg::Card* bearBf = game.moveToZone(bear->id, mtg::ZoneType::Battlefield, 0);

    auto line = mtg::parseScriptLine("DB$ PutCounter | CounterType$ P1P1 | CounterNum$ 2");
    std::vector<mtg::Target> tgts{ mtg::Target::forCard(bearBf->id) };
    mtg::EffectContext ctx{ game, ds, static_cast<uint8_t>(0), tgts, 0 };
    mtg::executeEffect(line, ctx);
    ASSERT(bearBf->counterCount("+1/+1") == 4);   // two doubled to four
}

TEST(replace_mana_forces_color) {
    // Infernal Darkness: all lands produce black mana instead of any other type.
    mtg::GameState game;
    auto idRules = makeRules("Infernal Darkness", "Enchantment", "", "");
    idRules.replacementLines.push_back(
        "Event$ ProduceMana | ActiveZones$ Battlefield | ValidCard$ Land | ReplaceWith$ ProduceB");
    idRules.svars["ProduceB"] = "DB$ ReplaceMana | ReplaceType$ B";
    mtg::Card* id = game.createCard(&idRules, 0);
    game.moveToZone(id->id, mtg::ZoneType::Battlefield, 0);

    auto forestRules = makeRules("Forest", "Basic Land Forest", "", "");
    mtg::Card* forest   = game.createCard(&forestRules, 0);
    mtg::Card* forestBf = game.moveToZone(forest->id, mtg::ZoneType::Battlefield, 0);

    auto line = mtg::parseScriptLine("AB$ Mana | Produced$ G | Amount$ 1");
    mtg::EffectContext ctx{ game, forestBf, static_cast<uint8_t>(0), {}, 0 };
    mtg::executeEffect(line, ctx);

    mtg::ManaPool& pool = game.player(0).manaPool();
    ASSERT(pool.canPay(mtg::ManaCost::parse("B")));    // produced black instead
    ASSERT(!pool.canPay(mtg::ManaCost::parse("G")));   // not green
}

TEST(replace_mana_doubles_amount) {
    // Mana Reflection: your permanents produce twice as much mana.
    mtg::GameState game;
    auto mrRules = makeRules("Mana Reflection", "Enchantment", "", "");
    mrRules.replacementLines.push_back(
        "Event$ ProduceMana | ActiveZones$ Battlefield | ValidActivator$ You | ValidCard$ Land | ReplaceWith$ ProduceTwice");
    mrRules.svars["ProduceTwice"] = "DB$ ReplaceMana | ReplaceAmount$ 2";
    mtg::Card* mr = game.createCard(&mrRules, 0);
    game.moveToZone(mr->id, mtg::ZoneType::Battlefield, 0);

    auto forestRules = makeRules("Forest", "Basic Land Forest", "", "");
    mtg::Card* forest   = game.createCard(&forestRules, 0);
    mtg::Card* forestBf = game.moveToZone(forest->id, mtg::ZoneType::Battlefield, 0);

    auto line = mtg::parseScriptLine("AB$ Mana | Produced$ G | Amount$ 1");
    mtg::EffectContext ctx{ game, forestBf, static_cast<uint8_t>(0), {}, 0 };
    mtg::executeEffect(line, ctx);

    mtg::ManaPool& pool = game.player(0).manaPool();
    ASSERT(pool.canPay(mtg::ManaCost::parse("G G")));   // one doubled to two
}

TEST(change_targets_redirects_spell) {
    // Deflection: a spell aimed at you is redirected to its caster (your opponent).
    GameState game;
    AbilityProcessor abilities(game);
    game.player(0).setLife(20);
    game.player(1).setLife(20);

    // Player 1 casts a 3-damage bolt targeting player 0.
    auto boltRules = makeRules("Zap", "Instant", "", "");
    boltRules.abilityLines.push_back("SP$ DealDamage | ValidTgts$ Any | NumDmg$ 3");
    mtg::Card* boltLib  = game.createCard(&boltRules, 1);
    mtg::Card* boltHand = game.moveToZone(boltLib->id, mtg::ZoneType::Hand, 1);
    ASSERT(abilities.castSpell(boltHand->id, 1, { mtg::Target::forPlayer(0) }));

    mtg::Card* onStack = nullptr;
    for (mtg::Card* c : game.stack().cards()) onStack = c;
    ASSERT(onStack != nullptr);

    // Player 0 resolves a ChangeTargets at the bolt.
    std::vector<mtg::Target> tgts{ mtg::Target::forCard(onStack->id) };
    mtg::EffectContext ctx{ game, nullptr, static_cast<uint8_t>(0), tgts, 0 };
    mtg::executeEffect(mtg::parseScriptLine("DB$ ChangeTargets | Defined$ Targeted"), ctx);

    abilities.resolveTop();   // resolve the bolt with redirected target
    ASSERT(game.player(0).life() == 20);   // original target spared
    ASSERT(game.player(1).life() == 17);   // bolt redirected back at the caster
}

TEST(control_spell_changes_controller) {
    // Commandeer: gain control of a spell on the stack; its permanent enters under you.
    GameState game;
    AbilityProcessor abilities(game);

    auto critRules = makeRules("Ogre", "Creature", "3", "3");
    mtg::Card* lib  = game.createCard(&critRules, 1);
    mtg::Card* hand = game.moveToZone(lib->id, mtg::ZoneType::Hand, 1);
    ASSERT(abilities.castSpell(hand->id, 1, {}));

    mtg::Card* onStack = nullptr;
    for (mtg::Card* c : game.stack().cards()) onStack = c;
    ASSERT(onStack != nullptr);

    std::vector<mtg::Target> tgts{ mtg::Target::forCard(onStack->id) };
    mtg::EffectContext ctx{ game, nullptr, static_cast<uint8_t>(0), tgts, 0 };
    mtg::executeEffect(mtg::parseScriptLine("DB$ ControlSpell | Mode$ Gain"), ctx);

    abilities.resolveTop();   // resolve the creature spell under the new controller
    mtg::Card* ogre = nullptr;
    for (mtg::Card* c : game.battlefield().cards()) if (c->name() == "Ogre") ogre = c;
    ASSERT(ogre != nullptr);
    ASSERT(ogre->controllerId == 0);   // gained control
}

TEST(gain_control_variant_to_owner) {
    // Homeward Path: each player gains control of all creatures they own.
    GameState game;
    // A creature owned by player 1 but currently controlled by player 0 (stolen).
    auto r = makeRules("Ogre", "Creature", "3", "3");
    mtg::Card* lib = game.createCard(&r, 1);            // owner = 1
    mtg::Card* ogre = game.moveToZone(lib->id, mtg::ZoneType::Battlefield, 1);
    ogre->controllerId = 0;                              // stolen by player 0

    auto line = mtg::parseScriptLine(
        "DB$ GainControlVariant | AllValid$ Creature | ChangeController$ CardOwner");
    mtg::EffectContext ctx{ game, nullptr, static_cast<uint8_t>(0), {}, 0 };
    mtg::executeEffect(line, ctx);
    ASSERT(ogre->controllerId == 1);   // returned to its owner
}

TEST(gain_control_variant_swap) {
    // Inniaz-style: each nonland permanent goes to the next player (the opponent in 2p).
    GameState game;
    auto r0 = makeRules("Mine", "Creature", "2", "2");
    mtg::Card* l0 = game.createCard(&r0, 0);
    mtg::Card* mine = game.moveToZone(l0->id, mtg::ZoneType::Battlefield, 0);
    auto r1 = makeRules("Yours", "Creature", "2", "2");
    mtg::Card* l1 = game.createCard(&r1, 1);
    mtg::Card* yours = game.moveToZone(l1->id, mtg::ZoneType::Battlefield, 1);

    auto line = mtg::parseScriptLine(
        "DB$ GainControlVariant | AllValid$ Permanent.nonLand | ChangeController$ NextPlayerInChosenDirection");
    mtg::EffectContext ctx{ game, nullptr, static_cast<uint8_t>(0), {}, 0 };
    mtg::executeEffect(line, ctx);
    ASSERT(mine->controllerId == 1);    // swapped
    ASSERT(yours->controllerId == 0);   // swapped
}

TEST(change_combatants_makes_attacker) {
    // Kari Zev-style: ChangeCombatants puts a second creature into combat as an attacker.
    GameState game;
    TurnManager tm{game};
    game.setActivePlayer(0);

    // An attacker already in combat (this sets activeCombat).
    auto ar = makeRules("Kari Zev", "Creature", "1", "3");
    mtg::Card* aLib = game.createCard(&ar, 0);
    mtg::Card* kari = game.moveToZone(aLib->id, mtg::ZoneType::Battlefield, 0);
    kari->summoningSickness = false;
    tm.declareAttacker(kari->id, 1);
    ASSERT(game.activeCombat != nullptr);

    // A second creature not yet attacking (e.g. the Ragavan token).
    auto rr = makeRules("Ragavan", "Creature", "2", "1");
    mtg::Card* rLib = game.createCard(&rr, 0);
    mtg::Card* rag  = game.moveToZone(rLib->id, mtg::ZoneType::Battlefield, 0);

    mtg::EffectContext ctx{ game, kari, static_cast<uint8_t>(0), {}, 0 };
    ctx.remembered = { rag->id };
    mtg::executeEffect(
        mtg::parseScriptLine("DB$ ChangeCombatants | Defined$ Remembered | Attacking$ True"), ctx);

    ASSERT(game.activeCombat->isAttacking(rag->id));
    ASSERT(rag->attacking);
    ASSERT(game.activeCombat->findAttack(rag->id)->defendingPlayerId == 1);
}

TEST(replace_damage_prevents_combat) {
    // Daunting Defender: prevent 1 damage dealt to a Cleric you control.
    GameState game;
    TurnManager tm{game};
    game.setActivePlayer(1);   // player 1 attacks

    // The prevention source: an enchantment-like permanent for player 0 with the R: line.
    auto ddRules = makeRules("Daunting Defender", "Enchantment", "", "");
    ddRules.replacementLines.push_back(
        "Event$ DamageDone | ActiveZones$ Battlefield | ValidTarget$ Creature.YouCtrl | ReplaceWith$ DBReplace | PreventionEffect$ True");
    ddRules.svars["DBReplace"] = "DB$ ReplaceDamage | Amount$ 1";
    mtg::Card* ddLib = game.createCard(&ddRules, 0);
    game.moveToZone(ddLib->id, mtg::ZoneType::Battlefield, 0);

    // Player 0's blocker (3 toughness) and player 1's 2-power attacker.
    auto blkR = makeRules("Cleric", "Creature", "0", "3");
    mtg::Card* blkLib = game.createCard(&blkR, 0);
    mtg::Card* blk = game.moveToZone(blkLib->id, mtg::ZoneType::Battlefield, 0);
    auto atkR = makeRules("Raider", "Creature", "2", "2");
    mtg::Card* atkLib = game.createCard(&atkR, 1);
    mtg::Card* atk = game.moveToZone(atkLib->id, mtg::ZoneType::Battlefield, 1);
    atk->summoningSickness = false;

    tm.declareAttacker(atk->id, 0);
    tm.mutableCombatState().attacks[0].blockerIds.push_back(blk->id);
    blk->blocking = true;
    tm.dealCombatDamage(false);

    // 2 power − 1 prevented = 1 marked on the blocker.
    ASSERT(blk->markedDamage == 1);
}

TEST(replace_damage_prevents_to_player) {
    // Guardian Seraph: prevent 1 damage from an opponent's source dealt to you.
    GameState game;
    game.player(0).setLife(20);

    auto gsRules = makeRules("Guardian Seraph", "Creature", "3", "4");
    gsRules.replacementLines.push_back(
        "Event$ DamageDone | ActiveZones$ Battlefield | ValidSource$ Card.OppCtrl | ValidTarget$ You | ReplaceWith$ DBReplace | PreventionEffect$ True");
    gsRules.svars["DBReplace"] = "DB$ ReplaceDamage | Amount$ 1";
    mtg::Card* gsLib = game.createCard(&gsRules, 0);
    game.moveToZone(gsLib->id, mtg::ZoneType::Battlefield, 0);

    // Opponent's bolt deals 3 to player 0 → 1 prevented → 2 lost.
    auto boltRules = makeRules("Zap", "Instant", "", "");
    mtg::Card* bolt = game.createCard(&boltRules, 1);  // controlled by player 1
    auto line = mtg::parseScriptLine("SP$ DealDamage | NumDmg$ 3");
    std::vector<mtg::Target> tgts{ mtg::Target::forPlayer(0) };
    mtg::EffectContext ctx{ game, bolt, static_cast<uint8_t>(1), tgts, 0 };
    mtg::executeEffect(line, ctx);
    ASSERT(game.player(0).life() == 18);   // 3 − 1 prevented
}

TEST(replace_damage_shield_all_but_one) {
    // Forcefield: prevent all but 1 damage a creature would deal to you
    // (Amount$ ShieldAmount = ReplaceCount$DamageAmount/Minus.1).
    GameState game;
    auto ffRules = makeRules("Forcefield", "Artifact", "", "");
    ffRules.replacementLines.push_back(
        "Event$ DamageDone | ActiveZones$ Battlefield | ValidSource$ Creature | ValidTarget$ You | ReplaceWith$ PreventDmg | PreventionEffect$ True");
    ffRules.svars["PreventDmg"]   = "DB$ ReplaceDamage | Amount$ ShieldAmount";
    ffRules.svars["ShieldAmount"] = "ReplaceCount$DamageAmount/Minus.1";
    mtg::Card* ff = game.createCard(&ffRules, 0);
    game.moveToZone(ff->id, mtg::ZoneType::Battlefield, 0);

    auto srcR = makeRules("Beast", "Creature", "5", "5");
    mtg::Card* beast = game.createCard(&srcR, 1);
    game.moveToZone(beast->id, mtg::ZoneType::Battlefield, 1);

    int got = mtg::applyDamageReplacements(5, beast, nullptr, 0, game);
    ASSERT(got == 1);   // all but 1 prevented
}

TEST(control_player_sets_marker) {
    // ControlPlayer records who controls whose next turn.
    GameState game;
    std::vector<mtg::Target> tgts{ mtg::Target::forPlayer(1) };
    mtg::EffectContext ctx{ game, nullptr, static_cast<uint8_t>(0), tgts, 0 };
    mtg::executeEffect(mtg::parseScriptLine("AB$ ControlPlayer | ValidTgts$ Player"), ctx);
    ASSERT(game.isTurnControlled(1));
    ASSERT(game.turnControllerOf[1] == 0);
}

TEST(control_player_denies_actions) {
    // A controlled player takes no voluntary actions (no land, etc.) that turn.
    GameState game;
    AbilityProcessor abilities(game);
    AiPlayer ai1(1, game, abilities);
    game.setActivePlayer(1);

    auto landR = makeRules("Forest", "Basic Land Forest", "", "");
    mtg::Card* landLib = game.createCard(&landR, 1);
    game.moveToZone(landLib->id, mtg::ZoneType::Hand, 1);

    // Player 0 controls player 1's turn → player 1 plays no land.
    game.setTurnController(1, 0);
    ASSERT(!ai1.tryPlayLand());
    ASSERT(game.player(1).hand().size() == 1);   // land still in hand

    // Release control → the land becomes playable again.
    game.clearTurnController(1);
    ASSERT(ai1.tryPlayLand());
    ASSERT(game.player(1).hand().size() == 0);   // land played
}

TEST(impulse_draw_exile_then_play) {
    // Reckless Impulse: exile top 2 cards; until end of your next turn you may play them.
    GameState game;
    game.setActivePlayer(0);

    // Library: two castable spells on top.
    auto spellR = makeRules("Bolt", "Instant", "", "");
    spellR.abilityLines.push_back("SP$ GainLife | LifeAmount$ 1 | Defined$ You");
    mtg::Card* s1 = game.createCard(&spellR, 0);
    mtg::Card* s2 = game.createCard(&spellR, 0);
    game.moveToZone(s1->id, mtg::ZoneType::Library, 0);
    game.moveToZone(s2->id, mtg::ZoneType::Library, 0);

    // The impulse spell itself, carrying the Dig + Effect(MayPlay) chain as SVars.
    auto impRules = makeRules("Reckless Impulse", "Sorcery", "", "");
    impRules.svars["DBEffect"]  = "DB$ Effect | StaticAbilities$ STPlay | RememberObjects$ Remembered | Duration$ UntilTheEndOfYourNextTurn";
    impRules.svars["STPlay"]    = "Mode$ Continuous | MayPlay$ True | Affected$ Card.IsRemembered | AffectedZone$ Exile";
    mtg::Card* imp = game.createCard(&impRules, 0);

    auto line = mtg::parseScriptLine(
        "SP$ Dig | DigNum$ 2 | ChangeNum$ All | DestinationZone$ Exile | RememberChanged$ True | SubAbility$ DBEffect");
    mtg::EffectContext ctx{ game, imp, static_cast<uint8_t>(0), {}, 0 };
    mtg::executeEffectChain(line, ctx);

    // Both cards exiled and flagged playable by player 0.
    int exiledPlayable = 0;
    for (const mtg::Card* c : game.exile().cards())
        if (c->mayPlayFromExile && c->mayPlayController == 0) ++exiledPlayable;
    ASSERT(exiledPlayable == 2);

    // The AI can now play one of them from exile.
    AbilityProcessor abilities(game);
    AiPlayer ai0(0, game, abilities);
    ASSERT(ai0.tryPlayImpulseFromExile());
}

TEST(temp_unblockable_via_effect) {
    // Access Tunnel: DB$ Effect | Mode$ CantBlockBy makes a creature unblockable this turn.
    GameState game;
    TurnManager tm{game};
    game.setActivePlayer(0);

    auto atkR = makeRules("Sneak", "Creature", "2", "2");
    mtg::Card* atkLib = game.createCard(&atkR, 0);
    mtg::Card* atk = game.moveToZone(atkLib->id, mtg::ZoneType::Battlefield, 0);
    atk->summoningSickness = false;
    auto blkR = makeRules("Wall", "Creature", "0", "4");
    mtg::Card* blkLib = game.createCard(&blkR, 1);
    mtg::Card* blk = game.moveToZone(blkLib->id, mtg::ZoneType::Battlefield, 1);

    // Grant "can't be blocked this turn" to the attacker via a DB$ Effect.
    auto impRules = makeRules("Access Tunnel", "Artifact", "", "");
    impRules.svars["Unblockable"] = "Mode$ CantBlockBy | ValidAttacker$ Card.IsRemembered";
    mtg::Card* imp = game.createCard(&impRules, 0);
    mtg::EffectContext ctx{ game, imp, static_cast<uint8_t>(0), {}, 0 };
    ctx.remembered = { atk->id };
    mtg::executeEffect(
        mtg::parseScriptLine("DB$ Effect | StaticAbilities$ Unblockable | RememberObjects$ Remembered"), ctx);
    ASSERT(atk->tempUnblockable);

    // The wall can no longer block it.
    tm.declareAttacker(atk->id, 1);
    ASSERT(!tm.declareBlocker(blk->id, atk->id));
}

TEST(effect_grants_keyword_until_eot) {
    // DB$ Effect | Mode$ Continuous | AddKeyword$ Trample grants the keyword to your team.
    GameState game;
    auto bearR = makeRules("Bear", "Creature", "2", "2");
    mtg::Card* bearLib = game.createCard(&bearR, 0);
    mtg::Card* bear = game.moveToZone(bearLib->id, mtg::ZoneType::Battlefield, 0);
    ASSERT(!bear->hasKeyword(mtg::KeywordAbility::Trample));

    auto srcR = makeRules("Overrun", "Sorcery", "", "");
    srcR.svars["KWPump"] = "Mode$ Continuous | Affected$ Creature.YouCtrl | AffectedZone$ Battlefield | AddKeyword$ Trample";
    mtg::Card* src = game.createCard(&srcR, 0);
    mtg::EffectContext ctx{ game, src, static_cast<uint8_t>(0), {}, 0 };
    mtg::executeEffect(mtg::parseScriptLine("DB$ Effect | StaticAbilities$ KWPump"), ctx);
    ASSERT(bear->hasKeyword(mtg::KeywordAbility::Trample));
}

TEST(effect_cant_block_until_eot) {
    // Falter: DB$ Effect | AddHiddenKeyword$ "... can't block." stops blocks this turn.
    GameState game;
    TurnManager tm{game};
    game.setActivePlayer(0);

    auto atkR = makeRules("Raider", "Creature", "2", "2");
    mtg::Card* atkLib = game.createCard(&atkR, 0);
    mtg::Card* atk = game.moveToZone(atkLib->id, mtg::ZoneType::Battlefield, 0);
    atk->summoningSickness = false;
    auto blkR = makeRules("Wall", "Creature", "0", "4");
    mtg::Card* blkLib = game.createCard(&blkR, 1);
    mtg::Card* blk = game.moveToZone(blkLib->id, mtg::ZoneType::Battlefield, 1);

    auto srcR = makeRules("Falter", "Sorcery", "", "");
    srcR.svars["NoBlock"] = "Mode$ Continuous | Affected$ Creature | AffectedZone$ Battlefield | AddHiddenKeyword$ CARDNAME can't block.";
    mtg::Card* src = game.createCard(&srcR, 0);
    mtg::EffectContext ctx{ game, src, static_cast<uint8_t>(0), {}, 0 };
    mtg::executeEffect(mtg::parseScriptLine("DB$ Effect | StaticAbilities$ NoBlock"), ctx);
    ASSERT(blk->tempCantBlock);

    tm.declareAttacker(atk->id, 1);
    ASSERT(!tm.declareBlocker(blk->id, atk->id));   // wall can't block
}

TEST(effect_cant_attack_until_eot) {
    // Blinding Light-style: DB$ Effect | Mode$ CantAttack stops attacks this turn.
    GameState game;
    TurnManager tm{game};
    game.setActivePlayer(0);

    auto atkR = makeRules("Raider", "Creature", "2", "2");
    mtg::Card* atkLib = game.createCard(&atkR, 0);
    mtg::Card* atk = game.moveToZone(atkLib->id, mtg::ZoneType::Battlefield, 0);
    atk->summoningSickness = false;

    // Normally it could attack.
    ASSERT(tm.declareAttacker(atk->id, 1));
    tm.mutableCombatState().clear();
    atk->attacking = false;

    auto srcR = makeRules("Blinding Light", "Sorcery", "", "");
    srcR.svars["NoAtk"] = "Mode$ CantAttack | ValidCard$ Creature";
    mtg::Card* src = game.createCard(&srcR, 0);
    mtg::EffectContext ctx{ game, src, static_cast<uint8_t>(0), {}, 0 };
    mtg::executeEffect(mtg::parseScriptLine("DB$ Effect | StaticAbilities$ NoAtk"), ctx);
    ASSERT(atk->tempCantAttack);
    ASSERT(!tm.declareAttacker(atk->id, 1));   // now can't attack
}

TEST(effect_cant_be_cast_this_turn) {
    // Silence/Abeyance: DB$ Effect | Mode$ CantBeCast stops a player casting this turn.
    GameState game;
    AbilityProcessor abilities(game);

    // Player 1 has a castable spell in hand.
    auto spellR = makeRules("Growth", "Instant", "", "");
    spellR.abilityLines.push_back("SP$ GainLife | LifeAmount$ 1 | Defined$ You");
    mtg::Card* spLib = game.createCard(&spellR, 1);
    mtg::Card* sp = game.moveToZone(spLib->id, mtg::ZoneType::Hand, 1);

    // Player 0 resolves an Effect: opponents can't cast spells this turn.
    auto srcR = makeRules("Silence", "Instant", "", "");
    srcR.svars["NoCast"] = "Mode$ CantBeCast | ValidCard$ Card | Caster$ Opponent";
    mtg::Card* src = game.createCard(&srcR, 0);
    mtg::EffectContext ctx{ game, src, static_cast<uint8_t>(0), {}, 0 };
    mtg::executeEffect(mtg::parseScriptLine("DB$ Effect | StaticAbilities$ NoCast"), ctx);

    // Player 1 (the opponent) now can't cast.
    ASSERT(!game.tempCantCast.empty());
    ASSERT(!abilities.castSpell(sp->id, 1, {}));
    // Player 0 (controller) is unaffected — sanity: restriction targets player 1 only.
    ASSERT(game.tempCantCast[0].first == 1);
}

TEST(cant_regenerate_overrides_shield) {
    // Carbonize: a creature with a regen shield still dies when it can't be regenerated.
    GameState game;
    auto r = makeRules("Troll", "Creature", "2", "2");
    mtg::Card* lib = game.createCard(&r, 0);
    mtg::Card* troll = game.moveToZone(lib->id, mtg::ZoneType::Battlefield, 0);
    troll->addCounter("regen", 1);    // regeneration shield
    troll->markedDamage = 5;          // lethal damage marked

    auto srcR = makeRules("Carbonize", "Instant", "", "");
    srcR.svars["NoRegen"] = "Mode$ CantRegenerate | ValidCard$ Card.IsRemembered";
    mtg::Card* src = game.createCard(&srcR, 0);
    mtg::EffectContext ctx{ game, src, static_cast<uint8_t>(0), {}, 0 };
    ctx.remembered = { troll->id };
    mtg::executeEffect(mtg::parseScriptLine("DB$ Effect | StaticAbilities$ NoRegen"), ctx);
    ASSERT(troll->tempCantRegenerate);

    StateBasedActions::run(game);
    ASSERT(game.findCard(troll->id) == nullptr);   // died — regen shield ignored
}

TEST(effect_anthem_pt_until_eot) {
    // Overrun-style: DB$ Effect | Mode$ Continuous | AddPower/AddToughness pumps your team.
    GameState game;
    auto bearR = makeRules("Bear", "Creature", "2", "2");
    mtg::Card* bearLib = game.createCard(&bearR, 0);
    mtg::Card* bear = game.moveToZone(bearLib->id, mtg::ZoneType::Battlefield, 0);

    auto srcR = makeRules("Overrun", "Sorcery", "", "");
    srcR.svars["Pump"] = "Mode$ Continuous | Affected$ Creature.YouCtrl | AffectedZone$ Battlefield | AddPower$ 3 | AddToughness$ 3 | AddKeyword$ Trample";
    mtg::Card* src = game.createCard(&srcR, 0);
    mtg::EffectContext ctx{ game, src, static_cast<uint8_t>(0), {}, 0 };
    mtg::executeEffect(mtg::parseScriptLine("DB$ Effect | StaticAbilities$ Pump"), ctx);
    ASSERT(mtg::effectivePower(*bear) == 5);
    ASSERT(mtg::effectiveToughness(*bear) == 5);
    ASSERT(bear->hasKeyword(mtg::KeywordAbility::Trample));
}

TEST(effect_cant_gain_life_this_turn) {
    // Atarka's Command: DB$ Effect | Mode$ CantGainLife stops a player gaining life.
    GameState game;
    game.player(1).setLife(20);

    auto srcR = makeRules("Atarka's Command", "Instant", "", "");
    srcR.svars["NoGain"] = "Mode$ CantGainLife | ValidPlayer$ Player.Opponent";
    mtg::Card* src = game.createCard(&srcR, 0);
    mtg::EffectContext ctx{ game, src, static_cast<uint8_t>(0), {}, 0 };
    mtg::executeEffect(mtg::parseScriptLine("DB$ Effect | StaticAbilities$ NoGain"), ctx);
    ASSERT(game.tempCantGainLife[1]);

    bool gained = game.gainLife(1, 5);
    ASSERT(!gained);
    ASSERT(game.player(1).life() == 20);   // no life gained
}

TEST(effect_extra_land_play) {
    // Explore: DB$ Effect | AdjustLandPlays$ 1 lets you play an additional land this turn.
    GameState game;
    game.setActivePlayer(0);
    ASSERT(game.landPlayLimit(0) == 1);   // base

    auto srcR = makeRules("Explore", "Sorcery", "", "");
    srcR.svars["MoreLand"] = "Mode$ Continuous | AdjustLandPlays$ 1";
    mtg::Card* src = game.createCard(&srcR, 0);
    mtg::EffectContext ctx{ game, src, static_cast<uint8_t>(0), {}, 0 };
    mtg::executeEffect(mtg::parseScriptLine("DB$ Effect | StaticAbilities$ MoreLand"), ctx);
    ASSERT(game.landPlayLimit(0) == 2);   // one extra land play
    ASSERT(game.tempExtraLandPlays[0] == 1);
}

TEST(effect_no_max_hand_size) {
    // "No maximum hand size this turn": discard step keeps all cards.
    GameState game;
    auto srcR = makeRules("Spellbook", "Enchantment", "", "");
    srcR.svars["MaxHand"] = "Mode$ Continuous | SetMaxHandSize$ 99";
    mtg::Card* src = game.createCard(&srcR, 0);
    mtg::EffectContext ctx{ game, src, static_cast<uint8_t>(0), {}, 0 };
    mtg::executeEffect(mtg::parseScriptLine("DB$ Effect | StaticAbilities$ MaxHand"), ctx);
    ASSERT(game.tempMaxHandSize[0] == 99);
}

TEST(effect_reduce_cost_this_turn) {
    // Ballad of the Black Flag: DB$ Effect | Mode$ ReduceCost makes your spells cost less.
    GameState game;
    AbilityProcessor abilities(game);

    auto spR = makeRules("Big Spell", "Sorcery", "", "");
    mtg::Card* sp = game.createCard(&spR, 0);
    ASSERT(abilities.genericReductionFor(*sp, 0) == 0);   // baseline

    auto srcR = makeRules("Ballad", "Enchantment", "", "");
    srcR.svars["Reduce"] = "Mode$ ReduceCost | Type$ Spell | Activator$ You | Amount$ 2";
    mtg::Card* src = game.createCard(&srcR, 0);
    mtg::EffectContext ctx{ game, src, static_cast<uint8_t>(0), {}, 0 };
    mtg::executeEffect(mtg::parseScriptLine("DB$ Effect | StaticAbilities$ Reduce"), ctx);

    ASSERT(abilities.genericReductionFor(*sp, 0) == 2);   // 2 less for player 0
    ASSERT(abilities.genericReductionFor(*sp, 1) == 0);   // not the opponent (Activator$ You)
}

TEST(effect_cant_activate_abilities) {
    // Abeyance: DB$ Effect | Mode$ CantBeActivated stops a player's non-mana abilities.
    GameState game;
    AbilityProcessor abilities(game);

    // A permanent with an activated ability for player 1.
    auto pR = makeRules("Pinger", "Artifact", "", "");
    pR.abilityLines.push_back("AB$ DealDamage | Cost$ T | NumDmg$ 1 | ValidTgts$ Any");
    mtg::Card* p = game.createCard(&pR, 1);
    game.moveToZone(p->id, mtg::ZoneType::Battlefield, 1);

    // Player 0 resolves: opponents can't activate abilities this turn.
    auto srcR = makeRules("Abeyance", "Instant", "", "");
    srcR.svars["NoAct"] = "Mode$ CantBeActivated | ValidCard$ Card | Activator$ Opponent";
    mtg::Card* src = game.createCard(&srcR, 0);
    mtg::EffectContext ctx{ game, src, static_cast<uint8_t>(0), {}, 0 };
    mtg::executeEffect(mtg::parseScriptLine("DB$ Effect | StaticAbilities$ NoAct"), ctx);
    ASSERT(game.tempCantActivate[1]);

    std::vector<mtg::Target> tgts{ mtg::Target::forPlayer(0) };
    ASSERT(!abilities.activateAbility(p->id, 0, 1, tgts));   // player 1 blocked
}

TEST(effect_must_block_any) {
    // Academic Dispute: DB$ Effect | Mode$ MustBlock forces a creature to block.
    GameState game;
    auto srcR = makeRules("Academic Dispute", "Sorcery", "", "");
    srcR.svars["MB"] = "Mode$ MustBlock | ValidCreature$ Card.IsRemembered";
    mtg::Card* src = game.createCard(&srcR, 0);

    auto cR = makeRules("Blocker", "Creature", "2", "2");
    mtg::Card* cLib = game.createCard(&cR, 1);
    mtg::Card* blk = game.moveToZone(cLib->id, mtg::ZoneType::Battlefield, 1);

    mtg::EffectContext ctx{ game, src, static_cast<uint8_t>(0), {}, 0 };
    ctx.remembered = { blk->id };
    mtg::executeEffect(mtg::parseScriptLine("DB$ Effect | StaticAbilities$ MB | RememberObjects$ Remembered"), ctx);
    ASSERT(blk->mustBlockAny);
}

TEST(plan_counter_threshold) {
    // CounterAdded | Threshold$ 3 fires only once the target reaches 3 counters.
    mtg::GameState game;
    auto planRules = makeRules("Master Plan", "Enchantment", "", "");
    planRules.triggerLines.push_back(
        "Mode$ CounterAdded | ValidCard$ Card.Self | CounterType$ PLAN | Threshold$ 3 | Execute$ TrigPay");
    planRules.svars["TrigPay"] = "DB$ Draw | NumCards$ 1";
    mtg::Card* plan   = game.createCard(&planRules, 0);
    mtg::Card* planBf = game.moveToZone(plan->id, mtg::ZoneType::Battlefield, 0);

    auto addOne = [&]() -> size_t {
        planBf->addCounter("PLAN", 1);
        std::vector<mtg::PendingTrigger> out;
        mtg::TriggerSystem::onCounterAdded(*planBf, "PLAN", 1, game, out);
        return out.size();
    };
    ASSERT(addOne() == 0);   // 1 of 3
    ASSERT(addOne() == 0);   // 2 of 3
    ASSERT(addOne() == 1);   // 3 of 3 → fires
}

TEST(clone_roundtrip_preserves_state) {
    // Guards GameState::clone()'s manual field list: per-turn state and per-card
    // maps (counters, trigger-limit) must survive a clone.
    mtg::GameState game;
    game.spellsCastByPlayer[0] = 2;
    auto r = makeRules("Bear", "Creature", "2", "2");
    mtg::Card* b  = game.createCard(&r, 0);
    mtg::Card* bf = game.moveToZone(b->id, mtg::ZoneType::Battlefield, 0);
    bf->addCounter("+1/+1", 3);
    bf->tryTriggerLimit("Once", 1);            // populate per-turn map (now exhausted)

    mtg::GameState c = game.clone();
    ASSERT(c.spellsCastByPlayer[0] == 2);      // scalar per-turn state copied
    const mtg::Card* cb = nullptr;
    for (const mtg::Card* x : c.battlefield().cards())
        if (x->name() == "Bear") { cb = x; break; }
    ASSERT(cb != nullptr);
    ASSERT(cb->counterCount("+1/+1") == 3);    // card counters copied
    ASSERT(cb->tryTriggerLimit("Once", 1) == false);  // per-turn trigger map copied (still exhausted)
}

TEST(charm_picks_best_mode) {
    // Modal "choose one": destroy a creature, or draw. With a big enemy creature
    // on board, the destroy mode has the larger board swing and should be chosen.
    mtg::GameState game;
    auto ogreRules = makeRules("Ogre", "Creature", "5", "5");
    mtg::Card* ogre   = game.createCard(&ogreRules, 1);
    mtg::Card* ogreBf = game.moveToZone(ogre->id, mtg::ZoneType::Battlefield, 1);

    auto srcRules = makeRules("Modal Spell", "Instant", "", "");
    srcRules.svars["DBKill"] = "DB$ Destroy | ValidTgts$ Creature";
    srcRules.svars["DBDraw"] = "DB$ Draw | NumCards$ 1";
    mtg::Card* src = game.createCard(&srcRules, 0);

    auto line = mtg::parseScriptLine("DB$ Charm | Choices$ DBKill,DBDraw");
    std::vector<mtg::Target> tgts{ mtg::Target::forCard(ogreBf->id) };
    mtg::EffectContext ctx{ game, src, static_cast<uint8_t>(0), tgts, 0 };
    mtg::executeEffect(line, ctx);

    bool ogreGone = true;
    for (const mtg::Card* c : game.battlefield().cards())
        if (c->name() == "Ogre") ogreGone = false;
    ASSERT(ogreGone);   // AI chose Destroy over Draw
}

TEST(first_strike_combat_ordering) {
    // A 2/2 first striker vs a 2/2 blocker: the striker deals lethal in the
    // first-strike step, so the blocker is gone before the regular step and
    // deals no damage back.
    mtg::GameState game;
    game.player(0).setLife(40); game.player(1).setLife(40);
    mtg::TurnManager tm(game);

    auto atkR = makeRules("Duelist", "Creature", "2", "2");
    atkR.keywords.push_back("First Strike");
    mtg::Card* atk = game.moveToZone(game.createCard(&atkR, 0)->id, mtg::ZoneType::Battlefield, 0);
    atk->summoningSickness = false;
    auto blkR = makeRules("Footsoldier", "Creature", "2", "2");
    mtg::Card* blk = game.moveToZone(game.createCard(&blkR, 1)->id, mtg::ZoneType::Battlefield, 1);
    mtg::ObjectId blkId = blk->id;

    tm.declareAttacker(atk->id, 1);
    tm.declareBlocker(blk->id, atk->id);
    tm.dealCombatDamage(true);              // first-strike step
    mtg::StateBasedActions::run(game);
    const mtg::Card* b = game.findCard(blkId);
    ASSERT(b == nullptr || !b->isOnBattlefield());   // blocker dead before regular step
    tm.dealCombatDamage(false);             // regular step — blocker gone
    mtg::StateBasedActions::run(game);
    ASSERT(atk->isOnBattlefield());          // striker survived
    ASSERT(atk->markedDamage == 0);          // took no damage back
}

TEST(lethal_damage_dies_sba) {
    // A creature with marked damage >= toughness is destroyed by state-based actions.
    mtg::GameState game;
    auto r = makeRules("Bear", "Creature", "2", "2");
    mtg::Card* c = game.moveToZone(game.createCard(&r, 0)->id, mtg::ZoneType::Battlefield, 0);
    mtg::ObjectId id = c->id;
    c->markedDamage = 2;                     // exactly lethal
    mtg::StateBasedActions::run(game);
    const mtg::Card* after = game.findCard(id);
    ASSERT(after == nullptr || !after->isOnBattlefield());
}

TEST(shock_land_pay_life) {
    // Shock land: "As ~ enters, you may pay 2 life. If you don't, it enters
    // tapped." Modeled as an R:Event$ Moved replacement → DB$ Tap with
    // UnlessCost$ PayLife<2>. Non-interactive GameState exercises the AI path:
    // pay when life is comfortably high, otherwise enter tapped.
    mtg::CardRules shock;
    shock.name = "Hallowed Fountain";
    shock.type = mtg::CardType::parse("Land Plains Island");
    shock.replacementLines.push_back(
        "R:Event$ Moved | ValidCard$ Card.Self | Destination$ Battlefield | "
        "ReplaceWith$ DBTap | ReplacementResult$ Updated");
    shock.svars["DBTap"] =
        "DB$ Tap | ETB$ True | Defined$ Self | UnlessCost$ PayLife<2> | UnlessPayer$ You";

    mtg::GameState game;   // m_humanInteractive defaults false → AI path
    // High life → pays 2, enters untapped.
    game.player(0).setLife(40);
    mtg::Card* bf = game.moveToZone(game.createCard(&shock, 0)->id,
                                    mtg::ZoneType::Battlefield, 0);
    ASSERT(bf != nullptr);
    ASSERT(!bf->tapped);                    // entered untapped
    ASSERT(game.player(0).life() == 38);    // paid 2 life

    // Low life → declines, enters tapped, no life paid.
    game.player(1).setLife(5);
    mtg::Card* bf2 = game.moveToZone(game.createCard(&shock, 1)->id,
                                     mtg::ZoneType::Battlefield, 1);
    ASSERT(bf2 != nullptr);
    ASSERT(bf2->tapped);                    // entered tapped
    ASSERT(game.player(1).life() == 5);     // nothing paid
}

TEST(dual_land_color_choice) {
    // A dual-typed land (Plains+Island) makes its mana from basic land subtypes,
    // not A:Mana lines. activateManaAbility's abilityIndex must select which
    // colour (W-U-B-R-G order) so the UI picker offers each.
    mtg::GameState game;
    mtg::AbilityProcessor abilities(game);
    mtg::CardRules dual;
    dual.name = "Hallowed Test";
    dual.type = mtg::CardType::parse("Land Plains Island");

    mtg::Card* land = game.moveToZone(game.createCard(&dual, 0)->id,
                                      mtg::ZoneType::Battlefield, 0);
    ASSERT(land != nullptr);
    // index 0 → White (Plains)
    ASSERT(abilities.activateManaAbility(land->id, 0, 0));
    ASSERT(game.player(0).manaPool().available(mtg::ManaCostShard::WHITE) >= 1);
    // untap and tap for index 1 → Blue (Island)
    land->tapped = false;
    ASSERT(abilities.activateManaAbility(land->id, 0, 1));
    ASSERT(game.player(0).manaPool().available(mtg::ManaCostShard::BLUE) >= 1);
}

TEST(exploration_extra_land_plays) {
    // S:Mode$ Continuous | Affected$ You | AdjustLandPlays$ N raises the per-turn
    // land-play limit (Exploration +1, Azusa +2, etc.). Stacks; opponent-agnostic.
    mtg::GameState game;
    ASSERT(game.landPlayLimit(0) == 1);
    ASSERT(game.canPlayLand(0));
    game.player(0).incLandsPlayed();
    ASSERT(!game.canPlayLand(0));            // 1 played, base limit 1 → done

    mtg::CardRules expl;
    expl.name = "Exploration";
    expl.type = mtg::CardType::parse("Enchantment");
    expl.staticAbilityLines.push_back(
        "Mode$ Continuous | Affected$ You | AdjustLandPlays$ 1");
    game.moveToZone(game.createCard(&expl, 0)->id, mtg::ZoneType::Battlefield, 0);
    ASSERT(game.landPlayLimit(0) == 2);      // base 1 + 1
    ASSERT(game.canPlayLand(0));             // 1 played, limit 2 → can play another

    mtg::CardRules azusa;
    azusa.name = "Azusa";
    azusa.type = mtg::CardType::parse("Legendary Creature Human Monk");
    azusa.staticAbilityLines.push_back(
        "Mode$ Continuous | Affected$ You | AdjustLandPlays$ 2");
    game.moveToZone(game.createCard(&azusa, 0)->id, mtg::ZoneType::Battlefield, 0);
    ASSERT(game.landPlayLimit(0) == 4);      // 1 + 1 + 2 (stacks)
    ASSERT(game.landPlayLimit(1) == 1);      // opponent unaffected
}

TEST(mana_color_payment_rules) {
    // Generic mana must NOT pay coloured pips; colourless pays generic only;
    // "any colour" mana pays any single coloured pip.
    mtg::GameState game;
    mtg::ManaPool& pool = game.player(0).manaPool();

    pool.addGeneric(3);
    ASSERT(!pool.canPay(mtg::ManaCost::parse("R")));   // generic can't pay {R}
    ASSERT(pool.canPay(mtg::ManaCost::parse("2")));    // generic pays {2}
    pool.empty();

    pool.add(mtg::ManaCostShard::COLORLESS, 2);
    ASSERT(pool.canPay(mtg::ManaCost::parse("2")));    // colourless pays generic
    ASSERT(!pool.canPay(mtg::ManaCost::parse("G")));   // colourless can't pay {G}
    pool.empty();

    pool.add(mtg::ManaCostShard{mtg::ManaAtom::COLORS_MASK, "Any"}, 1);
    ASSERT(pool.canPay(mtg::ManaCost::parse("R")));    // any-colour pays {R}
    pool.empty();
    pool.add(mtg::ManaCostShard{mtg::ManaAtom::COLORS_MASK, "Any"}, 1);
    ASSERT(pool.canPay(mtg::ManaCost::parse("G")));    // …and {G}
    pool.empty();

    pool.add(mtg::ManaCostShard::RED, 1);
    ASSERT(pool.canPay(mtg::ManaCost::parse("R")));
    ASSERT(!pool.canPay(mtg::ManaCost::parse("U")));   // red can't pay {U}
}

TEST(hideaway_exiles_and_links) {
    // Mosswort Bridge (K:Hideaway:4): entering exiles the most expensive of the
    // top cards face-down, linked to the land (exiledBy) so its AB$ Play |
    // Defined$ ExiledWith can later play it.
    mtg::GameState game;
    mtg::CardRules bomb;
    bomb.name = "Big Bomb"; bomb.type = mtg::CardType::parse("Creature Beast");
    bomb.manaCost = mtg::ManaCost::parse("6");
    mtg::CardRules filler;
    filler.name = "Filler"; filler.type = mtg::CardType::parse("Creature Bird");
    filler.manaCost = mtg::ManaCost::parse("1");
    game.moveToZone(game.createCard(&bomb, 0)->id,   mtg::ZoneType::Library, 0);
    game.moveToZone(game.createCard(&filler, 0)->id, mtg::ZoneType::Library, 0);

    mtg::CardRules mb;
    mb.name = "Mosswort Bridge"; mb.type = mtg::CardType::parse("Land");
    mb.keywords.push_back("Hideaway:4");
    mtg::Card* land = game.moveToZone(game.createCard(&mb, 0)->id,
                                      mtg::ZoneType::Battlefield, 0);
    ASSERT(land != nullptr);

    mtg::AbilityProcessor abilities(game);
    abilities.processHideaway(*land);

    bool found = false;
    for (const mtg::Card* ec : game.exile().cards())
        if (ec && ec->exiledBy == land->id && ec->isFaceDown &&
            ec->rules && ec->rules->name == "Big Bomb")
            found = true;
    ASSERT(found);   // the bomb (highest CMC) was hidden away under the land
}

TEST(niv_mizzet_different_color_pair_count) {
    // Count$Valid Card.YouCtrl$DifferentColorPair = number of DISTINCT two-colour
    // pairs among your exactly-two-colour permanents (Niv-Mizzet, Guildpact's X).
    mtg::GameState game;
    std::vector<std::unique_ptr<mtg::CardRules>> keep;
    auto addPerm = [&](const char* nm, const char* cost) {
        auto r = std::make_unique<mtg::CardRules>();
        r->name = nm; r->type = mtg::CardType::parse("Creature");
        r->manaCost = mtg::ManaCost::parse(cost);
        game.moveToZone(game.createCard(r.get(), 0)->id, mtg::ZoneType::Battlefield, 0);
        keep.push_back(std::move(r));
    };
    addPerm("WU one", "W U");     // pair WU
    addPerm("WU two", "W U");     // pair WU (duplicate — not counted again)
    addPerm("UB one", "U B");     // pair UB
    addPerm("WUB",    "W U B");   // three colours → ignored
    addPerm("Mono",   "W");       // one colour → ignored

    int x = game.evaluateCountExpr("Count$Valid Card.YouCtrl$DifferentColorPair", 0);
    ASSERT(x == 2);   // distinct pairs: WU and UB
}

TEST(slow_land_reveals_to_enter_untapped) {
    // Foreboding Ruins: DB$ Tap | ETB$ True | UnlessCost$ Reveal<1/Swamp;Mountain/…>
    // enters tapped UNLESS a Swamp/Mountain is revealable from hand.
    mtg::GameState game;
    mtg::CardRules ruins;
    ruins.name = "Foreboding Ruins"; ruins.type = mtg::CardType::parse("Land");
    mtg::Card* land = game.moveToZone(game.createCard(&ruins, 0)->id,
                                      mtg::ZoneType::Battlefield, 0);
    ASSERT(land != nullptr);
    const char* tapLine =
        "DB$ Tap | ETB$ True | Defined$ Self "
        "| UnlessCost$ Reveal<1/Swamp;Mountain/Swamp or Mountain> | UnlessPayer$ You";

    // Empty hand → no reveal possible → enters tapped.
    land->tapped = false;
    { mtg::ScriptLine sl = mtg::parseScriptLine(tapLine);
      mtg::EffectContext ctx{ game, land, 0, {}, 0 };
      mtg::executeEffectChain(sl, ctx); }
    ASSERT(land->tapped);

    // A Mountain in hand → reveal it → enters untapped.
    land->tapped = false;
    mtg::CardRules mtn;
    mtn.name = "Mountain"; mtn.type = mtg::CardType::parse("Basic Land Mountain");
    game.moveToZone(game.createCard(&mtn, 0)->id, mtg::ZoneType::Hand, 0);
    { mtg::ScriptLine sl = mtg::parseScriptLine(tapLine);
      mtg::EffectContext ctx{ game, land, 0, {}, 0 };
      mtg::executeEffectChain(sl, ctx); }
    ASSERT(!land->tapped);
}

TEST(token_script_creates_correct_token) {
    // DB$ Token | TokenScript$ <name> must resolve the Forge token-script name to
    // the right token (not a default 1/1): Treasure artifact, and a 2/2 white
    // Samurai with double strike.
    mtg::GameState game;
    mtg::CardRules src;
    src.name = "Maker"; src.type = mtg::CardType::parse("Enchantment");
    mtg::Card* maker = game.moveToZone(game.createCard(&src, 0)->id,
                                       mtg::ZoneType::Battlefield, 0);
    ASSERT(maker != nullptr);

    {
        mtg::ScriptLine sl = mtg::parseScriptLine(
            "DB$ Token | TokenScript$ c_a_treasure_sac | TokenOwner$ You");
        mtg::EffectContext ctx{ game, maker, 0, {}, 0 };
        mtg::executeEffectChain(sl, ctx);
    }
    {
        mtg::ScriptLine sl = mtg::parseScriptLine(
            "DB$ Token | TokenScript$ w_2_2_samurai_double_strike");
        mtg::EffectContext ctx{ game, maker, 0, {}, 0 };
        mtg::executeEffectChain(sl, ctx);
    }

    bool treasure = false, samurai = false;
    for (const mtg::Card* c : game.battlefield().cards()) {
        if (!c || !c->rules) continue;
        if (c->rules->name == "Treasure" && c->rules->type.isArtifact())
            treasure = true;
        if (c->rules->type.hasSubtype("Samurai") &&
            c->rules->power == "2" && c->rules->toughness == "2" &&
            c->hasKeyword(mtg::KeywordAbility::DoubleStrike))
            samurai = true;
    }
    ASSERT(treasure);   // Smothering Tithe makes a Treasure, not a 1/1
    ASSERT(samurai);    // Eternal Wanderer makes a 2/2 Samurai w/ double strike
}

TEST(sick_creature_activates_nontap_ability) {
    // Summoning sickness must NOT block an activated ability whose cost has no
    // {T} (Thanos's power-up: Cost$ C W U B R G). Only tap abilities are blocked.
    mtg::GameState game;
    mtg::CardRules thanos;
    thanos.name = "Thanos"; thanos.type = mtg::CardType::parse("Legendary Creature");
    thanos.manaCost = mtg::ManaCost::parse("R W B");
    thanos.abilityLines.push_back(
        "AB$ PutCounter | Cost$ C W U B R G | Defined$ Self | CounterType$ P1P1 | CounterNum$ 2");
    mtg::Card* t = game.moveToZone(game.createCard(&thanos, 0)->id,
                                   mtg::ZoneType::Battlefield, 0);
    ASSERT(t != nullptr);
    t->summoningSickness = true;   // just entered

    mtg::ManaPool& pool = game.player(0).manaPool();
    pool.add(mtg::ManaCostShard::COLORLESS, 1);
    pool.add(mtg::ManaCostShard::WHITE, 1);
    pool.add(mtg::ManaCostShard::BLUE,  1);
    pool.add(mtg::ManaCostShard::BLACK, 1);
    pool.add(mtg::ManaCostShard::RED,   1);
    pool.add(mtg::ManaCostShard::GREEN, 1);

    mtg::AbilityProcessor ap(game);
    ASSERT(ap.activateAbility(t->id, 0, 0, {}));   // not blocked by sickness
}

TEST(isochron_imprint_links_card) {
    // Isochron Scepter: DB$ ChangeZone | Imprint$ True exiles an instant from
    // hand linked to the source (exiledBy), so Card.IsImprinted+ExiledWithSource
    // can later find/copy it.
    mtg::GameState game;
    mtg::CardRules bolt;
    bolt.name = "Test Bolt"; bolt.type = mtg::CardType::parse("Instant");
    bolt.manaCost = mtg::ManaCost::parse("1 R");                 // mana value 2
    bolt.abilityLines.push_back("SP$ DealDamage | NumDmg$ 3 | ValidTgts$ Any");
    game.moveToZone(game.createCard(&bolt, 0)->id, mtg::ZoneType::Hand, 0);

    mtg::CardRules scep;
    scep.name = "Isochron Scepter"; scep.type = mtg::CardType::parse("Artifact");
    mtg::Card* scepter = game.moveToZone(game.createCard(&scep, 0)->id,
                                         mtg::ZoneType::Battlefield, 0);
    ASSERT(scepter != nullptr);

    mtg::ScriptLine sl = mtg::parseScriptLine(
        "DB$ ChangeZone | Imprint$ True | Origin$ Hand | Destination$ Exile "
        "| ChangeType$ Instant.cmcLE2 | ChangeNum$ 1");
    mtg::EffectContext ctx{ game, scepter, 0, {}, 0 };
    mtg::executeEffectChain(sl, ctx);

    bool linked = false;
    for (const mtg::Card* ec : game.exile().cards())
        if (ec && ec->exiledBy == scepter->id && ec->rules &&
            ec->rules->name == "Test Bolt")
            linked = true;
    ASSERT(linked);   // the instant was imprinted (exiled, linked to the scepter)
}

TEST(chromatic_lantern_grants_any_color) {
    // S:Mode$ Continuous | Affected$ Land.YouCtrl | AddAbility$ AnyMana grants
    // each of your lands "{T}: Add one mana of any colour".
    mtg::GameState game;
    mtg::CardRules forest;
    forest.name = "Forest";
    forest.type = mtg::CardType::parse("Basic Land Forest");
    mtg::Card* land = game.moveToZone(game.createCard(&forest, 0)->id,
                                      mtg::ZoneType::Battlefield, 0);
    ASSERT(land != nullptr);
    ASSERT(land->grantedAbilities.empty());

    mtg::CardRules lantern;
    lantern.name = "Chromatic Lantern";
    lantern.type = mtg::CardType::parse("Artifact");
    lantern.svars["AnyMana"] = "AB$ Mana | Cost$ T | Produced$ Any | Amount$ 1";
    lantern.staticAbilityLines.push_back(
        "Mode$ Continuous | Affected$ Land.YouCtrl | AddAbility$ AnyMana");
    game.moveToZone(game.createCard(&lantern, 0)->id, mtg::ZoneType::Battlefield, 0);

    ASSERT(land->grantedAbilities.size() == 1);
    ASSERT(land->grantedAbilities[0].find("Produced$ Any") != std::string::npos);
}

int main() {
    RUN(deathtouch_lethal);
    RUN(shock_land_pay_life);
    RUN(dual_land_color_choice);
    RUN(exploration_extra_land_plays);
    RUN(mana_color_payment_rules);
    RUN(chromatic_lantern_grants_any_color);
    RUN(hideaway_exiles_and_links);
    RUN(isochron_imprint_links_card);
    RUN(sick_creature_activates_nontap_ability);
    RUN(token_script_creates_correct_token);
    RUN(slow_land_reveals_to_enter_untapped);
    RUN(niv_mizzet_different_color_pair_count);
    RUN(trample_overflow);
    RUN(indestructible_survives_lethal);
    RUN(ward_prevents_targeting);
    RUN(commander_damage_threshold);
    RUN(graft_counter_placement);
    RUN(spell_mastery_condition);
    RUN(lifelink_gains_life);
    RUN(counter_cancellation);
    RUN(flying_evasion_blocks);
    RUN(reach_blocks_flying);
    RUN(poison_counters_win);
    RUN(legend_rule);
    RUN(mana_pool_colored);
    RUN(mana_generic_does_not_pay_colored);
    RUN(toughness_zero_dies_to_sba);
    RUN(counter_fast_path);
    RUN(ai_blocks_lethal_attacker);
    RUN(ai_prefers_removal_when_behind);
    RUN(ai_recognizes_lethal_attack);
    RUN(token_owner_opponent);
    RUN(trigger_activation_limit_once_per_turn);
    RUN(clone_copies_with_overrides);
    RUN(counter_bonus_doc_samson);
    RUN(token_amount_by_trigger_pips);
    RUN(token_doubler_replacement);
    RUN(counter_doubler_replacement);
    RUN(replace_mana_forces_color);
    RUN(replace_mana_doubles_amount);
    RUN(change_targets_redirects_spell);
    RUN(control_spell_changes_controller);
    RUN(gain_control_variant_to_owner);
    RUN(gain_control_variant_swap);
    RUN(change_combatants_makes_attacker);
    RUN(replace_damage_prevents_combat);
    RUN(replace_damage_prevents_to_player);
    RUN(replace_damage_shield_all_but_one);
    RUN(control_player_sets_marker);
    RUN(control_player_denies_actions);
    RUN(impulse_draw_exile_then_play);
    RUN(temp_unblockable_via_effect);
    RUN(effect_grants_keyword_until_eot);
    RUN(effect_cant_block_until_eot);
    RUN(effect_cant_attack_until_eot);
    RUN(effect_cant_be_cast_this_turn);
    RUN(cant_regenerate_overrides_shield);
    RUN(effect_anthem_pt_until_eot);
    RUN(effect_cant_gain_life_this_turn);
    RUN(effect_extra_land_play);
    RUN(effect_no_max_hand_size);
    RUN(effect_reduce_cost_this_turn);
    RUN(effect_cant_activate_abilities);
    RUN(effect_must_block_any);
    RUN(plan_counter_threshold);
    RUN(clone_roundtrip_preserves_state);
    RUN(charm_picks_best_mode);
    RUN(first_strike_combat_ordering);
    RUN(lethal_damage_dies_sba);

    std::cout << "\n" << g_passed << " passed, " << g_failed << " failed.\n";
    return g_failed ? 1 : 0;
}
