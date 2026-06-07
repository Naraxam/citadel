#pragma once
#include "ObjectId.h"
#include "ZoneType.h"
#include "KeywordAbility.h"
#include "../core/card/CardRules.h"
#include <climits>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace mtg {

// A mutable in-game card object. One instance exists per card currently in the
// game; when a card changes zones it is replaced by a new Card with a new id
// (enforced by GameState::moveToZone). The static rules data is shared via
// a pointer to the CardDb entry and never copied.
struct Card {
    ObjectId         id;
    const CardRules* rules;          // never null; points into CardDb or ownedRules

    // Non-null when this card is a copy of another (Clone, Phyrexian Metamorph, etc.)
    // or is a transformed face (DFC). Keeps the CardRules alive.
    std::shared_ptr<CardRules> ownedRules;

    // Original rules from CardDb (never owned, never null after createCard).
    // Used to revert to base face on zone change (transforms, clones).
    const CardRules* originalRules = nullptr;

    uint8_t  ownerId      = 0;   // player who owns this card (deck owner)
    uint8_t  controllerId = 0;   // player who currently controls it

    ZoneType zone = ZoneType::Library;

    // ── Battlefield state ─────────────────────────────────────────────────
    // These are only meaningful while zone == Battlefield.

    bool tapped              = false;
    bool summoningSickness   = true;  // cleared during the owner's Untap step
    int  markedDamage        = 0;     // reset during Cleanup
    int  basePowerOverride   = -1;    // -1 = use rules->power
    int  baseToughOverride   = -1;    // -1 = use rules->toughness

    // Bonuses from attached Equipment / Aura continuous effects (persistent)
    int      bonusPower     = 0;
    int      bonusToughness = 0;
    uint32_t bonusKeywords  = 0;

    // Bonuses from "until end of turn" effects — cleared at Cleanup
    int      tempPower      = 0;
    int      tempToughness  = 0;
    uint32_t tempKeywords   = 0;
    bool     tempIsCreature = false; // Vehicle crew / Animate effects

    // Keyword bitmask built from CardRules::keywords when entering a zone.
    // Can be modified by continuous effects (e.g. "gains Flying until EOT").
    uint32_t keywordMask = 0;

    // Set when this creature has received damage from a deathtouch source.
    // Checked by StateBasedActions; cleared during Cleanup.
    bool deathtouchDamage = false;

    // True for tokens created by effects (not drawn from a deck).
    bool isToken = false;

    // True for commander-format commanders. Preserved across zone changes by moveToZone.
    // When this card would move to GY or Exile, it goes to the Command zone instead.
    bool isCommander = false;

    // Bonuses from S:Mode$ Continuous static abilities (re-computed after each zone change).
    // Cleared and rebuilt by GameState::recomputeStaticBonuses().
    int      continuousPower     = 0;
    int      continuousToughness = 0;
    uint32_t continuousKeywords  = 0;

    // Subtypes added by S:Mode$ AddType (e.g. Xenograft granting "Elf" to all your creatures).
    // Cleared and rebuilt by GameState::recomputeStaticBonuses().
    std::vector<std::string> continuousSubtypes;

    // Activated abilities granted by S:Mode$ Continuous | AddAbility$ <SVar>
    // (Chromatic Lantern: "Lands you control have '{T}: Add one mana of any
    // colour.'"). Each entry is an ability-line string (the SVar value, minus
    // any leading "AB$ "/"SP$ " prefix is kept as-is for parseScriptLine).
    // Cleared and rebuilt by GameState::recomputeStaticBonuses().
    std::vector<std::string> grantedAbilities;

    // Restriction flags from static abilities (e.g. Pacifism, Arrest).
    bool cantAttack      = false;
    bool cantBlock       = false;
    // CantTarget: set by S:Mode$ CantTarget; prevents any player from targeting this permanent.
    bool cantBeTargeted  = false;

    // CombatDamageToughness: when set, this creature assigns combat damage
    // equal to its toughness instead of its power. Set by S:Mode$ CombatDamageToughness.
    bool dealsDamageByToughness = false;

    // CantBlockBy: when set, this permanent can't be blocked by any creature.
    // Set by S:Mode$ CantBlockBy with no ValidBlocker$ restriction.
    bool unblockable = false;

