#pragma once
#include "CardType.h"
#include "../mana/ManaCost.h"
#include <optional>
#include <string>
#include <vector>
#include <unordered_map>

namespace mtg {

// Static (immutable) data for a card, parsed from a Forge .txt script file.
// This is the card's identity — not its in-game state.
struct CardRules {
    std::string name;
    ManaCost    manaCost;
    CardType    type;

    std::string power;      // "" if not a creature; "*" if variable
    std::string toughness;

    std::vector<std::string> keywords;          // one entry per K: line
    std::vector<std::string> abilityLines;      // raw A: lines
    std::vector<std::string> triggerLines;      // raw T: lines
    std::vector<std::string> replacementLines;  // raw R: lines
    std::vector<std::string> staticAbilityLines;// raw S: lines (continuous effects)
    std::unordered_map<std::string, std::string> svars; // SVar name → body

    std::string oracleText;
    std::string altName;    // ALTNAME (e.g. split cards, adventures)
    int         initialLoyalty = 0; // planeswalkers only (from Loyalty: line)

    bool cantBeCountered = false;
    // Mandatory generic-mana surcharge to cast this spell (additional cost).
    // Models "as an additional cost to cast this spell, pay {N}". Parsed from
    // K:CastSurcharge:N. (Titania models "discard a card or pay {2}" as pay {2}.)
    int  castSurcharge = 0;
    ManaCost flashbackCost; // valid if hasFlashback
    ManaCost kickerCost;    // valid if hasKicker
    ManaCost buybackCost;   // valid if hasBuyback
    bool hasFlashback = false;
    bool hasPhasing   = false;  // K:Phasing — toggles phasedOut at owner's untap step
    bool hasAdventure = false;  // card has an Adventure half castable from hand
    ManaCost adventureCost;     // mana cost of the Adventure half
    std::string adventureName;  // name of the Adventure half (e.g. "Giant Killer")
    bool hasDisturb   = false;  // card can be cast from GY as enchantment face
    ManaCost disturbCost;
    bool hasKicker    = false;
    bool hasBuyback   = false;
    bool hasConvoke   = false; // tap creatures to help pay the mana cost
    bool hasImprovise = false; // tap artifacts to help pay the mana cost
    bool hasRetrace   = false; // cast from GY by discarding a land (pays base cost)
    ManaCost madnessCost; // non-NoCost if the card has the Madness keyword
    bool hasMadness   = false;
    bool hasCascade   = false; // reveal cards until finding cheaper non-land, cast free
    bool hasProwess   = false; // +1/+1 until EOT whenever you cast a non-creature spell
    ManaCost wardCost;          // counter targeting unless this cost is paid
    bool hasWard      = false;

    // ETB counter entries: each K:etbCounter:TYPE:AMOUNT line appends one entry.
    struct ETBCounterEntry {
        std::string counterType; // "P1P1", "M1M1", "CHARGE", etc.
        std::string amountSVar;  // plain number or SVar name e.g. "X", "2"
    };
    std::vector<ETBCounterEntry> etbCounters;

    // ETB copy entry: K:ETBReplacement:Copy:SVar[:Optional]
    struct ETBCopyEntry {
        std::string svarName; // SVar holding the DB$ Clone line (e.g. "DBCopy")
        bool optional = false;
    };
    std::optional<ETBCopyEntry> etbCopy;

    // Vehicles: K:Crew:N
    bool hasCrew  = false;
    int  crewCost = 0; // minimum total power of creatures to tap

    // Saga chapters: K:Chapter:N:SVar1,SVar2,...
    struct SagaEntry {
        int maxChapter = 0;
        std::vector<std::string> chapterSVars; // one per chapter (1-indexed: [0]=ch1)
    };
    std::optional<SagaEntry> saga;

    // Dredge N: when you would draw, you may instead mill N and return this from GY to hand
    bool hasDredge   = false;
    int  dredgeAmount = 0;

    // Evoke: alternative cost that sacrifices the creature after its ETB resolves
    ManaCost evokeCost;
    bool hasEvoke = false;

    // Delve: exile any number of cards from your GY to pay for {1} each
    bool hasDelve = false;

    // Bloodthirst N: enter with N +1/+1 counters if an opponent was dealt damage this turn
    bool hasBloodthirst    = false;
    int  bloodthirstAmount = 0;

    // Annihilator N: when attacks, defending player sacrifices N permanents
    bool hasAnnihilator   = false;
    int  annihilatorCount = 0;

