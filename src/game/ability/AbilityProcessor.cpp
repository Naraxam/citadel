#include "AbilityProcessor.h"
#include "Effects.h"
#include "ScriptLine.h"
#include "../GameState.h"
#include "../StateBasedActions.h"
#include "../TriggerSystem.h"
#include "../TurnManager.h"
#include "../EquipSystem.h"
#include "../CardFilter.h"
#include "../CardStats.h"
#include "../../core/mana/ManaAtom.h"
#include <algorithm>
#include <cassert>
#include <charconv>
#include <cctype>
#include <iostream>

namespace mtg {

// ── EffectContext helper ──────────────────────────────────────────────────────

std::string_view EffectContext::svar(const std::string& name) const noexcept {
    if (!source) return "";
    auto it = source->rules->svars.find(name);
    return it != source->rules->svars.end() ? std::string_view(it->second) : "";
}

// ── AbilityProcessor ─────────────────────────────────────────────────────────

AbilityProcessor::AbilityProcessor(GameState& game)
    : m_game(game)
{}

AbilityProcessor AbilityProcessor::cloneFor(GameState& newGame) const {
    AbilityProcessor copy(newGame);
    copy.m_stack         = m_stack;
    copy.m_humanTriggers = m_humanTriggers;
    return copy;
}

// ── TapsForMana trigger helper ────────────────────────────────────────────────
// Fired after a card is tapped for mana. Scans battlefield for Mode$ TapsForMana
// triggers and executes matching ones (Bubbling Muck, Blighted Burgeoning, etc.).
static void fireTapsForManaTriggers(const Card& tapped, uint8_t tappingPlayer,
                                    GameState& game) {
    for (const Card* watcher : game.battlefield().cards()) {
        for (const auto& raw : watcher->rules->triggerLines) {
            auto trig = parseScriptLine(raw);
            if (trig.get("Mode", "") != "TapsForMana") continue;
            auto zones = trig.get("TriggerZones", "Battlefield");
            if (zones.find("Battlefield") == std::string::npos) continue;

            // Activator$ You — only fires for the watcher's controller.
            auto activator = trig.get("Activator", "");
            if (!activator.empty() && activator == "You")
                if (watcher->controllerId != tappingPlayer) continue;

            // ValidCard$ — the tapped source must match.
            auto validCard = std::string(trig.get("ValidCard", ""));
            if (!validCard.empty() &&
                !cardMatchesAnyFilter(tapped, validCard, watcher->controllerId, watcher->id,
                                       const_cast<Card*>(watcher), &game))
                continue;

            // Execute$ — run the mana SVar.
            auto execName = std::string(trig.get("Execute", ""));
            if (execName.empty()) continue;
            auto it = watcher->rules->svars.find(execName);
            if (it == watcher->rules->svars.end()) continue;
            auto effect = parseScriptLine(it->second);
            if (effect.empty()) continue;

            EffectContext ctx{ game, const_cast<Card*>(watcher), tappingPlayer, {}, 0 };
            // TriggeredActivator = the tapping player; pass via controller field.
            ctx.controller = tappingPlayer;
            executeEffect(effect, ctx);
        }
    }
}

// ── Encore activation ─────────────────────────────────────────────────────────

bool AbilityProcessor::activateEncore(ObjectId cardId, uint8_t controller) {
    Card* source = m_game.findCard(cardId);
    if (!source || source->zone != ZoneType::Graveyard) return false;
    if (source->controllerId != controller)              return false;
    if (!source->rules || !source->rules->hasEncore)    return false;

    // Pay encore cost
    ManaPool& pool = m_game.player(controller).manaPool();
    if (pool.total() < source->rules->encoreCost.cmc())  return false;
    if (!payCost(source->rules->encoreCost, controller)) return false;

    // Exile the source
    std::string tokenName   = source->rules->name;
    std::string tokenTypes  = source->rules->type.toString();
    uint8_t     colorMask   = source->rules->manaCost.colorIdentity();
    std::string tokenPower  = source->rules->power;
    std::string tokenTough  = source->rules->toughness;
    std::vector<std::string> keywords = source->rules->keywords;

    m_game.moveToZone(cardId, ZoneType::Exile, controller);
    // source is now invalid — use stored values to create the token

    // In 2-player, create one token attacking the opponent
    (void)(controller ^ 1);  // opponent — used implicitly through attacking flag
    Card* token = m_game.createToken(tokenName, tokenTypes, colorMask,
                                      tokenPower, tokenTough, controller, keywords);
    if (token) {
        // Grant haste, mark as blitzed (auto-sacrifice at EOT)
        auto haste = static_cast<uint32_t>(KeywordAbility::Haste);
        token->tempKeywords     |= haste;
        token->keywordMask      |= haste;
        token->summoningSickness = false;
        m_game.blitzedCards.push_back(token->id);  // sac at EOT

        // Declare it as attacking
        token->attacking = true;
        m_game.attackedThisTurn[controller] = true;
        m_game.activeCombat = nullptr;  // will be set when combat starts
        // Note: token is on BF as if attacking; actual combat is resolved by the
        // combat step's normal flow.  This mirrors how other "enters attacking" work.
    }

    return true;
}

// ── Mana ability activation ───────────────────────────────────────────────────

bool AbilityProcessor::activateManaAbility(ObjectId sourceId, uint8_t controller,
                                            int abilityIndex) {
    Card* source = m_game.findCard(sourceId);
    if (!source)                    return false;
    if (!source->isOnBattlefield()) return false;
    if (source->controllerId != controller) return false;

    // MTG rules: each AB$ Mana line is a SEPARATE activated mana ability and
    // tapping the source can only fire one of them per tap (the {T} cost is
    // paid once). When no specific line is requested, default to the first
    // mana line — Shivan Reef previously fired both lines on a single tap,
    // producing {C} AND a colored pip AND dealing the 1 damage.
    bool activated = false;
    int  manaIdx   = 0;
    // Printed mana abilities plus any granted by statics (Chromatic Lantern's
    // "{T}: Add one mana of any colour" on your lands). Granted lines come after
    // printed ones so their indices line up with the UI's mana picker.
    std::vector<const std::string*> manaLines;
    for (const auto& l : source->rules->abilityLines) manaLines.push_back(&l);
    for (const auto& l : source->grantedAbilities)    manaLines.push_back(&l);
    for (const auto* linePtr : manaLines) {
        const auto& rawLine = *linePtr;
        auto script = parseScriptLine(rawLine);
        // "ManaReflected" (Exotic Orchard) is a mana ability too — it adds mana
        // without using the stack, so it belongs on this fast path, not the
        // generic targeted-ability path.
        if (script.abilityType != "AB" ||
            (script.effectType != "Mana" && script.effectType != "ManaReflected"))
            continue;

        int targetIdx = (abilityIndex >= 0) ? abilityIndex : 0;
        if (manaIdx != targetIdx) { ++manaIdx; continue; }

        auto costStr = std::string(script.get("Cost", ""));
        if (!payActivationCost(costStr, *source, controller)) return false;
        int xForMana = 0;
        {
            auto amtSv = script.get("Amount", "");
            if (amtSv == "X")
                xForMana = m_game.evaluateSVar("X", controller, source->rules);
        }
        EffectContext ctx{ m_game, source, controller, {}, xForMana };
        // executeEffectChain follows the SubAbility$ chain too — that's how
        // Shivan Reef's "{T}: Add {U} or {R}. CARDNAME deals 1 damage to
        // you." actually deals the damage (DB$ DealDamage on a sub-line).
        executeEffectChain(script, ctx);
        activated = true;
        break;
    }
    if (activated) {
        // Fire TapsForMana triggers (Bubbling Muck, Blighted Burgeoning, etc.)
        fireTapsForManaTriggers(*source, controller, m_game);
        return true;
    }

    // No mana ability found — check if it's a basic land (Oracle text based)
    // Basic lands in Forge don't have A: lines; they rely on the land subtype.
    if (source->isLand()) {
        if (source->tapped) return false;

        // Determine producible colours from the land's basic subtypes. A dual-
        // typed land (e.g. Plains+Island) can make either colour; abilityIndex
        // selects which (the UI offers a picker), defaulting to the first.
        std::vector<char> colors;
        auto addCol = [&](char p) {
            if (p && std::find(colors.begin(), colors.end(), p) == colors.end())
                colors.push_back(p);
        };
        // Fixed W-U-B-R-G order so the colour index matches the UI's picker.
        const auto& lt = source->rules->type;
        if (lt.hasSubtype("Plains"))   addCol('W');
        if (lt.hasSubtype("Island"))   addCol('U');
        if (lt.hasSubtype("Swamp"))    addCol('B');
        if (lt.hasSubtype("Mountain")) addCol('R');
        if (lt.hasSubtype("Forest"))   addCol('G');
        if (colors.empty()) return false; // unrecognised land subtype
        int cidx = (abilityIndex >= 0 && abilityIndex < (int)colors.size()) ? abilityIndex : 0;
        char produced = colors[cidx];

        source->tapped = true;
        // Route through effectMana so mana-production replacement effects
        // (Contamination, Infernal Darkness, Mana Reflection) apply to basic lands too.
        ScriptLine manaScript;
        manaScript.abilityType = "AB";
        manaScript.effectType  = "Mana";
        manaScript.params["Produced"] = std::string(1, produced);
        manaScript.params["Amount"]   = "1";
        EffectContext mctx{ m_game, source, controller, {}, 0 };
        effectMana(manaScript, mctx);
        fireTapsForManaTriggers(*source, controller, m_game);
        return true;
    }

    return false;
}

// ── Cost reduction ──────────────────────────────────────────────────────────

int AbilityProcessor::genericReductionFor(const Card& spell,
                                          uint8_t controller) const {
    if (!spell.rules) return 0;
    int discount  = 0;
    int surcharge = 0;

    // Amount$/Cost$ may be a digit string or an SVar name on the source card.
    auto resolveAmount = [&](std::string_view amountStr, const Card* srcCard) -> int {
        if (amountStr.empty()) return 1;
        if (std::isdigit(static_cast<unsigned char>(amountStr[0]))) {
            int v = 0;
            std::from_chars(amountStr.data(), amountStr.data() + amountStr.size(), v);
            return v;
        }
        return m_game.evaluateSVar(std::string(amountStr), srcCard->controllerId,
                                   srcCard->rules, srcCard->id);
    };

    // IsPresent$ condition on a static ability.
    auto checkIsPresent = [&](const ScriptLine& s, ObjectId srcId) -> bool {
        auto isPresent = s.get("IsPresent", "");
        if (isPresent.empty()) return true;
        auto presentCmp = s.get("PresentCompare", "GE1");
        int count = 0;
        const Card* srcCard2 = m_game.findCard(srcId);
        for (const Card* c2 : m_game.battlefield().cards())
            if (cardMatchesAnyFilter(*c2, std::string(isPresent),
                                     controller, srcId, srcCard2, &m_game)) ++count;
        if (presentCmp.size() < 3) return count >= 1;
        auto op = presentCmp.substr(0, 2);
        int n = 0;
        std::from_chars(presentCmp.data() + 2,
                        presentCmp.data() + presentCmp.size(), n);
        if (op == "GE") return count >= n;
        if (op == "LE") return count <= n;
        if (op == "EQ") return count == n;
        if (op == "GT") return count >  n;
        if (op == "LT") return count <  n;
        return false;
    };

    // Process S: lines on srcCard for ReduceCost/RaiseCost. isSelf=true means the
    // source is the spell card itself (its EffectZone$ All/Hand/Stack applies).
    auto processLines = [&](const std::vector<std::string>& lines,
                            const Card* srcCard, bool isSelf) {
        for (const auto& raw : lines) {
            auto s = parseScriptLine(raw);
            auto mode = s.get("Mode", "");
            bool isReduce = (mode == "ReduceCost");
            bool isRaise  = (mode == "RaiseCost");
            if (!isReduce && !isRaise) continue;

            auto ez = s.get("EffectZone", isSelf ? "All" : "Battlefield");
            if (ez != "All" && !(isSelf && (ez == "Hand" || ez == "Stack")))
                if (!srcCard->isOnBattlefield()) continue;

            auto activator = s.get("Activator", "");
            if (!activator.empty() && activator == "You")
                if (srcCard->controllerId != controller) continue;

            auto validCard = s.get("ValidCard", "");
            if (!validCard.empty()) {
                if (!cardMatchesAnyFilter(spell, std::string(validCard),
                                          controller, srcCard->id, srcCard, &m_game))
                    continue;
            }

            if (!checkIsPresent(s, srcCard->id)) continue;

            int amount = resolveAmount(
                isReduce ? s.get("Amount", "1") : s.get("Cost", "1"), srcCard);
            if (amount <= 0) continue;

            if (isReduce) discount  += amount;
            else          surcharge += amount;
        }
    };

    processLines(spell.rules->staticAbilityLines, &spell, true);
    for (const Card* bf : m_game.battlefield().cards())
        processLines(bf->rules->staticAbilityLines, bf, false);

    // Temporary "spells cost less this turn" reductions from a DB$ Effect.
    for (const auto& m : m_game.tempCostMods) {
        if (m.activator != 255 && m.activator != controller) continue;
        if (!m.validCard.empty() &&
            !cardMatchesAnyFilter(spell, m.validCard, controller, kInvalidId, nullptr, &m_game))
            continue;
        discount += m.amount;
    }

    return discount - surcharge;
}

// ── Spell casting ─────────────────────────────────────────────────────────────

bool AbilityProcessor::castSpell(ObjectId cardId, uint8_t controller,
                                  const std::vector<Target>& targets) {
    Card* source = m_game.findCard(cardId);
    if (!source)                            return false;
    if (source->controllerId != controller) return false;

    // Split second: while a split-second spell is on the stack, no new spells
    // may be cast (rule 702.61b). Mana abilities are still allowed.
    if (m_game.splitSecondOnStack() && source->zone != ZoneType::Command)
        return false;

    // Epic: once an Epic spell resolved, the controller can't cast new spells.
    // (Upkeep copies are triggered abilities, not casts — they bypass this check.)
    if (m_game.epicRestriction[controller])
        return false;

    // Allow casting from Hand, Graveyard (Flashback/Retrace/Unearth/Escape/Jump-start),
    // or Exile (Foretell/Suspend/Rebound).
    bool isFlashback   = false;
    bool isUnearth     = false;
    bool isForetell    = false;
    bool isSuspend     = false;
    bool isRebound     = false;
    bool isEscape      = false;
    bool isJumpStart   = false;
    bool isAftermath   = false;
    bool isFromCommand = false;
    bool isPrototype   = false;
    if (source->zone == ZoneType::Graveyard) {
        if (!source->rules->hasFlashback && !source->rules->hasRetrace &&
            !source->rules->hasUnearth  && !source->rules->hasEscape &&
            !source->rules->hasJumpStart && !source->rules->hasDisturb &&
            !source->rules->hasAftermath)
            return false;

        // Aftermath: the right side of a split card is castable only from the graveyard.
        // (The left side can only be cast from hand as a normal spell.)
        if (source->rules->hasAftermath) {
            // Aftermath is sorcery-speed: active player only, empty stack
            if (m_game.activePlayerId() != controller) return false;
            if (!m_game.stack().cards().empty()) return false;
            isAftermath = true;
        }


        if (source->rules->hasUnearth && !source->rules->hasFlashback &&
            !source->rules->hasRetrace && !source->rules->hasEscape &&
            !source->rules->hasJumpStart) {
            isUnearth = true;
        } else if (source->rules->hasEscape) {
            // Escape: exile escapeExile other cards from the controller's graveyard
            isEscape = true;
            Player& p = m_game.player(controller);
            int needed = source->rules->escapeExile;
            // Count other GY cards (exclude this card itself)
            std::vector<ObjectId> gyOthers;
            for (Card* c : p.graveyard().cards())
                if (c->id != source->id) gyOthers.push_back(c->id);
            if ((int)gyOthers.size() < needed) return false;
            // Exile the first `needed` other GY cards as the additional cost
            for (int i = 0; i < needed; ++i)
                m_game.moveToZone(gyOthers[i], ZoneType::Exile, controller);
            // Re-find source: still in GY after moving others
        } else if (source->rules->hasJumpStart) {
            // Jump-start: discard a card from hand as additional cost
            isJumpStart = true;
            Player& p = m_game.player(controller);
            if (p.hand().empty()) return false;
            // AI/auto: discard the cheapest non-land card, else any card
            Card* toDiscard = nullptr;
            for (Card* c : p.hand().cards())
                if (!c->isLand()) { toDiscard = c; break; }
            if (!toDiscard) toDiscard = p.hand().front();
            m_game.moveToZone(toDiscard->id, ZoneType::Graveyard, controller);
        } else {
            isFlashback = true; // flashback and retrace exile the card after

            // Retrace: pay additional cost of discarding a land from hand
            if (source->rules->hasRetrace && !source->rules->hasFlashback) {
                Player& p = m_game.player(controller);
                Card* land = nullptr;
                for (Card* c : p.hand().cards())
                    if (c->isLand()) { land = c; break; }
                if (!land) return false; // no land to discard
                m_game.moveToZone(land->id, ZoneType::Graveyard, controller);
                // source pointer is still valid (only land was moved)
            }
        }
    } else if (source->zone == ZoneType::Exile && source->foretold) {
        // Foretell: may only cast on a turn AFTER the one it was foretold
        if (m_game.turnNumber() <= source->foretoldOnTurn) return false;
        if (!source->rules->hasForetell) return false;
        isForetell = true;
    } else if (source->zone == ZoneType::Exile && source->suspended &&
               source->counterCount("TIME") == 0) {
        // Suspend: last TIME counter was just removed — cast for free
        isSuspend = true;
    } else if (source->zone == ZoneType::Exile && source->rebound) {
        // Rebound: cast for free from exile; goes to GY after resolving
        isRebound = true;
    } else if (source->zone == ZoneType::Exile && source->adventureExiled) {
        // Adventure card: cast the creature face from exile after the adventure resolved.
        // Uses normal hand-cast rules from here; adventureExiled flag is cleared on cast.
    } else if (source->zone == ZoneType::Exile && source->mayPlayFromExile) {
        // Impulse draw (Light Up the Stage, Reckless Impulse): play the exiled card
        // using its normal cost from here.
    } else if (source->isCommander && source->zone == ZoneType::Command) {
        // Commander: cast from the command zone; commander tax applied below.
        isFromCommand = true;
    } else if (source->zone != ZoneType::Hand) {
        return false;
    }

    // Mode$ CantBeCast — check if any static ability prevents casting this spell.
    for (const Card* bf : m_game.battlefield().cards()) {
        for (const auto& raw : bf->rules->staticAbilityLines) {
            auto sl = parseScriptLine(raw);
            if (sl.get("Mode", "") != "CantBeCast") continue;
            // ValidCard$ — must match the spell being cast.
            auto validCard = sl.get("ValidCard", "");
            if (!validCard.empty() &&
                !cardMatchesAnyFilter(*source, std::string(validCard), controller, bf->id, bf))
                continue;
            // Caster$ — which player is restricted from casting.
            auto casterStr = sl.get("Caster", "");
            if (!casterStr.empty()) {
                bool casterMatches = false;
                if (casterStr == "Opponent" || casterStr == "Player.Opponent") {
                    casterMatches = (bf->controllerId != controller);
                } else if (casterStr == "Opponent.castSpellThisTurn") {
                    casterMatches = (bf->controllerId != controller &&
                                     m_game.spellsCastByPlayer[controller] > 0);
                } else if (casterStr == "Opponent.attackedWithCreaturesThisTurn") {
                    casterMatches = (bf->controllerId != controller &&
                                     m_game.attackedThisTurn[controller]);
                } else if (casterStr == "Player" || casterStr == "Any") {
                    casterMatches = true;
                }
                if (!casterMatches) continue;
            }
            return false; // cast prevented by static ability
        }
    }

    // Temporary "can't cast" restrictions from a DB$ Effect (Silence, Abeyance, Azor).
    for (const auto& [pid, filt] : m_game.tempCantCast) {
        if (pid != controller) continue;
        if (filt.empty() ||
            cardMatchesAnyFilter(*source, filt, controller, kInvalidId, nullptr))
            return false;
    }

    // Snapshot rules pointer before zone change destroys this Card object.
    const CardRules* rules = source->rules;

    // Suspend/Rebound cast for free — use a static zero-cost sentinel.
    static const ManaCost kZeroCost{};
    // Determine the casting cost, considering alternative costs in priority order.
    // Disturb: cast from graveyard as enchantment face using disturbCost
    bool isDisturb = (!isFlashback && source->zone == ZoneType::Graveyard
                      && rules->hasDisturb);
    // Prototype: if affordable at prototype cost but not at full cost, use prototype
    if (rules->hasPrototype && !rules->prototypeCost.isNoCost() && source->zone == ZoneType::Hand) {
        ManaPool& pool = m_game.player(controller).manaPool();
        bool canFull  = pool.canPay(rules->manaCost);
        bool canProto = pool.canPay(rules->prototypeCost);
        if (!canFull && canProto) isPrototype = true;
    }
    const ManaCost* usedCostPtr = isFlashback   ? &rules->flashbackCost
                                : isUnearth     ? &rules->unearthCost
                                : isForetell    ? &rules->foretellCost
                                : isEscape      ? &rules->escapeCost
                                : isDisturb     ? &rules->disturbCost
                                : isSuspend     ? &kZeroCost
                                : isRebound     ? &kZeroCost
                                : isPrototype   ? &rules->prototypeCost
                                :                 &rules->manaCost;
    // Jump-start uses the normal mana cost (no override needed)
    bool evoke      = false;
    bool dashed     = false;
    bool blitzed    = false;
    bool emerged    = false;
    bool overloaded = false;
    bool morphed    = false;
    // Static {3} morph cost sentinel (generic mana only, CMC = 3)
    static const ManaCost kMorphCost = ManaCost::parse("3");
    if (!isFlashback && !isUnearth && !isForetell && !isSuspend && !isEscape && !isJumpStart) {
        ManaPool& pool = m_game.player(controller).manaPool();
        // Surge: cheaper cost when a spell was cast this turn (prefer if affordable)
        if (rules->hasSurge && m_game.spellsCastThisTurn > 0 &&
            pool.total() >= rules->surgeCost.cmc() &&
            rules->surgeCost.cmc() < usedCostPtr->cmc()) {
            usedCostPtr = &rules->surgeCost;
        }
        // Spectacle: cheaper cost when an opponent has lost life this turn
        if (rules->hasSpectacle && m_game.playerDamagedThisTurn[controller ^ 1] &&
            pool.total() >= rules->spectacleCost.cmc() &&
            rules->spectacleCost.cmc() < usedCostPtr->cmc()) {
            usedCostPtr = &rules->spectacleCost;
        }
        // Prowl: cheaper cost when a Prowl creature dealt combat damage to a player this turn
        if (rules->hasProwl && m_game.prowlActive[controller] &&
            pool.total() >= rules->prowlCost.cmc() &&
            rules->prowlCost.cmc() < usedCostPtr->cmc()) {
            usedCostPtr = &rules->prowlCost;
        }
        // Cleave: pay the higher cleave cost to remove bracketed text from the effect.
        // Use cleave cost when the caster can afford it and hasn't chosen a cheaper alt.
        if (rules->hasCleave && !evoke && !dashed && !blitzed &&
            pool.total() >= rules->cleaveCost.cmc() &&
            rules->cleaveCost.cmc() > usedCostPtr->cmc()) {
            usedCostPtr = &rules->cleaveCost;
            source->cleaved = true;  // flag read by effect resolution to strip bracketed text
        }
        // Miracle: cheaper cost when drawn as the first card this turn
        if (rules->hasMiracle && source->miracleEligible &&
            pool.total() >= rules->miracleCost.cmc() &&
            rules->miracleCost.cmc() < usedCostPtr->cmc()) {
            usedCostPtr = &rules->miracleCost;
        }
        // Emerge: sacrifice highest-CMC creature to reduce cost; only if can't afford normal
        if (rules->hasEmerge && pool.total() < usedCostPtr->cmc()) {
            Card* victim = nullptr;
            int   victimCmc = -1;
            for (Card* c : m_game.battlefield().cards()) {
                if (c->controllerId != controller || !c->isCreature()) continue;
                if (c->rules->cmc() > victimCmc) { victimCmc = c->rules->cmc(); victim = c; }
            }
            if (victim && pool.total() + victimCmc >= rules->emergeCost.cmc()) {
                m_game.moveToZone(victim->id, ZoneType::Graveyard, controller);
                pool.addGeneric(victimCmc);
                usedCostPtr = &rules->emergeCost;
                emerged     = true;
            }
        }
        // Dash: use dash cost if can't afford normal but can afford dash
        if (rules->hasDash &&
            pool.total() < usedCostPtr->cmc() &&
            pool.total() >= rules->dashCost.cmc()) {
            usedCostPtr = &rules->dashCost;
            dashed = true;
        }
        // Blitz: use blitz cost if can't afford normal but can afford blitz
        if (rules->hasBlitz && !dashed &&
            pool.total() < usedCostPtr->cmc() &&
            pool.total() >= rules->blitzCost.cmc()) {
            usedCostPtr = &rules->blitzCost;
            blitzed = true;
        }
        // Evoke: use evoke cost if can't afford normal but can afford evoke
        if (rules->hasEvoke &&
            pool.total() < usedCostPtr->cmc() &&
            pool.total() >= rules->evokeCost.cmc()) {
            usedCostPtr = &rules->evokeCost;
            evoke = true;
        }
        // Morph/Megamorph: cast face-down as a 2/2 for {3} when can't afford real cost
        if ((rules->hasMorph || rules->hasMegamorph) && !evoke && !dashed && !blitzed &&
            pool.total() < usedCostPtr->cmc() &&
            pool.total() >= 3) {
            usedCostPtr = &kMorphCost;
            morphed     = true;
        }
        // Overload: use the overload cost (applies effect to all valid targets)
        // Prefer overload if we can afford it and it changes the scope of the spell
        if (rules->hasOverload && !evoke && !dashed && !morphed &&
            pool.total() >= rules->overloadCost.cmc()) {
            usedCostPtr = &rules->overloadCost;
            overloaded  = true;
        }
    }

    // S:Mode$ AlternativeCost — check the spell's own S: lines and battlefield sources.
    // altManaCostStore must outlive usedCostPtr if we switch to an alternative cost.
    std::optional<ManaCost> altManaCostStore;
    if (!isFlashback && !isUnearth && !isForetell && !isSuspend && !isEscape
        && !isJumpStart && !evoke && !dashed && !emerged && !overloaded) {
        ManaPool& pool = m_game.player(controller).manaPool();

        // Split "Cost$" into a mana-symbol string and action tokens.
        auto parseAltCostStr = [](std::string_view str)
            -> std::pair<std::string, std::vector<std::string>> {
            std::string mana;
            std::vector<std::string> actions;
            while (!str.empty()) {
                auto sp = str.find(' ');
                std::string tok(sp == std::string_view::npos ? str : str.substr(0, sp));
                str = (sp == std::string_view::npos) ? std::string_view{} : str.substr(sp + 1);
                if (tok.empty()) continue;
                // Mana token: digit(s), single color letter, X, C, S, or hybrid (W/U, 2/B)
                bool isMana = false;
                if (!tok.empty() && std::isdigit(static_cast<unsigned char>(tok[0]))) {
                    isMana = true;
                } else if (tok.size() == 1) {
                    char c = tok[0];
                    isMana = (c=='W'||c=='U'||c=='B'||c=='R'||c=='G'||c=='C'||c=='X'||c=='S');
                } else if (tok.find('/') != std::string::npos) {
                    isMana = true; // hybrid: W/U, 2/B
                }
                if (isMana) { if (!mana.empty()) mana += ' '; mana += tok; }
                else         actions.push_back(tok);
            }
            return {mana, actions};
        };

        // Returns the integer N from tokens like "Return<N/Filter>", "Sac<N/Filter>", etc.
        // Also fills outFilter with the filter string.
        auto parseActionN = [](std::string_view tok, std::string& outFilter) -> int {
            auto lt = tok.find('<');
            if (lt == std::string_view::npos) return 1;
            auto inner = tok.substr(lt + 1);
            if (!inner.empty() && inner.back() == '>') inner.remove_suffix(1);
            auto slash = inner.find('/');
            int n = 1;
            if (slash != std::string_view::npos) {
                std::from_chars(inner.data(), inner.data() + slash, n);
                outFilter = std::string(inner.substr(slash + 1));
            } else {
                std::from_chars(inner.data(), inner.data() + inner.size(), n);
            }
            return n;
        };

        // Check that all action costs can be paid given the current board/hand state.
        auto feasible = [&](const std::vector<std::string>& actions) -> bool {
            for (const auto& tok : actions) {
                std::string filter;
                int n = parseActionN(tok, filter);
                std::string prefix = tok.substr(0, tok.find('<'));
                if (prefix == "Return" || prefix == "Sac") {
                    int cnt = 0;
                    for (const Card* c2 : m_game.battlefield().cards()) {
                        if (c2->controllerId != controller) continue;
                        if (c2->id == cardId) continue;
                        if (filter.empty() || cardMatchesAnyFilter(*c2, filter, controller, kInvalidId))
                            ++cnt;
                    }
                    if (cnt < n) return false;
                } else if (prefix == "ExileFromHand") {
                    int cnt = 0;
                    for (const Card* c2 : m_game.player(controller).hand().cards()) {
                        if (c2->id == cardId) continue;
                        if (filter.empty() || cardMatchesAnyFilter(*c2, filter, controller, kInvalidId))
                            ++cnt;
                    }
                    if (cnt < n) return false;
                } else if (prefix == "tapXType") {
                    int cnt = 0;
                    for (const Card* c2 : m_game.battlefield().cards()) {
                        if (c2->controllerId != controller || c2->tapped) continue;
                        if (filter.empty() || cardMatchesAnyFilter(*c2, filter, controller, kInvalidId))
                            ++cnt;
                    }
                    if (cnt < n) return false;
                } else if (prefix == "Discard") {
                    int cnt = 0;
                    for (const Card* c2 : m_game.player(controller).hand().cards()) {
                        if (c2->id == cardId) continue;
                        if (filter.empty() || cardMatchesAnyFilter(*c2, filter, controller, kInvalidId))
                            ++cnt;
                    }
                    if (cnt < n) return false;
                } else if (prefix == "PayLife") {
                    int n2 = 0; std::from_chars(tok.data()+8, tok.data()+tok.size()-1, n2);
                    if (m_game.player(controller).life() <= n2) return false;
                } else if (prefix == "GainLife") {
                    // always feasible — opponent gains life, we "pay" nothing tangible
                }
            }
            return true;
        };

        // Execute all action costs (Return, Sac, ExileFromHand, tapXType, Discard, PayLife, GainLife).
        auto execActions = [&](const std::vector<std::string>& actions) {
            for (const auto& tok : actions) {
                std::string filter;
                int n = parseActionN(tok, filter);
                std::string prefix = tok.substr(0, tok.find('<'));
                if (prefix == "Return") {
                    int done = 0;
                    std::vector<ObjectId> toMove;
                    for (const Card* c2 : m_game.battlefield().cards()) {
                        if (c2->controllerId != controller || c2->id == cardId) continue;
                        if (filter.empty() || cardMatchesAnyFilter(*c2, filter, controller, kInvalidId))
                            { toMove.push_back(c2->id); if (++done >= n) break; }
                    }
                    for (auto id2 : toMove) m_game.moveToZone(id2, ZoneType::Hand, controller);
                } else if (prefix == "Sac") {
                    int done = 0;
                    std::vector<ObjectId> toMove;
                    for (const Card* c2 : m_game.battlefield().cards()) {
                        if (c2->controllerId != controller || c2->id == cardId) continue;
                        if (filter.empty() || cardMatchesAnyFilter(*c2, filter, controller, kInvalidId))
                            { toMove.push_back(c2->id); if (++done >= n) break; }
                    }
                    for (auto id2 : toMove) m_game.moveToZone(id2, ZoneType::Graveyard, controller);
                } else if (prefix == "ExileFromHand") {
                    int done = 0;
                    std::vector<ObjectId> toMove;
                    for (const Card* c2 : m_game.player(controller).hand().cards()) {
                        if (c2->id == cardId) continue;
                        if (filter.empty() || cardMatchesAnyFilter(*c2, filter, controller, kInvalidId))
                            { toMove.push_back(c2->id); if (++done >= n) break; }
                    }
                    for (auto id2 : toMove) m_game.moveToZone(id2, ZoneType::Exile, controller);
                } else if (prefix == "tapXType") {
                    int done = 0;
                    std::vector<ObjectId> toTap;
                    for (const Card* c2 : m_game.battlefield().cards()) {
                        if (c2->controllerId != controller || c2->tapped) continue;
                        if (filter.empty() || cardMatchesAnyFilter(*c2, filter, controller, kInvalidId))
                            { toTap.push_back(c2->id); if (++done >= n) break; }
                    }
                    for (auto id2 : toTap) {
                        Card* c2 = m_game.findCard(id2);
                        if (c2) c2->tapped = true;
                    }
                } else if (prefix == "Discard") {
                    int done = 0;
                    std::vector<ObjectId> toMove;
                    for (const Card* c2 : m_game.player(controller).hand().cards()) {
                        if (c2->id == cardId) continue;
                        if (filter.empty() || cardMatchesAnyFilter(*c2, filter, controller, kInvalidId))
                            { toMove.push_back(c2->id); if (++done >= n) break; }
                    }
                    for (auto id2 : toMove) m_game.moveToZone(id2, ZoneType::Graveyard, controller);
                } else if (prefix == "PayLife") {
                    int n2 = 0; std::from_chars(tok.data()+8, tok.data()+tok.size()-1, n2);
                    m_game.loseLife(controller, n2);
                } else if (prefix == "GainLife") {
                    // e.g. GainLife<3/Player.Opponent> — opponent gains N life
                    m_game.gainLife(controller ^ 1, n);
                }
            }
        };

        // Check CheckSVar$/SVarCompare$ condition on an AlternativeCost line.
        auto checkCondition = [&](const ScriptLine& s, const Card* srcCard) -> bool {
            auto checkSVar = std::string(s.get("CheckSVar", ""));
            if (!checkSVar.empty()) {
                auto svarCmp = std::string(s.get("SVarCompare", "GE1"));
                int val = m_game.evaluateSVar(checkSVar, controller, srcCard->rules, srcCard->id);
                if (svarCmp.size() >= 3) {
                    auto op = svarCmp.substr(0, 2);
                    int n = 0;
                    std::from_chars(svarCmp.data() + 2, svarCmp.data() + svarCmp.size(), n);
                    bool ok = (op=="GE") ? (val>=n) : (op=="LE") ? (val<=n)
                            : (op=="EQ") ? (val==n) : (op=="GT") ? (val>n)
                            : (op=="LT") ? (val<n)  : false;
                    if (!ok) return false;
                }
            }
            auto checkSVar2 = std::string(s.get("CheckSecondSVar", ""));
            if (!checkSVar2.empty()) {
                auto svarCmp2 = std::string(s.get("SecondSVarCompare", "GE1"));
                int val2 = m_game.evaluateSVar(checkSVar2, controller, srcCard->rules, srcCard->id);
                if (svarCmp2.size() >= 3) {
                    auto op = svarCmp2.substr(0, 2);
                    int n = 0;
                    std::from_chars(svarCmp2.data() + 2, svarCmp2.data() + svarCmp2.size(), n);
                    bool ok = (op=="GE") ? (val2>=n) : (op=="LE") ? (val2<=n)
                            : (op=="EQ") ? (val2==n) : (op=="GT") ? (val2>n)
                            : (op=="LT") ? (val2<n)  : false;
                    if (!ok) return false;
                }
            }
            return true;
        };

        auto tryAltLine = [&](const ScriptLine& s, const Card* srcCard) -> bool {
            if (s.get("Mode", "") != "AlternativeCost") return false;
            // Only handle spell casting (not Activated.Cycling etc.)
            auto validSA = std::string(s.get("ValidSA", ""));
            if (!validSA.empty() && validSA != "Spell.Self" && validSA != "Spell") return false;
            // ValidPlayer$ You — must be the controller
            auto validPlayer = std::string(s.get("ValidPlayer", ""));
            if (validPlayer == "You" && srcCard->controllerId != controller) return false;
            // EffectZone$ — default All for own card; Battlefield for external sources
            auto ez = std::string(s.get("EffectZone", srcCard->id == cardId ? "All" : "Battlefield"));
            if (ez != "All" && ez != "Graveyard" && !srcCard->isOnBattlefield()) return false;
            // ValidCard$ — spell being cast must match
            auto validCard = std::string(s.get("ValidCard", ""));
            if (!validCard.empty()) {
                const Card* spell = m_game.findCard(cardId);
                if (!spell || !cardMatchesAnyFilter(*spell, validCard, controller, srcCard->id, srcCard, &m_game))
                    return false;
            }
            // Conditional check
            if (!checkCondition(s, srcCard)) return false;
            // IsPresent$ condition
            auto isPresent = std::string(s.get("IsPresent", ""));
            if (!isPresent.empty()) {
                bool found = false;
                for (const Card* c2 : m_game.battlefield().cards())
                    if (cardMatchesAnyFilter(*c2, isPresent, controller, kInvalidId, srcCard, &m_game))
                        { found = true; break; }
                if (!found) return false;
            }
            // Parse the alternative cost
            auto costStr = std::string(s.get("Cost", "0"));
            auto [manaPart, actions] = parseAltCostStr(costStr);
            ManaCost altMana = ManaCost::parse(manaPart.empty() ? "0" : manaPart);
            // AI: only use the alternative if it saves mana OR we can't afford normal
            if (altMana.cmc() > usedCostPtr->cmc() && pool.total() >= usedCostPtr->cmc())
                return false;
            // Check that action costs can be fulfilled
            if (!feasible(actions)) return false;
            // Execute action costs (exile card from hand, return land, etc.)
            execActions(actions);
            // Switch to the alternative mana cost
            altManaCostStore = altMana;
            usedCostPtr = &*altManaCostStore;
            return true;
        };

        // Try spell's own static lines, then battlefield sources.
        bool altUsed = false;
        for (const auto& raw : rules->staticAbilityLines) {
            if (tryAltLine(parseScriptLine(raw), source)) { altUsed = true; break; }
        }
        if (!altUsed) {
            for (const Card* bf : m_game.battlefield().cards()) {
                if (bf->id == cardId) continue;
                for (const auto& raw : bf->rules->staticAbilityLines) {
                    if (tryAltLine(parseScriptLine(raw), bf)) { altUsed = true; break; }
                }
                if (altUsed) break;
            }
        }
    }

    const ManaCost& usedCost = *usedCostPtr;

    // Delve: exile cards from graveyard to reduce the generic portion of the cost.
    if (rules->hasDelve) {
        ManaPool& pool  = m_game.player(controller).manaPool();
        int shortfall   = usedCost.cmc() - pool.total();
        int maxDelve    = usedCost.genericAmount();
        if (shortfall > 0 && maxDelve > 0) {
            // Collect GY card IDs before any exiles invalidate pointers.
            std::vector<std::pair<ObjectId, int>> gyCards; // (id, cmc)
            for (const Card* c : m_game.player(controller).graveyard().cards())
                gyCards.emplace_back(c->id, c->rules->cmc());
            // Exile cheapest cards first.
            std::sort(gyCards.begin(), gyCards.end(),
                [](const auto& a, const auto& b){ return a.second < b.second; });
            int toExile = std::min(shortfall, std::min(maxDelve, (int)gyCards.size()));
            for (int i = 0; i < toExile; ++i) {
                m_game.moveToZone(gyCards[i].first, ZoneType::Exile, controller);
                pool.addGeneric(1); // each exiled card pays for {1}
            }
        }
    }

    // Affinity: reduce cost by 1 for each permanent matching affinityType you control.
    if (rules->hasAffinity && !rules->affinityType.empty()) {
        ManaPool& pool    = m_game.player(controller).manaPool();
        int       discount = 0;
        const std::string& atype = rules->affinityType;
        for (const Card* c : m_game.battlefield().cards()) {
            if (c->controllerId != controller) continue;
            bool match = false;
            if      (atype == "Artifacts")       match = c->rules->type.isArtifact();
            else if (atype == "Enchantments")    match = c->rules->type.isEnchantment();
            else if (atype == "Creatures")       match = c->isCreature();
            else if (atype == "Lands")           match = c->isLand();
            else if (atype == "Plains")          match = c->rules->type.hasSubtype("Plains");
            else if (atype == "Islands")         match = c->rules->type.hasSubtype("Island");
            else if (atype == "Swamps")          match = c->rules->type.hasSubtype("Swamp");
            else if (atype == "Mountains")       match = c->rules->type.hasSubtype("Mountain");
            else if (atype == "Forests")         match = c->rules->type.hasSubtype("Forest");
            else                                 match = c->rules->type.hasSubtype(atype); // e.g. "Ally", "Citizen"
            if (match) ++discount;
        }
        int toAdd = std::min(discount, usedCost.genericAmount());
        if (toAdd > 0) pool.addGeneric(toAdd);
    }

    // S:Mode$ ReduceCost / RaiseCost static abilities — adjust the generic
    // portion of the cost we actually pay. Apply it to the cost rather than
    // padding the pool with free mana: padding overpaid (the player tapped the
    // full cost out of habit and the extra floated away), and it never let an
    // affordability check see the lower cost. genericReductionFor() is the same
    // function the UI/AI gates use, so what's shown, checked, and paid agree.
    ManaCost effectiveCost = usedCost;
    if (const Card* spellCard = m_game.findCard(cardId)) {
        int reduction = genericReductionFor(*spellCard, controller);
        if (reduction != 0) effectiveCost = effectiveCost.reduceGeneric(reduction);
    }

    // Commander tax: +{2} generic mana per prior cast from the command zone.
    // Pre-drain from pool before payCost so it acts as a surcharge.
    if (isFromCommand) {
        int tax = 2 * m_game.commanderCastCount[controller];
        if (tax > 0) {
            ManaPool& pool = m_game.player(controller).manaPool();
            int toDrain = std::min(tax, pool.total());
            if (toDrain < tax) return false; // can't afford the commander tax
            pool.addGeneric(-toDrain);
        }
    }

    // Additional-cost surcharge (K:CastSurcharge:N — e.g. Titania "as an additional
    // cost to cast this spell, pay {2}"). Drained as a generic surcharge like the
    // commander tax; if the player can't cover it, the cast fails.
    if (source->rules->castSurcharge > 0) {
        ManaPool& pool = m_game.player(controller).manaPool();
        int sc = source->rules->castSurcharge;
        if (pool.total() < sc) return false;
        pool.addGeneric(-sc);
    }

    // For X-cost spells, compute X = all mana left after paying the fixed colored cost.
    int xVal = 0;
    if (effectiveCost.hasX()) {
        ManaPool& pool = m_game.player(controller).manaPool();
        int fixedCmc  = effectiveCost.cmc();
        int available = pool.total();
        if (available < fixedCmc) return false;
        xVal = available - fixedCmc;
    }

    // Pass the spell card as the payee so restricted mana (e.g. Secluded
    // Courtyard's "any colour") can pay only when this is a matching creature spell.
    if (!payCost(effectiveCost, controller, source, /*isSpell=*/true, /*isActivated=*/false))
        return false;

    // Pay the X portion (drain all remaining mana)
    if (xVal > 0) {
        ManaPool& pool = m_game.player(controller).manaPool();
        int drain = pool.total();
        if (drain > 0) pool.addGeneric(-drain);
    }

    // Move card Command/Hand → Stack — source is invalid after this line
    Card* onStack = m_game.moveToZone(cardId, ZoneType::Stack, controller);
    if (!onStack) return false;

    // Track commander cast count for commander tax on subsequent casts.
    if (isFromCommand)
        ++m_game.commanderCastCount[controller];

    // Filter out illegal targets (Hexproof / Shroud / Ward / Protection).
    uint8_t sourceColor = rules->manaCost.colorIdentity();
    std::vector<Target> legalTargets;
    for (const auto& t : targets) {
        if (t.isCard()) {
            const Card* tc = m_game.findCard(t.cardId);
            if (tc) {
                if (tc->cantBeTargeted) continue;
                if (tc->hasKeyword(KeywordAbility::Shroud)) continue;
                if (tc->controllerId != controller) {
                    if (tc->hasKeyword(KeywordAbility::Hexproof)) continue;
                    // Colour-specific hexproof ("Hexproof from Black" etc.)
                    if (tc->rules->hexproofFromColor != 0 &&
                        hasHexproofFrom(tc->rules->hexproofFromColor, sourceColor)) continue;
                }
                // Protection from everything — can't be targeted by any source
                if (maskHas(tc->keywordMask, KeywordAbility::ProtectionAll) &&
                    tc->controllerId != controller) continue;
                // Type-based protection ("Protection from artifacts", "from creatures"…)
                if (tc->rules->protectionTypeMask && tc->controllerId != controller) {
                    bool blocked = false;
                    if ((tc->rules->protectionTypeMask & 0x01) && rules->type.isArtifact())    blocked = true;
                    if ((tc->rules->protectionTypeMask & 0x02) && rules->type.isEnchantment()) blocked = true;
                    if ((tc->rules->protectionTypeMask & 0x04) && rules->type.isCreature())    blocked = true;
                    if ((tc->rules->protectionTypeMask & 0x08) && rules->type.isInstant())     blocked = true;
                    if ((tc->rules->protectionTypeMask & 0x10) && rules->type.isSorcery())     blocked = true;
                    if ((tc->rules->protectionTypeMask & 0xFF) == 0xFF) blocked = true;
                    if (blocked) continue;
                }
                // Ward: pay the ward cost or the target is illegal for this spell
                if (tc->rules->hasWard && tc->controllerId != controller) {
                    if (!payCost(tc->rules->wardCost, controller)) continue;
                }
                if (hasProtectionFrom(tc->keywordMask, sourceColor)) continue;
            }
        }
        legalTargets.push_back(t);
    }

    // Convoke: tap untapped creatures to generate mana toward the casting cost.
    if (rules->hasConvoke) {
        ManaPool& pool = m_game.player(controller).manaPool();
        if (!pool.canPay(usedCost)) {
            for (Card* c : m_game.battlefield().cards()) {
                if (pool.canPay(usedCost)) break;
                if (c->controllerId != controller || !c->isCreature() || c->tapped) continue;
                // Tap the creature; it contributes its color (or generic if colorless)
                uint8_t col = c->rules->manaCost.colorIdentity();
                c->tapped = true;
                if (col)
                    pool.add(ManaCostShard::fromAtoms(col & -col), 1); // lowest-bit color
                else
                    pool.addGeneric(1);
            }
        }
    }

    // Improvise: tap untapped artifacts to pay for {1} each.
    if (rules->hasImprovise) {
        ManaPool& pool = m_game.player(controller).manaPool();
        if (!pool.canPay(usedCost)) {
            for (Card* c : m_game.battlefield().cards()) {
                if (pool.canPay(usedCost)) break;
                if (c->controllerId != controller || !c->rules->type.isArtifact() || c->tapped) continue;
                c->tapped = true;
                pool.addGeneric(1);
            }
        }
    }

    // Optional Bargain cost — sacrifice an artifact, enchantment, or token you control
    // as an additional cost for a bonus effect.
    bool bargained = false;
    if (rules->hasBargain) {
        // Find a valid sacrificeable permanent (artifact / enchantment / token)
        Card* toSac = nullptr;
        for (Card* c : m_game.battlefield().cards()) {
            if (c->controllerId != controller) continue;
            if (c->id == source->id) continue;
            if (c->isToken || c->rules->type.isArtifact() || c->rules->type.isEnchantment()) {
                toSac = c; break;
            }
        }
        if (toSac) {
            m_game.moveToZone(toSac->id, ZoneType::Graveyard, toSac->ownerId);
            bargained = true;
        }
    }

    // ── New additional-cost mechanics ─────────────────────────────────────────

    // Casualty N: sacrifice a creature with power ≥ N to copy this spell
    if (rules->hasCasualty) {
        Card* victim = nullptr;
        for (Card* c : m_game.battlefield().cards()) {
            if (c->controllerId != controller || !c->isCreature()) continue;
            if (effectivePower(*c) >= rules->casualtyAmount) { victim = c; break; }
        }
        if (victim) {
            m_game.moveToZone(victim->id, ZoneType::Graveyard, victim->ownerId);
            drainPendingTriggers();
            // The spell copy is queued after it's put on the stack
        }
    }

    // Replicate: pay replicateCost once (AI: once) to copy the spell
    bool replicated = false;
    if (rules->hasReplicate) {
        ManaPool& pool = m_game.player(controller).manaPool();
        if (pool.total() >= rules->replicateCost.cmc()) {
            if (payCost(rules->replicateCost, controller))
                replicated = true;
        }
    }

    // Conspire: tap two untapped creatures sharing a colour with this spell
    bool conspired = false;
    if (rules->hasConspire) {
        uint8_t spellColor = rules->manaCost.colorIdentity();
        std::vector<Card*> tapped;
        for (Card* c : m_game.battlefield().cards()) {
            if (c->controllerId != controller || !c->isCreature()) continue;
            if (c->tapped || c->summoningSickness) continue;
            if (c->rules->manaCost.colorIdentity() & spellColor) {
                tapped.push_back(c);
                if (tapped.size() == 2) break;
            }
        }
        if (tapped.size() == 2) {
            for (Card* c : tapped) { c->tapped = true; }
            conspired = true;
        }
    }

    // Bestow: if cast for bestow cost (different from normal cast — handled before
    // the card reached the stack), mark as bestowed so it becomes an Aura on ETB.
    // Actual bestow detection: if the card is an enchantment-creature and the cast
    // cost matches bestow cost, the card should attach to a target creature.
    // (Full implementation requires a pre-cast choice; for now AI always bestows if viable.)
    bool bestowed = false;
    if (rules->hasBestow && !legalTargets.empty()) {
        const Target& tgt = legalTargets[0];
        const Card* tgtCard = m_game.findCard(tgt.cardId);
        if (tgtCard && tgtCard->isCreature() && tgtCard->isOnBattlefield())
            bestowed = true;
    }

    // Optional Kicker cost — pay greedily if the card has a Kicker keyword and mana allows
    bool kicked = false;
    if (rules->hasKicker) {
        ManaPool& pool = m_game.player(controller).manaPool();
        if (pool.total() >= rules->kickerCost.cmc()) {
            if (payCost(rules->kickerCost, controller))
                kicked = true;
        }
    }

    // Optional Buyback cost — pay greedily if the card has Buyback and mana allows
    bool buyback = false;
    if (rules->hasBuyback) {
        ManaPool& pool = m_game.player(controller).manaPool();
        if (pool.total() >= rules->buybackCost.cmc()) {
            if (payCost(rules->buybackCost, controller))
                buyback = true;
        }
    }

    StackAbility ability;
    ability.sourceCardId = onStack->id;
    ability.controllerId = controller;
    ability.targets      = std::move(legalTargets);
    ability.xValue       = xVal;
    ability.flashback      = isFlashback;
    ability.kicked         = kicked;
    ability.bargained      = bargained;
    ability.replicated     = replicated;
    ability.conspired      = conspired;
    ability.bestowed       = bestowed;
    ability.buyback        = buyback;
    ability.evoke          = evoke;
    ability.dashed         = dashed;
    ability.blitzed        = blitzed;
    ability.morphed        = morphed;
    ability.unearthed      = isUnearth;
    ability.prototyped     = isPrototype;
    ability.overloaded     = overloaded;
    ability.castViForetell = isForetell;
    ability.escape         = isEscape;
    ability.jumpStart      = isJumpStart;
    ability.aftermath      = isAftermath;
    // Rebound: exile instead of GY when cast from hand (first cast only; isRebound = second cast)
    ability.rebound        = !isRebound && rules->hasRebound && !rules->isPermanent();

    for (const auto& rawLine : rules->abilityLines) {
        auto script = parseScriptLine(rawLine);
        if (script.abilityType == "SP" || script.abilityType == "DB") {
            if (ability.script.empty()) {
                ability.script = std::move(script);
            } else {
                ability.subEffects.push_back(std::move(script));
            }
        }
    }

    // Count this spell for Storm and ActivatorThisTurnCast$ purposes
    ++m_game.spellsCastThisTurn;
    ++m_game.spellsCastByPlayer[controller];

    // Extort: for each permanent the caster controls with Extort, if they have ≥1 mana,
    // pay it and drain each opponent for 1 life (caster gains that much life).
    {
        ManaPool& pool = m_game.player(controller).manaPool();
        for (const Card* c : m_game.battlefield().cards()) {
            if (c->controllerId != controller || !c->rules->hasExtort) continue;
            if (pool.total() < 1) break;
            pool.addGeneric(-1); // pay {W/B} simplified as any 1 mana
            uint8_t opp = controller ^ 1;
            m_game.loseLife(opp, 1);
            m_game.playerDamagedThisTurn[opp] = true;
            m_game.gainLife(controller, 1);
        }
    }

    // Process Cascade: reveals cards until finding a cheaper non-land, casts it free
    if (rules->hasCascade)
        processCascade(rules->cmc() - 1, controller);

    m_stack.push_back(std::move(ability));

    // Fire SpellCast triggers ("whenever you cast a spell…")
    {
        const Card* onStackNow = m_game.findCard(m_stack.back().sourceCardId);
        if (onStackNow) {
            std::vector<PendingTrigger> triggered;
            const auto& stackedTargets = m_stack.back().targets;
            const std::vector<Target>* targetsPtr =
                stackedTargets.empty() ? nullptr : &stackedTargets;
            TriggerSystem::onSpellCast(*onStackNow, controller, m_game, triggered, targetsPtr);
            if (!triggered.empty()) {
                m_game.queueTriggers(std::move(triggered));
                drainPendingTriggers();
            }
        }
    }

    // Fire BecomesTarget triggers for each card targeted by this spell
    {
        ObjectId spellId = m_stack.back().sourceCardId;
        for (const auto& tgt : m_stack.back().targets) {
            if (!tgt.isCard()) continue;
            std::vector<PendingTrigger> triggered;
            TriggerSystem::onBecomesTarget(tgt.cardId, spellId, true, controller,
                                            m_game, triggered);
            if (!triggered.empty()) {
                m_game.queueTriggers(std::move(triggered));
                drainPendingTriggers();
            }
        }
    }
    return true;
}

// ── Stack resolution ──────────────────────────────────────────────────────────

void AbilityProcessor::redirectSpellTargets(StackAbility& ability, uint8_t redirector) {
    uint8_t opp = redirector ^ 1;  // the player the redirector wants the spell aimed at
    auto validTgts = std::string(ability.script.get("ValidTgts", ""));
    Card* source = m_game.findCard(ability.sourceCardId);
    for (Target& t : ability.targets) {
        if (t.isPlayer()) {
            t = Target::forPlayer(opp);
        } else if (t.isCard()) {
            // Pick the opponent's most valuable permanent matching the spell's
            // ValidTgts (sending a harmful spell at their best creature).
            Card* best = nullptr; int bestScore = -1;
            for (Card* c : m_game.battlefield().cards()) {
                if (c->controllerId != opp) continue;
                if (!validTgts.empty() &&
                    !cardMatchesAnyFilter(*c, validTgts, redirector, kInvalidId, source, &m_game))
                    continue;
                int score = effectivePower(*c) + effectiveToughness(*c);
                if (score > bestScore) { bestScore = score; best = c; }
            }
            if (best) t = Target::forCard(best->id);
            // else: no legal new target — leave the original (may fizzle naturally)
        }
    }
}

void AbilityProcessor::resolveTop() {
    if (m_stack.empty()) return;

    StackAbility ability = std::move(m_stack.back());
    m_stack.pop_back();

    Card* source = m_game.findCard(ability.sourceCardId);

    // Storm/replicate copies have no physical source card — resolve effects directly.
    if (ability.isCopy) {
        EffectContext ctx{m_game, nullptr, ability.controllerId, ability.targets, ability.xValue};
        if (!ability.script.empty()) executeEffectChain(ability.script, ctx);
        drainPendingTriggers();
        return;
    }

    // If a spell's source card was removed from the stack (e.g. countered by another
    // spell), the StackAbility lingering in m_stack should fizzle — pop without effect.
    // (Activated abilities keep the source permanent alive, so they always have source.)
    if (!ability.isActivatedAbility &&
        ability.sourceCardId != kInvalidId && !source) {
        drainPendingTriggers();
        return;
    }

    // Overload: replace any targets with ALL cards matching the spell's ValidTgts filter.
    if (ability.overloaded) {
        ability.targets.clear();
        auto validTgts = std::string(ability.script.get("ValidTgts", ""));
        if (!validTgts.empty()) {
            for (Card* c : m_game.battlefield().cards()) {
                if (cardMatchesAnyFilter(*c, validTgts, ability.controllerId, kInvalidId, source, &m_game))
                    ability.targets.push_back(Target::forCard(c->id));
            }
        }
    }

    // ControlSpell (Aethersnatch, Commandeer): a prior spell changed who controls
    // this one. Applied before resolution so a permanent enters under the new
    // controller and the effect benefits them.
    if (!m_game.pendingControlChange.empty()) {
        auto it = m_game.pendingControlChange.find(ability.sourceCardId);
        if (it != m_game.pendingControlChange.end()) {
            ability.controllerId = it->second;
            m_game.pendingControlChange.erase(it);
        }
    }

    // ChangeTargets (Deflection, Divert, Bolt Bend …): a prior spell redirected this
    // one's targets toward the redirector's opponent.
    if (!m_game.pendingRetarget.empty()) {
        auto it = m_game.pendingRetarget.find(ability.sourceCardId);
        if (it != m_game.pendingRetarget.end()) {
            uint8_t redirector = it->second;
            m_game.pendingRetarget.erase(it);
            redirectSpellTargets(ability, redirector);
        }
    }

    // For spells where X comes from a Count$ SVar (not mana-paid), evaluate it now.
    int xVal = ability.xValue;
    if (xVal == 0 && source) {
        int svarX = m_game.evaluateSVar("X", ability.controllerId, source->rules, source->id);
        if (svarX > 0) xVal = svarX;
    }

    // Expose xVal and kicked flag so Count$xPaid / Count$Kicked evaluations can read them.
    m_game.etbXHint  = xVal;
    m_game.kickedHint = ability.kicked;

    // Stack fizzle (rule 608.2b): if ALL targets are illegal at resolution,
    // the spell/ability is countered by the rules without effect.
    // "Illegal" means: the card no longer exists, or the target has protection/shroud,
    // or it left the zone it was targeted in.
    // Only fizzle when the spell required at least one target AND every one is illegal.
    if (!ability.isActivatedAbility && !ability.targets.empty()) {
        bool hasLegalTarget = false;
        for (const Target& t : ability.targets) {
            if (t.isPlayer()) { hasLegalTarget = true; break; }
            if (t.isCard()) {
                const Card* tc = m_game.findCard(t.cardId);
                if (tc && tc->isOnBattlefield()) { hasLegalTarget = true; break; }
                if (tc && tc->zone == ZoneType::Graveyard) { hasLegalTarget = true; break; }
                // Spells targeting an object on the stack (Counter, ChangeTargets,
                // ControlSpell) — a stack object is a legal target.
                if (tc && tc->zone == ZoneType::Stack) { hasLegalTarget = true; break; }
            }
        }
        if (!hasLegalTarget) {
            // Move source card to graveyard (or exile for flashback, etc.) and return
            if (source && !ability.isActivatedAbility) {
                bool exileAfter = ability.flashback || ability.escape ||
                                  ability.jumpStart || ability.aftermath;
                ZoneType fizzleDest = (source->isPermanent() || exileAfter)
                                      ? ZoneType::Exile : ZoneType::Graveyard;
                m_game.moveToZone(source->id, fizzleDest, source->ownerId);
            }
            drainPendingTriggers();
            return;
        }
    }

    // Snapshot the first target's controller BEFORE effects run (targets may move zones)
    uint8_t firstTargetCtrl = 255;
    if (!ability.targets.empty() && ability.targets[0].isCard()) {
        const Card* tc = m_game.findCard(ability.targets[0].cardId);
        if (tc) firstTargetCtrl = tc->controllerId;
    }

    EffectContext ctx{
        m_game, source, ability.controllerId, ability.targets,
        xVal,
        /*triggeredCardId=*/kInvalidId,
        /*triggerPlayer=*/static_cast<uint8_t>(255),
        /*triggerAmount=*/0,
        ability.kicked,
        ability.buyback,
        firstTargetCtrl
    };

    // Execute the main effect (with SubAbility$ chain)
    if (!ability.script.empty())
        executeEffectChain(ability.script, ctx);

    // Execute any explicitly listed sub-effects (DB$ lines on the same card)
    for (const auto& sub : ability.subEffects)
        executeEffect(sub, ctx);

    // Storm: push N independent stack copies (one per spell cast before this one this turn).
    // Each copy is a separate StackAbility so they can be responded to and countered
    // individually (rule 702.40c). Copies resolve from top-to-bottom (LIFO).
    if (source && source->rules->hasKeyword("Storm") && !ability.script.empty()) {
        int copies = std::max(0, m_game.spellsCastThisTurn - 1);
        for (int i = 0; i < copies; ++i) {
            StackAbility copy;
            copy.sourceCardId       = ability.sourceCardId;  // still references the source
            copy.controllerId       = ability.controllerId;
            copy.targets            = ability.targets;       // same targets as original
            copy.script             = ability.script;
            copy.xValue             = ability.xValue;
            copy.kicked             = ability.kicked;
            copy.isActivatedAbility = false;
            copy.isCopy             = true;
            m_stack.push_back(std::move(copy));
        }
    }

    // Replicate / Conspire: create one copy of the spell's effect chain
    if ((ability.replicated || ability.conspired) && !ability.script.empty()) {
        executeEffectChain(ability.script, ctx);
    }

    // Cipher: after a cipher spell resolves, exile it encoded onto the best creature.
    // The encoded card is marked with cipheredOnto; when that creature deals combat
    // damage to a player, onDamageDone fires an attempt to cast the encoded spell.
    if (source && source->rules->hasCipher && !ability.isActivatedAbility) {
        // Find best attacking/biggest creature owned by this controller to encode onto
        Card* encodeTarget = nullptr;
        int   bestPow = -1;
        for (Card* c : m_game.battlefield().cards()) {
            if (c->controllerId != ability.controllerId || !c->isCreature()) continue;
            int p = effectivePower(*c);
            if (p > bestPow) { bestPow = p; encodeTarget = c; }
        }
        if (encodeTarget) {
            // Exile the spell and mark the target creature as hosting the cipher
            ObjectId srcId = source->id;
            Card* exiled = m_game.moveToZone(srcId, ZoneType::Exile, ability.controllerId);
            if (exiled) exiled->cipheredOnto = encodeTarget->id;
            source = nullptr;  // invalidated by moveToZone
        }
    }

    // Move the source card to its destination after resolution.
    // Activated abilities and triggered abilities leave the source card in place.
    ObjectId enteredBattlefieldId = kInvalidId;
    // Capture Epic flag BEFORE moveToZone — source becomes dangling after the zone change.
    bool sourceHasEpic = source && source->rules && source->rules->hasEpic;

    if (source && !ability.isActivatedAbility) {
        bool     isPermanent    = source->isPermanent();
        ObjectId srcId          = source->id;
        uint8_t  ownerId        = source->ownerId;
        // Escape/Flashback/Jump-start spells are exiled after resolving.
        // Adventure spells exile themselves so the creature face can be cast later
        bool castViaAdventure = (!isPermanent && source->rules->hasAdventure);
        bool castViaAftermath = ability.aftermath;
        ZoneType dest           = isPermanent            ? ZoneType::Battlefield
                                : ability.buyback        ? ZoneType::Hand
                                : ability.flashback      ? ZoneType::Exile
                                : ability.escape         ? ZoneType::Exile
                                : ability.jumpStart      ? ZoneType::Exile
                                : ability.rebound        ? ZoneType::Exile
                                : castViaAdventure       ? ZoneType::Exile   // adventure → exile for creature cast
                                : castViaAftermath       ? ZoneType::Exile   // aftermath → exile after resolving
                                :                          ZoneType::Graveyard;
        uint8_t  destController = isPermanent ? ability.controllerId : ownerId;
        Card* entered = m_game.moveToZone(srcId, dest, destController);
        // Mark the exiled card as a Rebound cast (enables free upkeep cast)
        if (ability.rebound && entered && dest == ZoneType::Exile)
            entered->rebound = true;
        // Mark adventure-exiled cards so the creature face can be cast
        if (castViaAdventure && entered && dest == ZoneType::Exile)
            entered->adventureExiled = true;
        // Mark the entered permanent as having escaped (suppresses self-sacrifice triggers)
        if (ability.escape && entered && dest == ZoneType::Battlefield)
            entered->escaped = true;
        if (entered && dest == ZoneType::Battlefield) {
            enteredBattlefieldId = entered->id;

            // Aura: attach to the spell's first card target when entering the battlefield.
            // recomputeStaticBonuses() is called after attachment so the Aura's
            // continuous effects (EnchantedCard bonuses) apply immediately.
            if (entered->rules->type.isEnchantment() &&
                entered->rules->type.hasSubtype("Aura") &&
                !ability.targets.empty() && ability.targets[0].isCard()) {
                Card* enchTarget = m_game.findCard(ability.targets[0].cardId);
                if (enchTarget && enchTarget->isOnBattlefield()) {
                    entered->attachedTo = enchTarget->id;
                    enchTarget->attachments.push_back(entered->id);
                    m_game.recomputeStaticBonuses();
                }
            }
        }
    }

    // Remove any StackAbilities whose source spell was countered during this resolution
    // (effectCounter moves the source card to GY, leaving the StackAbility orphaned).
    m_stack.erase(
        std::remove_if(m_stack.begin(), m_stack.end(),
            [&](const StackAbility& a) {
                return !a.isActivatedAbility
                    && a.sourceCardId != kInvalidId
                    && m_game.findCard(a.sourceCardId) == nullptr;
            }),
        m_stack.end());

    // Clear the ETB X hint and kicked flag after resolution to avoid stale values leaking.
    m_game.etbXHint   = 0;
    m_game.kickedHint = false;

    // Drain any triggers that fired during this resolution (ETB etc.)
    drainPendingTriggers();

    // Epic: set restriction flag when an Epic spell resolves.
    // Use sourceHasEpic (captured before moveToZone) — source is dangling after zone change.
    if (sourceHasEpic && !ability.isCopy)
        m_game.epicRestriction[ability.controllerId] = true;

    // Suspend haste: creatures cast via Suspend gain Haste until they leave the battlefield.
    if (ability.isSuspendHaste && enteredBattlefieldId != kInvalidId) {
        Card* sc = m_game.findCard(enteredBattlefieldId);
        if (sc && sc->isCreature()) {
            sc->grantKeyword(KeywordAbility::Haste);
            sc->tempKeywords |= static_cast<uint32_t>(KeywordAbility::Haste);
        }
    }

    // Prototype: override the card's power/toughness with the prototype values.
    if (ability.prototyped && enteredBattlefieldId != kInvalidId) {
        Card* pc = m_game.findCard(enteredBattlefieldId);
        if (pc && pc->isOnBattlefield() && pc->rules->hasPrototype) {
            auto parseStaticStat = [](const std::string& s) -> int {
                int v = 0;
                std::from_chars(s.data(), s.data() + s.size(), v);
                return v;
            };
            if (!pc->rules->prototypePower.empty())
                pc->basePowerOverride = parseStaticStat(pc->rules->prototypePower);
            if (!pc->rules->prototypeToughness.empty())
                pc->baseToughOverride = parseStaticStat(pc->rules->prototypeToughness);
        }
    }

    // Evoke: sacrifice the creature after its ETB triggers have resolved.
    if (ability.evoke && enteredBattlefieldId != kInvalidId) {
        Card* toSac = m_game.findCard(enteredBattlefieldId);
        if (toSac && toSac->isOnBattlefield()) {
            m_game.moveToZone(enteredBattlefieldId, ZoneType::Graveyard, toSac->ownerId);
            while (StateBasedActions::run(m_game)) {}
            drainPendingTriggers();
        }
    }

    // Dash: grant Haste and queue a return-to-hand at the next end step.
    if (ability.dashed && enteredBattlefieldId != kInvalidId) {
        Card* dCard = m_game.findCard(enteredBattlefieldId);
        if (dCard && dCard->isOnBattlefield()) {
            auto haste = static_cast<uint32_t>(KeywordAbility::Haste);
            dCard->tempKeywords |= haste;
            dCard->keywordMask  |= haste;
            m_game.dashedCards.push_back(enteredBattlefieldId);
        }
    }

    // Unearth: grant Haste and queue exile at the next end step.
    if (ability.unearthed && enteredBattlefieldId != kInvalidId) {
        Card* uCard = m_game.findCard(enteredBattlefieldId);
        if (uCard && uCard->isOnBattlefield()) {
            auto haste = static_cast<uint32_t>(KeywordAbility::Haste);
            uCard->tempKeywords      |= haste;
            uCard->keywordMask       |= haste;
            uCard->summoningSickness  = false;
            m_game.unearthedCards.push_back(enteredBattlefieldId);
        }
    }

    // Blitz: grant Haste and queue sacrifice at the next end step.
    // The card's own T:Mode$ Dies trigger handles drawing a card on death.
    if (ability.blitzed && enteredBattlefieldId != kInvalidId) {
        Card* bCard = m_game.findCard(enteredBattlefieldId);
        if (bCard && bCard->isOnBattlefield()) {
            auto haste = static_cast<uint32_t>(KeywordAbility::Haste);
            bCard->tempKeywords      |= haste;
            bCard->keywordMask       |= haste;
            bCard->summoningSickness  = false;
            m_game.blitzedCards.push_back(enteredBattlefieldId);
        }
    }

    // Connive: draw N, discard N; each nonland discarded → +1/+1 counter on this creature.
    if (enteredBattlefieldId != kInvalidId) {
        Card* cCard = m_game.findCard(enteredBattlefieldId);
        if (cCard && cCard->isOnBattlefield() && cCard->rules->hasConnive) {
            uint8_t ctrl = ability.controllerId;
            int n = cCard->rules->conniveAmount;
            // Draw N
            Player& cp = m_game.player(ctrl);
            for (int i = 0; i < n && !cp.library().empty(); ++i) {
                Card* top = cp.library().front();
                m_game.moveToZone(top->id, ZoneType::Hand, ctrl);
                ++m_game.cardsDrawnThisTurn[ctrl];
            }
            // Discard N: human gets pending-connive UI; AI auto-discards worst cards
            if (ctrl == 0 && m_game.isHumanInteractive()) {
                m_game.pendingConnive = { true, enteredBattlefieldId, n, 0, {} };
            } else {
                Player& dp = m_game.player(ctrl);
                for (int i = 0; i < n; ++i) {
                    Card* toDiscard = nullptr;
                    int   bestScore = INT_MAX;
                    for (Card* hc : dp.hand().cards()) {
                        if (hc->id == enteredBattlefieldId) continue;
                        int score = hc->rules->manaCost.cmc();
                        if (hc->rules->type.isLand()) score -= 100;
                        if (score < bestScore) { bestScore = score; toDiscard = hc; }
                    }
                    if (!toDiscard && !dp.hand().empty()) toDiscard = dp.hand().front();
                    if (toDiscard) {
                        bool nonland = !toDiscard->rules->type.isLand();
                        m_game.moveToZone(toDiscard->id, ZoneType::Graveyard, ctrl);
                        drainPendingTriggers();
                        if (nonland) {
                            Card* creature = m_game.findCard(enteredBattlefieldId);
                            if (creature) creature->addCounter("+1/+1", 1);
                        }
                    }
                }
            }
        }
    }

    // Backup N: put N +1/+1 counters on another target creature; that creature also
    // gains the first keyword ability of this card until EOT.
    if (enteredBattlefieldId != kInvalidId) {
        Card* bCard = m_game.findCard(enteredBattlefieldId);
        if (bCard && bCard->isOnBattlefield() && bCard->rules->hasBackup) {
            uint8_t ctrl = ability.controllerId;
            int n = bCard->rules->backupAmount;
            // Pick the best-value own creature that is not this card
            Card* target = nullptr;
            int   bestVal = -1;
            for (Card* c : m_game.battlefield().cards()) {
                if (c->controllerId != ctrl || c->id == enteredBattlefieldId) continue;
                if (!c->isCreature()) continue;
                int val = effectivePower(*c) + effectiveToughness(*c);
                if (val > bestVal) { bestVal = val; target = c; }
            }
            if (target) {
                target->addCounter("+1/+1", n);
                std::vector<PendingTrigger> cnt;
                TriggerSystem::onCounterAdded(*target, "+1/+1", n, m_game, cnt);
                m_game.queueTriggers(std::move(cnt));
                // Grant the first keyword from the backup card until EOT
                if (!bCard->rules->keywords.empty()) {
                    auto kw = parseKeyword(bCard->rules->keywords.front());
                    if (kw != KeywordAbility::None) {
                        target->tempKeywords  |= static_cast<uint32_t>(kw);
                        target->keywordMask   |= static_cast<uint32_t>(kw);
                    }
                }
            }
        }
    }

    // Graft N: enters the battlefield with N +1/+1 counters.
    // (The counter-move trigger when another creature ETBs is handled in TriggerSystem.)
    if (enteredBattlefieldId != kInvalidId) {
        Card* gCard = m_game.findCard(enteredBattlefieldId);
        if (gCard && gCard->isOnBattlefield() && gCard->rules->hasGraft
                  && gCard->rules->graftCount > 0) {
            gCard->addCounter("+1/+1", gCard->rules->graftCount);
        }
    }

    // Sunburst: enters with one +1/+1 counter per colour of mana spent to cast.
    // We approximate the colour count from the card's own colour identity since
    // we don't track which specific mana colours were spent for each individual shard.
    if (enteredBattlefieldId != kInvalidId) {
        Card* sCard = m_game.findCard(enteredBattlefieldId);
        if (sCard && sCard->isOnBattlefield() && sCard->rules->hasSunburst) {
            uint8_t ci = sCard->rules->manaCost.colorIdentity();
            // Count set WUBRG bits (bits 0-4) — portable popcount
            int wubrgBits = ci & 0x1F;
            int cols = 0;
            for (int bit = wubrgBits; bit; bit &= bit - 1) ++cols;
            if (cols > 0) {
                sCard->addCounter("+1/+1", cols);
                // Artifacts with Sunburst use charge counters instead
                if (!sCard->isCreature())
                    sCard->addCounter("charge", cols);
            }
        }
    }

    // Amplify N: as this enters, for each creature card with a shared creature type
    // you reveal from your hand, put N additional +1/+1 counters on this creature.
    if (enteredBattlefieldId != kInvalidId) {
        Card* aCard = m_game.findCard(enteredBattlefieldId);
        if (aCard && aCard->isOnBattlefield() && aCard->rules->hasAmplify
                  && aCard->isCreature()) {
            // Count hand cards with shared creature subtypes (simplified: count creatures)
            int revealed = 0;
            for (const Card* hc : m_game.player(ability.controllerId).hand().cards()) {
                if (!hc->rules->isCreature()) continue;
                // Check if any creature subtype overlaps
                for (const auto& sub : aCard->rules->type.subtypes) {
                    if (hc->rules->type.hasSubtype(sub)) { ++revealed; break; }
                }
            }
            if (revealed > 0)
                aCard->addCounter("+1/+1", revealed * aCard->rules->amplifyAmount);
        }
    }

    // Fading N: enters with N fading counters; handled by TurnManager upkeep.
    if (enteredBattlefieldId != kInvalidId) {
        Card* fCard = m_game.findCard(enteredBattlefieldId);
        if (fCard && fCard->isOnBattlefield() && fCard->rules->hasFading
                  && fCard->rules->fadingAmount > 0) {
            fCard->addCounter("fading", fCard->rules->fadingAmount);
        }
    }

    // Vanishing N: enters with N time counters; handled by TurnManager upkeep.
    if (enteredBattlefieldId != kInvalidId) {
        Card* vCard = m_game.findCard(enteredBattlefieldId);
        if (vCard && vCard->isOnBattlefield() && vCard->rules->hasVanishing
                  && vCard->rules->vanishingAmount > 0) {
            vCard->addCounter("time", vCard->rules->vanishingAmount);
        }
    }

    // Manifest: entering the battlefield face-down means we need to mark it as a 2/2.
    // (Actual manifest cast happens via a separate activateManaAbility path when the
    // player pays the card's mana cost to flip it face-up.)
    if (ability.morphed && enteredBattlefieldId != kInvalidId) {
        Card* mfCard = m_game.findCard(enteredBattlefieldId);
        if (mfCard && mfCard->isOnBattlefield() && mfCard->rules->hasManifest) {
            mfCard->isFaceDown = true;
            mfCard->keywordMask = 0;  // face-down has no text
        }
    }

    // Morph: enter the battlefield face-down as a 2/2 with no types or text visible.
    // Clear the keywordMask (face-down morphs have no keyword abilities).
    if (ability.morphed && enteredBattlefieldId != kInvalidId) {
        Card* mCard = m_game.findCard(enteredBattlefieldId);
        if (mCard && mCard->isOnBattlefield()) {
            mCard->isFaceDown  = true;
            mCard->keywordMask = 0;
        }
    }
}

void AbilityProcessor::resolveHumanTrigger(const std::vector<Target>& targets) {
    if (m_humanTriggers.empty()) return;

    PendingTrigger trig = std::move(m_humanTriggers.front());
    m_humanTriggers.pop_front();

    Card* src = m_game.findCard(trig.sourceCardId);
    int trigXVal = 0;
    if (src) {
        int sv = m_game.evaluateSVar("X", trig.controllerId, src->rules, src->id);
        if (sv > 0) trigXVal = sv;
    }

    EffectContext ctx{
        m_game, src, trig.controllerId, targets, trigXVal,
        trig.triggeredCardId
    };
    executeEffectChain(trig.effect, ctx);
    while (StateBasedActions::run(m_game)) {}
    // Drain any new auto-resolvable triggers created during this resolution
    drainPendingTriggers();
}

void AbilityProcessor::processCascade(int maxCmc, uint8_t controller) {
    Player& p = m_game.player(controller);
    std::vector<Card*> revealed;
    Card* target = nullptr;

    // Reveal cards from the top until finding a non-land with CMC ≤ maxCmc
    while (!p.library().empty()) {
        Card* top = p.library().front();
        p.library().remove(top->id);
        if (!top->isLand() && top->rules->cmc() > 0 && top->rules->cmc() <= maxCmc) {
            target = top;
            break;
        }
        revealed.push_back(top);
    }

    // Put revealed-but-not-chosen cards on the bottom
    for (Card* c : revealed)
        p.library().addToBack(c);

    if (!target) return;

    // Human-interactive: give player the choice to cast or not (cascade is "may").
    // AI always casts. Human sees a charm-style overlay via setPendingCascade.
    if (controller == 0 && m_game.isHumanInteractive()) {
        m_game.setPendingCascade(target->id, controller, maxCmc);
        // Revealed cards stay on bottom; cascade resolves when the UI confirms.
        for (Card* c : revealed)
            p.library().addToBack(c);
        return;
    }
    for (Card* c : revealed)
        p.library().addToBack(c);

    // Cast the cascade target for free — push it onto the stack without paying mana
    Card* onStack = m_game.moveToZone(target->id, ZoneType::Stack, controller);
    if (!onStack) return;

    ++m_game.spellsCastThisTurn;
    ++m_game.spellsCastByPlayer[controller];

    // Auto-pick targets using the same AI heuristic used for normal spells
    std::vector<Target> targets;
    for (const auto& raw : onStack->rules->abilityLines) {
        auto s = parseScriptLine(raw);
        if (s.abilityType != "SP" && s.abilityType != "DB") continue;
        auto validTgts = std::string(s.get("ValidTgts", ""));
        if (!validTgts.empty()) {
            for (const Card* c : m_game.battlefield().cards()) {
                if (!cardMatchesAnyFilter(*c, validTgts, controller,
                                          onStack->id, onStack, &m_game)) continue;
                if (c->cantBeTargeted) continue;
                if (c->hasKeyword(KeywordAbility::Shroud)) continue;
                if (c->hasKeyword(KeywordAbility::Hexproof) && c->controllerId == controller) continue;
                targets = { Target::forCard(c->id) };
                break;
            }
            if (targets.empty() && validTgts.find("Player") != std::string::npos)
                targets = { Target::forPlayer(static_cast<uint8_t>(controller ^ 1)) };
        }
        break;
    }

    StackAbility ability;
    ability.sourceCardId = onStack->id;
    ability.controllerId = controller;
    ability.targets      = std::move(targets);
    for (const auto& raw : onStack->rules->abilityLines) {
        auto s = parseScriptLine(raw);
        if (s.abilityType == "SP" || s.abilityType == "DB") {
            ability.script = std::move(s); break;
        }
    }
    m_stack.push_back(std::move(ability));

    // Fire SpellCast triggers for the cascaded spell
    const Card* now = m_game.findCard(m_stack.back().sourceCardId);
    if (now) {
        std::vector<PendingTrigger> cascadeTrig;
        TriggerSystem::onSpellCast(*now, controller, m_game, cascadeTrig);
        if (!cascadeTrig.empty()) {
            m_game.queueTriggers(std::move(cascadeTrig));
            drainPendingTriggers();
        }
        // Cascade-into-cascade (rule 702.84): if the cascaded spell also has Cascade,
        // it was just "cast" for free, so its own cascade trigger fires immediately.
        if (now->rules->hasCascade)
            processCascade(now->rules->cmc() - 1, controller);
    }
}

void AbilityProcessor::firePhaseTriggersAndDrain(TurnStep step, uint8_t activePlayer) {
    std::vector<PendingTrigger> triggered;
    TriggerSystem::onPhaseBegin(step, activePlayer, m_game, triggered);
    if (!triggered.empty()) {
        m_game.queueTriggers(std::move(triggered));
        drainPendingTriggers();
    }
}

void AbilityProcessor::castMadnessCard(ObjectId cardId, uint8_t ctrl) {
    Card* c = m_game.findCard(cardId);
    if (!c || c->zone != ZoneType::Exile) return;
    const CardRules* rules = c->rules;

    bool casted = false;
    if (rules->madnessCost.isNoCost() ||
        (m_game.player(ctrl).manaPool().total() >= rules->madnessCost.cmc() &&
         payCost(rules->madnessCost, ctrl))) {
        Card* onStack = m_game.moveToZone(c->id, ZoneType::Stack, ctrl);
        if (onStack) {
            StackAbility ability;
            ability.sourceCardId = onStack->id;
            ability.controllerId = ctrl;
            for (const auto& raw : rules->abilityLines) {
                auto s = parseScriptLine(raw);
                if (s.abilityType == "SP" || s.abilityType == "DB") {
                    ability.script = std::move(s); break;
                }
            }
            m_stack.push_back(std::move(ability));
            resolveTop();
            casted = true;
        }
    }

    if (!casted && m_game.findCard(cardId))
        m_game.moveToZone(cardId, ZoneType::Graveyard, ctrl);
}

void AbilityProcessor::drainPendingTriggers() {
    // Recursion guard: prevents stack overflow when complex trigger chains (madness,
    // ability loops) call back into drainPendingTriggers via resolveTop.
    if (m_triggerDepth >= 4) return;
    ++m_triggerDepth;
    struct Guard { int& d; ~Guard() { --d; } } guard{m_triggerDepth};

    // Madness: cards in exile that were discarded; cast for madness cost or put in GY.
    // Human player (ctrl 0) in interactive mode defers to the UI overlay.
    if (m_game.hasPendingMadnessCasts()) {
        auto casts = m_game.drainMadnessCasts();
        for (size_t i = 0; i < casts.size(); ++i) {
            auto [cardId, ctrl] = casts[i];
            if (ctrl == 0 && m_game.isHumanInteractive()) {
                // Defer to GameWindow overlay; re-queue any remaining items.
                Card* c = m_game.findCard(cardId);
                std::string name = (c && c->rules) ? c->rules->name : "card";
                m_game.setPendingMadnessCast(cardId, ctrl, name);
                for (size_t j = i + 1; j < casts.size(); ++j)
                    m_game.queueMadnessCast(casts[j].first, casts[j].second);
                return; // GameWindow calls drainPendingTriggers again after choice
            }
            castMadnessCard(cardId, ctrl);
        }
    }

    auto pending = m_game.drainTriggers();
    if (pending.empty()) return;

    // APNAP ordering (rule 603.3b): active player's triggers go on the stack first
    // (ending up lower, resolving last); non-active players follow in turn order.
    // A stable sort by seat distance from active player achieves this.
    {
        uint8_t ap = m_game.activePlayerId();
        uint8_t n  = m_game.numPlayers();
        std::stable_sort(pending.begin(), pending.end(),
            [ap, n](const PendingTrigger& a, const PendingTrigger& b) {
                auto dist = [ap, n](uint8_t pid) -> uint8_t {
                    return static_cast<uint8_t>((pid + n - ap) % n);
                };
                return dist(a.controllerId) < dist(b.controllerId);
            });
    }

    for (auto& trig : pending) {
        Card* src = m_game.findCard(trig.sourceCardId);

        std::vector<Target> autoTargets;
        auto validTgts = std::string(trig.effect.get("ValidTgts", ""));
        auto defined   = trig.effect.get("Defined", "");

        // Triggers owned by player 0 that require target selection → queue for human
        // unless no legal targets exist (rule 603.4: trigger is removed from stack)
        // In non-interactive mode (self-play), auto-resolve rather than deferring.
        if (m_game.isHumanInteractive() && trig.controllerId == 0 && !validTgts.empty()) {
            bool hasLegalTarget = (validTgts == "Any" ||
                                   validTgts.find("Player") != std::string::npos);
            if (!hasLegalTarget) {
                for (const Card* c : m_game.battlefield().cards()) {
                    if (!cardMatchesAnyFilter(*c, validTgts, trig.controllerId,
                                              trig.sourceCardId, src, &m_game)) continue;
                    if (c->cantBeTargeted) continue;
                    if (c->hasKeyword(KeywordAbility::Shroud)) continue;
                    if (c->hasKeyword(KeywordAbility::Hexproof) &&
                        c->controllerId == trig.controllerId) continue;
                    hasLegalTarget = true;
                    break;
                }
            }
            if (hasLegalTarget) {
                // Log the trigger so the human gets a toast notification
                if (src && src->rules)
                    std::cout << "[trigger] " << src->rules->name << " fires (choose target)\n";
                m_humanTriggers.push_back(std::move(trig));
                continue;
            }
            // No legal targets — trigger fizzles; fall through to execute with empty targets
        }
        if (!validTgts.empty()) {
            // Pick best matching card (prefer opponents', then any)
            Card* best = nullptr;
            int   bestScore = -1;
            for (Card* c : m_game.battlefield().cards()) {
                if (!cardMatchesAnyFilter(*c, validTgts, trig.controllerId,
                                          trig.sourceCardId, src, &m_game)) continue;
                if (c->cantBeTargeted) continue;
                if (c->hasKeyword(KeywordAbility::Shroud)) continue;
                if (c->hasKeyword(KeywordAbility::Hexproof) &&
                    c->controllerId == trig.controllerId) continue;
                int score = c->isCreature()
                            ? effectivePower(*c) + effectiveToughness(*c) : 1;
                if (c->controllerId != trig.controllerId) score += 100; // prefer opponent
                if (score > bestScore) { bestScore = score; best = c; }
            }
            if (best) autoTargets = { Target::forCard(best->id) };
        } else if (defined == "TriggeredCard" || defined == "TriggeredCardLKI") {
            if (trig.triggeredCardId != kInvalidId)
                autoTargets = { Target::forCard(trig.triggeredCardId) };
        }

        // Evaluate SVar X if the trigger source uses a Count$ or TriggerCount$ expression
        int trigXVal = 0;
        if (src && src->rules) {  // guard: rules may be null for malformed cards
            int sv = m_game.evaluateSVar("X", trig.controllerId, src->rules, src->id);
            if (sv > 0) trigXVal = sv;
            // TriggerCount$DamageAmount — use the stored trigger amount
            if (trigXVal == 0 && trig.triggerAmount > 0) {
                auto it = src->rules->svars.find("X");
                if (it != src->rules->svars.end() &&
                    it->second.find("TriggerCount$") != std::string::npos)
                    trigXVal = trig.triggerAmount;
            }
        }

        EffectContext ctx{
            m_game, src, trig.controllerId, autoTargets, trigXVal,
            trig.triggeredCardId
        };
        ctx.triggerPlayer = trig.triggerPlayer;
        ctx.triggerAmount = trig.triggerAmount;
        m_game.triggerAmountHint = trig.triggerAmount;
        executeEffectChain(trig.effect, ctx);  // follows SubAbility$ chain
        m_game.triggerAmountHint = 0;
        // Run basic SBAs (creature death, player loss, etc.) after each trigger.
        // Intentionally NOT checkAlwaysTriggers here: drainTriggers() above cleared
        // pending status for all cards, so checkAlwaysTriggers would re-queue every
        // Always-trigger card on each pass, causing O(M) accumulation per resolution.
        while (StateBasedActions::runBasic(m_game)) {}
        // Stop processing triggers once the game is over — subsequent effects would
        // access game objects in a post-loss/post-win state, causing use-after-free.
        if (m_game.player(0).hasLost() || m_game.player(1).hasLost()) break;
    }
}

bool AbilityProcessor::activateCompanion(uint8_t pid) {
    if (pid >= 4) return false;
    ObjectId cid = m_game.companionId[pid];
    if (cid == kInvalidId || m_game.companionUsed[pid]) return false;
    Card* comp = m_game.findCard(cid);
    if (!comp) return false;
    ManaCost cost = ManaCost::parse("3");
    if (!payCost(cost, pid)) return false;           // pay {3} from pool
    m_game.moveToZone(cid, ZoneType::Hand, pid);     // companion enters hand
    m_game.companionUsed[pid] = true;                // once per game
    return true;
}

// ── Private helpers ───────────────────────────────────────────────────────────

bool AbilityProcessor::payCost(const ManaCost& cost, uint8_t controller) {
    return payCost(cost, controller, nullptr, false, false);
}

bool AbilityProcessor::payCost(const ManaCost& cost, uint8_t controller,
                               const Card* payee, bool isSpell, bool isActivated) {
    if (cost.isNoCost()) return true;
    ManaPool& pool = m_game.player(controller).manaPool();

    // Handle Phyrexian mana shards: pay 2 life OR the colored pip.
    // For the human player in interactive mode, the UI sets pendingPhyrexian to let
    // them choose. We read lifePayCount from the pending state if set.
    for (const auto& shard : cost.shards()) {
        if (!shard.isPhyrexian()) continue;
        bool payLife = false;
        if (controller == 0 && m_game.isHumanInteractive()) {
            // Human choice: if we can afford the pip, prefer pool; otherwise life.
            // The UI overlay (GameWindow) sets hasPendingPhyrexian and the human
            // clicks to resolve. For now we auto-pay the cheapest option and let
            // the overlay override if needed.
            payLife = (pool.available(shard) == 0);
        } else {
            payLife = (pool.available(shard) == 0);
        }
        if (!payLife) {
            pool.add(shard, -1);
        } else if (m_game.player(controller).life() > 2) {
            m_game.loseLife(controller, 2);
        } else {
            return false;
        }
    }

    // Restricted mana (Secluded Courtyard, Cavern of Souls…) is spendable only
    // when the thing being paid for satisfies its RestrictValid clause. With no
    // payee (alternative/additional costs, abilities that aren't creature-typed)
    // the predicate is false, so restricted mana stays untouched.
    ManaUsePredicate canUseRestricted = [&](const RestrictedMana& rm) {
        return payee && manaRestrictionAllows(rm.restriction, *payee, isSpell,
                                              isActivated, controller,
                                              m_game.findCard(rm.producerId), &m_game);
    };
    if (!pool.canPay(cost, canUseRestricted)) return false;
    pool.pay(cost, canUseRestricted);
    return true;
}

bool AbilityProcessor::payActivationCost(const std::string& costStr,
                                          Card& source, uint8_t controller) {
    // Parse cost tokens: "T", "Sac<...>", or mana cost tokens
    bool needsTap     = false;
    bool needsSac     = false;
    bool needsSacSelf = false;   // sacrifice the source itself (Treasure, Clue, Food, Blood…)
    std::string manaCostTokens;

    std::string_view remaining = costStr;
    while (!remaining.empty()) {
        auto space = remaining.find(' ');
        std::string_view token = (space == std::string_view::npos)
                                 ? remaining : remaining.substr(0, space);
        remaining = (space == std::string_view::npos)
                    ? std::string_view{} : remaining.substr(space + 1);

        if (token == "T") {
            needsTap = true;
        } else if (token == "Sac<Self>" || token == "SacrificeSource") {
            // Sacrifice the source itself (used by Treasure, Clue, Food, Blood tokens)
            needsSacSelf = true;
        } else if (token.substr(0, 4) == "Sac<") {
            // Sac<N/Filter>. Forge uses "CARDNAME" as a self-reference, so any
            // Sac<*/CARDNAME> (or Sac<*/Self>) means sacrifice the source — the
            // fetch-land pattern (Sac<1/CARDNAME>) and most self-sac activated
            // abilities. Other filters sac one matching battlefield permanent.
            auto inner = token.substr(4);
            if (!inner.empty() && inner.back() == '>') inner.remove_suffix(1);
            auto slash = inner.find('/');
            std::string_view filt = (slash != std::string_view::npos)
                                    ? inner.substr(slash + 1) : inner;
            if (filt == "CARDNAME" || filt == "Self")
                needsSacSelf = true;
            else
                needsSac = true;
        } else if (token.substr(0, 8) == "Discard<") {
            // Discard<N/Type> — discard N cards of matching type.
            // For Discard<1/Self>, discard the source card itself.
            // The source card is discarded (moved to GY) as part of paying the cost.
            auto inner = token.substr(8);
            auto slash = inner.find('/');
            if (slash != std::string_view::npos) {
                auto typeStr = inner.substr(slash + 1);
                // Remove trailing '>'
                if (!typeStr.empty() && typeStr.back() == '>') typeStr.remove_suffix(1);
                if (typeStr == "Self" || typeStr == "CARDNAME") {
                    // Discard the source card (the cycling card itself)
                    if (source.zone == ZoneType::Hand) {
                        m_game.moveToZone(source.id, ZoneType::Graveyard, source.ownerId);
                        // source is now invalid — further cost processing may fail if id is reused
                        // The StackAbility sourceCardId will be stale but isActivatedAbility = true
                        // so it won't try to move it again in resolveTop.
                    }
                } else {
                    // Discard a random card from hand (simplified)
                    Player& p = m_game.player(controller);
                    if (!p.hand().empty()) {
                        Card* last = p.hand().back();
                        if (last) m_game.moveToZone(last->id, ZoneType::Graveyard, controller);
                    }
                }
            }
        } else if (token.find("AddCounter<") != std::string::npos &&
                   token.find("LOYALTY") != std::string::npos) {
            // Planeswalker +N or 0 ability: AddCounter<N/LOYALTY>
            auto lt = token.find('<'), sl = token.find('/');
            if (lt != std::string::npos && sl != std::string::npos) {
                int n = 0;
                std::from_chars(token.data() + lt + 1, token.data() + sl, n);
                source.addCounter("loyalty", n);
            }
        } else if (token.find("SubCounter<") != std::string::npos &&
                   token.find("LOYALTY") != std::string::npos) {
            // Planeswalker -N ability: SubCounter<N/LOYALTY>
            auto lt = token.find('<'), sl = token.find('/');
            if (lt != std::string::npos && sl != std::string::npos) {
                int n = 0;
                std::from_chars(token.data() + lt + 1, token.data() + sl, n);
                if (source.counterCount("loyalty") < n) return false; // not enough loyalty
                source.addCounter("loyalty", -n);
            }
        } else if (token.size() > 10 && token.substr(0, 10) == "PayEnergy<") {
            // PayEnergy<N> — pay N energy counters.
            auto inner = token.substr(10);
            if (!inner.empty() && inner.back() == '>') inner.remove_suffix(1);
            int n = 0;
            // N can be a literal or an SVar name (e.g. "X")
            if (!inner.empty() && std::isdigit(static_cast<unsigned char>(inner[0])))
                std::from_chars(inner.data(), inner.data() + inner.size(), n);
            else
                n = m_game.evaluateSVar(std::string(inner), controller, source.rules, source.id);
            Player& p = m_game.player(controller);
            if (p.counterCount("ENERGY") < n) return false;
            p.removeCounter("ENERGY", n);
        } else if (token.size() > 8 && token.substr(0, 8) == "PayLife<") {
            // PayLife<N> — pay N life as an additional cost.
            auto inner = token.substr(8);
            if (!inner.empty() && inner.back() == '>') inner.remove_suffix(1);
            int n = 0;
            std::from_chars(inner.data(), inner.data() + inner.size(), n);
            if (m_game.player(controller).life() <= n) return false; // would be lethal
            m_game.loseLife(controller, n);
        } else {
            // Mana token (W, U, B, R, G, digit, X, hybrid like 2/W, etc.)
            if (!manaCostTokens.empty()) manaCostTokens += ' ';
            manaCostTokens += std::string(token);
        }
    }

    // Pay mana cost if any mana tokens were found. The source permanent is the
    // payee so restricted mana usable for "activate an ability of a creature of
    // the chosen type" (Secluded Courtyard) can apply when source matches.
    if (!manaCostTokens.empty()) {
        ManaCost cost = ManaCost::parse(manaCostTokens);
        if (!payCost(cost, controller, &source, /*isSpell=*/false, /*isActivated=*/true))
            return false;
    }

    // Validate and apply
    if (needsTap) {
        if (source.tapped) return false;
        // Creatures with summoning sickness can't use tap abilities, unless a
        // static effect (e.g. Thousand-Year Elixir) grants activateAbilityAsIfHaste.
        if (source.isCreature() && source.summoningSickness && !source.activateAbilityAsIfHaste)
            return false;
        source.tapped = true;
        // Fire Inspired triggers when a permanent taps to activate
        {
            std::vector<PendingTrigger> tapTrigs;
            TriggerSystem::onTap(source, m_game, tapTrigs);
            m_game.queueTriggers(std::move(tapTrigs));
        }
    }

    // Sacrifice a non-self permanent matching the filter
    if (needsSac) {
        for (Card* c : m_game.battlefield().cards()) {
            if (c->controllerId != controller) continue;
            if (c->id == source.id) continue;
            m_game.moveToZone(c->id, ZoneType::Graveyard, c->ownerId);
            break;
        }
    }

    // Sacrifice the source itself (Treasure/Clue/Food/Blood token abilities)
    // Must be last — source is invalid after this point.
    if (needsSacSelf) {
        ObjectId srcId = source.id;
        m_game.moveToZone(srcId, ZoneType::Graveyard, source.ownerId);
        // source reference is now dangling — do not use it
    }

    (void)controller;
    return true;
}

bool AbilityProcessor::activateAbility(ObjectId sourceId, int abilityIndex,
                                        uint8_t controller,
                                        const std::vector<Target>& targets) {
    Card* source = m_game.findCard(sourceId);
    if (!source) return false;
    if (source->controllerId != controller) return false;
    if (source->allAbilitiesRemoved) return false; // Humility etc.
    // CantBeActivated (Abeyance / Braided Net) — non-mana abilities are blocked.
    // (Mana abilities go through activateManaAbility, which is unaffected.)
    if ((controller < 2 && m_game.tempCantActivate[controller]) ||
        source->tempCantActivate) return false;
    // Most abilities require the source to be on the battlefield.
    // Some (like Cycling) declare ActivationZone$ Hand — handled below.

    // Collect all non-mana AB$ lines
    std::vector<std::pair<int, ScriptLine>> abilityScripts;
    for (int i = 0; i < static_cast<int>(source->rules->abilityLines.size()); ++i) {
        auto s = parseScriptLine(source->rules->abilityLines[i]);
        if (s.abilityType == "AB") abilityScripts.emplace_back(i, std::move(s));
    }

    if (abilityIndex < 0 || abilityIndex >= static_cast<int>(abilityScripts.size()))
        return false;

    auto& [lineIdx, script] = abilityScripts[abilityIndex];

    // ActivationLimit$ N — "activate only once [per game]" (Power-up etc.).
    // Counted per ability line on this Card instance; never reset during a game.
    int activationLimit = script.getInt("ActivationLimit", 0);
    if (activationLimit > 0) {
        auto it = source->abilityUses.find(lineIdx);
        if (it != source->abilityUses.end() && it->second >= activationLimit)
            return false;
    }

    // Determine the required zone for this ability
    auto activationZone = script.get("ActivationZone", "Battlefield");
    if (activationZone == "Hand") {
        if (source->zone != ZoneType::Hand) return false;
    } else if (activationZone == "Graveyard") {
        if (source->zone != ZoneType::Graveyard) return false;
    } else {
        // Default: must be on battlefield
        if (!source->isOnBattlefield()) return false;
    }

    // Split second: non-mana abilities can't be activated while split second is on stack.
    // (Mana abilities — script.effectType == "Mana" — are exempt, rule 702.61b.)
    if (m_game.splitSecondOnStack() && script.effectType != "Mana")
        return false;

    // Timing restrictions on activated abilities:
    //   SorcerySpeed$ True: only during the controller's main phase with empty stack.
    //   InstantSpeed$ False (rare): additional restriction, treated as sorcery speed here.
    {
        auto timing = script.get("SorcerySpeed", "False");
        bool requiresSorcery = (timing == "True" || timing == "1");
        // Also catch "Activate only as a sorcery" encoded as ActivateOnce$ True with Sorcery
        if (!requiresSorcery) {
            auto activateOnce = script.get("ActivateOnce", "");
            auto phaseReq     = script.get("Phase", "");
            if (!phaseReq.empty() && phaseReq.find("Main") != std::string_view::npos)
                requiresSorcery = true;
        }
        if (requiresSorcery && script.effectType != "Mana") {
            if (m_game.activePlayerId() != controller) return false;
            if (!stackEmpty()) return false;
        }
    }

    // Planeswalker loyalty abilities (Cost$ contains AddCounter or SubCounter with LOYALTY):
    //   - Only once per turn (rule 606.3)
    //   - Only during the controller's main phase when the stack is empty (sorcery speed)
    bool isLoyaltyAbility = false;
    if (source->rules->type.isPlaneswalker() && source->isOnBattlefield()) {
        auto costStr2 = script.get("Cost", "");
        isLoyaltyAbility = (costStr2.find("LOYALTY") != std::string_view::npos);
        if (isLoyaltyAbility) {
            if (source->loyaltyUsedThisTurn) return false;
            if (m_game.activePlayerId() != controller) return false;
            if (!stackEmpty()) return false;
        }
    }

    // Ward: if this ability targets a permanent with Ward controlled by an opponent,
    // the activating player must pay the ward cost or the activation is cancelled.
    for (const auto& tgt : targets) {
        if (!tgt.isCard()) continue;
        const Card* tc = m_game.findCard(tgt.cardId);
        if (tc && tc->controllerId != controller
               && tc->rules->hasWard
               && !payCost(tc->rules->wardCost, controller))
            return false;
    }

    // Pay the activation cost
    auto costStr = std::string(script.get("Cost", ""));
    if (!payActivationCost(costStr, *source, controller)) return false;

    // Commit the activation against its per-game limit now that the cost is paid.
    if (activationLimit > 0) source->abilityUses[lineIdx]++;

    if (script.effectType == "Mana") {
        EffectContext ctx{ m_game, source, controller, targets, 0 };
        effectMana(script, ctx);
        drainPendingTriggers();
        return true;
    }

    // Non-mana: push to stack as activated ability (source stays in place)
    StackAbility ability;
    ability.sourceCardId       = sourceId;
    ability.controllerId       = controller;
    ability.targets            = targets;
    ability.isActivatedAbility = true;

    // If the AB$ line delegates to a SVar via Execute$, resolve it to get the effect.
    auto execSVar = script.get("Execute", "");
    if (!execSVar.empty()) {
        auto it = source->rules->svars.find(std::string(execSVar));
        if (it != source->rules->svars.end())
            ability.script = parseScriptLine(it->second);
        else
            ability.script = script;
    } else {
        ability.script = script;
    }

    // Mark planeswalker as having used its loyalty ability this turn
    if (isLoyaltyAbility) {
        Card* pw = m_game.findCard(sourceId);
        if (pw) pw->loyaltyUsedThisTurn = true;
    }

    m_stack.push_back(std::move(ability));

    // Fire BecomesTarget triggers for each card targeted by this ability
    for (const auto& tgt : targets) {
        if (!tgt.isCard()) continue;
        std::vector<PendingTrigger> triggered;
        TriggerSystem::onBecomesTarget(tgt.cardId, sourceId, false, controller,
                                        m_game, triggered);
        if (!triggered.empty()) {
            m_game.queueTriggers(std::move(triggered));
            drainPendingTriggers();
        }
    }
    return true;
}

bool AbilityProcessor::activateEquip(ObjectId equipId, ObjectId targetId,
                                      uint8_t controller) {
    Card* equip  = m_game.findCard(equipId);
    Card* target = m_game.findCard(targetId);
    if (!equip || !target) return false;
    if (equip->controllerId != controller) return false;
    if (!target->isCreature() || !target->isOnBattlefield()) return false;
    if (target->controllerId != controller) return false;

    // Find the equip cost from K:Equip:N keyword
    int equipCost = -1;
    for (const auto& kw : equip->rules->keywords) {
        equipCost = parseEquipCost(kw);
        if (equipCost >= 0) break;
    }
    if (equipCost < 0) return false; // not an equipment

    // Pay equip cost from mana pool
    ManaCost cost = ManaCost::parse(std::to_string(equipCost));
    if (!payCost(cost, controller)) return false;

    // Attach
    attachEquipment(*equip, *target, m_game);
    drainPendingTriggers();
    return true;
}

bool AbilityProcessor::activateFortify(ObjectId fortId, ObjectId targetLandId,
                                         uint8_t controller) {
    // Fortify: pay cost to attach this Fortification to a land you control
    // (mirrors the Equip system; Fortifications are artifact Fortifications)
    Card* fort = m_game.findCard(fortId);
    Card* land = m_game.findCard(targetLandId);
    if (!fort || !land) return false;
    if (fort->controllerId != controller || land->controllerId != controller) return false;
    if (!fort->isOnBattlefield() || !land->isOnBattlefield()) return false;
    if (!fort->rules->hasFortify || !land->isLand()) return false;

    ManaPool& pool = m_game.player(controller).manaPool();
    if (pool.total() < fort->rules->fortifyCost.cmc()) return false;
    if (!payCost(fort->rules->fortifyCost, controller)) return false;

    // Attach using the standard equipment system
    attachEquipment(*fort, *land, m_game);
    return true;
}

bool AbilityProcessor::activateTransmute(ObjectId cardId, uint8_t controller) {
    Card* source = m_game.findCard(cardId);
    if (!source || source->zone != ZoneType::Hand) return false;
    if (source->controllerId != controller)         return false;
    const CardRules* rules = source->rules;
    if (!rules->hasTransmute) return false;

    ManaPool& pool = m_game.player(controller).manaPool();
    if (pool.total() < rules->transmuteCost.cmc()) return false;
    if (!payCost(rules->transmuteCost, controller)) return false;

    // Discard the card
    int cardCmc = rules->manaCost.cmc();
    m_game.moveToZone(source->id, ZoneType::Graveyard, controller);
    drainPendingTriggers();

    // Search for a card with the same CMC (library search overlay for human)
    std::string filter = "Card.cmcEQ" + std::to_string(cardCmc);
    m_game.setPendingSearch(controller, ZoneType::Hand, controller, filter);
    return true;
}

bool AbilityProcessor::activateForecast(ObjectId cardId, uint8_t controller) {
    Card* source = m_game.findCard(cardId);
    if (!source) return false;
    if (source->zone != ZoneType::Hand) return false;
    if (source->controllerId != controller) return false;
    if (!source->rules->hasForecast) return false;
    if (!payCost(source->rules->forecastCost, controller)) return false;

    // "Reveal this card" effect: trigger the Forecast SVar
    auto it = source->rules->svars.find("Forecast");
    if (it == source->rules->svars.end()) {
        // Try to find any T: line with Mode$ Forecast
        for (const auto& raw : source->rules->triggerLines) {
            auto s = parseScriptLine(raw);
            if (s.get("Mode","") == "Forecast") {
                auto execName = s.get("Execute","");
                auto eit = source->rules->svars.find(std::string(execName));
                if (eit != source->rules->svars.end()) {
                    EffectContext ctx{m_game, source, controller, {}, 0};
                    executeEffectChain(parseScriptLine(eit->second), ctx);
                    drainPendingTriggers();
                    return true;
                }
            }
        }
        return false;
    }
    EffectContext ctx{m_game, source, controller, {}, 0};
    executeEffectChain(parseScriptLine(it->second), ctx);
    drainPendingTriggers();
    return true;
}

bool AbilityProcessor::activateCycling(ObjectId cardId, uint8_t controller) {
    Card* source = m_game.findCard(cardId);
    if (!source) return false;
    if (source->zone != ZoneType::Hand) return false;
    if (source->controllerId != controller) return false;

    const CardRules* rules = source->rules;
    const bool isType = rules->hasTypeCycling;
    if (!rules->hasCycling && !isType) return false;

    const ManaCost& cost = isType ? rules->typeCyclingCost : rules->cyclingCost;
    if (!payCost(cost, controller)) return false;

    // Discard the card to the graveyard (source pointer dangles after this)
    Card* inGy = m_game.moveToZone(cardId, ZoneType::Graveyard, controller);
    ObjectId gyId = inGy ? inGy->id : kInvalidId;

    Player& p = m_game.player(controller);
    if (!isType) {
        // Simple cycling: draw one card from the top of library
        if (!p.library().empty()) {
            Card* drawn = p.library().front();
            m_game.moveToZone(drawn->id, ZoneType::Hand, controller);
        }
    } else {
        // TypeCycling (landcycling): search library for a matching land
        const std::string& searchType = rules->cyclingType;
        Card* found = nullptr;
        for (Card* c : p.library().cards()) {
            if (!c->isLand()) continue;
            if (searchType == "Basic") {
                if (c->rules->type.isBasic()) { found = c; break; }
            } else {
                if (c->rules->type.hasSubtype(searchType)) { found = c; break; }
            }
        }
        if (found)
            m_game.moveToZone(found->id, ZoneType::Hand, controller);
    }

    // Fire T:Mode$ Cycled triggers on the cycled card
    if (gyId != kInvalidId) {
        std::vector<PendingTrigger> cycleTriggers;
        TriggerSystem::onCycle(*rules, gyId, controller, m_game, cycleTriggers);
        if (!cycleTriggers.empty()) {
            m_game.queueTriggers(std::move(cycleTriggers));
        }
    }

    while (StateBasedActions::run(m_game)) {}
    drainPendingTriggers();
    return true;
}

// ── Crew ──────────────────────────────────────────────────────────────────────

bool AbilityProcessor::crewVehicle(ObjectId vehicleId,
                                   const std::vector<ObjectId>& crewIds,
                                   uint8_t controller) {
    Card* vehicle = m_game.findCard(vehicleId);
    if (!vehicle)                            return false;
    if (!vehicle->isOnBattlefield())         return false;
    if (vehicle->controllerId != controller) return false;
    if (!vehicle->rules->hasCrew)            return false;

    // Validate crew: all untapped, controlled by same player, compute total power
    int totalPower = 0;
    for (ObjectId cid : crewIds) {
        Card* c = m_game.findCard(cid);
        if (!c || !c->isOnBattlefield())         return false;
        if (c->controllerId != controller)        return false;
        if (c->tapped)                            return false;
        if (!c->isCreature())                     return false;
        totalPower += effectivePower(*c);
    }

    if (totalPower < vehicle->rules->crewCost) return false;

    // Tap each crew member
    for (ObjectId cid : crewIds) {
        Card* c = m_game.findCard(cid);
        if (c) c->tapped = true;
    }

    // Vehicle becomes an artifact creature until EOT
    vehicle->tempIsCreature = true;

    return true;
}

bool AbilityProcessor::activateScavenge(ObjectId cardId, ObjectId targetId,
                                        uint8_t controller) {
    Card* source = m_game.findCard(cardId);
    if (!source)                              return false;
    if (source->zone != ZoneType::Graveyard)  return false;
    if (source->controllerId != controller)   return false;
    if (!source->rules->hasScavenge)          return false;
    if (!source->rules->isCreature())         return false;

    Card* target = m_game.findCard(targetId);
    if (!target || !target->isOnBattlefield()) return false;

    if (!payCost(source->rules->scavengeCost, controller)) return false;

    // Power of the scavenged creature (before it's exiled)
    int counters = effectivePower(*source);

    // Exile the creature card from GY
    m_game.moveToZone(cardId, ZoneType::Exile, controller);

    // Put +1/+1 counters on the target
    if (counters > 0) {
        target->addCounter("+1/+1", counters);
        std::vector<PendingTrigger> t;
        TriggerSystem::onCounterAdded(*target, "+1/+1", counters, m_game, t);
        m_game.queueTriggers(std::move(t));
    }

    drainPendingTriggers();
    return true;
}

bool AbilityProcessor::activateEmbalm(ObjectId cardId, uint8_t controller) {
    Card* card = m_game.findCard(cardId);
    if (!card || card->zone != ZoneType::Graveyard) return false;
    if (!card->rules->hasEmbalm) return false;
    if (card->controllerId != controller) return false;
    if (!card->rules->isCreature()) return false;
    if (!payCost(card->rules->embalmCost, controller)) return false;

    const CardRules* orig = card->rules;
    // Build type string: "Creature Zombie <original subtypes>"
    std::string typeStr = "Creature Zombie";
    for (const auto& st : orig->type.subtypes) typeStr += " " + st;
    const std::string power     = orig->power;
    const std::string toughness = orig->toughness;
    std::vector<std::string> kws = orig->keywords;

    m_game.moveToZone(cardId, ZoneType::Exile, controller);
    m_game.createToken(orig->name, typeStr,
                       static_cast<uint8_t>(ManaAtom::WHITE),
                       power, toughness, controller, kws);
    drainPendingTriggers();
    return true;
}

bool AbilityProcessor::activateEternalize(ObjectId cardId, uint8_t controller) {
    Card* card = m_game.findCard(cardId);
    if (!card || card->zone != ZoneType::Graveyard) return false;
    if (!card->rules->hasEternalize) return false;
    if (card->controllerId != controller) return false;
    if (!card->rules->isCreature()) return false;
    if (!payCost(card->rules->eternalizeCost, controller)) return false;

    const CardRules* orig = card->rules;
    std::string typeStr = "Creature Zombie";
    for (const auto& st : orig->type.subtypes) typeStr += " " + st;
    std::vector<std::string> kws = orig->keywords;

    m_game.moveToZone(cardId, ZoneType::Exile, controller);
    // Eternalize token is always 4/4
    m_game.createToken(orig->name, typeStr,
                       static_cast<uint8_t>(ManaAtom::WHITE),
                       "4", "4", controller, kws);
    drainPendingTriggers();
    return true;
}

bool AbilityProcessor::activateForetell(ObjectId cardId, uint8_t controller) {
    Card* card = m_game.findCard(cardId);
    if (!card || card->zone != ZoneType::Hand) return false;
    if (!card->rules->hasForetell) return false;
    if (card->controllerId != controller) return false;

    // Foretell cost: always exactly {2} generic (pay from pool)
    ManaPool& pool = m_game.player(controller).manaPool();
    if (pool.total() < 2) return false;
    pool.addGeneric(-2);

    Card* exiled = m_game.moveToZone(cardId, ZoneType::Exile, controller);
    if (!exiled) return false;
    exiled->foretold       = true;
    exiled->foretoldOnTurn = m_game.turnNumber();
    std::cout << "  [Foretell] " << exiled->rules->name << " exiled face-down.\n";
    return true;
}

bool AbilityProcessor::activateSuspend(ObjectId cardId, uint8_t controller, int xValue) {
    Card* card = m_game.findCard(cardId);
    if (!card || card->zone != ZoneType::Hand) return false;
    if (!card->rules->hasSuspend) return false;
    if (card->controllerId != controller) return false;

    // Pay the suspend cost
    if (!payCost(card->rules->suspendCost, controller)) return false;

    // Determine counter count (fixed N or X)
    int n = card->rules->suspendCountIsX ? xValue : card->rules->suspendCount;
    if (n <= 0) n = 1;

    Card* exiled = m_game.moveToZone(cardId, ZoneType::Exile, controller);
    if (!exiled) return false;
    exiled->suspended = true;
    exiled->addCounter("TIME", n);
    std::cout << "  [Suspend] " << exiled->rules->name << " exiled with " << n << " time counter(s).\n";
    return true;
}

bool AbilityProcessor::activateLevelUp(ObjectId cardId, uint8_t controller) {
    Card* c = m_game.findCard(cardId);
    if (!c)                              return false;
    if (!c->isOnBattlefield())           return false;
    if (c->controllerId != controller)   return false;
    if (!c->rules->hasLevelUp)           return false;

    if (!payCost(c->rules->levelUpCost, controller)) return false;

    c->addCounter("LEVEL", 1);
    int totalLevels = c->counterCount("LEVEL");

    std::vector<PendingTrigger> trigs;
    TriggerSystem::onCounterAdded(*c, "LEVEL", 1, m_game, trigs);
    m_game.queueTriggers(std::move(trigs));
    drainPendingTriggers();
    m_game.recomputeStaticBonuses(); // re-apply level-band static abilities

    std::cout << "  [Level Up] " << c->rules->name
              << " leveled up (LEVEL=" << totalLevels << ").\n";
    return true;
}

bool AbilityProcessor::activateOutlast(ObjectId cardId, uint8_t controller) {
    Card* c = m_game.findCard(cardId);
    if (!c)                              return false;
    if (!c->isOnBattlefield())           return false;
    if (c->controllerId != controller)   return false;
    if (!c->rules->hasOutlast)           return false;
    if (c->tapped)                       return false;
    if (c->summoningSickness)            return false;
    // Outlast is sorcery-speed only: active player, empty stack
    if (m_game.activePlayerId() != controller) return false;
    if (!m_game.stack().cards().empty()) return false;

    if (!payCost(c->rules->outlastCost, controller)) return false;

    c->tapped = true;
    c->addCounter("+1/+1", 1);

    std::vector<PendingTrigger> trigs;
    TriggerSystem::onCounterAdded(*c, "+1/+1", 1, m_game, trigs);
    m_game.queueTriggers(std::move(trigs));
    drainPendingTriggers();
    m_game.recomputeStaticBonuses();
    return true;
}

bool AbilityProcessor::activateAdapt(ObjectId cardId, uint8_t controller) {
    Card* c = m_game.findCard(cardId);
    if (!c)                              return false;
    if (!c->isOnBattlefield())           return false;
    if (c->controllerId != controller)   return false;
    if (!c->rules->hasAdapt)             return false;
    // Adapt can only add counters when the creature has none
    if (c->counterCount("+1/+1") > 0)   return false;

    if (!payCost(c->rules->adaptCost, controller)) return false;

    int n = c->rules->adaptAmount;
    if (n <= 0) n = 1;
    c->addCounter("+1/+1", n);

    std::vector<PendingTrigger> trigs;
    TriggerSystem::onCounterAdded(*c, "+1/+1", n, m_game, trigs);
    m_game.queueTriggers(std::move(trigs));
    drainPendingTriggers();
    m_game.recomputeStaticBonuses();
    return true;
}

bool AbilityProcessor::activateNinjutsu(ObjectId ninjaId, ObjectId attackerId,
                                         uint8_t controller, TurnManager& tm) {
    Card* ninja = m_game.findCard(ninjaId);
    Card* atk   = m_game.findCard(attackerId);
    if (!ninja || !atk)                       return false;
    if (ninja->zone != ZoneType::Hand)        return false;
    if (!atk->isOnBattlefield())              return false;
    if (ninja->controllerId != controller)    return false;
    if (atk->controllerId   != controller)    return false;
    if (!ninja->rules->hasNinjutsu)           return false;

    // The attacker must be unblocked in the current combat
    const CombatState::Attack* atkEntry = tm.combatState().findAttack(attackerId);
    if (!atkEntry || !atkEntry->blockerIds.empty()) return false;

    uint8_t defendingPlayer = atkEntry->defendingPlayerId;
    std::string ninjaName   = ninja->rules->name;

    if (!payCost(ninja->rules->ninjutsuCost, controller)) return false;

    // Remove the old attacker from the combat state before moving it out of the BF
    {
        auto& attacks = tm.mutableCombatState().attacks;
        attacks.erase(
            std::remove_if(attacks.begin(), attacks.end(),
                [attackerId](const CombatState::Attack& a){ return a.attackerId == attackerId; }),
            attacks.end());
    }

    // Return the swapped-out attacker to its owner's hand
    uint8_t atkOwner = atk->ownerId;
    m_game.moveToZone(attackerId, ZoneType::Hand, atkOwner);

    // Put the ninja onto the battlefield tapped and attacking (bypasses summoning sickness)
    Card* ninjaOnBF = m_game.moveToZone(ninjaId, ZoneType::Battlefield, controller);
    if (ninjaOnBF) {
        ninjaOnBF->tapped = true;
        ninjaOnBF->summoningSickness = false; // already "attacking" — no sickness for this combat
        // Register in the existing combat as an attacker (no declare-attackers check)
        CombatState::Attack newAtk;
        newAtk.attackerId        = ninjaOnBF->id;
        newAtk.defendingPlayerId = defendingPlayer;
        tm.mutableCombatState().attacks.push_back(newAtk);
    }

    return true;
}

bool AbilityProcessor::processSuspendUpkeep(uint8_t playerId) {
    bool anyCast = false;
    // Collect suspended cards in exile owned by this player
    std::vector<ObjectId> suspended;
    for (const Card* c : m_game.exile().cards()) {
        if (c->ownerId == playerId && c->suspended && c->counterCount("TIME") > 0)
            suspended.push_back(c->id);
    }
    for (ObjectId id : suspended) {
        Card* c = m_game.findCard(id);
        if (!c || !c->suspended) continue;
        c->removeCounter("TIME", 1);
        int remaining = c->counterCount("TIME");
        std::cout << "  [Suspend] " << c->rules->name << " — " << remaining << " time counter(s) left.\n";
        if (remaining == 0) {
            // Cast for free — goes on stack to be resolved later
            bool isCreature = c->rules->type.isCreature();
            std::string name = c->rules->name;
            if (castSpell(c->id, playerId, {})) {
                std::cout << "  [Suspend] " << name << " is now cast for free.\n";
                anyCast = true;
                // Suspend rule 702.61d: if a creature is cast via suspend, it gains haste
                // until it leaves the battlefield. We grant it now by tagging isSuspend.
                // The haste is applied when the spell resolves and the creature ETBs.
                // We use the suspend flag on the StackAbility to signal this.
                if (isCreature && !m_stack.empty())
                    m_stack.back().isSuspendHaste = true;
            }
        }
    }
    return anyCast;
}

bool AbilityProcessor::processReboundUpkeep(uint8_t playerId) {
    bool anyCast = false;
    std::vector<ObjectId> toRebound;
    for (const Card* c : m_game.exile().cards()) {
        if (c->ownerId == playerId && c->rebound)
            toRebound.push_back(c->id);
    }
    for (ObjectId id : toRebound) {
        Card* c = m_game.findCard(id);
        if (!c || !c->rebound) continue;
        std::string cardName = c->rules->name; // snapshot before castSpell destroys c
        if (castSpell(c->id, playerId, {})) {
            std::cout << "  [Rebound] " << cardName << " cast for free via rebound.\n";
            anyCast = true;
        }
    }
    return anyCast;
}

void AbilityProcessor::processHideaway(Card& source) {
    // Find the Hideaway keyword + its count (all printed Hideaway cards use 4).
    int n = 0;
    for (const auto& kw : source.rules->keywords) {
        if (kw.rfind("Hideaway", 0) == 0) {
            auto colon = kw.find(':');
            n = (colon != std::string::npos) ? std::atoi(kw.c_str() + colon + 1) : 4;
            break;
        }
    }
    if (n <= 0) return;

    uint8_t owner = source.controllerId;
    auto libCards = m_game.player(owner).library().cards();  // top-first
    if (libCards.empty()) return;

    // Look at the top N; exile the most expensive one face-down (a reasonable
    // payoff pick for both human and AI). The rest stay where they were — we
    // skip the "put on bottom" reorder to avoid same-zone shuffling.
    ObjectId chosen = kInvalidId;
    int bestCmc = -1;
    for (int i = 0; i < n && i < static_cast<int>(libCards.size()); ++i) {
        Card* c = libCards[static_cast<size_t>(i)];
        if (!c || !c->rules) continue;
        int cmc = c->rules->manaCost.cmc();
        if (cmc > bestCmc) { bestCmc = cmc; chosen = c->id; }
    }
    if (chosen == kInvalidId) return;

    Card* moved = m_game.moveToZone(chosen, ZoneType::Exile, owner);
    if (moved) {
        moved->exiledBy   = source.id;   // links to source for Defined$ ExiledWith
        moved->isFaceDown = true;
    }
}

void AbilityProcessor::processEchoUpkeep(uint8_t playerId) {
    // Two-pass: first pass marks creatures that haven't seen payment yet;
    // second pass processes the ones already flagged from last upkeep.
    std::vector<ObjectId> toPay;
    for (Card* c : m_game.battlefield().cards()) {
        if (c->controllerId != playerId) continue;
        if (!c->rules->hasEcho) continue;
        if (c->echoNeedsPayment) {
            toPay.push_back(c->id);
        } else {
            c->echoNeedsPayment = true; // will require payment next upkeep
        }
    }
    for (ObjectId id : toPay) {
        Card* c = m_game.findCard(id);
        if (!c || !c->isOnBattlefield()) continue;
        // AI and non-interactive: pay if we can, else sacrifice.
        bool paid = payCost(c->rules->echoCost, playerId);
        if (!paid) {
            std::cout << "  [Echo] Can't pay echo for " << c->rules->name << " — sacrificing.\n";
            ObjectId cid = c->id;
            std::vector<PendingTrigger> t;
            TriggerSystem::onSacrificed(*c, m_game, t);
            m_game.queueTriggers(std::move(t));
            m_game.moveToZone(cid, ZoneType::Graveyard, playerId);
        } else {
            c->echoNeedsPayment = false;
            std::cout << "  [Echo] Paid echo for " << c->rules->name << ".\n";
        }
    }
    drainPendingTriggers();
}

void AbilityProcessor::processCumulativeUpkeep(uint8_t playerId) {
    std::vector<ObjectId> toProcess;
    for (Card* c : m_game.battlefield().cards()) {
        if (c->controllerId != playerId) continue;
        if (!c->rules->hasCumulativeUpkeep) continue;
        toProcess.push_back(c->id);
    }
    for (ObjectId id : toProcess) {
        Card* c = m_game.findCard(id);
        if (!c || !c->isOnBattlefield()) continue;
        // Add an age counter
        c->addCounter("age", 1);
        int ageCount = c->counterCount("age");
        if (c->rules->cumulativeUpkeepIsCounter) {
            // Counter-based: add the specified counter type N times
            std::string key = "+1/+1";
            if      (c->rules->cumulativeUpkeepCounterType == "P1P1")  key = "+1/+1";
            else if (c->rules->cumulativeUpkeepCounterType == "M1M1")  key = "-1/-1";
            else if (c->rules->cumulativeUpkeepCounterType == "CHARGE") key = "charge";
            else                                                         key = c->rules->cumulativeUpkeepCounterType;
            c->addCounter(key, 1);
            std::cout << "  [Cumulative Upkeep] " << c->rules->name
                      << " gets a " << key << " counter (" << ageCount << " age).\n";
        } else {
            // Pay ageCount × upkeep cost or sacrifice
            ManaCost totalCost = c->rules->cumulativeUpkeepCost;
            // Scale the generic portion: multiply by ageCount
            // Simple approach: pay the base cost ageCount times
            bool paid = true;
            for (int i = 0; i < ageCount && paid; ++i)
                paid = payCost(c->rules->cumulativeUpkeepCost, playerId);
            if (!paid) {
                std::cout << "  [Cumulative Upkeep] Can't pay for " << c->rules->name
                          << " (" << ageCount << " age) — sacrificing.\n";
                ObjectId cid = c->id;
                std::vector<PendingTrigger> t;
                TriggerSystem::onSacrificed(*c, m_game, t);
                m_game.queueTriggers(std::move(t));
                m_game.moveToZone(cid, ZoneType::Graveyard, playerId);
            } else {
                std::cout << "  [Cumulative Upkeep] Paid upkeep for " << c->rules->name
                          << " (age=" << ageCount << ").\n";
            }
        }
    }
    drainPendingTriggers();

    // ── Fading ────────────────────────────────────────────────────────────────
    // At beginning of upkeep: remove one fading counter; if none remain, sacrifice.
    {
        std::vector<ObjectId> toSac;
        for (Card* c : m_game.battlefield().cards()) {
            if (c->controllerId != playerId) continue;
            if (!c->rules->hasFading) continue;
            int cnt = c->counterCount("fading");
            if (cnt <= 0) { toSac.push_back(c->id); continue; }
            c->counters["fading"] = cnt - 1;
            if (c->counters["fading"] <= 0) toSac.push_back(c->id);
        }
        for (ObjectId id : toSac) {
            Card* c = m_game.findCard(id);
            if (!c) continue;
            std::vector<PendingTrigger> t;
            TriggerSystem::onSacrificed(*c, m_game, t);
            m_game.queueTriggers(std::move(t));
            m_game.moveToZone(id, ZoneType::Graveyard, playerId);
        }
    }

    // ── Vanishing ─────────────────────────────────────────────────────────────
    // At beginning of upkeep: remove one time counter; if none remain, exile.
    {
        std::vector<ObjectId> toExile;
        for (Card* c : m_game.battlefield().cards()) {
            if (c->controllerId != playerId) continue;
            if (!c->rules->hasVanishing) continue;
            int cnt = c->counterCount("time");
            if (cnt <= 0) { toExile.push_back(c->id); continue; }
            c->counters["time"] = cnt - 1;
            if (c->counters["time"] <= 0) toExile.push_back(c->id);
        }
        for (ObjectId id : toExile)
            m_game.moveToZone(id, ZoneType::Exile, playerId);
    }

    drainPendingTriggers();
}

bool AbilityProcessor::activateMonstrosity(ObjectId cardId, uint8_t controller) {
    Card* c = m_game.findCard(cardId);
    if (!c)                              return false;
    if (!c->isOnBattlefield())           return false;
    if (c->controllerId != controller)   return false;
    if (!c->rules->hasMonstrosity)       return false;
    if (c->monstrous)                    return false; // already monstrous — can't activate again
    if (c->tapped)                       return false; // monstrosity is sorcery-speed only (simplified)
    if (!payCost(c->rules->monstrosityCost, controller)) return false;
    c->monstrous = true;
    int n = c->rules->monstrosityCount;
    if (n > 0) {
        c->addCounter("+1/+1", n);
        std::vector<PendingTrigger> ct;
        TriggerSystem::onCounterAdded(*c, "+1/+1", n, m_game, ct);
        m_game.queueTriggers(std::move(ct));
    }
    std::vector<PendingTrigger> mt;
    TriggerSystem::onBecomesMonstrous(*c, m_game, mt);
    m_game.queueTriggers(std::move(mt));
    drainPendingTriggers();
    return true;
}

bool AbilityProcessor::activateMorph(ObjectId cardId, uint8_t controller) {
    Card* c = m_game.findCard(cardId);
    if (!c)                            return false;
    if (!c->isOnBattlefield())         return false;
    if (c->controllerId != controller) return false;
    if (!c->isFaceDown)                return false;

    bool isMegamorph = c->rules->hasMegamorph;
    bool isMorph     = c->rules->hasMorph;
    if (!isMorph && !isMegamorph)      return false;

    const ManaCost& cost = isMegamorph ? c->rules->megamorphCost : c->rules->morphCost;
    if (!payCost(cost, controller))    return false;

    c->isFaceDown  = false;
    c->keywordMask = buildKeywordMask(*c->rules);

    if (isMegamorph) {
        c->addCounter("+1/+1", 1);
        std::vector<PendingTrigger> ct;
        TriggerSystem::onCounterAdded(*c, "+1/+1", 1, m_game, ct);
        m_game.queueTriggers(std::move(ct));
    }

    m_game.recomputeStaticBonuses();

    std::vector<PendingTrigger> trigs;
    TriggerSystem::onTurnFaceUp(*c, m_game, trigs);
    m_game.queueTriggers(std::move(trigs));
    drainPendingTriggers();

    std::cout << "  [Morph] " << c->rules->name << " turned face up"
              << (isMegamorph ? " (Megamorph)" : "") << ".\n";
    return true;
}

} // namespace mtg