    // CantBlockBy with ValidBlocker$: this attacker can only be blocked by creatures
    // matching this filter. Empty = no such restriction.
    // Set by S:Mode$ CantBlockBy with ValidBlocker$ present.
    std::string blockOnlyBy;

    // Remaining damage-prevention shield (set by Healing Salve, etc.; cleared each Cleanup).
    int damageShield = 0;

    // Morph: true when this creature was cast face-down.
    // While true: P/T = 2/2, no name/type/text visible to opponent, no keyword abilities.
    // Cleared when turned face-up via activateMorph().
    bool isFaceDown = false;

    // Combat restriction flags (cleared each Cleanup or end of turn)
    bool goaded   = false;  // must attack if able; can't attack the player who goaded it
    uint8_t goadedBy = 255; // controller of the goading effect (255 = not goaded)

    // MustAttack: creature is forced to attack this turn if able.
    // mustAttackTarget = 255 means "any player"; otherwise the specific player id.
    bool    mustAttack       = false;
    uint8_t mustAttackTarget = 255;

    // MustBlock: this creature must block a specific attacker if able.
    // kInvalidId = no forced block. Cleared each Cleanup.
    ObjectId mustBlockTarget = kInvalidId;

    // S:Mode$ ActivateAbilityAsIfHaste: this permanent can use tap abilities
    // even if it has summoning sickness. Set by recomputeStaticBonuses().
    bool activateAbilityAsIfHaste = false;

    // S:Mode$ Continuous with RemoveAllAbilities$ True: this permanent has all
    // activated, triggered, and keyword abilities stripped (e.g. Humility).
    bool allAbilitiesRemoved = false;

    // NameCard: the card name designated by DB$ NameCard (e.g. Anointed Peacekeeper).
    // Used by Card.NamedCard filter in ValidCard$ / CantBeCast / RaiseCost checks.
    // Persists while the card is on the battlefield; cleared on zone change (new Card object).
    std::string namedCard;

    // MinMaxBlocker: maximum number of creatures that may block this permanent.
    // INT_MAX = no restriction. Set by S:Mode$ MinMaxBlocker in recomputeStaticBonuses.
    int maxBlockerCount = INT_MAX;

    // Game-mechanic attributes set by DB$ AlterAttribute (Suspected, Saddled, Prepared, etc.)
    // Cleared on zone change (new Card object).
    std::unordered_set<std::string> attributes;
    bool hasAttribute(std::string_view attr) const noexcept {
        return attributes.count(std::string(attr)) != 0;
    }

    // Phase-out flag (phased-out permanents are treated as if they don't exist)
    bool phasedOut = false;

    // Adventure flag: true when the card is in exile waiting to be cast as its creature face
    bool adventureExiled = false;

    // Cipher: id of the creature this card is encoded onto (kInvalidId = not encoded).
    // When non-invalid and the creature deals combat damage, you may cast a free copy.
    ObjectId cipheredOnto = kInvalidId;

    // Zone-change sequence number: incremented by GameState each time a card moves.
    // Used to display the graveyard in chronological (most-recent-first) order.
    uint32_t zoneChangeSeq = 0;

    // Face-down (manifested) flag: card entered as a 2/2 without its normal types
    bool manifested = false;

    // Combat participation flags — set during Declare Attackers/Blockers, cleared at EndCombat.
    bool attacking = false;
    bool blocking  = false;
    bool isBlocked = false;  // true if this attacker has at least one blocker assigned
    // Per-creature "attacked this turn" flag — used by Boast timing check.
    // Cleared each Cleanup step alongside other per-turn state.
    bool attackedThisTurn = false;

    // Keywords stripped by LoseAbility static effects (e.g. Humility).
    // Restored in recomputeStaticBonuses() reset before rebuilding.
    uint32_t removedKeywords = 0;

    // SetColor / ChangeColor effect: overrides this card's color identity.
    // 0xFF = use rules default; otherwise bitmask W=0x01 U=0x02 B=0x04 R=0x08 G=0x10.
    uint8_t colorIdOverride = 0xFF;

    // GainControl Duration$ EndOfTurn: 0xFF = not stolen; otherwise the original
    // controllerId to revert to at Cleanup.
    uint8_t originalControllerId = 0xFF;