    // Dash: alternative cast cost; creature ETBs with Haste and returns to hand at EOT
    bool     hasDash   = false;
    ManaCost dashCost;

    // Blitz: alternative cast cost; creature ETBs with Haste, sacrificed at EOT; draws a card on death
    bool     hasBlitz  = false;
    ManaCost blitzCost;

    // Connive N: draw N cards, then discard N; for each nonland discarded put a +1/+1 counter
    bool hasConnive    = false;
    int  conniveAmount = 1;

    // Bargain: sacrifice an artifact, enchantment, or token as additional cast cost
    // for a bonus effect.  The bonus is described in the card's SVar "Bargain".
    bool hasBargain = false;

    // Backup N: ETB — put N +1/+1 counters on another target creature; until EOT that
    // creature also gains one of this card's keyword abilities (the first K: line).
    bool hasBackup    = false;
    int  backupAmount = 1;

    // Encore N: {N}, Exile this from your graveyard → create token copies attacking
    // each opponent.  Tokens gain haste; sac at EOT.
    bool     hasEncore  = false;
    ManaCost encoreCost;

    // Type-based protection bitmask (parsed from "Protection from artifacts" etc.)
    // Uses ProtectionType enum from KeywordAbility.h.
    uint8_t protectionTypeMask = 0;

    // Note: color-based protection ("Protection from Red" etc.) is encoded in the
    // keywordMask via KeywordAbility::ProtectionWhite/Blue/Black/Red/Green/All.

    // Companion: this card can start the game as a companion if your deck meets its
    // condition. Once per game: {3}, put the companion from outside the game into
    // your hand. Condition is encoded in T: lines with Mode$ CompanionCondition.
    bool hasCompanion = false;

    // Epic: "For the rest of the game, you can't cast spells. At the beginning of each
    // of your upkeeps, copy this spell." The first part prevents future casting;
    // the upkeep copy is handled by a T: trigger line.
    bool hasEpic = false;

    // Banding: creatures with Banding may attack in a "band" — the attacking player
    // assigns combat damage from blockers to the band as a group, then distributes it.
    bool hasBanding = false;

    // Haunt: when this card is put into a graveyard from anywhere, exile it haunting
    // a target creature. When that creature dies, the haunt ability triggers again.
    // Haunt effects are encoded in the card's trigger lines with Mode$ Haunt.
    bool hasHaunt = false;

    // Disguise: like Morph (cast face-down for {3}) but the face-down permanent also
    // has ward {2}. Turn face-up by paying the disguise cost.
    bool     hasDisguise  = false;
    ManaCost disguiseCost;

    // Background: legendary enchantment that can be your second commander
    // when paired with a "Choose a Background" commander.
    bool hasBackground = false;
    bool choosesBackground = false;  // "Choose a Background" keyword on commander

    // Colour-specific hexproof bitmask (WUBRG; parsed from "Hexproof from Black" etc.)
    uint8_t hexproofFromColor = 0;

    // Graft N: ETB with N +1/+1 counters; when another creature enters the BF the
    // controller may move one counter from this creature to that creature.
    bool hasGraft   = false;
    int  graftCount = 0;

    // Manifest: cast face-down as a 2/2 colourless creature (no types/text).
    // The card can be turned face-up by paying its mana cost.
    bool hasManifest = false;

    // Sunburst: ETB with one +1/+1 counter for each colour of mana spent to cast.
    // Requires the engine to expose colour-count at cast time via a hint.
    bool hasSunburst = false;

    // Forecast: {cost}, Reveal this card from your hand during upkeep → get effect.
    bool     hasForecast = false;
    ManaCost forecastCost;

    // ── New mechanics ─────────────────────────────────────────────────────────

    // Myriad: when attacks, create a token copy attacking each other opponent (≥3p).
    bool hasMyriad = false;

    // Cipher: after this spell resolves, exile it encoded onto a creature you control.
    // Whenever that creature deals combat damage, you may cast the encoded copy free.
    bool hasCipher = false;

    // Casualty N: sacrifice a creature with power ≥ N to copy this spell at cast time.
    bool hasCasualty    = false;
    int  casualtyAmount = 1;

    // Replicate: pay an additional {cost} any number of times at cast time; each
    // payment creates a copy of the spell.  AI pays once if affordable.
    bool     hasReplicate  = false;
    ManaCost replicateCost;

    // Bestow: cast as an Aura for the bestow cost; becomes a creature if detached.
    bool     hasBestow  = false;
    ManaCost bestowCost;

