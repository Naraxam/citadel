#include "LayerEngine.h"
#include "ContinuousEffect.h"
#include "GameState.h"
#include "CardFilter.h"
#include "CardStats.h"
#include "KeywordAbility.h"
#include <algorithm>
#include <memory>

namespace mtg {

// ── Internal helpers ───────────────────────────────────────────────────────────

// Resolve which battlefield cards an effect targets.
// Uses pre-populated affectedIds when available; otherwise evaluates affectedFilter.
static std::vector<Card*> resolveTargets(const ContinuousEffect& e, GameState& gs) {
    std::vector<Card*> result;
    if (!e.affectedIds.empty()) {
        for (ObjectId id : e.affectedIds) {
            Card* c = gs.findCard(id);
            if (c && c->zone == ZoneType::Battlefield)
                result.push_back(c);
        }
        return result;
    }
    if (e.affectedFilter.empty()) return result;

    const Card* source = (e.sourceId != kInvalidId) ? gs.findCard(e.sourceId) : nullptr;
    for (Card* c : gs.battlefield().cards()) {
        if (cardMatchesAnyFilter(*c, e.affectedFilter, e.sourceController,
                                 e.sourceId, source, &gs))
            result.push_back(c);
    }
    return result;
}

// ── LayerEngine::apply() ───────────────────────────────────────────────────────

void LayerEngine::apply(GameState& gs) {
    auto& effects = gs.continuousEffects();
    if (effects.empty()) return;

    // Sort by layer value (stable so 7c timestamp order is preserved).
    // Layer enum underlying values encode correct ordering.
    std::stable_sort(effects.begin(), effects.end(),
        [](const ContinuousEffect& a, const ContinuousEffect& b) {
            return static_cast<uint8_t>(a.layer) < static_cast<uint8_t>(b.layer);
        });

    for (ContinuousEffect& e : effects) {
        auto targets = resolveTargets(e, gs);
        if (targets.empty()) continue;

        switch (e.layer) {

        // ── Layer 1: Copy ──────────────────────────────────────────────────
        case Layer::Copy: {
            if (e.copySourceId == kInvalidId) break;
            const Card* src = gs.findCard(e.copySourceId);
            if (!src) break;
            for (Card* t : targets) {
                // Copy all characteristics from source.
                // Preserve: counters, damage, attachments, token flag, zone-change ids.
                t->ownedRules = std::make_shared<CardRules>(*src->rules);
                t->rules      = t->ownedRules.get();
                // Rebuild keyword mask from newly copied rules.
                t->keywordMask = buildKeywordMask(*t->rules);
            }
            break;
        }

        // ── Layer 2: Control ───────────────────────────────────────────────
        case Layer::Control: {
            if (e.newController == 255) break;
            for (Card* t : targets)
                t->controllerId = e.newController;
            break;
        }

        // ── Layer 3: Text-changing ─────────────────────────────────────────
        // Basic implementation: replace one basic land type with another in the
        // card's oracle text, affecting mana-fetching and land type checks.
        // The substitution is recorded on the card so rendering and effect
        // resolution can access it (rule 701.4).
        case Layer::TextChanging: {
            if (e.textFind.empty() || e.textReplace.empty()) break;
            for (Card* t : targets) {
                // Only replace if not already in the card's substitution map
                auto it = t->textChanges.find(e.textFind);
                if (it == t->textChanges.end())
                    t->textChanges[e.textFind] = e.textReplace;
            }
            break;
        }

        // ── Layer 4: Type / subtype ────────────────────────────────────────
        case Layer::TypeChanging: {
            for (Card* t : targets) {
                if (e.removeAllCreatureTypes)
                    t->continuousSubtypes.clear();
                for (const std::string& st : e.removeSubtypes) {
                    auto it = std::find(t->continuousSubtypes.begin(),
                                        t->continuousSubtypes.end(), st);
                    if (it != t->continuousSubtypes.end())
                        t->continuousSubtypes.erase(it);
                    // Also mark removal in base rules subtypes via a flag if needed —
                    // for now continuousSubtypes only holds additions.
                }
                for (const std::string& st : e.addSubtypes)
                    t->continuousSubtypes.push_back(st);
            }
            break;
        }

        // ── Layer 5: Color ─────────────────────────────────────────────────
        case Layer::ColorChanging: {
            for (Card* t : targets) {
                if (e.removeAllColors)
                    t->colorIdOverride = 0x00;  // colorless
                else if (e.setColorMask)
                    t->colorIdOverride = e.setColorMask;
                else if (e.addColorMask)
                    t->colorIdOverride = (t->colorIdOverride == 0xFF)
                        ? e.addColorMask
                        : (t->colorIdOverride | e.addColorMask);
            }
            break;
        }

        // ── Layer 6: Ability add / remove ──────────────────────────────────
        case Layer::AbilityAddRem: {
            for (Card* t : targets) {
                if (e.removeAllAbilities) {
                    t->allAbilitiesRemoved = true;
                    t->keywordMask         = 0;
                    t->continuousKeywords  = 0;
                } else {
                    if (e.addKeywords) {
                        t->continuousKeywords |= e.addKeywords;
                        t->keywordMask        |= e.addKeywords;
                    }
                    if (e.removeKeywords) {
                        t->removedKeywords |= e.removeKeywords;
                        t->keywordMask     &= ~e.removeKeywords;
                    }
                }
                if (e.cantAttack)          t->cantAttack          = true;
                if (e.cantBlock)           t->cantBlock           = true;
                if (e.cantBeTargeted)      t->cantBeTargeted      = true;
                if (e.unblockable)         t->unblockable         = true;
                if (e.dealsDmgByToughness) t->dealsDamageByToughness = true;
                if (e.activateAsIfHaste)   t->activateAbilityAsIfHaste = true;
            }
            break;
        }

        // ── Layer 7a: Set P/T from characteristic-defining ability ─────────
        case Layer::SetPTFromChar: {
            for (Card* t : targets) {
                if (e.varPower     >= 0) t->varPower     = e.varPower;
                if (e.varToughness >= 0) t->varToughness = e.varToughness;
            }
            break;
        }

        // ── Layer 7b: Set P/T ──────────────────────────────────────────────
        case Layer::SetPT: {
            for (Card* t : targets) {
                if (e.setPower     >= 0) t->setPower     = e.setPower;
                if (e.setToughness >= 0) t->setToughness = e.setToughness;
            }
            break;
        }

        // ── Layer 7c: Modify P/T (additive, timestamp order) ──────────────
        case Layer::ModifyPT: {
            for (Card* t : targets) {
                t->continuousPower     += e.addPower;
                t->continuousToughness += e.addToughness;
            }
            break;
        }

        // ── Layer 7e: Switch P/T ───────────────────────────────────────────
        case Layer::SwitchPT: {
            if (e.switchPT)
                for (Card* t : targets)
                    t->swapPT = true;
            break;
        }

        } // switch
    }
}

} // namespace mtg