    // Exert: set by the Exert effect; creature skips its next untap, then flag clears.
    bool exerted = false;

    // Monstrous: set by the Monstrosity effect; prevents re-triggering monstrosity.
    bool monstrous = false;

    // Enlist: power bonus from tapping a support creature while attacking.
    // Cleared at end of turn. Added into effectivePower for damage calculation.
    int enlistBonus = 0;

    // Renowned: set when a Renown creature has dealt combat damage to a player.
    bool renowned = false;

    // Miracle: set when this card was drawn as the first card of its controller's turn.
    // Cleared on zone change (new Card object has miracleEligible = false by default).
    bool miracleEligible = false;

    // Foretell: card was exiled face-down from hand via the Foretell action.
    // foretoldOnTurn records the turn it was foretold; must cast on a later turn.
    bool foretold       = false;
    int  foretoldOnTurn = -1;

    // Rebound: card was cast from hand and exiled by the Rebound replacement effect.
    // During the controller's next upkeep, they may cast it for free from exile.
    bool rebound = false;

    // Suspend: card was exiled from hand via the Suspend action.
    // TIME counters are stored in counterCount("TIME"). When the last is removed at
    // upkeep, the card is cast for free.
    bool suspended = false;

    // Champion: the ID (in Exile) of the creature this card championed on ETB.
    // kInvalidId if nothing is currently championed.
    ObjectId championedCreature = kInvalidId;

    // Tribute: set when the entering creature's tribute was paid (opponent put counters).
    // If true, the card-specific ETB bonus does NOT fire.
    bool tributed = false;

    // Cleave: set when this spell was cast for its Cleave cost.
    // Effects strip bracketed text ([text]) from oracle text before resolving.
    bool cleaved = false;

    // Exploit: set when the controller sacrificed a creature to trigger this card's bonus.
    bool exploited = false;

    // Room (Duskmourn): tracks which Doors have been unlocked.
    bool doorAUnlocked = false;
    bool doorBUnlocked = false;

    // Layer 3 text-changes: maps original text token → replacement token.
    // Applied by LayerEngine; cleared at start of each recomputeStaticBonuses cycle.
    std::unordered_map<std::string, std::string> textChanges;

    // Transform (DFC): true when showing the back face.
    // Cleared on zone change (new Card always starts as front face).
    bool transformed = false;

    // Escape: set when this card was cast from the graveyard via Escape cost.
    // Used by triggers checking ConditionNotPresent$ Card.Self+escaped (e.g. Uro/Kroxa
    // self-sacrifice triggers only fire when the card *didn't* escape).
    bool escaped = false;

    // Echo: set when the creature has been on the battlefield for one full upkeep.
    // On the NEXT upkeep the controller must pay the echo cost or sacrifice it.
    bool echoNeedsPayment = false;

    // Haunt: this card (in exile) is haunting a creature. When that creature dies,
    // the haunted effect fires again. hauntedCreatureId = the creature being haunted.
    ObjectId hauntedCreatureId = kInvalidId;

    // Case (Murders at Karlov Manor): set to true once the "To solve" condition is met.
    // Cases grant a permanent bonus after solving — this flag prevents re-evaluation.
    bool caseSolved = false;

    // Planeswalker: true if a loyalty ability has been activated this turn.
    // A planeswalker can only activate one loyalty ability per turn (rule 606.3).
    // Cleared during the Cleanup step.
    bool loyaltyUsedThisTurn = false;

    // Pre-computed variable P/T for cards whose power/toughness string contains '*'.
    // -1 means "not variable — use parseStat(rules->power/toughness)".
    // Set by GameState::recomputeStaticBonuses().
    int varPower    = -1;
    int varToughness= -1;

    // Override power/toughness to a fixed value from a continuous effect (Humility, etc.).
    // -1 = no override.  Takes precedence over var/base but NOT over counters.
    int setPower    = -1;
    int setToughness= -1;

    // Layer 7e: when true, power and toughness are exchanged in effectivePower/Toughness.
    // Set by LayerEngine; cleared in recomputeStaticBonuses() clear phase.
    bool swapPT = false;