    // Transmute: {cost}, Discard this card from hand: search for a card with the
    // same mana value.
    bool     hasTransmute  = false;
    ManaCost transmuteCost;

    // Flanking: blocking creature(s) get -1/-1 until EOT.
    bool hasFlanking = false;

    // Rampage N: for each creature blocking beyond the first, +N/+N until EOT.
    bool hasRampage    = false;
    int  rampageAmount = 1;

    // Boast: activate once per turn, only if this creature attacked this turn.
    // The ability is stored in the card's AB$ lines; hasBoast gates the timing check.
    bool hasBoast = false;

    // Strive: pay an additional {cost} for each extra target on a targeted spell.
    bool     hasStrive  = false;
    ManaCost striveCost;

    // Fortify N: {N}{Mana}, attach this to a land you control (like Equip for lands).
    bool     hasFortify  = false;
    ManaCost fortifyCost;

    // Reinforce N: {cost}, Discard this card: put N +1/+1 counters on target creature.
    bool     hasReinforce    = false;
    int      reinforceAmount = 1;
    ManaCost reinforceCost;

    // Amplify N: as this enters, for each creature card with a shared type you reveal
    // from your hand, it enters with N additional +1/+1 counters.
    bool hasAmplify    = false;
    int  amplifyAmount = 1;

    // Fading N: enters with N fading counters; remove one at upkeep; if none, sacrifice.
    bool hasFading    = false;
    int  fadingAmount = 0;

    // Vanishing N: enters with N time counters; remove one at upkeep; if none, exile.
    bool hasVanishing    = false;
    int  vanishingAmount = 0;

    // Battle Cry: whenever attacks, each other attacking creature you control gets
    // +1/+0 until EOT.
    bool hasBattleCry = false;

    // Conspire: as you cast this, may tap two untapped creatures sharing a color with
    // it you control to copy the spell.
    bool hasConspire = false;

    // Split card: this card has two independent halves (e.g. Fire // Ice).
    // splitName / splitCost / splitOracleText describe the second half.
    // The engine handles the split by letting the player choose which half to cast.
    bool        hasSplit         = false;
    std::string splitName;
    ManaCost    splitCost;
    std::string splitOracleText;
    std::string splitPower;
    std::string splitToughness;
    std::vector<std::string> splitAbilityLines;

    // Rally: triggered ability that fires whenever an Ally enters the BF
    // under your control.  Handled by standard T: trigger lines — no field needed.

    // Exalted: whenever a creature you control attacks alone, it gets +1/+1 until EOT.
    // hasExalted already exists (line ~293).  Trigger enforcement is in TriggerSystem.

    // Renown N: when deals combat damage to a player for the first time, put N +1/+1 counters
    bool hasRenown    = false;
    int  renownAmount = 0;

    // Mentor: when attacks, put a +1/+1 counter on another attacking creature with less power
    bool hasMentor = false;

    // Surge: alternative lower cost if you or a teammate cast a spell this turn
    bool     hasSurge  = false;
    ManaCost surgeCost;

    // Spectacle: alternative cost when an opponent has lost life this turn
    bool     hasSpectacle   = false;
    ManaCost spectacleCost;

    // Miracle: alternative cost when drawn as the first card this turn
    bool     hasMiracle   = false;
    ManaCost miracleCost;

    // Riot: on ETB, choose between haste or +1/+1 counter
    bool hasRiot = false;

    // Emerge: sacrifice a creature to reduce the cost by that creature's CMC
    bool     hasEmerge   = false;
    ManaCost emergeCost;

    // Afflict N: when blocked, the defending player loses N life
    bool hasAfflict    = false;
    int  afflictAmount = 0;

    // Champion: ETB exile a matching creature; when champion leaves, return it
    bool        hasChampion    = false;
    std::string championFilter; // e.g. "Creature.Elf", "Creature.Human Warrior"

    // Modular N: enters with N +1/+1 counters; when dies, transfer counters to an artifact creature
    bool hasModular   = false;
    int  modularCount = 0;

    // Unearth: from GY pay cost → return to BF with haste; exile at end of turn
    bool     hasUnearth   = false;
    ManaCost unearthCost;

    // Extort: when you cast a spell, may pay {W/B} to drain each opponent for 1 life
    bool hasExtort = false;

    // Soulshift N: when a creature with this keyword dies, return a Spirit with CMC ≤ N from GY to hand
    bool hasSoulshift    = false;
    int  soulshiftAmount = 0;