    // Fast-path counters for the two most common types (avoids map lookup hot path).
    // These mirror the values in `counters` and are kept in sync by addCounter/removeCounter.
    int m_p1p1 = 0;   // "+1/+1" counter count
    int m_m1m1 = 0;   // "-1/-1" counter count

    // All counters (including the above two for completeness/serialisation).
    std::unordered_map<std::string, int> counters;

    // Per-game activation count per ability line index, for AB$ lines that
    // declare ActivationLimit$ N ("activate only once", e.g. Power-up). Never
    // reset during a game (a fresh Card is built each game).
    std::unordered_map<int, int> abilityUses;

    // Per-turn trigger-fire count keyed by Execute-SVar hash, backing
    // ActivationLimit$ N on T: lines ("triggers only once each turn").
    // Lives on the Card (its maps deep-copy safely in GameState::clone, unlike
    // a GameState-level map); cleared each turn by TurnManager.
    mutable std::unordered_map<uint64_t, int> triggerFiresThisTurn;
    // Returns true (and records a fire) if this card's trigger keyed by execName
    // may still fire under `limit` this turn; false once it has hit the limit.
    bool tryTriggerLimit(std::string_view execName, int limit) const {
        uint64_t key = std::hash<std::string_view>{}(execName);
        int& n = triggerFiresThisTurn[key];
        if (n >= limit) return false;
        ++n;
        return true;
    }

    // Attachments (auras, equipment, fortifications)
    ObjectId              attachedTo  = kInvalidId;
    std::vector<ObjectId> attachments;

    // Duration$ UntilHostLeavesPlay: the ObjectId of the battlefield permanent that caused
    // this card to be exiled. When that permanent leaves the battlefield, this card returns.
    // kInvalidId = not held in exile by a host.
    ObjectId exiledBy = kInvalidId;

    // ── Keyword queries ───────────────────────────────────────────────────

    bool hasKeyword(KeywordAbility kw) const noexcept {
        return maskHas(keywordMask, kw);
    }
    void grantKeyword(KeywordAbility kw) noexcept {
        keywordMask |= static_cast<uint32_t>(kw);
    }
    void revokeKeyword(KeywordAbility kw) noexcept {
        keywordMask &= ~static_cast<uint32_t>(kw);
    }

    // ── Convenience accessors ─────────────────────────────────────────────

    const std::string& name()  const noexcept { return rules->name; }
    bool isCreature()          const noexcept { return rules->isCreature() || tempIsCreature; }
    bool isLand()              const noexcept { return rules->isLand(); }
    bool isPermanent()         const noexcept { return rules->isPermanent(); }
    bool isOnBattlefield()     const noexcept { return zone == ZoneType::Battlefield; }

    // Returns true if this card is protected from sources with the given color identity
    // or type mask (DAET rule: Damage, Attached, Enchanted, Targeted, Blocked).
    // Color protection is stored in keywordMask via KeywordAbility::ProtectionWhite etc.
    bool hasProtectionFrom(uint8_t sourceColorMask, uint8_t sourceTypeMask = 0) const noexcept {
        if (!rules) return false;
        // Color protection via keyword mask
        if (sourceColorMask && ::mtg::hasProtectionFrom(keywordMask, sourceColorMask))
            return true;
        if (rules->protectionTypeMask  && (rules->protectionTypeMask  & sourceTypeMask))
            return true;
        return false;
    }

    int  counterCount(const std::string& type) const noexcept {
        // Fast path for the two most common counter types
        if (type == "+1/+1") return m_p1p1;
        if (type == "-1/-1") return m_m1m1;
        auto it = counters.find(type);
        return it != counters.end() ? it->second : 0;
    }
    void addCounter(const std::string& type, int n = 1) {
        if (type == "+1/+1") { m_p1p1 += n; counters[type] += n; return; }
        if (type == "-1/-1") { m_m1m1 += n; counters[type] += n; return; }
        counters[type] += n;
    }
    void removeCounter(const std::string& type, int n = 1) {
        if (type == "+1/+1") { m_p1p1 = std::max(0, m_p1p1 - n); }
        if (type == "-1/-1") { m_m1m1 = std::max(0, m_m1m1 - n); }
        auto it = counters.find(type);
        if (it != counters.end()) {
            it->second -= n;
            if (it->second <= 0) counters.erase(it);
        }
    }
};

} // namespace mtg