    // Tribute N: as creature ETBs, an opponent may put N +1/+1 counters on it
    // (if they don't, a bonus effect fires — in AI, opponent always pays tribute)
    bool hasTribute    = false;
    int  tributeAmount = 0;

    // Scavenge: from GY, pay cost + exile this → put +1/+1 counters = power on a creature
    bool     hasScavenge  = false;
    ManaCost scavengeCost;

    // Heroic: when you target this creature with a spell, put a +1/+1 counter on it
    bool hasHeroic = false;

    // Inspired: when this creature becomes untapped, fire its T:Mode$ Untap trigger
    bool hasInspired = false;

    // Evolve: when a creature with greater power or toughness ETBs under your control,
    // put a +1/+1 counter on this creature
    bool hasEvolve = false;

    // Monstrosity N: pay cost → put N +1/+1 counters and become monstrous (once only)
    bool     hasMonstrosity   = false;
    int      monstrosityCount = 0;
    ManaCost monstrosityCost;

    // Overload: alternative higher cost that affects all valid targets instead of one
    bool     hasOverload  = false;
    ManaCost overloadCost;

    // Embalm: from GY pay cost → exile this, create a white Zombie token copy (sorcery speed)
    bool     hasEmbalm  = false;
    ManaCost embalmCost;

    // Eternalize: like Embalm but the token is always 4/4
    bool     hasEternalize  = false;
    ManaCost eternalizeCost;

    // Foretell: pay {2} + exile from hand face-down; cast later for foretell cost
    bool     hasForetell  = false;
    ManaCost foretellCost;

    // Suspend: pay suspendCost from hand → exile with suspendCount TIME counters.
    // Each upkeep: remove 1 counter. When last is removed, cast for free.
    bool     hasSuspend      = false;
    int      suspendCount    = 0;     // number of time counters (0 = X-valued)
    bool     suspendCountIsX = false; // true when the count is X (variable)
    ManaCost suspendCost;

    // Cycling: pay cost + discard → draw a card
    bool hasCycling   = false;
    ManaCost cyclingCost;

    // Rebound: when cast from hand, exile as it resolves; cast for free at start of next upkeep.
    bool hasRebound = false;

    // Escape: cast from graveyard for escapeCost by exiling escapeExile other GY cards.
    bool     hasEscape    = false;
    int      escapeExile  = 0;   // number of other GY cards to exile
    ManaCost escapeCost;

    // Jump-start: cast from graveyard for the same cost by also discarding a card.
    bool hasJumpStart = false;

    // Fabricate N: on ETB, choose to put N +1/+1 counters OR create N 1/1 colorless Servo tokens.
    bool hasFabricate    = false;
    int  fabricateAmount = 0;

    // TypeCycling (landcycling variants): pay cost + discard → search for a matching land
    // cyclingType is "Basic", "Swamp", "Forest", etc.
    bool hasTypeCycling = false;
    std::string cyclingType;
    ManaCost typeCyclingCost;

    // Echo: at the beginning of your next upkeep after the creature ETBs, pay this cost
    // or sacrifice the creature.
    bool     hasEcho   = false;
    ManaCost echoCost;

    // Cumulative Upkeep: each upkeep, put an age counter on this; pay N×cost or sacrifice.
    // When the body starts with "AddCounter<N/TYPE>:", the "cost" is instead a counter action.
    bool        hasCumulativeUpkeep   = false;
    ManaCost    cumulativeUpkeepCost;          // mana cost to pay per age counter (may be empty)
    bool        cumulativeUpkeepIsCounter = false; // true when the effect is "add counter" not pay mana
    std::string cumulativeUpkeepCounterType;   // e.g. "M1M1" when isCounter==true

    // Affinity: reduce mana cost by 1 for each permanent of the given type you control.
    // affinityType is "Artifacts", "Forests", "Plains", "Islands", etc.
    bool        hasAffinity   = false;
    std::string affinityType;

    // Toxic N: this creature deals N poison counters when it deals combat damage to a player.
    bool hasToxic    = false;
    int  toxicAmount = 0;

    // Exalted: when a creature you control attacks alone, it gets +1/+1 until end of turn
    // for each creature with Exalted you control.
    bool hasExalted = false;

    // Training: when this creature attacks alongside a creature with greater power,
    // put a +1/+1 counter on this creature.
    bool hasTraining = false;

    // Ninjutsu: return an unblocked attacking creature you control to its owner's hand
    // and put this card from your hand onto the battlefield tapped and attacking.
    bool     hasNinjutsu  = false;
    ManaCost ninjutsuCost;

    // Level Up: sorcery-speed activated ability that puts a LEVEL counter on this creature.
    // Level bands define P/T and abilities at each level range.
    struct LevelBand {
        int minLevel = 0;
        int maxLevel = 9999;  // 9999 = open-ended (X+)
        std::string power;
        std::string toughness;
        std::vector<std::string> keywords;  // extra keywords granted at this level
    };
    std::vector<LevelBand> levelBands;
    // Level bands are encoded as S:Mode$ Continuous lines with IsPresent$ counter conditions.
    bool     hasLevelUp  = false;
    ManaCost levelUpCost;

    // Morph: cast face-down as a 2/2 for {3}; pay morphCost to turn face-up at instant speed.
    bool     hasMorph   = false;
    ManaCost morphCost;
    // Megamorph: like Morph but also puts a +1/+1 counter on the card when turned face-up.
    bool     hasMegamorph   = false;
    ManaCost megamorphCost;

    // Prototype: alternate lower cost that casts the card as a reduced-stats artifact creature.
    // When cast for prototype cost, the card becomes an artifact with the prototype P/T instead.
    bool        hasPrototype     = false;
    ManaCost    prototypeCost;
    std::string prototypePower;
    std::string prototypeToughness;

    // Cases (Murders at Karlov Manor): enchantments that advance through conditions.
    // A Case checks its "To solve" condition each upkeep; once solved, the bonus is permanent.
    // The condition and bonus are encoded in T: and S: lines with Mode$ Case conditions.
    bool hasCase = false;

    // Channel: pay a cost and discard this card from hand to activate an ability.
    // Encoded as an AB$ line with ActivationZone$ Hand and Discard<1/Self> in the cost.
    // hasChanmnel is set when such a line exists (detection-only; actual activation
    // goes through the standard activateAbility path).
    bool hasChannel = false;

    // Meld: this card can be exiled together with meldPartner to create meldResult.
    // Both components must be on the battlefield; the active player exiles both,
    // then creates the meld permanent (a new token-like card) on the battlefield.
    bool        hasMeld      = false;
    std::string meldPartner;   // name of the other required card
    std::string meldResult;    // name of the resulting meld permanent

    // Cleave: alternate higher cost that removes bracketed text from the effect.
    bool     hasCleave  = false;
    ManaCost cleaveCost;

    // Exploit: when this creature ETBs, you may sacrifice a creature for a bonus.
    bool hasExploit = false;

    // Outlast: {cost}, {T}: put a +1/+1 counter on this creature. Sorcery speed.
    bool     hasOutlast  = false;
    ManaCost outlastCost;

    // Enlist: while attacking, tap another untapped creature you control
    // to add its power to this creature's power until end of turn.
    bool hasEnlist = false;

    // Prowl: alternate cheaper cost if a creature with Prowl dealt combat damage
    // to a player this turn.
    bool     hasProwl  = false;
    ManaCost prowlCost;

    // Adapt N: {cost}: if this creature has no +1/+1 counters on it,
    // put N +1/+1 counters on it.
    bool     hasAdapt    = false;
    int      adaptAmount = 0;
    ManaCost adaptCost;

    // Partner with: these two specific named cards can be co-commanders.
    bool        hasPartnerWith  = false;
    std::string partnerWithName;    // name of the other half

    // Aftermath: the right side of a split card castable only from graveyard.
    bool hasAftermath = false;

    // Room (Duskmourn 2024): enchantment with two Doors, each unlocked by paying a cost.
    // When both Doors are unlocked the room may grant an additional bonus.
    // Doors are encoded as two AB$ lines with Mode$UnlockDoor.
    bool hasRoom = false;
    // doorACost / doorBCost: parsed from the two UnlockDoor AB$ lines.
    ManaCost doorACost;
    ManaCost doorBCost;

    // Split Second: while this spell is on the stack, players can't cast spells
    // or activate non-mana abilities (rule 702.61).
    bool hasSplitSecond = false;

    // Double-faced card: pointer to the other face's rules in CardDb.
    // Set by CardDb::wireBackFaces() after loading. Null for single-faced cards.
    const CardRules* backFace = nullptr;

    // Convenience
    bool hasPT()       const noexcept { return !power.empty(); }
    int  cmc()         const noexcept { return manaCost.cmc(); }
    bool isCreature()  const noexcept { return type.isCreature(); }
    bool isLand()      const noexcept { return type.isLand(); }
    bool isPermanent() const noexcept { return type.isPermanent(); }

    bool hasKeyword(std::string_view kw) const noexcept;
};

} // namespace mtg
