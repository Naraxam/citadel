#include "DeckEditorScreen.h"
#include "CardDownloadScreen.h"
#include "CardImageDownloader.h"
#include "CardRenderer.h"
#include "UiScale.h"
#include "../core/mana/ManaAtom.h"
#include "../game/DeckLoader.h"
#include "../game/ai/EdhrecClient.h"
#include <SFML/Window/Clipboard.hpp>
#include <thread>
#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <unordered_set>
#include <sstream>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numeric>
#include <sstream>

namespace ui {

namespace fs = std::filesystem;

// ── Color palette — Citadel design system ─────────────────────────────────────
// Warm charcoal + gold/ember, matching design/styles.css
static const sf::Color kBg          {11,  10,   9};    // --bg-0  deepest void
static const sf::Color kDeckPanelBg {18,  17,  16};    // --bg-1  app base
static const sf::Color kRowEven     {26,  24,  22};    // --bg-2  panel
static const sf::Color kRowOdd      {26,  24,  22};    // same — no alternating in Citadel
static const sf::Color kRowHover    {34,  31,  27};    // --bg-3  elevated
static const sf::Color kRowDeck     {44,  34,  14};    // warm gold tint for "in deck"
static const sf::Color kHdrBg       {11,  10,   9};    // --bg-0  header
static const sf::Color kHdrText     {148, 139, 124};   // --ink-3 muted
static const sf::Color kBody        {241, 234, 220};   // --ink   warm off-white
static const sf::Color kDim         {107,  99,  87};   // --ink-4 faint
static const sf::Color kAccent      {203, 163,  90};   // --gold  accent
static const sf::Color kGreen       {203, 163,  90};   // --gold  for valid/in-deck
static const sf::Color kRed         {214,  91,  74};   // --bad
static const sf::Color kBorder      {240, 220, 180, 30}; // --line warm gold hairline

// Tiny WUBRG colour-identity dots (defined lower; used by curve header + load menu).
static void deckEditorColorPips(sf::RenderTarget& t, float x, float y, uint8_t ci);

// ── Static helpers ─────────────────────────────────────────────────────────────

int DeckEditorScreen::typeGroup(const mtg::CardRules* r) {
    auto& t = r->type;
    if (t.isCreature())     return 0;
    if (t.isPlaneswalker()) return 1;
    if (t.isInstant())      return 2;
    if (t.isSorcery())      return 3;
    if (t.isEnchantment())  return 4;
    if (t.isArtifact())     return 5;
    if (t.isLand())         return 6;
    return 7;
}

const char* DeckEditorScreen::typeGroupName(int g) {
    switch (g) {
        case 0: return "Creatures";
        case 1: return "Planeswalkers";
        case 2: return "Instants";
        case 3: return "Sorceries";
        case 4: return "Enchantments";
        case 5: return "Artifacts";
        case 6: return "Lands";
        default: return "Other";
    }
}

std::string DeckEditorScreen::manaCostStr(const mtg::CardRules* r) {
    return r->manaCost.toString();
}

std::string DeckEditorScreen::typeStr(const mtg::CardRules* r) {
    return r->type.toString();
}

std::string DeckEditorScreen::ptStr(const mtg::CardRules* r) {
    if (!r->hasPT()) return "";
    return r->power + "/" + r->toughness;
}

sf::Color DeckEditorScreen::colorForCard(const mtg::CardRules* r) {
    // Citadel: darker, desaturated card colours matching premium feel
    if (r->type.isLand()) return sf::Color(52, 44, 30);   // dark warm brown
    uint8_t ci = r->manaCost.colorIdentity();
    int bits = 0;
    for (uint8_t m = ci; m; m >>= 1) bits += (m & 1);
    if (bits == 0) return sf::Color(52, 48, 42);           // colorless — warm grey
    if (bits >= 2) return sf::Color(72, 58, 24);           // multicolour — deep gold
    if (ci & mtg::ManaAtom::WHITE) return sf::Color(68, 60, 44);
    if (ci & mtg::ManaAtom::BLUE)  return sf::Color(24, 46, 76);
    if (ci & mtg::ManaAtom::BLACK) return sf::Color(36, 26, 50);
    if (ci & mtg::ManaAtom::RED)   return sf::Color(70, 26, 18);
    if (ci & mtg::ManaAtom::GREEN) return sf::Color(24, 52, 32);
    return sf::Color(52, 48, 42);
}

// ── Constructor ───────────────────────────────────────────────────────────────

DeckEditorScreen::DeckEditorScreen(const sf::Font& font,
                                   const mtg::CardDb& db,
                                   const fs::path& decksRoot,
                                   std::string picsDir,
                                   const mtg::EditionDb* editions)
    : m_font(font), m_db(db), m_editions(editions),
      m_decksRoot(decksRoot), m_picsDir(std::move(picsDir))
{
    buildCatalog();
    applyFilters();
    scanSavedDecks();
    scanAiDecks();
}

// ── Catalog ───────────────────────────────────────────────────────────────────

void DeckEditorScreen::buildCatalog() {
    m_allCards.clear();
    m_allCards.reserve(m_db.size());
    for (auto& [name, rules] : m_db)
        m_allCards.push_back(&rules);
    std::sort(m_allCards.begin(), m_allCards.end(),
              [](const mtg::CardRules* a, const mtg::CardRules* b) {
                  return a->name < b->name;
              });
}

void DeckEditorScreen::refreshCatalog() {
    buildCatalog();
    applyFilters();
}

void DeckEditorScreen::applyFilters() {
    m_filtered.clear();
    m_filtered.reserve(m_allCards.size());

    bool anyColor = false;
    for (bool b : m_colorFilter) if (b) { anyColor = true; break; }

    std::string lsearch = m_searchText;
    for (char& c : lsearch) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    static const char* TYPE_NAMES[] = {
        "All","Creature","Land","Instant","Sorcery",
        "Enchantment","Artifact","Planeswalker"
    };

    for (int i = 0; i < (int)m_allCards.size(); ++i) {
        const auto* r = m_allCards[i];

        if (anyColor) {
            uint8_t ci = r->manaCost.colorIdentity();
            bool colorless = (ci == 0) && !r->type.isLand();
            bool pass = false;
            if (m_colorFilter[0] && (ci & mtg::ManaAtom::WHITE)) pass = true;
            if (m_colorFilter[1] && (ci & mtg::ManaAtom::BLUE))  pass = true;
            if (m_colorFilter[2] && (ci & mtg::ManaAtom::BLACK)) pass = true;
            if (m_colorFilter[3] && (ci & mtg::ManaAtom::RED))   pass = true;
            if (m_colorFilter[4] && (ci & mtg::ManaAtom::GREEN)) pass = true;
            if (m_colorFilter[5] && colorless)                   pass = true;
            if (!pass) continue;
        }

        if (m_typeFilterIdx > 0 && m_typeFilterIdx < 8) {
            const char* tf = TYPE_NAMES[m_typeFilterIdx];
            bool pass = false;
            if      (std::strcmp(tf, "Creature")     == 0) pass = r->type.isCreature();
            else if (std::strcmp(tf, "Land")         == 0) pass = r->type.isLand();
            else if (std::strcmp(tf, "Instant")      == 0) pass = r->type.isInstant();
            else if (std::strcmp(tf, "Sorcery")      == 0) pass = r->type.isSorcery();
            else if (std::strcmp(tf, "Enchantment")  == 0) pass = r->type.isEnchantment();
            else if (std::strcmp(tf, "Artifact")     == 0) pass = r->type.isArtifact();
            else if (std::strcmp(tf, "Planeswalker") == 0) pass = r->type.isPlaneswalker();
            if (!pass) continue;
        }

        if (!lsearch.empty()) {
            std::string lname = r->name;
            for (char& c : lname) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (lname.find(lsearch) == std::string::npos) {
                std::string ltype = r->type.toString();
                for (char& c : ltype) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (ltype.find(lsearch) == std::string::npos) continue;
            }
        }

        // Owned-only filter: skip cards not seen in any game yet
        if (m_ownedOnly && m_collection) {
            if (m_collection->find(r->name) == m_collection->end()) continue;
        }

        // Mana curve filter: skip cards not matching the clicked CMC bar
        if (m_cmcFilter >= 0) {
            int cardCmc = std::min(r->cmc(), 7);
            if (cardCmc != m_cmcFilter) continue;
        }

        // Hide Alchemy reprints (the "A-CardName" digital-only variants).
        if (m_hideAlchemy && r->name.size() >= 2 &&
            (r->name[0] == 'A' || r->name[0] == 'a') && r->name[1] == '-')
            continue;

        // Set filter — show only cards printed in the chosen set code.
        // We don't (yet) split rows by printing, so a card with multiple
        // printings still appears once but is hidden when the active set
        // doesn't contain it.
        if (!m_setFilter.empty() && m_editions) {
            if (!m_editions->cardInSet(r->name, m_setFilter)) continue;
        }

        m_filtered.push_back(i);
    }

    // Sort filtered results by current sort mode
    std::sort(m_filtered.begin(), m_filtered.end(),
        [&](int ai, int bi) {
            const auto* a = m_allCards[ai];
            const auto* b = m_allCards[bi];
            bool lt = false;
            switch (m_sortBy) {
                case SortBy::Name:  lt = a->name < b->name; break;
                case SortBy::Cmc:   lt = a->cmc() < b->cmc() ||
                                         (a->cmc() == b->cmc() && a->name < b->name); break;
                case SortBy::Color: {
                    // Canonical color-identity grouping: mono colours in WUBRG
                    // order first, then multicolour (grouped by identity), then
                    // colourless, then lands — the conventional card-browser sort.
                    auto rank = [](const mtg::CardRules* r) -> int {
                        uint8_t ci = r->manaCost.colorIdentity();
                        int n = 0; for (uint8_t v = ci; v; v &= v - 1) ++n;
                        if (r->type.isLand()) return 800;
                        if (ci == 0)          return 700;   // colourless non-land
                        if (n == 1) {
                            if (ci & mtg::ManaAtom::WHITE) return 100;
                            if (ci & mtg::ManaAtom::BLUE)  return 110;
                            if (ci & mtg::ManaAtom::BLACK) return 120;
                            if (ci & mtg::ManaAtom::RED)   return 130;
                            if (ci & mtg::ManaAtom::GREEN) return 140;
                        }
                        return 400 + static_cast<int>(ci);  // multicolour by identity
                    };
                    int ra = rank(a), rb = rank(b);
                    lt = ra < rb || (ra == rb && a->name < b->name);
                    break;
                }
                case SortBy::Type:  lt = a->type.toString() < b->type.toString() ||
                                         (a->type.toString() == b->type.toString() && a->name < b->name);
                                    break;
            }
            return m_sortAsc ? lt : !lt;
        });
}

// ── File scanning ─────────────────────────────────────────────────────────────

// Both deck scans now look only in the user's appdata decks folder. We used
// to recurse through `m_decksRoot` (which is the Forge res/ tree — hundreds
// of thousands of files including all of cardsfolder/) and stat every file
// looking for .dck — that froze the deck editor open and the whole startup
// when the screen was built. All curated decks (precons + EDHREC imports)
// live in the appdata dir anyway.
static std::filesystem::path userDecksDir() {
    if (const char* ap = std::getenv("APPDATA"))
        return std::filesystem::path(ap) / "CitadelMTG" / "decks";
    return {};
}

void DeckEditorScreen::scanSavedDecks() {
    m_savedDecks.clear();
    m_savedNames.clear();
    m_savedCI.clear();
    m_savedCIKnown.clear();
    auto dir = userDecksDir();
    if (dir.empty() || !fs::exists(dir)) { rebuildLoadFilter(); return; }
    try {
        for (auto& entry : fs::directory_iterator(dir)) {
            if (entry.path().extension() == ".dck")
                m_savedDecks.push_back(entry.path());
        }
    } catch (...) {}
    std::sort(m_savedDecks.begin(), m_savedDecks.end());
    for (auto& p : m_savedDecks) {
        m_savedNames.push_back(p.stem().string());
        bool known = false;
        m_savedCI.push_back(deckColorIdentity(p, known));
        m_savedCIKnown.push_back(known ? 1 : 0);
    }
    rebuildLoadFilter();
}

// Combined colour identity (WUBRG bits) of a deck's commander(s).
uint8_t DeckEditorScreen::deckColorIdentity(const fs::path& deck, bool& known) const {
    known = false;
    uint8_t ci = 0;
    std::ifstream f(deck);
    if (!f) return 0;
    std::string line;
    bool inCmd = false;
    while (std::getline(f, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' ||
                                  line.back() == '\t'))
            line.pop_back();
        if (line.empty()) continue;
        if (line.front() == '[') {
            std::string lc;
            for (char c : line) lc += static_cast<char>(std::tolower((unsigned char)c));
            inCmd = (lc == "[commander]");
            continue;
        }
        if (!inCmd) continue;
        auto sp = line.find(' ');
        std::string name = (sp == std::string::npos) ? line : line.substr(sp + 1);
        auto pipe = name.find('|');
        if (pipe != std::string::npos) name = name.substr(0, pipe);
        while (!name.empty() && (name.back() == ' ' || name.back() == '\t'))
            name.pop_back();
        if (const mtg::CardRules* r = m_db.find(name)) {
            ci |= r->manaCost.colorIdentity();
            known = true;
        }
    }
    return ci;
}

void DeckEditorScreen::rebuildLoadFilter() {
    m_loadVisible.clear();
    std::string needle = m_loadFilter;
    for (char& c : needle) c = static_cast<char>(std::tolower((unsigned char)c));
    for (int i = 0; i < (int)m_savedDecks.size(); ++i) {
        if (needle.empty()) { m_loadVisible.push_back(i); continue; }
        std::string nm = m_savedNames[i];
        for (char& c : nm) c = static_cast<char>(std::tolower((unsigned char)c));
        if (nm.find(needle) != std::string::npos) m_loadVisible.push_back(i);
    }
}

void DeckEditorScreen::scanAiDecks() {
    m_aiDecks.clear();
    m_aiDeckNames.clear();
    auto dir = userDecksDir();
    if (dir.empty() || !fs::exists(dir)) return;
    try {
        for (auto& entry : fs::directory_iterator(dir)) {
            if (entry.path().extension() == ".dck")
                m_aiDecks.push_back(entry.path());
        }
    } catch (...) {}
    std::sort(m_aiDecks.begin(), m_aiDecks.end());
    for (auto& p : m_aiDecks) m_aiDeckNames.push_back(p.stem().string());
    if (!m_aiDecks.empty()) {
        m_aiDeckIdx  = 0;
        m_aiDeckPath = m_aiDecks[0];
    }
}

// ── Deck operations ───────────────────────────────────────────────────────────

int DeckEditorScreen::maxCopies(const mtg::CardRules* r) const {
    if (r->type.isBasic()) return 99;
    return 1;  // Commander: always singleton
}

bool DeckEditorScreen::isValidDeck() const {
    int total = totalDeckCards();
    if (total != 100) return false;
    if (!m_commanderRules) return false;
    // At least the primary commander must be in the deck
    bool foundCmd = false;
    for (auto& e : m_deck)
        if (e.rules == m_commanderRules) { foundCmd = true; break; }
    if (!foundCmd) return false;
    // Background partner validation: if commander has "Choose a Background", allow
    // a Background legendary enchantment as the second commander.
    if (m_partnerRules &&
        m_commanderRules->choosesBackground &&
        !m_partnerRules->hasBackground)
        return false;  // partner must be a Background

    // Color identity check: every card must be within the commander's color identity
    uint8_t cmdCI = m_commanderRules->manaCost.colorIdentity();
    if (m_partnerRules) cmdCI |= m_partnerRules->manaCost.colorIdentity();
    for (auto& e : m_deck) {
        if (e.rules == m_commanderRules || e.rules == m_partnerRules) continue;
        if (e.rules->type.isBasic()) continue;  // basic lands are always legal
        uint8_t cardCI = e.rules->manaCost.colorIdentity();
        if ((cardCI & ~cmdCI) != 0) return false;  // off-color card found
    }
    return true;
}

std::string DeckEditorScreen::validationMessage() const {
    int total = totalDeckCards();
    if (!m_commanderRules) {
        if (total < 100) return std::to_string(total) + "/100 cards — need a commander (right-click)";
        return std::to_string(total) + " cards — need a commander (right-click a legendary creature)";
    }
    if (total < 100) return std::to_string(total) + "/100 cards (need " + std::to_string(100 - total) + " more)";
    if (total > 100) return std::to_string(total) + " cards — remove " + std::to_string(total - 100) + " card(s)";
    // Check singleton violations
    for (auto& e : m_deck) {
        if (e.rules->type.isBasic()) continue;
        if (e.rules == m_commanderRules || e.rules == m_partnerRules) continue;
        if (e.count > 1)
            return "Too many copies: " + e.rules->name + " (only 1 allowed)";
    }
    // Check color identity violations — report first offender with color hint
    uint8_t cmdCI = m_commanderRules->manaCost.colorIdentity();
    if (m_partnerRules) cmdCI |= m_partnerRules->manaCost.colorIdentity();
    static const char* kColorNames[] = {"W","U","B","R","G"};
    static const uint8_t kColorBits[] = {0x01,0x02,0x04,0x08,0x10};
    for (auto& e : m_deck) {
        if (e.rules == m_commanderRules || e.rules == m_partnerRules) continue;
        if (e.rules->type.isBasic()) continue;
        uint8_t cardCI = e.rules->manaCost.colorIdentity();
        uint8_t illegal = cardCI & ~cmdCI;
        if (illegal != 0) {
            std::string colors;
            for (int ci = 0; ci < 5; ++ci)
                if (illegal & kColorBits[ci]) { if (!colors.empty()) colors += '/'; colors += kColorNames[ci]; }
            return e.rules->name + " has " + colors + " outside commander identity";
        }
    }
    // Format legality: check for common banned cards by name
    // (simplified ban list — the full list would come from a downloaded JSON)
    static const std::unordered_set<std::string> kCommanderBanned = {
        "Ancestral Recall", "Balance", "Black Lotus", "Channel",
        "Emrakul, the Aeons Torn", "Erayo, Soratami Ascendant",
        "Fastbond", "Golos, Tireless Pilgrim", "Griselbrand",
        "Hulk Flash", "Leovold, Emissary of Trest",
        "Library of Alexandria", "Limited Resources",
        "Lutri, the Spellchaser", "Mox Emerald", "Mox Jet",
        "Mox Pearl", "Mox Ruby", "Mox Sapphire",
        "Panoptic Mirror", "Primeval Titan", "Prophet of Kruphix",
        "Recurring Nightmare", "Rofellos, Llanowar Emissary",
        "Sundering Titan", "Sway of the Stars",
        "Sylvan Primordial", "Time Stretch", "Time Walk",
        "Tinker", "Tolarian Academy", "Trade Secrets",
        "Upheaval", "Yawgmoth's Bargain",
    };
    for (const auto& e : m_deck) {
        if (kCommanderBanned.count(e.rules->name))
            return e.rules->name + " is BANNED in Commander!";
    }
    if (m_commanderRules && kCommanderBanned.count(m_commanderRules->name))
        return m_commanderRules->name + " is BANNED as Commander!";

    // Deck archetype auto-detection
    int rampCount = 0, drawCount = 0, removalCount = 0, comboCount = 0;
    for (auto& e : m_deck) {
        const std::string& oracle = e.rules->oracleText;
        std::string lo = oracle;
        for (char& c : lo) c = static_cast<char>(std::tolower((unsigned char)c));
        if (lo.find("add") != std::string::npos && lo.find("mana") != std::string::npos)
            ++rampCount;
        if (lo.find("draw") != std::string::npos && lo.find("card") != std::string::npos)
            ++drawCount;
        if (lo.find("destroy") != std::string::npos || lo.find("exile") != std::string::npos)
            ++removalCount;
        if (e.rules->hasCascade || e.rules->hasKicker) ++comboCount;
    }
    std::string archetype;
    if (rampCount >= 12 && drawCount >= 8) archetype = " [Midrange]";
    else if (rampCount >= 15)              archetype = " [Ramp]";
    else if (removalCount >= 15)           archetype = " [Control]";
    else if (comboCount >= 5)              archetype = " [Combo]";
    else if (drawCount >= 12)              archetype = " [Card Draw]";
    return "100 cards  [Commander OK]" + archetype;
}

int DeckEditorScreen::deckCount(const mtg::CardRules* r) const {
    for (auto& e : m_deck) if (e.rules == r) return e.count;
    return 0;
}

int DeckEditorScreen::totalDeckCards() const {
    int n = 0;
    for (auto& e : m_deck) n += e.count;
    return n;
}

void DeckEditorScreen::addCard(const mtg::CardRules* r, int n) {
    int cap = maxCopies(r);
    for (auto& e : m_deck) {
        if (e.rules == r) {
            e.count = std::min(e.count + n, cap);
            m_deckRowsDirty = true;
            return;
        }
    }
    m_deck.push_back({r, std::min(n, cap)});
    m_deckRowsDirty = true;
}

void DeckEditorScreen::removeCard(const mtg::CardRules* r, int n) {
    for (auto it = m_deck.begin(); it != m_deck.end(); ++it) {
        if (it->rules == r) {
            it->count -= n;
            if (it->count <= 0) m_deck.erase(it);
            m_deckRowsDirty = true;
            return;
        }
    }
}

void DeckEditorScreen::rebuildDeckRows() const {
    if (!m_deckRowsDirty) return;
    m_deckRows.clear();

    // Commander(s) pinned to the top
    if (m_commanderRules) {
        m_deckRows.push_back({true, "COMMANDER", nullptr, 0});
        int cnt = 1;
        for (auto& e : m_deck) if (e.rules == m_commanderRules) { cnt = e.count; break; }
        m_deckRows.push_back({false, {}, m_commanderRules, cnt});
        // Partner commander (second slot)
        if (m_partnerRules) {
            int cnt2 = 1;
            for (auto& e : m_deck) if (e.rules == m_partnerRules) { cnt2 = e.count; break; }
            m_deckRows.push_back({false, {}, m_partnerRules, cnt2});
        }
    }

    // Companion pinned below the commander(s).
    if (m_companionRules) {
        m_deckRows.push_back({true, "COMPANION", nullptr, 0});
        m_deckRows.push_back({false, {}, m_companionRules, 1});
    }

    std::vector<const Entry*> sorted;
    sorted.reserve(m_deck.size());
    for (auto& e : m_deck) {
        if (e.rules != m_commanderRules && e.rules != m_partnerRules &&
            e.rules != m_companionRules) sorted.push_back(&e);
    }
    std::sort(sorted.begin(), sorted.end(), [](const Entry* a, const Entry* b) {
        int ga = typeGroup(a->rules), gb = typeGroup(b->rules);
        if (ga != gb) return ga < gb;
        return a->rules->name < b->rules->name;
    });

    int curGroup = -1;
    for (auto* ep : sorted) {
        int g = typeGroup(ep->rules);
        if (g != curGroup) {
            int groupTotal = 0;
            for (auto* p : sorted) if (typeGroup(p->rules) == g) groupTotal += p->count;
            DeckRow hdr;
            hdr.isHeader = true;
            hdr.header   = std::string(typeGroupName(g)) +
                            " (" + std::to_string(groupTotal) + ")";
            hdr.rules = nullptr; hdr.count = 0;
            m_deckRows.push_back(hdr);
            curGroup = g;
        }
        m_deckRows.push_back({false, {}, ep->rules, ep->count});
    }
    m_deckRowsDirty = false;
}

mtg::DeckLoader::Deck DeckEditorScreen::buildDeck() const {
    mtg::DeckLoader::Deck deck;
    deck.name = m_deckName;
    for (auto& e : m_deck) {
        if (e.rules == m_commanderRules || e.rules == m_partnerRules)
            deck.commander.push_back({e.rules->name, 1});
        else if (e.rules == m_companionRules)
            deck.companion.push_back({e.rules->name, 1});   // set aside, not in 99
        else
            deck.mainboard.push_back({e.rules->name, e.count});
    }
    return deck;
}

mtg::DeckLoader::Deck DeckEditorScreen::playerDeck() const { return buildDeck(); }

void DeckEditorScreen::saveDeck() {
    // Save to the SAME directory the load menu / match setup scan
    // (%APPDATA%/CitadelMTG/decks), not m_decksRoot (which is the bundled res
    // tree). Writing elsewhere is why saved decks "disappeared" — they were on
    // disk but in a folder nothing reads.
    fs::path dir = userDecksDir();
    if (dir.empty()) {
        std::cerr << "[DeckEditor] No %APPDATA% deck dir — cannot save.\n";
        return;
    }
    std::error_code mkec;
    fs::create_directories(dir, mkec);

    std::string fname = m_deckName;
    for (char& c : fname)
        if (c=='/'||c=='\\'||c==':'||c=='*'||c=='?'||c=='"'||c=='<'||c=='>'||c=='|') c='_';
    if (fname.empty()) fname = "NewDeck";

    fs::path path = dir / (fname + ".dck");
    std::ofstream out(path);
    if (!out) { std::cerr << "[DeckEditor] Save failed: " << path << '\n'; return; }
    out << "[metadata]\nName=" << m_deckName << "\n";
    if (m_commanderRules) {
        out << "\n[Commander]\n1 " << m_commanderRules->name << "\n";
        if (m_partnerRules)
            out << "1 " << m_partnerRules->name << "\n";
    }
    if (m_companionRules)
        out << "\n[Companion]\n1 " << m_companionRules->name << "\n";
    out << "\n[Main]\n";
    for (auto& e : m_deck) {
        if (e.rules != m_commanderRules && e.rules != m_partnerRules &&
            e.rules != m_companionRules)
            out << e.count << " " << e.rules->name << "\n";
    }
    // If the name changed and an old file existed, remove the old file (rename).
    if (!m_loadedPath.empty() && fs::exists(m_loadedPath) && m_loadedPath != path) {
        std::error_code ec;
        fs::remove(m_loadedPath, ec);
        if (!ec) std::cout << "[DeckEditor] Renamed: " << m_loadedPath << " -> " << path << '\n';
    }
    m_loadedPath = path;
    std::cout << "[DeckEditor] Saved: " << path << '\n';
    scanSavedDecks();
    scanAiDecks();
}

void DeckEditorScreen::deleteDeck() {
    if (m_loadedPath.empty() || !fs::exists(m_loadedPath)) return;
    std::error_code ec;
    fs::remove(m_loadedPath, ec);
    if (!ec) {
        std::cout << "[DeckEditor] Deleted: " << m_loadedPath << '\n';
        m_loadedPath.clear();
        m_deck.clear();
        m_commanderRules = nullptr;
        m_partnerRules   = nullptr;
        m_deckName = "New Deck";
        m_deckRowsDirty = true;
        scanSavedDecks();
        scanAiDecks();
    }
}

void DeckEditorScreen::loadDeckFromFile(const fs::path& p) {
    auto deck = mtg::DeckLoader::loadFromFile(p);
    if (!deck) { std::cerr << "[DeckEditor] Load failed: " << p << '\n'; return; }
    m_deck.clear();
    m_commanderRules = nullptr;
    m_partnerRules   = nullptr;
    m_companionRules = nullptr;
    m_deckName = deck->name.empty() ? p.stem().string() : deck->name;
    m_loadedPath = p;
    for (auto& ce : deck->commander) {
        const auto* rules = m_db.find(ce.name);
        if (!rules) continue;
        addCard(rules, 1);
        if (!m_commanderRules) m_commanderRules = rules;
        else if (!m_partnerRules) m_partnerRules = rules;
    }
    for (auto& ce : deck->companion) {
        const auto* rules = m_db.find(ce.name);
        if (!rules) continue;
        addCard(rules, 1);                 // keep in the editable list
        if (!m_companionRules) m_companionRules = rules;
    }
    for (auto& ce : deck->mainboard) {
        const auto* rules = m_db.find(ce.name);
        if (rules) addCard(rules, ce.count);
    }
    m_deckRowsDirty = true;
    m_deckScroll    = 0;
    std::cout << "[DeckEditor] Loaded: " << p << " (" << totalDeckCards() << " cards)\n";
}

void DeckEditorScreen::importDeckFromClipboard() {
    const sf::String clip = sf::Clipboard::getString();
    if (clip.isEmpty()) {
        std::cout << "[DeckEditor] Paste: clipboard empty.\n";
        return;
    }
    const std::string text = clip.toAnsiString();
    auto parsed = mtg::DeckLoader::loadFromString(text);
    if (parsed.mainboard.empty() && parsed.commander.empty()) {
        std::cout << "[DeckEditor] Paste: no card lines recognised.\n";
        return;
    }

    int added = 0, missing = 0;
    std::string firstMissing;
    auto mergeEntry = [&](const mtg::DeckLoader::CardEntry& ce, bool asCommander) {
        const auto* rules = m_db.find(ce.name);
        if (!rules) {
            if (++missing == 1) firstMissing = ce.name;
            return;
        }
        addCard(rules, ce.count);
        added += ce.count;
        if (asCommander) {
            if (!m_commanderRules)      m_commanderRules = rules;
            else if (!m_partnerRules)   m_partnerRules   = rules;
        }
    };
    for (const auto& ce : parsed.commander) mergeEntry(ce, /*asCommander=*/true);
    for (const auto& ce : parsed.mainboard) mergeEntry(ce, /*asCommander=*/false);

    if (!parsed.name.empty()) m_deckName = parsed.name;
    m_deckRowsDirty = true;
    m_deckScroll    = 0;

    std::cout << "[DeckEditor] Paste: added " << added << " card(s)";
    if (missing > 0) {
        std::cout << " (" << missing << " unrecognised — first: \""
                  << firstMissing << "\")";
    }
    std::cout << ".\n";
}

void DeckEditorScreen::closeEdhrecPicker() noexcept {
    m_edhrecOpen = false;
    m_edhrecHover = -1;
    m_edhrecScroll = 0;
}

void DeckEditorScreen::openEdhrecPicker() {
    if (!m_commanderRules) {
        std::cout << "[DeckEditor] EDHREC: designate a commander first "
                     "(right-click a card in the deck list to make it your commander).\n";
        return;
    }
    if (m_edhrecBusy.load()) return;  // already fetching

    m_edhrecOpen      = true;
    m_edhrecHover     = -1;
    m_edhrecScroll    = 0;
    m_edhrecCommander = m_commanderRules->name;
    {
        std::lock_guard<std::mutex> lk(m_edhrecMutex);
        m_edhrecTags.clear();
        m_edhrecStatus = "Fetching tags for " + m_edhrecCommander + " from EDHREC…";
    }
    m_edhrecBusy.store(true);

    // Worker thread: fetch tags, populate, then release the busy flag. The
    // overlay polls the mutex-guarded state during draw — no callback needed.
    const std::string cmdCopy = m_edhrecCommander;
    std::thread([this, cmdCopy]() {
        auto result = mtg::EdhrecClient::fetchCommanderTags(cmdCopy);
        std::lock_guard<std::mutex> lk(m_edhrecMutex);
        if (!result) {
            m_edhrecStatus = "EDHREC fetch failed (offline or commander slug "
                             "not recognised). Try the exact card name.";
        } else if (result->empty()) {
            m_edhrecStatus = "EDHREC returned no archetypes for this commander.";
        } else {
            m_edhrecStatus.clear();
            m_edhrecTags.clear();
            for (const auto& t : *result) {
                EdhrecTagRow row;
                row.name = t.name;
                row.slug = t.slug;
                row.count = t.count;
                m_edhrecTags.push_back(std::move(row));
            }
        }
        m_edhrecBusy.store(false);
    }).detach();
}

void DeckEditorScreen::importEdhrecDeckForTag(const std::string& tagSlug,
                                                const std::string& tagLabel) {
    if (m_edhrecBusy.load()) return;
    if (m_edhrecCommander.empty()) return;
    m_edhrecBusy.store(true);
    {
        std::lock_guard<std::mutex> lk(m_edhrecMutex);
        m_edhrecStatus = "Importing \"" + tagLabel + "\" deck…";
    }

    const std::string cmdCopy = m_edhrecCommander;
    const std::string slugCopy = tagSlug;
    const std::string labelCopy = tagLabel;
    std::thread([this, cmdCopy, slugCopy, labelCopy]() {
        auto deck = mtg::EdhrecClient::fetchAverageDeck(cmdCopy, slugCopy);
        std::lock_guard<std::mutex> lk(m_edhrecMutex);
        if (!deck) {
            m_edhrecStatus = "Fetch failed for \"" + labelCopy + "\".";
            m_edhrecBusy.store(false);
            return;
        }
        // Merge into the current deck. Skip the commander entry (it's already
        // designated) and skip cards the local DB doesn't recognise.
        int added = 0, missing = 0;
        for (const auto& line : *deck) {
            const auto* rules = m_db.find(line.name);
            if (!rules) { ++missing; continue; }
            if (rules == m_commanderRules || rules == m_partnerRules) continue;
            addCard(rules, line.count);
            added += line.count;
        }
        m_deckRowsDirty = true;
        m_deckName = labelCopy + " - " + cmdCopy;
        m_edhrecStatus = "Imported " + std::to_string(added) +
                         " cards from \"" + labelCopy + "\"" +
                         (missing > 0 ? " (" + std::to_string(missing) +
                                        " unrecognised)." : ".");
        m_edhrecOpen = false;  // close on success
        m_edhrecBusy.store(false);
    }).detach();
}

void DeckEditorScreen::exportDeckToClipboard() {
    // Emit a Forge .dck-style listing so a round-trip through the clipboard
    // preserves commanders, name, and counts.
    std::ostringstream out;
    out << "[metadata]\n";
    out << "Name=" << m_deckName << '\n';
    if (m_commanderRules) {
        out << "[Commander]\n";
        out << "1 " << m_commanderRules->name << '\n';
        if (m_partnerRules && m_partnerRules != m_commanderRules)
            out << "1 " << m_partnerRules->name << '\n';
    }
    out << "[Main]\n";
    int total = 0;
    for (const auto& [rules, n] : m_deck) {
        if (!rules || n <= 0) continue;
        if (rules == m_commanderRules || rules == m_partnerRules) continue;
        out << n << ' ' << rules->name << '\n';
        total += n;
    }
    const std::string s = out.str();
    sf::Clipboard::setString(sf::String::fromUtf8(s.begin(), s.end()));
    std::cout << "[DeckEditor] Copy: " << total
              << " mainboard card(s) copied to clipboard.\n";
}

// ── Hit-testing ───────────────────────────────────────────────────────────────

int DeckEditorScreen::hitCatalogRow(float px, float py) const {
    if (px < 0 || px >= CAT_W)          return -1;
    if (py < CAT_ROWS_Y || py >= WIN_H) return -1;
    int row = static_cast<int>((py - CAT_ROWS_Y) / CAT_ROW_H) + m_catScroll;
    if (row < 0 || row >= (int)m_filtered.size()) return -1;
    return row;
}

int DeckEditorScreen::hitDeckRow(float px, float py) const {
    if (px < MID_X || px >= MID_X + MID_W)              return -1;
    if (py < DP_LIST_Y || py >= DP_LIST_Y + DP_LIST_H)  return -1;
    rebuildDeckRows();
    int row = static_cast<int>((py - DP_LIST_Y) / DP_ITEM_H) + m_deckScroll;
    if (row < 0 || row >= (int)m_deckRows.size()) return -1;
    return row;
}

bool DeckEditorScreen::hitSearch(float px, float py) const {
    return px >= 70.f && px < CAT_W - 10.f && py >= 4.f && py < 36.f;
}

int DeckEditorScreen::hitColorBtn(float px, float py) const {
    if (py < 40.f || py >= 68.f) return -1;
    for (int i = 0; i < 6; ++i) {
        float bx = 4.f + i * 40.f;
        if (px >= bx && px < bx + 36.f) return i;
    }
    return -1;
}

int DeckEditorScreen::hitTypeFilter(float px, float py) const {
    if (py < 74.f || py >= 102.f) return -1;
    for (int i = 0; i < 8; ++i) {
        float bx = 2.f + i * 62.f;   // matches draw stride
        if (px >= bx && px < bx + 60.f) return i;
    }
    return -1;
}

bool DeckEditorScreen::hitOwnedToggle(float px, float py) const {
    if (!m_collection || m_collection->empty()) return false;
    float ox = 2.f + 8 * 62.f;
    return px >= ox && px < ox + 58.f && py >= 74.f && py < 102.f;
}

bool DeckEditorScreen::hitStart(float px, float py) const {
    return px >= MID_X + 195.f && px < MID_X + MID_W - 5.f
        && py >= DP_START_Y + 5.f && py < DP_START_Y + DP_START_H - 5.f;
}
bool DeckEditorScreen::hitSave(float px, float py) const {
    return px >= MID_X+179.f && px < MID_X+231.f
        && py >= DP_CTRL_Y+4.f && py < DP_CTRL_Y+DP_CTRL_H-4.f;
}
bool DeckEditorScreen::hitLoad(float px, float py) const {
    return px >= MID_X+235.f && px < MID_X+287.f
        && py >= DP_CTRL_Y+4.f && py < DP_CTRL_Y+DP_CTRL_H-4.f;
}
bool DeckEditorScreen::hitNew(float px, float py) const {
    return px >= MID_X+291.f && px < MID_X+343.f
        && py >= DP_CTRL_Y+4.f && py < DP_CTRL_Y+DP_CTRL_H-4.f;
}
bool DeckEditorScreen::hitClear(float px, float py) const {
    return px >= MID_X+347.f && px < MID_X+399.f
        && py >= DP_CTRL_Y+4.f && py < DP_CTRL_Y+DP_CTRL_H-4.f;
}
bool DeckEditorScreen::hitDelete(float px, float py) const {
    return px >= MID_X+403.f && px < MID_X+455.f
        && py >= DP_CTRL_Y+4.f && py < DP_CTRL_Y+DP_CTRL_H-4.f;
}
bool DeckEditorScreen::hitImport(float px, float py) const {
    return px >= MID_X+459.f && px < MID_X+511.f
        && py >= DP_CTRL_Y+4.f && py < DP_CTRL_Y+DP_CTRL_H-4.f;
}
bool DeckEditorScreen::hitExport(float px, float py) const {
    return px >= MID_X+515.f && px < MID_X+567.f
        && py >= DP_CTRL_Y+4.f && py < DP_CTRL_Y+DP_CTRL_H-4.f;
}
bool DeckEditorScreen::hitEdhrec(float px, float py) const {
    return px >= MID_X+571.f && px < MID_X+635.f
        && py >= DP_CTRL_Y+4.f && py < DP_CTRL_Y+DP_CTRL_H-4.f;
}
bool DeckEditorScreen::hitBackBtn(float px, float py) const {
    return px >= MID_X+5.f && px < MID_X+190.f
        && py >= DP_START_Y+5.f && py < DP_START_Y+DP_START_H-5.f;
}
bool DeckEditorScreen::hitDeckName(float px, float py) const {
    return px >= MID_X+5.f && px < MID_X+175.f
        && py >= DP_CTRL_Y+4.f && py < DP_CTRL_Y+DP_CTRL_H-4.f;
}
int DeckEditorScreen::hitFormatBtn(float px, float py) const {
    if (py < DP_CTRL_Y+4.f || py >= DP_CTRL_Y+DP_CTRL_H-4.f) return 0;
    if (px >= MID_X+515.f && px < MID_X+567.f) return 1;  // Standard
    if (px >= MID_X+571.f && px < MID_X+635.f) return 2;  // Commander
    return 0;
}
int DeckEditorScreen::hitAiArrow(float px, float py) const {
    if (py < DP_AI_Y || py >= DP_AI_Y + DP_AI_H) return 0;
    if (px >= MID_X+55.f   && px < MID_X+83.f)           return -1;
    if (px >= MID_X+MID_W-35.f && px < MID_X+MID_W-7.f) return  1;
    return 0;
}

// ── onEvent ───────────────────────────────────────────────────────────────────

DeckEditorScreen::Result DeckEditorScreen::onEvent(const sf::Event& ev) {
    // ── Text input ────────────────────────────────────────────────────────────
    if (ev.type == sf::Event::TextEntered) {
        uint32_t code = ev.text.unicode;
        // URL import text box
        if (m_showUrlImport) {
            if (code == '\b' && !m_urlBuffer.empty()) m_urlBuffer.pop_back();
            else if (code >= 32 && code < 127) m_urlBuffer += static_cast<char>(code);
            return Result::None;
        }
        // Paste-import text box
        if (m_showPasteImport) {
            if (code == '\b' && !m_pasteBuffer.empty()) m_pasteBuffer.pop_back();
            else if (code == '\n' || code == '\r') m_pasteBuffer += '\n';
            else if (code >= 32 && code < 127) m_pasteBuffer += static_cast<char>(code);
            return Result::None;
        }
        auto handleInput = [&](std::string& str, size_t maxLen = 256) {
            if (code == '\b') { if (!str.empty()) str.pop_back(); }
            else if (code >= 32 && code < 128 && str.size() < maxLen)
                str += static_cast<char>(code);
        };
        if (m_searchActive) { handleInput(m_searchText); applyFilters(); m_catScroll = 0; }
        if (m_nameEditing)  { handleInput(m_deckName, 40); }
        if (m_showLoadMenu) { handleInput(m_loadFilter, 60); rebuildLoadFilter(); m_loadMenuScroll = 0; }
        return Result::None;
    }

    // ── Keyboard ──────────────────────────────────────────────────────────────
    if (ev.type == sf::Event::KeyPressed) {
        auto k = ev.key.code;
        // Paste-import overlay controls
        if (m_showPasteImport) {
            if (k == sf::Keyboard::Escape) {
                m_showPasteImport = false; m_pasteBuffer.clear();
            } else if (k == sf::Keyboard::Return && ev.key.control) {
                importFromPastedList(m_pasteBuffer);
            }
            return Result::None;
        }
        // Ctrl+U → open URL import dialog
        if (k == sf::Keyboard::U && ev.key.control) {
            m_showUrlImport = !m_showUrlImport;
            m_urlBuffer.clear();
            return Result::None;
        }
        // Ctrl+R → toggle staple recommendations
        if (k == sf::Keyboard::R && ev.key.control) {
            m_showRecommendations = !m_showRecommendations;
            return Result::None;
        }
        // Ctrl+V → open paste-import
        if (k == sf::Keyboard::V && ev.key.control) {
            m_showPasteImport = true; m_pasteBuffer.clear();
            return Result::None;
        }
        if (k == sf::Keyboard::Return && m_showUrlImport) {
            if (!m_urlBuffer.empty()) importFromUrl(m_urlBuffer);
            m_showUrlImport = false; m_urlBuffer.clear();
            return Result::None;
        }
        if (k == sf::Keyboard::Escape && m_showUrlImport) {
            m_showUrlImport = false; m_urlBuffer.clear();
            return Result::None;
        }
        if (k == sf::Keyboard::Escape) {
            if (m_searchActive || m_nameEditing || m_showLoadMenu || m_edhrecOpen) {
                m_searchActive = m_nameEditing = m_showLoadMenu = false;
                if (m_edhrecOpen) closeEdhrecPicker();
            } else {
                return Result::Back;
            }
        }
        if (k == sf::Keyboard::Return || k == sf::Keyboard::Tab)
            m_searchActive = m_nameEditing = m_showLoadMenu = false;
        if (k == sf::Keyboard::Delete && m_searchActive) {
            m_searchText.clear(); applyFilters(); m_catScroll = 0;
        }
        return Result::None;
    }

    // ── Scroll ────────────────────────────────────────────────────────────────
    if (ev.type == sf::Event::MouseWheelScrolled) {
        float px = ev.mouseWheelScroll.x;
        float py = ev.mouseWheelScroll.y;
        int delta = (ev.mouseWheelScroll.delta > 0) ? -3 : 3;
        // Set picker overlay takes precedence when open.
        if (m_setMenuOpen && m_editions &&
            px >= 248.f && px <= 248.f + 220.f && py >= 68.f) {
            int total = 1 + (int)m_editions->sets().size();
            m_setMenuScroll = std::clamp(m_setMenuScroll + delta, 0,
                                         std::max(0, total - 20));
            return Result::None;
        }
        if (m_edhrecOpen) {
            std::lock_guard<std::mutex> lk(m_edhrecMutex);
            int total = static_cast<int>(m_edhrecTags.size());
            m_edhrecScroll = std::clamp(m_edhrecScroll + delta, 0,
                                         std::max(0, total - 14));
            return Result::None;
        }
        if (px < CAT_W) {
            m_catScroll = std::clamp(m_catScroll + delta, 0,
                std::max(0, (int)m_filtered.size() - CAT_VISIBLE));
        } else if (px < DETAIL_X) {
            if (m_showLoadMenu) {
                int maxV = static_cast<int>((DP_LIST_H - 44.f) / 22.f);
                m_loadMenuScroll = std::clamp(m_loadMenuScroll + delta, 0,
                    std::max(0, (int)m_loadVisible.size() - maxV));
            } else {
                rebuildDeckRows();
                m_deckScroll = std::clamp(m_deckScroll + delta, 0,
                    std::max(0, (int)m_deckRows.size() - DP_VISIBLE));
            }
        }
        return Result::None;
    }

    // ── Mouse hover (+ scrollbar drag tracking) ──────────────────────────────
    if (ev.type == sf::Event::MouseMoved) {
        float px = static_cast<float>(ev.mouseMove.x);
        float py = static_cast<float>(ev.mouseMove.y);

        // Active scrollbar drag → translate cursor y into a scroll value.
        if (m_dragScroll != ScrollTarget::None) {
            auto applyDrag = [&](float sbY, float sbH, int total, int visible,
                                  int& scroll) {
                if (total <= visible) { scroll = 0; return; }
                float thumbH = std::max(20.f, sbH * visible / (float)total);
                float track  = sbH - thumbH;
                if (track < 1.f) { scroll = 0; return; }
                float topY   = py - m_dragScrollGrabY;       // requested thumb top
                float frac   = (topY - sbY) / track;
                int   maxS   = total - visible;
                scroll = std::clamp(static_cast<int>(frac * maxS + 0.5f),
                                    0, maxS);
            };
            switch (m_dragScroll) {
                case ScrollTarget::Catalog:
                    applyDrag(CAT_ROWS_Y, WIN_H - CAT_ROWS_Y,
                              static_cast<int>(m_filtered.size()), CAT_VISIBLE,
                              m_catScroll);
                    break;
                case ScrollTarget::Deck: {
                    rebuildDeckRows();
                    applyDrag(DP_LIST_Y, DP_LIST_H,
                              static_cast<int>(m_deckRows.size()), DP_VISIBLE,
                              m_deckScroll);
                    break;
                }
                case ScrollTarget::LoadMenu: {
                    int maxV = static_cast<int>((DP_LIST_H - 44.f) / 22.f);
                    applyDrag(DP_LIST_Y + 44.f, DP_LIST_H - 44.f,
                              static_cast<int>(m_loadVisible.size()), maxV,
                              m_loadMenuScroll);
                    break;
                }
                default: break;
            }
            return Result::None;
        }

        m_catHover  = hitCatalogRow(px, py);
        m_deckHover = hitDeckRow(px, py);

        // Update detail panel from hover position
        if (m_catHover >= 0 && m_catHover < (int)m_filtered.size()) {
            m_detailCard = m_allCards[m_filtered[m_catHover]];
        } else if (m_deckHover >= 0 && m_deckHover < (int)m_deckRows.size()
                   && !m_deckRows[m_deckHover].isHeader) {
            m_detailCard = m_deckRows[m_deckHover].rules;
        }
        return Result::None;
    }

    // End of scrollbar drag.
    if (ev.type == sf::Event::MouseButtonReleased) {
        m_dragScroll = ScrollTarget::None;
    }

    // ── Mouse click ───────────────────────────────────────────────────────────
    if (ev.type == sf::Event::MouseButtonPressed) {
        float px = static_cast<float>(ev.mouseButton.x);
        float py = static_cast<float>(ev.mouseButton.y);
        bool isLeft  = (ev.mouseButton.button == sf::Mouse::Left);
        bool isRight = (ev.mouseButton.button == sf::Mouse::Right);

        // EDHREC overlay click intercept — modal while open. Hit-tests:
        //   - Close "X" top-right
        //   - One row per tag in the scrollable list (left-click → import)
        //   - Click-outside the panel → close
        if (m_edhrecOpen && isLeft) {
            constexpr float w = 480.f, h = 480.f;
            float x0 = (1560.f - w) * 0.5f;
            float y0 = (800.f - h) * 0.5f;
            bool inside = px >= x0 && px <= x0 + w && py >= y0 && py <= y0 + h;
            if (!inside) { closeEdhrecPicker(); return Result::None; }

            // Close button (top-right "X")
            if (px >= x0 + w - 32.f && px <= x0 + w - 4.f &&
                py >= y0 + 6.f && py <= y0 + 30.f) {
                closeEdhrecPicker();
                return Result::None;
            }

            // Row hit-test: rows start at y0 + 70, each row 26px tall.
            constexpr float rowH = 26.f;
            const float rowTop = y0 + 70.f;
            const float listH  = h - 70.f - 16.f;
            int visible = static_cast<int>(listH / rowH);
            int idx = static_cast<int>((py - rowTop) / rowH) + m_edhrecScroll;
            std::string slug, label;
            {
                std::lock_guard<std::mutex> lk(m_edhrecMutex);
                if (py >= rowTop && py < rowTop + visible * rowH &&
                    idx >= 0 && idx < (int)m_edhrecTags.size()) {
                    slug  = m_edhrecTags[idx].slug;
                    label = m_edhrecTags[idx].name;
                }
            }
            if (!slug.empty()) importEdhrecDeckForTag(slug, label);
            return Result::None;
        }

        // Deck-list scrollbar (middle panel right edge, DP_LIST_Y..end)
        if (isLeft && px >= MID_X + MID_W - 7.f && px < MID_X + MID_W &&
            py >= DP_LIST_Y && py < DP_LIST_Y + DP_LIST_H) {
            rebuildDeckRows();
            int total = (int)m_deckRows.size();
            if (total > DP_VISIBLE) {
                float sbH    = DP_LIST_H;
                float thumbH = std::max(15.f, sbH * DP_VISIBLE / (float)total);
                float thumbY = DP_LIST_Y + (sbH - thumbH) * m_deckScroll /
                               (float)std::max(1, total - DP_VISIBLE);
                if (py >= thumbY && py < thumbY + thumbH)
                    m_dragScrollGrabY = py - thumbY;
                else
                    m_dragScrollGrabY = thumbH * 0.5f;
                m_dragScroll = ScrollTarget::Deck;
            }
            return Result::None;
        }

        // ── Scrollbar tracks: jump-to-position on click, then drag ──────────
        // Catalog scrollbar lives in the rightmost 7px of the catalog panel
        // (CAT_W-7 .. CAT_W) over CAT_ROWS_Y .. WIN_H.
        if (isLeft && px >= CAT_W - 7.f && px < CAT_W &&
            py >= CAT_ROWS_Y && py < WIN_H) {
            int total = (int)m_filtered.size();
            if (total > CAT_VISIBLE) {
                float sbH    = WIN_H - CAT_ROWS_Y;
                float thumbH = std::max(20.f, sbH * CAT_VISIBLE / (float)total);
                float thumbY = CAT_ROWS_Y +
                               (sbH - thumbH) * m_catScroll /
                               (float)std::max(1, total - CAT_VISIBLE);
                if (py >= thumbY && py < thumbY + thumbH) {
                    m_dragScrollGrabY = py - thumbY;
                } else {
                    // Centre the thumb on the click, then start drag.
                    m_dragScrollGrabY = thumbH * 0.5f;
                    m_catScroll = std::clamp(
                        static_cast<int>((py - CAT_ROWS_Y - thumbH * 0.5f) /
                                          std::max(1.f, sbH - thumbH) *
                                          (total - CAT_VISIBLE) + 0.5f),
                        0, total - CAT_VISIBLE);
                }
                m_dragScroll = ScrollTarget::Catalog;
            }
            return Result::None;
        }

        // Load-menu overlay
        if (m_showLoadMenu) {
            // Load-menu scrollbar drag (drawn in drawLoadMenu's right edge —
            // see scrollbar update below). Track is along DP_LIST_Y+22 to
            // DP_LIST_Y+DP_LIST_H, 7 px wide just inside the panel border.
            int maxV = static_cast<int>((DP_LIST_H - 44.f) / 22.f);
            if (isLeft && (int)m_loadVisible.size() > maxV &&
                px >= MID_X + MID_W - 11.f && px <= MID_X + MID_W - 4.f &&
                py >= DP_LIST_Y + 44.f && py < DP_LIST_Y + DP_LIST_H) {
                float sbH    = DP_LIST_H - 44.f;
                int   total  = (int)m_loadVisible.size();
                float thumbH = std::max(20.f, sbH * maxV / (float)total);
                float thumbY = DP_LIST_Y + 44.f +
                               (sbH - thumbH) * m_loadMenuScroll /
                               (float)std::max(1, total - maxV);
                if (py >= thumbY && py < thumbY + thumbH) {
                    m_dragScrollGrabY = py - thumbY;
                } else {
                    m_dragScrollGrabY = thumbH * 0.5f;
                }
                m_dragScroll = ScrollTarget::LoadMenu;
                return Result::None;
            }
            if (isLeft) {
                // Rows start 44px below the panel top (header + search box).
                // Map the visible row through the filter list, and DON'T close
                // when the click lands on the header/search box so typing works.
                bool inPanel = (px >= MID_X && px < MID_X + MID_W &&
                                py >= DP_LIST_Y && py < DP_LIST_Y + DP_LIST_H);
                if (px >= MID_X && py >= DP_LIST_Y + 44.f && py < DP_LIST_Y + DP_LIST_H) {
                    int vi = static_cast<int>((py - (DP_LIST_Y + 44.f)) / 22.f) + m_loadMenuScroll;
                    if (vi >= 0 && vi < (int)m_loadVisible.size())
                        loadDeckFromFile(m_savedDecks[m_loadVisible[vi]]);
                    m_showLoadMenu = false;
                } else if (!inPanel) {
                    m_showLoadMenu = false;   // clicked outside → dismiss
                }
            }
            return Result::None;
        }

        if (!hitSearch  (px, py)) m_searchActive = false;
        if (!hitDeckName(px, py)) m_nameEditing  = false;

        if (isLeft) {
            if (hitSearch  (px, py)) { m_searchActive = true;  return Result::None; }
            if (hitDeckName(px, py)) { m_nameEditing  = true;  return Result::None; }
            if (hitStart   (px, py)) {
                if (isValidDeck()) return Result::StartGame;
                return Result::None;
            }
            if (hitBackBtn (px, py)) return Result::Back;
            if (hitSave    (px, py)) { saveDeck(); return Result::None; }
            if (hitLoad    (px, py)) {
                m_loadFilter.clear();
                scanSavedDecks(); m_showLoadMenu = true; m_loadMenuScroll = 0;
                return Result::None;
            }
            if (hitNew(px, py)) {
                m_deck.clear(); m_deckName = "New Deck"; m_commanderRules = nullptr;
                m_loadedPath.clear(); m_deckRowsDirty = true;
                return Result::None;
            }
            if (hitClear(px, py)) {
                m_deck.clear(); m_commanderRules = nullptr; m_deckRowsDirty = true;
                return Result::None;
            }
            if (hitDelete(px, py)) { deleteDeck(); return Result::None; }
            if (hitImport(px, py)) { importDeckFromClipboard(); return Result::None; }
            if (hitExport(px, py)) { exportDeckToClipboard();   return Result::None; }
            if (hitEdhrec(px, py)) { openEdhrecPicker();        return Result::None; }

            int arrow = hitAiArrow(px, py);
            if (arrow != 0 && !m_aiDecks.empty()) {
                int n = static_cast<int>(m_aiDecks.size());
                m_aiDeckIdx  = ((m_aiDeckIdx < 0 ? 0 : m_aiDeckIdx) + arrow + n) % n;
                m_aiDeckPath = m_aiDecks[m_aiDeckIdx];
                return Result::None;
            }

            int ci = hitColorBtn(px, py);
            if (ci >= 0) {
                m_colorFilter[ci] = !m_colorFilter[ci];
                applyFilters(); m_catScroll = 0;
                return Result::None;
            }
            int ti = hitTypeFilter(px, py);
            if (ti >= 0) {
                m_typeFilterIdx = ti; applyFilters(); m_catScroll = 0;
                return Result::None;
            }
            if (hitOwnedToggle(px, py)) {
                m_ownedOnly = !m_ownedOnly; applyFilters(); m_catScroll = 0;
                return Result::None;
            }

            // Set dropdown button (toggles the overlay)
            if (px >= 248.f && px <= 248.f + 220.f &&
                py >= 40.f  && py <= 40.f  + 26.f) {
                m_setMenuOpen   = !m_setMenuOpen;
                m_setMenuScroll = 0;
                return Result::None;
            }
            // Alchemy show/hide toggle
            if (px >= 474.f && px <= 474.f + 80.f &&
                py >= 40.f  && py <= 40.f  + 26.f) {
                m_hideAlchemy = !m_hideAlchemy;
                applyFilters(); m_catScroll = 0;
                return Result::None;
            }
            // Set-menu overlay click (when open)
            if (m_setMenuOpen && m_editions) {
                constexpr float mx = 248.f, my = 68.f, mw = 220.f, rowH = 18.f;
                float mh = 18.f * 20.f;  // up to 20 visible
                if (px >= mx && px <= mx + mw &&
                    py >= my && py <= my + mh) {
                    int idx = static_cast<int>((py - my) / rowH) + m_setMenuScroll;
                    // Index 0 is the "All sets" sentinel.
                    if (idx == 0) {
                        m_setFilter.clear();
                    } else if (idx - 1 < (int)m_editions->sets().size()) {
                        m_setFilter = m_editions->sets()[(size_t)(idx - 1)].code;
                    }
                    m_setMenuOpen = false;
                    applyFilters(); m_catScroll = 0;
                    return Result::None;
                }
                m_setMenuOpen = false;
            }

            // Mana curve chart click: filter catalog by CMC
            if (px >= MID_X && px < MID_X + MID_W &&
                py >= DP_CURVE_Y && py < DP_CURVE_Y + DP_CURVE_H) {
                float chartX = MID_X + 18.f;
                float barW   = (MID_W - 36.f) / 8.f;
                int bar = static_cast<int>((px - chartX) / barW);
                if (bar >= 0 && bar < 8) {
                    m_cmcFilter = (m_cmcFilter == bar) ? -1 : bar;  // toggle
                    applyFilters(); m_catScroll = 0;
                }
                return Result::None;
            }

            // Column header click: toggle sort mode
            if (px < CAT_W && py >= CAT_FILTER_H && py < CAT_FILTER_H + CAT_COLHDR_H) {
                SortBy clicked = SortBy::Name;
                if      (px >= CC_NAME_X && px < CC_COST_X) clicked = SortBy::Name;
                else if (px >= CC_COST_X && px < CC_TYPE_X) clicked = SortBy::Cmc;
                else if (px >= CC_TYPE_X && px < CC_PT_X)   clicked = SortBy::Type;
                else if (px >= CC_CMC_X)                    clicked = SortBy::Cmc;
                if (m_sortBy == clicked) m_sortAsc = !m_sortAsc;
                else { m_sortBy = clicked; m_sortAsc = true; }
                applyFilters(); m_catScroll = 0;
                return Result::None;
            }

            int catRow = hitCatalogRow(px, py);
            if (catRow >= 0 && catRow < (int)m_filtered.size()) {
                addCard(m_allCards[m_filtered[catRow]], 1);
                return Result::None;
            }

            int deckRowIdx = hitDeckRow(px, py);
            if (deckRowIdx >= 0) {
                rebuildDeckRows();
                if (deckRowIdx < (int)m_deckRows.size() && !m_deckRows[deckRowIdx].isHeader)
                    removeCard(m_deckRows[deckRowIdx].rules, 1);
                return Result::None;
            }
        }

        if (isRight) {
            // Right-click catalog: add max copies
            int catRow = hitCatalogRow(px, py);
            if (catRow >= 0 && catRow < (int)m_filtered.size()) {
                const auto* r = m_allCards[m_filtered[catRow]];
                addCard(r, maxCopies(r));
                return Result::None;
            }
            // Right-click deck row: designate/clear commander (legendary creatures only)
            int deckRowIdx = hitDeckRow(px, py);
            if (deckRowIdx >= 0) {
                rebuildDeckRows();
                if (deckRowIdx < (int)m_deckRows.size() && !m_deckRows[deckRowIdx].isHeader) {
                    const auto* r = m_deckRows[deckRowIdx].rules;
                    if (r == m_commanderRules) {
                        m_commanderRules = nullptr;
                        m_partnerRules   = nullptr;
                    } else if (r == m_partnerRules) {
                        m_partnerRules = nullptr;
                    } else if (r == m_companionRules) {
                        m_companionRules = nullptr;   // clear companion
                    } else if (r->hasCompanion) {
                        m_companionRules = r;         // designate as companion
                    } else if (r->type.isLegendary()) {
                        if (!m_commanderRules) {
                            m_commanderRules = r;     // set primary commander
                        } else if (!m_partnerRules) {
                            // Allow a second legendary as partner if the first has Partner keyword
                            bool primaryHasPartner = (m_commanderRules->hasKeyword("Partner") ||
                                                      m_commanderRules->hasKeyword("Partner with"));
                            bool thisHasPartner    = (r->hasKeyword("Partner") ||
                                                      r->hasKeyword("Partner with"));
                            if (primaryHasPartner || thisHasPartner)
                                m_partnerRules = r;   // set partner commander
                            else
                                m_commanderRules = r; // replace primary (no partner keyword)
                        } else {
                            m_commanderRules = r;     // replace primary, clear partner
                            m_partnerRules   = nullptr;
                        }
                    }
                    m_deckRowsDirty = true;
                }
                return Result::None;
            }
        }
    }
    return Result::None;
}

// ── Drawing helpers ───────────────────────────────────────────────────────────

void DeckEditorScreen::drawRect(sf::RenderTarget& t,
                                float x, float y, float w, float h,
                                sf::Color fill, sf::Color outline, float thick) const {
    sf::RectangleShape s({w, h});
    s.setPosition(x, y);
    s.setFillColor(fill);
    if (outline != sf::Color::Transparent) {
        s.setOutlineColor(outline); s.setOutlineThickness(-thick);
    }
    t.draw(s);
}

void DeckEditorScreen::drawText(sf::RenderTarget& t, const std::string& s,
                                float x, float y, unsigned sz,
                                sf::Color col, bool bold) const {
    if (s.empty()) return;
    sf::Text txt;
    txt.setFont(m_font);
    txt.setString(s);
    txt.setCharacterSize(sz);
    txt.setFillColor(col);
    if (bold) txt.setStyle(sf::Text::Bold);
    txt.setPosition(x, y);
    ui::applyTextScale(txt);
    t.draw(txt);
}

void DeckEditorScreen::drawButton(sf::RenderTarget& t, const std::string& label,
                                  float x, float y, float w, float h,
                                  sf::Color fill, sf::Color textCol) const {
    drawRect(t, x, y, w, h, fill, kBorder, 1.f);
    sf::Text txt;
    txt.setFont(m_font);
    txt.setString(label);
    txt.setCharacterSize(12);
    txt.setFillColor(textCol);
    auto b = txt.getLocalBounds();
    txt.setPosition(x + (w - b.width) * 0.5f - b.left,
                    y + (h - b.height) * 0.5f - b.top);
    ui::applyTextScale(txt);
    t.draw(txt);
}

// ── draw (top-level) ──────────────────────────────────────────────────────────

void DeckEditorScreen::draw(sf::RenderWindow& window) const {
    window.clear(kBg);
    drawCatalogPanel(window);
    drawDeckPanel(window);
    if (m_showLoadMenu) drawLoadMenu(window);
    drawDetailPanel(window);

    // Custom-card hotkey hint (bottom-left of the catalog panel).
    drawText(window, "F6  New custom card      F5  Reload custom cards",
             10.f, WIN_H - 15.f, 10, kDim);

    // ── Set picker overlay (when toggled open from the filter bar) ───────────
    if (m_setMenuOpen && m_editions) {
        constexpr float mx = 248.f, my = 68.f, mw = 220.f, rowH = 18.f;
        int rows = std::min<int>(20,
                                  1 + static_cast<int>(m_editions->sets().size()));
        float mh = rowH * rows;
        drawRect(window, mx, my, mw, mh, sf::Color(18, 17, 16),
                 sf::Color(203, 163, 90), 1.f);
        // Row 0 is "All sets"; subsequent rows are sets newest-first.
        for (int r = 0; r < rows; ++r) {
            int idx = r + m_setMenuScroll;
            float ry = my + r * rowH;
            std::string label;
            bool selected = false;
            if (idx == 0) {
                label = "All sets";
                selected = m_setFilter.empty();
            } else if (idx - 1 < (int)m_editions->sets().size()) {
                const auto& s = m_editions->sets()[(size_t)(idx - 1)];
                label = s.code + "  " + s.name;
                if (label.size() > 30) label = label.substr(0, 28) + "..";
                selected = (s.code == m_setFilter);
            } else break;
            drawRect(window, mx, ry, mw, rowH,
                     selected ? sf::Color(44, 34, 14) : sf::Color(26, 24, 22));
            drawText(window, label, mx + 6.f, ry + 3.f, 10,
                     selected ? kAccent : kBody, selected);
        }
    }
    if (m_showPasteImport) drawPasteImportOverlay(window);

    // ── EDHREC tag picker overlay ────────────────────────────────────────────
    if (m_edhrecOpen) {
        sf::RectangleShape dim({WIN_W, WIN_H});
        dim.setFillColor(sf::Color(0, 0, 0, 170));
        window.draw(dim);

        constexpr float w = 480.f, h = 480.f;
        float x0 = (WIN_W - w) * 0.5f;
        float y0 = (WIN_H - h) * 0.5f;
        sf::RectangleShape pan({w, h});
        pan.setPosition(x0, y0);
        pan.setFillColor(sf::Color(18, 17, 16, 252));
        pan.setOutlineColor(sf::Color(200, 140, 255));
        pan.setOutlineThickness(2.f);
        window.draw(pan);

        // Header
        std::string hdr = "EDHREC ARCHETYPES - " + m_edhrecCommander;
        if (hdr.size() > 52) hdr = hdr.substr(0, 50) + "..";
        drawText(window, hdr, x0 + 12.f, y0 + 10.f, 14, sf::Color(220, 200, 240), true);
        drawText(window, "Click a tag to import its average deck.",
                 x0 + 12.f, y0 + 32.f, 9, kInk4);
        // Close X
        drawRect(window, x0 + w - 32.f, y0 + 6.f, 28.f, 22.f,
                 sf::Color(48, 22, 18), sf::Color(160, 100, 60), 1.f);
        drawText(window, "X", x0 + w - 22.f, y0 + 9.f, 12, sf::Color(230, 170, 130), true);

        // Body: rows OR status text
        std::lock_guard<std::mutex> lk(m_edhrecMutex);
        if (!m_edhrecStatus.empty() && m_edhrecTags.empty()) {
            drawText(window, m_edhrecStatus, x0 + 16.f, y0 + 70.f, 12, kBody);
        } else {
            constexpr float rowH = 26.f;
            const float rowTop = y0 + 70.f;
            const float listH  = h - 70.f - 16.f;
            int visible = static_cast<int>(listH / rowH);
            int total = static_cast<int>(m_edhrecTags.size());
            int start = std::clamp(m_edhrecScroll, 0, std::max(0, total - visible));
            for (int i = 0; i < visible && start + i < total; ++i) {
                const auto& row = m_edhrecTags[start + i];
                float ry = rowTop + i * rowH;
                drawRect(window, x0 + 8.f, ry, w - 16.f, rowH - 2.f,
                         sf::Color(28, 22, 34), sf::Color(80, 60, 100), 1.f);
                drawText(window, row.name, x0 + 16.f, ry + 4.f, 12, kInk, true);
                std::string countStr = std::to_string(row.count) + " decks";
                drawText(window, countStr, x0 + w - 100.f, ry + 5.f, 10, sf::Color(180, 140, 220));
            }
            // Status line at bottom while a background fetch is running.
            if (!m_edhrecStatus.empty())
                drawText(window, m_edhrecStatus, x0 + 16.f, y0 + h - 18.f,
                         10, sf::Color(220, 180, 240));
        }
    }

    // URL import overlay (Ctrl+U)
    if (m_showUrlImport) {
        sf::RectangleShape dim({WIN_W, WIN_H});
        dim.setFillColor(sf::Color(0, 0, 0, 160));
        window.draw(dim);
        constexpr float pw = 500.f, ph = 100.f;
        float px0 = (WIN_W - pw) * 0.5f, py0 = (WIN_H - ph) * 0.5f;
        sf::RectangleShape pan({pw, ph});
        pan.setPosition(px0, py0);
        pan.setFillColor(sf::Color(18, 17, 16, 252));
        pan.setOutlineColor(kAccent);
        pan.setOutlineThickness(1.f);
        window.draw(pan);
        drawText(window, "Paste Moxfield/Archidekt URL (Enter to import, Esc to cancel):",
                 px0+8.f, py0+8.f, 10, kInk3);
        sf::RectangleShape box({pw-16.f, 24.f});
        box.setPosition(px0+8.f, py0+28.f);
        box.setFillColor(kBg3);
        box.setOutlineColor(kBorder);
        box.setOutlineThickness(1.f);
        window.draw(box);
        drawText(window, m_urlBuffer + "|", px0+12.f, py0+32.f, 10, kInk);
        drawText(window, "Note: requires internet connection. Ctrl+V = paste decklist instead.",
                 px0+8.f, py0+60.f, 8, kInk4);
    }

    // Staple recommendations overlay (Ctrl+R)
    if (m_showRecommendations && m_commanderRules) {
        auto recs = getStapleRecommendations();
        constexpr float rw = 320.f;
        float rh = std::min(static_cast<float>(recs.size()) * 16.f + 44.f, WIN_H * 0.7f);
        float rx = MID_X + (MID_W - rw) * 0.5f;
        float ry = (WIN_H - rh) * 0.5f;
        sf::RectangleShape dim({WIN_W, WIN_H});
        dim.setFillColor(sf::Color(0, 0, 0, 150));
        window.draw(dim);
        sf::RectangleShape pan({rw, rh});
        pan.setPosition(rx, ry);
        pan.setFillColor(sf::Color(18, 17, 16, 252));
        pan.setOutlineColor(kAccent);
        pan.setOutlineThickness(1.f);
        window.draw(pan);
        drawText(window, "STAPLE RECOMMENDATIONS  (Ctrl+R to close)",
                 rx + 8.f, ry + 6.f, 10, kAccent, true);
        drawText(window, "Cards not yet in your deck:",
                 rx + 8.f, ry + 20.f, 9, kInk3);
        float iy = ry + 34.f;
        for (const auto& name : recs) {
            if (iy > ry + rh - 10.f) break;
            drawText(window, "- " + name, rx + 10.f, iy, 10, kInk);
            iy += 15.f;
        }
        if (recs.empty())
            drawText(window, "(All staples already in deck!)", rx + 10.f, ry + 40.f, 10, kGood);
    }
}

// ── Catalog panel ─────────────────────────────────────────────────────────────

void DeckEditorScreen::drawCatalogPanel(sf::RenderTarget& t) const {
    drawRect(t, 0, 0, CAT_W, WIN_H, kBg);
    drawRect(t, CAT_W - 1.f, 0, 1.f, WIN_H, kBorder);
    drawFilterBar(t);
    drawCatalogHeader(t);
    drawCatalogRows(t);
}

void DeckEditorScreen::drawFilterBar(sf::RenderTarget& t) const {
    drawRect(t, 0, 0, CAT_W, CAT_FILTER_H, kHdrBg);
    drawRect(t, 0, CAT_FILTER_H - 1.f, CAT_W, 1.f, kBorder);

    // Row 1 (y=4..36): Search box with a card-count badge on the right side.
    drawText(t, "Search:", 4.f, 9.f, 12, kHdrText);
    constexpr float kCountW = 80.f;
    sf::Color sBg = m_searchActive ? kBg3 : kBg2;
    drawRect(t, 70.f, 4.f, CAT_W - 80.f - kCountW, 30.f, sBg,
             m_searchActive ? kAccent : kBorder, 1.f);
    if (m_searchText.empty() && !m_searchActive)
        drawText(t, "card name or type...", 76.f, 9.f, 12, kDim);
    else
        drawText(t, m_searchText + (m_searchActive ? "|" : ""), 76.f, 9.f, 12, kBody);

    // Card-count badge (moved from the catalog column header so it stops
    // overlapping the "#" column there).
    drawText(t, std::to_string(m_filtered.size()) + " cards",
             CAT_W - kCountW + 6.f, 12.f, 11, kDim);

    // Row 2 (y=40..68): Color filter buttons
    static const char*     CLABELS[] = {"W","U","B","R","G","C"};
    static const sf::Color CACT[6]   = {
        {240,235,210}, {70,120,210}, {50,40,65}, {200,75,55}, {55,145,70}, {130,130,130}
    };
    static const sf::Color CFG[6] = {
        {20,20,20}, {255,255,255}, {220,210,235}, {255,255,255}, {255,255,255}, {240,240,240}
    };
    for (int i = 0; i < 6; ++i) {
        float bx = 4.f + i * 40.f;
        sf::Color bg = m_colorFilter[i] ? CACT[i] : kBg3;
        drawRect(t, bx, 40.f, 36.f, 26.f, bg, kBorder, 1.f);
        sf::Text lbl;
        lbl.setFont(m_font); lbl.setString(CLABELS[i]);
        lbl.setCharacterSize(12); lbl.setStyle(sf::Text::Bold);
        lbl.setFillColor(m_colorFilter[i] ? CFG[i] : kInk3);
        auto lb = lbl.getLocalBounds();
        lbl.setPosition(bx + (36.f - lb.width) * 0.5f - lb.left,
                        40.f + (26.f - lb.height) * 0.5f - lb.top);
        t.draw(lbl);
    }

    // Set dropdown (right of the color buttons)
    {
        constexpr float bx = 248.f, by = 40.f, bw = 220.f, bh = 26.f;
        std::string lbl = m_setFilter.empty()
            ? std::string("Set: All")
            : (std::string("Set: ") + m_setFilter +
               (m_editions
                ? std::string("  - ") + m_editions->setName(m_setFilter).substr(0, 18)
                : std::string{}));
        bool active = !m_setFilter.empty();
        drawRect(t, bx, by, bw, bh,
                 active ? sf::Color(34, 26, 10) : kBg3,
                 active ? kAccent : kBorder, 1.f);
        drawText(t, lbl, bx + 6.f, by + 6.f, 11, active ? kAccent : kInk3, true);
        drawText(t, m_setMenuOpen ? "v" : ">", bx + bw - 14.f, by + 6.f, 11, kInk3);
    }

    // Alchemy toggle to the right of the set dropdown
    {
        constexpr float bx = 474.f, by = 40.f, bw = 80.f, bh = 26.f;
        drawRect(t, bx, by, bw, bh,
                 m_hideAlchemy ? sf::Color(28, 44, 24) : kBg3,
                 m_hideAlchemy ? kGood : kBorder, 1.f);
        drawText(t, m_hideAlchemy ? "Hide Alch" : "Show Alch",
                 bx + 6.f, by + 6.f, 11,
                 m_hideAlchemy ? kGood : kInk3, true);
    }

    // Row 3 (y=74..100): Type filter chips (7 chips) + Owned toggle
    static const char* TNAMES[] = {
        "All","Creature","Land","Instant","Sorcery","Enchant","Artifact","PW"
    };
    for (int i = 0; i < 8; ++i) {
        float bx = 2.f + i * 62.f;
        bool active = (m_typeFilterIdx == i);
        drawRect(t, bx, 74.f, 60.f, 26.f,
                 active ? sf::Color(34, 26, 10) : kBg2,
                 active ? kAccent : kBorder, 1.f);
        sf::Text lbl;
        lbl.setFont(m_font); lbl.setString(TNAMES[i]);
        lbl.setCharacterSize(10);
        lbl.setFillColor(active ? kAccent : kInk3);
        auto lb = lbl.getLocalBounds();
        lbl.setPosition(bx + (60.f - lb.width) * 0.5f - lb.left,
                        74.f + (26.f - lb.height) * 0.5f - lb.top);
        t.draw(lbl);
    }
    // "Owned" toggle (right of type chips; only visible when collection data is present)
    if (m_collection && !m_collection->empty()) {
        float ox = 2.f + 8 * 62.f;   // 498px
        drawRect(t, ox, 74.f, 58.f, 26.f,
                 m_ownedOnly ? sf::Color(28, 44, 24) : kBg2,
                 m_ownedOnly ? kGood : kBorder, 1.f);
        sf::Text olbl;
        olbl.setFont(m_font); olbl.setString(m_ownedOnly ? "Owned" : "All");
        olbl.setCharacterSize(10);
        olbl.setFillColor(m_ownedOnly ? kGood : kInk3);
        auto ob = olbl.getLocalBounds();
        olbl.setPosition(ox + (58.f - ob.width) * 0.5f - ob.left,
                         74.f + (26.f - ob.height) * 0.5f - ob.top);
        t.draw(olbl);
    }
}

void DeckEditorScreen::drawCatalogHeader(sf::RenderTarget& t) const {
    float hy = CAT_FILTER_H;
    drawRect(t, 0, hy, CAT_W, CAT_COLHDR_H, kHdrBg);
    drawRect(t, 0, hy + CAT_COLHDR_H - 1.f, CAT_W, 1.f, kBorder);

    const char* arrow = m_sortAsc ? " ^" : " v";
    auto hdr = [&](const char* label, float x, float y, SortBy mode) {
        bool active = (m_sortBy == mode);
        sf::Color col = active ? kAccent : kHdrText;
        std::string lbl = label;
        if (active) lbl += arrow;
        drawText(t, lbl, x, y, 11, col, true);
    };
    float ty = hy + 4.f;
    hdr("Name",  CC_NAME_X + 4.f, ty, SortBy::Name);
    hdr("Cost",  CC_COST_X + 2.f, ty, SortBy::Cmc);
    hdr("Type",  CC_TYPE_X + 2.f, ty, SortBy::Type);
    hdr("P/T",   CC_PT_X   + 2.f, ty, SortBy::Name);
    hdr("CMC",   CC_CMC_X  + 2.f, ty, SortBy::Cmc);
    hdr("#",     CC_QTY_X  + 8.f, ty, SortBy::Name);

    // (The "N cards" count used to live here and overlapped the "#" column
    // header; it now sits in the filter bar above the column row.)
}

void DeckEditorScreen::drawCatalogRows(sf::RenderTarget& t) const {
    float baseY  = CAT_ROWS_Y;
    int   visEnd = std::min(m_catScroll + CAT_VISIBLE + 1, (int)m_filtered.size());

    for (int vi = m_catScroll; vi < visEnd; ++vi) {
        float ry = baseY + (vi - m_catScroll) * CAT_ROW_H;
        if (ry >= WIN_H) break;

        const auto* r   = m_allCards[m_filtered[vi]];
        int          qty = deckCount(r);
        bool         hov = (vi == m_catHover);

        sf::Color bg = hov ? kRowHover : (qty > 0 ? kRowDeck : ((vi%2==0) ? kRowEven : kRowOdd));
        drawRect(t, 0, ry, CAT_W, CAT_ROW_H, bg);
        drawRect(t, 0, ry, 2.f, CAT_ROW_H, colorForCard(r));

        auto clip = [](const std::string& s, size_t max) -> std::string {
            return s.size() > max ? s.substr(0, max - 2) + ".." : s;
        };

        drawText(t, clip(r->name, 26),        CC_NAME_X+4.f, ry+3.f, 12, kBody);
        drawText(t, clip(manaCostStr(r), 14), CC_COST_X+2.f, ry+3.f, 11, kGoldBrt);
        drawText(t, clip(typeStr(r), 22),     CC_TYPE_X+2.f, ry+3.f, 11, kDim);
        drawText(t, ptStr(r),                 CC_PT_X  +2.f, ry+3.f, 11, kInk2);
        if (!r->type.isLand())
            drawText(t, std::to_string(r->cmc()), CC_CMC_X+8.f, ry+3.f, 11, kDim);
        if (qty > 0)
            drawText(t, std::to_string(qty), CC_QTY_X+8.f, ry+3.f, 12, kGreen, true);
    }

    // Scrollbar
    int total = (int)m_filtered.size();
    if (total > CAT_VISIBLE) {
        float sbH    = WIN_H - CAT_ROWS_Y;
        float thumbH = std::max(20.f, sbH * CAT_VISIBLE / (float)total);
        float thumbY = CAT_ROWS_Y + (sbH - thumbH) * m_catScroll /
                       (float)std::max(1, total - CAT_VISIBLE);
        drawRect(t, CAT_W - 7.f, CAT_ROWS_Y, 7.f, sbH, kBg2);
        drawRect(t, CAT_W - 7.f, thumbY,     7.f, thumbH, kGoldDeep);
    }
}

// ── Deck (middle) panel ───────────────────────────────────────────────────────

void DeckEditorScreen::drawDeckPanel(sf::RenderTarget& t) const {
    drawRect(t, MID_X, 0, MID_W, WIN_H, kDeckPanelBg);
    drawDeckControls(t);
    drawDeckList(t);
    drawManaChart(t);
    drawDeckStats(t);
    drawAiPicker(t);
    drawStartButton(t);
}

void DeckEditorScreen::drawDeckControls(sf::RenderTarget& t) const {
    drawRect(t, MID_X, 0, MID_W, DP_CTRL_H, kHdrBg);
    drawRect(t, MID_X, DP_CTRL_H - 1.f, MID_W, 1.f, kBorder);

    // Deck name box
    sf::Color nameBg = m_nameEditing ? kBg3 : kBg2;
    drawRect(t, MID_X+5.f, DP_CTRL_Y+4.f, 170.f, DP_CTRL_H-8.f,
             nameBg, m_nameEditing ? kAccent : kBorder, 1.f);
    drawText(t, m_deckName + (m_nameEditing ? "|" : ""),
             MID_X+9.f, DP_CTRL_Y+10.f, 12, kBody);

    drawButton(t, "Save",   MID_X+179.f, DP_CTRL_Y+4.f, 52.f, DP_CTRL_H-8.f,
               sf::Color(28, 44, 24), kGood);
    drawButton(t, "Load",   MID_X+235.f, DP_CTRL_Y+4.f, 52.f, DP_CTRL_H-8.f,
               kBg3, kBody);
    drawButton(t, "New",    MID_X+291.f, DP_CTRL_Y+4.f, 52.f, DP_CTRL_H-8.f,
               kBg3, kBody);
    drawButton(t, "Clear",  MID_X+347.f, DP_CTRL_Y+4.f, 52.f, DP_CTRL_H-8.f,
               sf::Color(44, 22, 18), kBad);
    {
        bool canDel = !m_loadedPath.empty() && fs::exists(m_loadedPath);
        drawButton(t, "Delete", MID_X+403.f, DP_CTRL_Y+4.f, 52.f, DP_CTRL_H-8.f,
                   canDel ? sf::Color(52, 18, 14) : kBg2,
                   canDel ? kBad : kDim);
    }
    // Clipboard import / export — paste a decklist into the editor or copy
    // the current deck out as text. Tooltip-style help is in the status bar.
    drawButton(t, "Paste", MID_X+459.f, DP_CTRL_Y+4.f, 52.f, DP_CTRL_H-8.f,
               sf::Color(20, 38, 52), sf::Color(160, 200, 230));
    drawButton(t, "Copy",  MID_X+515.f, DP_CTRL_Y+4.f, 52.f, DP_CTRL_H-8.f,
               sf::Color(20, 38, 52), sf::Color(160, 200, 230));
    // EDHREC button — enabled once a commander is designated. Click pulls
    // the commander's archetype/tag list from EDHREC; selecting a tag
    // imports its average deck. Disabled state grays it out.
    {
        bool canEdh = (m_commanderRules != nullptr);
        drawButton(t, "EDHREC", MID_X+571.f, DP_CTRL_Y+4.f, 64.f, DP_CTRL_H-8.f,
                   canEdh ? sf::Color(36, 22, 52) : kBg2,
                   canEdh ? sf::Color(200, 140, 255) : kDim);
    }
}

void DeckEditorScreen::drawDeckList(sf::RenderTarget& t) const {
    drawRect(t, MID_X, DP_LIST_Y, MID_W, DP_LIST_H, kBg1);
    rebuildDeckRows();

    int total = (int)m_deckRows.size();
    int end   = std::min(m_deckScroll + DP_VISIBLE + 1, total);

    for (int vi = m_deckScroll; vi < end; ++vi) {
        float ry = DP_LIST_Y + (vi - m_deckScroll) * DP_ITEM_H;
        if (ry + DP_ITEM_H > DP_LIST_Y + DP_LIST_H + DP_ITEM_H) break;

        auto& row = m_deckRows[vi];
        bool isCommanderSection = (row.header == "COMMANDER");
        if (row.isHeader) {
            sf::Color hdrBg  = isCommanderSection ? sf::Color(30, 16, 46) : kBg2;
            sf::Color accent = isCommanderSection ? sf::Color(200,140,255): kAccent;
            drawRect(t, MID_X, ry, MID_W, DP_ITEM_H, hdrBg);
            drawRect(t, MID_X, ry, 3.f, DP_ITEM_H, accent);
            std::string hdrTxt = isCommanderSection
                ? "COMMANDER  (right-click card to designate)"
                : row.header;
            drawText(t, hdrTxt, MID_X+8.f, ry+2.f, 11, accent, true);
        } else {
            bool isCmd = (row.rules == m_commanderRules);
            bool hov   = (vi == m_deckHover);
            sf::Color bg = isCmd
                ? sf::Color(36, 20, 56)
                : (hov ? kRowHover : kRowEven);
            drawRect(t, MID_X, ry, MID_W, DP_ITEM_H, bg);
            if (isCmd) drawRect(t, MID_X, ry, 2.f, DP_ITEM_H, sf::Color(200,140,255));

            sf::Color cntCol = isCmd ? sf::Color(200,140,255) : kGreen;
            drawText(t, std::to_string(row.count), MID_X+4.f, ry+2.f, 12, cntCol, true);

            sf::Color nameCol = isCmd ? sf::Color(230,210,255) : kBody;
            std::string nm = row.rules->name;
            if (nm.size() > 34) nm = nm.substr(0,32) + "..";
            drawText(t, nm, MID_X+22.f, ry+2.f, 12, nameCol, isCmd);

            std::string cost = manaCostStr(row.rules);
            if (cost.size() > 12) cost = cost.substr(0,10) + "..";
            sf::Text costTxt;
            costTxt.setFont(m_font); costTxt.setString(cost); costTxt.setCharacterSize(11);
            auto cb = costTxt.getLocalBounds();
            // Subtract scrollbar width (7px) + small padding so the cost
            // pip string doesn't run into the scrollbar.
            float costX = MID_X + MID_W - cb.width - cb.left - 18.f;
            drawText(t, cost, costX, ry+2.f, 11, kGoldBrt);
        }
    }

    if (total > DP_VISIBLE) {
        float sbH    = DP_LIST_H;
        float thumbH = std::max(15.f, sbH * DP_VISIBLE / (float)total);
        float thumbY = DP_LIST_Y + (sbH - thumbH) * m_deckScroll /
                       (float)std::max(1, total - DP_VISIBLE);
        drawRect(t, MID_X+MID_W-7.f, DP_LIST_Y, 7.f, sbH, kBg2);
        drawRect(t, MID_X+MID_W-7.f, thumbY,    7.f, thumbH, kGoldDeep);
    }
}

void DeckEditorScreen::drawManaChart(sf::RenderTarget& t) const {
    drawRect(t, MID_X, DP_CURVE_Y, MID_W, DP_CURVE_H, kBg1);
    drawRect(t, MID_X, DP_CURVE_Y, MID_W, 1.f, kBorder);
    drawText(t, "Mana Curve", MID_X+5.f, DP_CURVE_Y+3.f, 10, kHdrText, true);

    // Deck colour identity (commander identity, or the union of all cards if no
    // commander is set yet) shown as pips in the header's right edge.
    {
        uint8_t ci = 0;
        if (m_commanderRules) {
            ci = m_commanderRules->manaCost.colorIdentity();
            if (m_partnerRules) ci |= m_partnerRules->manaCost.colorIdentity();
        } else {
            for (auto& e : m_deck) ci |= e.rules->manaCost.colorIdentity();
        }
        ci &= mtg::ManaAtom::COLORS_MASK;
        int pips = 0; for (uint8_t v = ci; v; v &= v - 1) ++pips; if (!pips) pips = 1;
        float px = MID_X + MID_W - 12.f - pips * 10.f;
        drawText(t, "Identity", px - 50.f, DP_CURVE_Y+4.f, 9, kDim);
        deckEditorColorPips(t, px, DP_CURVE_Y+4.f, ci);
    }

    int bins[8] = {};
    for (auto& e : m_deck) {
        if (e.rules->type.isLand()) continue;
        bins[std::min(e.rules->cmc(), 7)] += e.count;
    }
    int maxBin = *std::max_element(bins, bins+8);
    if (maxBin == 0) maxBin = 1;

    float chartX = MID_X + 18.f;
    float chartY = DP_CURVE_Y + 17.f;
    float chartH = DP_CURVE_H - 34.f;
    float barW   = (MID_W - 36.f) / 8.f;

    // Citadel: warm desaturated bar colours, CMC 0..7+
    static const sf::Color CMC_COL[8] = {
        {80, 76, 68},    // 0 — colorless / warm grey
        {80, 140, 90},   // 1 — low CMC green
        {68, 110, 170},  // 2 — blue
        {180, 80, 65},   // 3 — red
        {80, 60, 145},   // 4 — purple
        {130, 65, 150},  // 5 — deep purple
        {160, 120, 45},  // 6 — gold
        {130, 45, 45},   // 7+ — dark red
    };
    for (int i = 0; i < 8; ++i) {
        float bx   = chartX + i * barW;
        float barH = (bins[i] > 0) ? (bins[i] * chartH / (float)maxBin) : 0.f;
        float by   = chartY + chartH - barH;

        // Highlight selected CMC bar
        bool barSelected = (m_cmcFilter == i);
        sf::Color barCol = barSelected ? kAccent : CMC_COL[i];
        if (barH > 0) {
            drawRect(t, bx+2.f, by, barW-4.f, barH, barCol);
            drawText(t, std::to_string(bins[i]),
                     bx + barW/2.f - 5.f, by - 14.f, 10, barSelected ? kAccent : kBody);
        }
        // Always draw a thin background bar so zero-count bars are still clickable
        if (barH == 0) {
            drawRect(t, bx+2.f, chartY, barW-4.f, chartH, sf::Color(30, 28, 24));
        }
        std::string lbl = (i == 7) ? "7+" : std::to_string(i);
        drawText(t, lbl, bx + barW/2.f - 5.f, chartY + chartH + 2.f, 10, kDim);
    }
}

void DeckEditorScreen::drawDeckStats(sf::RenderTarget& t) const {
    drawRect(t, MID_X, DP_STATS_Y, MID_W, DP_STATS_H, kBg1);
    drawRect(t, MID_X, DP_STATS_Y, MID_W, 1.f, kBorder);

    bool valid = isValidDeck();
    drawText(t, validationMessage(), MID_X+5.f, DP_STATS_Y+7.f, 11,
             valid ? kGreen : kWarn, valid);

    int lands = 0, nonLand = 0;
    float sumCmc = 0.f;
    for (auto& e : m_deck) {
        if (e.rules->type.isLand()) lands += e.count;
        else { sumCmc += e.rules->cmc() * e.count; nonLand += e.count; }
    }
    float avgCmc = (nonLand > 0) ? sumCmc / nonLand : 0.f;

    drawText(t, "Lands: " + std::to_string(lands),
             MID_X+310.f, DP_STATS_Y+7.f, 11, kBody);
    char buf[32]; std::snprintf(buf, sizeof(buf), "Avg: %.1f", avgCmc);
    drawText(t, buf, MID_X+445.f, DP_STATS_Y+7.f, 11, kBody);

    // ── Rule-based deck suggestions ───────────────────────────────────────────
    // Count ramp cards (basic land fetchers, mana rocks) and card draw
    int rampCount = 0, drawCount = 0;
    for (auto& e : m_deck) {
        const std::string& oracle = e.rules->oracleText;
        auto lower = [](std::string s) {
            std::transform(s.begin(), s.end(), s.begin(), ::tolower); return s;
        };
        std::string lo = lower(oracle);
        if (lo.find("search your library") != std::string::npos &&
            lo.find("basic land")         != std::string::npos)
            rampCount += e.count;
        if (e.rules->type.isArtifact() && lo.find("add") != std::string::npos &&
            lo.find("mana")             != std::string::npos)
            rampCount += e.count;
        if (lo.find("draw") != std::string::npos &&
            lo.find("card")  != std::string::npos)
            drawCount += e.count;
    }

    // Count board wipes and interaction spells too
    int wipeCount = 0, interactCount = 0;
    for (auto& e : m_deck) {
        std::string lo2 = e.rules->oracleText;
        std::transform(lo2.begin(), lo2.end(), lo2.begin(),
            [](unsigned char c){ return static_cast<char>(::tolower(c)); });
        if ((lo2.find("destroy all") != std::string::npos ||
             lo2.find("exile all")   != std::string::npos) &&
             lo2.find("creature") != std::string::npos)
            wipeCount += e.count;
        if ((lo2.find("destroy target") != std::string::npos ||
             lo2.find("exile target")   != std::string::npos ||
             lo2.find("counter target") != std::string::npos))
            interactCount += e.count;
    }

    std::vector<std::string> tips;
    if (lands < 30) tips.push_back("Low lands (" + std::to_string(lands) + ") — aim 33-38");
    if (lands > 42) tips.push_back("High lands (" + std::to_string(lands) + ") — may be spell-light");
    if (avgCmc > 3.8f) tips.push_back(std::string("High avg CMC (") + buf + ") — add ramp");
    if (rampCount < 8)  tips.push_back("Low ramp (" + std::to_string(rampCount) + ") — add Sol Ring, Cultivate");
    if (drawCount < 8)  tips.push_back("Low draw (" + std::to_string(drawCount) + ") — add Rhystic Study, Phyrexian Arena");
    if (wipeCount < 2)  tips.push_back("Low board wipes — add Wrath of God, Cyclonic Rift");
    if (interactCount < 6) tips.push_back("Low removal — add Path to Exile, Swords to Plowshares");

    float ty = DP_STATS_Y + 22.f;
    for (const auto& tip : tips) {
        if (ty + 12.f > DP_STATS_Y + DP_STATS_H) break;
        drawText(t, "- " + tip, MID_X + 5.f, ty, 9, kGold);
        ty += 13.f;
    }
    if (tips.empty() && valid) {
        drawText(t, "+ Deck looks well-rounded!", MID_X + 5.f, ty, 9, kGreen);
    }
}

void DeckEditorScreen::drawAiPicker(sf::RenderTarget& t) const {
    drawRect(t, MID_X, DP_AI_Y, MID_W, DP_AI_H, kBg1);
    drawRect(t, MID_X, DP_AI_Y, MID_W, 1.f, kBorder);
    drawText(t, "vs AI:", MID_X+5.f, DP_AI_Y+10.f, 11, kHdrText);

    if (m_aiDecks.empty()) {
        drawText(t, "(built-in fallback)", MID_X+60.f, DP_AI_Y+10.f, 11, kDim);
        return;
    }
    drawButton(t, "<", MID_X+55.f, DP_AI_Y+4.f, 26.f, DP_AI_H-8.f, kBg3, kBody);
    std::string nm = (m_aiDeckIdx >= 0 && m_aiDeckIdx < (int)m_aiDeckNames.size())
                     ? m_aiDeckNames[m_aiDeckIdx] : "None";
    if (nm.size() > 36) nm = nm.substr(0,34) + "..";
    float nw = MID_W - 90.f - 38.f;
    drawRect(t, MID_X+85.f, DP_AI_Y+4.f, nw, DP_AI_H-8.f, kBg2, kBorder, 1.f);
    drawText(t, nm, MID_X+90.f, DP_AI_Y+10.f, 11, kBody);
    drawButton(t, ">", MID_X+MID_W-33.f, DP_AI_Y+4.f, 26.f, DP_AI_H-8.f, kBg3, kBody);
}

void DeckEditorScreen::drawStartButton(sf::RenderTarget& t) const {
    // "Main Menu" button — left portion
    drawRect(t, MID_X+5.f, DP_START_Y+5.f, 185.f, DP_START_H-10.f,
             kBg2, kBorder, 1.5f);
    {
        sf::Text mtxt("< Main Menu", m_font, 13);
        mtxt.setStyle(sf::Text::Bold);
        mtxt.setFillColor(kInk3);
        auto b = mtxt.getLocalBounds();
        mtxt.setPosition(MID_X+5.f + (185.f - b.width) * 0.5f - b.left,
                         DP_START_Y+5.f + (DP_START_H-10.f - b.height) * 0.5f - b.top);
        ui::applyTextScale(mtxt);
        t.draw(mtxt);
    }

    // "Start Game" button — right portion
    bool ok = isValidDeck();
    sf::Color fill   = ok ? kGold : kBg3;
    sf::Color border = ok ? kGoldBrt : kBorder;
    sf::Color text   = ok ? sf::Color(20, 12, 4) : kDim;

    drawRect(t, MID_X+195.f, DP_START_Y+5.f, MID_W-200.f, DP_START_H-10.f,
             fill, border, 2.f);

    std::string label = ok
        ? "START GAME  (Commander, " + std::to_string(totalDeckCards()) + " cards)"
        : "BUILD A VALID COMMANDER DECK TO START";

    sf::Text txt;
    txt.setFont(m_font); txt.setString(label); txt.setCharacterSize(14);
    txt.setStyle(sf::Text::Bold); txt.setFillColor(text);
    auto b = txt.getLocalBounds();
    float btnX = MID_X+195.f, btnW = MID_W-200.f;
    txt.setPosition(btnX + (btnW - b.width) * 0.5f - b.left,
                    DP_START_Y+5.f + (DP_START_H-10.f - b.height) * 0.5f - b.top);
    t.draw(txt);
}

// Tiny WUBRG colour-identity dots (colourless → one grey dot).
static void deckEditorColorPips(sf::RenderTarget& t, float x, float y, uint8_t ci) {
    static const struct { uint8_t bit; sf::Color col; } kP[5] = {
        {mtg::ManaAtom::WHITE, sf::Color(248, 246, 235)},
        {mtg::ManaAtom::BLUE,  sf::Color(70, 130, 210)},
        {mtg::ManaAtom::BLACK, sf::Color(120, 112, 120)},
        {mtg::ManaAtom::RED,   sf::Color(208, 80, 70)},
        {mtg::ManaAtom::GREEN, sf::Color(80, 170, 100)},
    };
    float cx = x; bool any = false;
    auto dot = [&](sf::Color c) {
        sf::CircleShape d(3.5f); d.setPosition(cx, y); d.setFillColor(c);
        d.setOutlineThickness(1.f); d.setOutlineColor(sf::Color(0, 0, 0, 90));
        t.draw(d); cx += 10.f;
    };
    for (auto& p : kP) if (ci & p.bit) { dot(p.col); any = true; }
    if (!any) dot(sf::Color(150, 143, 130));
}

void DeckEditorScreen::drawLoadMenu(sf::RenderTarget& t) const {
    float mx = MID_X + 3.f;
    float mw = MID_W - 6.f;
    float itemH = 22.f;
    float rowsY = DP_LIST_Y + 44.f;                 // header + search box above
    int   maxV  = static_cast<int>((DP_LIST_Y + DP_LIST_H - rowsY) / itemH);

    drawRect(t, mx, DP_LIST_Y, mw, DP_LIST_H, kBg1, kAccent, 2.f);
    drawText(t, "Load Deck — type to filter, click to select",
             mx+6.f, DP_LIST_Y+4.f, 12, kAccent, true);

    // Search box (always captures typing while the menu is open).
    drawRect(t, mx+6.f, DP_LIST_Y+20.f, mw-12.f, 18.f, kBg3, kAccent, 1.f);
    drawText(t, m_loadFilter.empty() ? std::string("(filter by name)")
                                     : m_loadFilter + "|",
             mx+12.f, DP_LIST_Y+22.f, 11, m_loadFilter.empty() ? kDim : kBody);
    drawText(t, std::to_string(m_loadVisible.size()) + "/" +
                std::to_string(m_savedDecks.size()),
             mx+mw-58.f, DP_LIST_Y+23.f, 10, kDim);

    for (int vi = m_loadMenuScroll;
         vi < (int)m_loadVisible.size() && vi < m_loadMenuScroll + maxV; ++vi) {
        int i = m_loadVisible[vi];
        float ry = rowsY + (vi - m_loadMenuScroll) * itemH;
        sf::Color bg = (vi % 2 == 0) ? kBg2 : kRowEven;
        drawRect(t, mx, ry, mw, itemH, bg);
        if (i < (int)m_savedCIKnown.size() && m_savedCIKnown[i]) {
            uint8_t ci = static_cast<uint8_t>(m_savedCI[i] & mtg::ManaAtom::COLORS_MASK);
            int pips = 0; for (uint8_t v = ci; v; v &= v - 1) ++pips; if (!pips) pips = 1;
            deckEditorColorPips(t, mx + mw - 14.f - pips * 10.f, ry + 6.f, ci);
        }
        std::string nm = m_savedNames[i];
        if (nm.size() > 40) nm = nm.substr(0, 38) + "..";
        drawText(t, nm, mx+8.f, ry+4.f, 12, kBody);
    }
    if (m_savedDecks.empty())
        drawText(t, "(no decks found in: " + userDecksDir().string() + ")",
                 mx+10.f, rowsY, 11, kDim);
    else if (m_loadVisible.empty())
        drawText(t, "(no decks match \"" + m_loadFilter + "\")", mx+10.f, rowsY, 11, kDim);
}

// ── Detail panel ──────────────────────────────────────────────────────────────

void DeckEditorScreen::drawDetailPanel(sf::RenderTarget& t) const {
    drawRect(t, DETAIL_X, 0, DETAIL_W, WIN_H, kDeckPanelBg);
    drawRect(t, DETAIL_X, 0, 1.f, WIN_H, kBorder);

    if (!m_detailCard) {
        drawText(t, "Hover a card", DETAIL_X + 20.f, WIN_H * 0.45f,       12, kDim);
        drawText(t, "to see details", DETAIL_X + 20.f, WIN_H * 0.45f + 18.f, 12, kDim);
        return;
    }

    const auto* r = m_detailCard;

    // Card image — fit in 320px tall, preserve 480:680 aspect ratio (~0.706)
    constexpr float IMG_H = 320.f;
    constexpr float IMG_W = IMG_H * 480.f / 680.f;   // ~226
    float imgX = DETAIL_X + (DETAIL_W - IMG_W) * 0.5f;
    constexpr float IMG_Y = 10.f;

    std::string imgPath = findCardImage(r->name, "");
    bool drewImage = false;
    if (!imgPath.empty()) {
        const sf::Texture* tex = TextureCache::get(imgPath);
        if (tex) {
            auto ts = tex->getSize();
            sf::Sprite spr(*tex);
            spr.setScale(IMG_W / static_cast<float>(ts.x),
                         IMG_H / static_cast<float>(ts.y));
            spr.setPosition(imgX, IMG_Y);
            t.draw(spr);

            sf::RectangleShape frame({IMG_W, IMG_H});
            frame.setPosition(imgX, IMG_Y);
            frame.setFillColor(sf::Color::Transparent);
            frame.setOutlineColor(colorForCard(r));
            frame.setOutlineThickness(2.f);
            t.draw(frame);
            drewImage = true;
        }
    }
    if (!drewImage) {
        drawRect(t, imgX, IMG_Y, IMG_W, IMG_H, kRowEven, colorForCard(r), 2.f);
        drawText(t, "No Image", imgX + IMG_W * 0.5f - 24.f, IMG_Y + IMG_H * 0.5f, 11, kDim);
    }

    // Text info below image
    float ty = IMG_Y + IMG_H + 14.f;
    float tx = DETAIL_X + 10.f;

    // Name
    std::string nm = r->name;
    if (nm.size() > 28) nm = nm.substr(0,26) + "..";
    drawText(t, nm, tx, ty, 13, kBody, true);
    ty += 20.f;

    // Mana cost
    std::string cost = manaCostStr(r);
    if (!cost.empty()) {
        drawText(t, cost, tx, ty, 12, kGoldBrt);
        ty += 17.f;
    }

    // Type line
    std::string tline = typeStr(r);
    if (tline.size() > 32) tline = tline.substr(0,30) + "..";
    drawText(t, tline, tx, ty, 11, kInk3);
    ty += 17.f;

    // P/T
    std::string pt = ptStr(r);
    if (!pt.empty()) {
        drawText(t, pt, tx, ty, 12, kInk, true);
        ty += 17.f;
    }

    ty += 8.f;
    drawRect(t, tx, ty, DETAIL_W - 20.f, 1.f, kBorder);
    ty += 8.f;

    // Format copy limit
    int maxC = maxCopies(r);
    std::string limitStr = r->type.isBasic()
        ? "Basic land (unlimited)"
        : "Max copies: " + std::to_string(maxC) + "  [Commander]";
    drawText(t, limitStr, tx, ty, 10, kDim);
    ty += 15.f;

    // How many currently in deck
    int inDeck = deckCount(r);
    if (inDeck > 0) {
        sf::Color ic = (r == m_commanderRules) ? sf::Color(200, 140, 255) : kGreen;
        std::string ds = (r == m_commanderRules) ? "COMMANDER" : "In deck: " + std::to_string(inDeck);
        drawText(t, ds, tx, ty, 10, ic, r == m_commanderRules);
        ty += 15.f;
    }

    // Commander hint for legendary creatures
    if (r->type.isLegendary() && inDeck > 0 && r != m_commanderRules) {
        drawText(t, "Right-click in deck list to set as commander",
                 tx, ty, 10, sf::Color(180, 145, 220));
        ty += 15.f;
    }

    // CMC
    if (!r->type.isLand()) {
        drawText(t, "CMC: " + std::to_string(r->cmc()), tx, ty, 10, kDim);
    }
}

// ── Paste-import overlay ──────────────────────────────────────────────────────

void DeckEditorScreen::drawPasteImportOverlay(sf::RenderTarget& t) const {
    // Dim background
    sf::RectangleShape dim({WIN_W, WIN_H});
    dim.setFillColor(sf::Color(0, 0, 0, 160));
    t.draw(dim);

    constexpr float pw = 540.f, ph = 480.f;
    float px0 = (WIN_W - pw) * 0.5f, py0 = (WIN_H - ph) * 0.5f;

    sf::RectangleShape pan({pw, ph});
    pan.setPosition(px0, py0);
    pan.setFillColor(sf::Color(18, 17, 16, 252));   // kBg1
    pan.setOutlineColor(kGold);
    pan.setOutlineThickness(2.f);
    t.draw(pan);

    drawText(t, "Paste Decklist  (Moxfield / MTGO / Arena)  [Esc to cancel]",
             px0 + 10.f, py0 + 10.f, 11, kGoldBrt);
    drawText(t, "Paste your list below and press Enter to import.",
             px0 + 10.f, py0 + 26.f, 9, kInk3);

    // Text area background
    sf::RectangleShape box({pw - 20.f, ph - 80.f});
    box.setPosition(px0 + 10.f, py0 + 44.f);
    box.setFillColor(kBg0);
    box.setOutlineColor(kBorder);
    box.setOutlineThickness(1.5f);
    t.draw(box);

    // Show typed text (last 20 lines)
    float ty = py0 + 48.f;
    std::string buf = m_pasteBuffer + "|";  // cursor
    size_t pos = 0;
    int shown = 0;
    while (pos < buf.size() && shown < 22) {
        auto nl = buf.find('\n', pos);
        std::string line = (nl == std::string::npos)
                         ? buf.substr(pos) : buf.substr(pos, nl - pos);
        if (line.size() > 60) line = line.substr(0, 59) + "...";
        drawText(t, line, px0 + 14.f, ty, 9, kInk2);
        ty += 13.f;
        if (nl == std::string::npos) break;
        pos = nl + 1;
        ++shown;
    }

    // Import button — gold primary action
    sf::RectangleShape btn({120.f, 24.f});
    btn.setPosition(px0 + pw - 130.f, py0 + ph - 32.f);
    btn.setFillColor(kGold);
    btn.setOutlineColor(sf::Color::Transparent);
    btn.setOutlineThickness(0.f);
    t.draw(btn);
    drawText(t, "Import  [Enter]", px0 + pw - 128.f, py0 + ph - 27.f, 10,
             sf::Color(20, 12, 4));
}

void DeckEditorScreen::importFromPastedList(const std::string& text) {
    // Parse Moxfield / MTGO / Arena / Archidekt formats.
    // Archidekt CSV: "Quantity,Name,Type,Set,Collector Number,Foil,Category"
    // We detect CSV by presence of commas in the first non-empty line.
    bool isCsv = false;
    {
        std::istringstream testSs(text);
        std::string firstLine;
        while (std::getline(testSs, firstLine)) {
            if (firstLine.empty()) continue;
            isCsv = (firstLine.find(',') != std::string::npos);
            break;
        }
    }
    if (isCsv) {
        // Archidekt / generic CSV: "Quantity,CardName,..."
        m_deck.clear(); m_commanderRules = nullptr; m_partnerRules = nullptr;
        m_deckName = "Imported Deck";
        std::istringstream csvSs(text);
        std::string csvLine;
        bool firstRow = true;
        while (std::getline(csvSs, csvLine)) {
            if (firstRow) { firstRow = false; continue; }  // skip header
            std::istringstream row(csvLine);
            std::string qty, name, type, set, col, foil, cat;
            if (!std::getline(row, qty, ','))  continue;
            if (!std::getline(row, name, ',')) continue;
            // Remove quotes if present
            if (!name.empty() && name.front() == '"') {
                name = name.substr(1);
                if (!name.empty() && name.back() == '"') name.pop_back();
            }
            int n = 0;
            std::from_chars(qty.data(), qty.data() + qty.size(), n);
            if (n <= 0) n = 1;
            const auto* rules = m_db.find(name);
            if (!rules) continue;
            if (std::getline(row, type, ',') && type.find("Commander") != std::string::npos) {
                if (!m_commanderRules) m_commanderRules = rules;
            } else {
                addCard(rules, n);
            }
        }
        m_deckRowsDirty = true;
        m_showPasteImport = false;
        m_pasteBuffer.clear();
        return;
    }
    // Each line is one of:
    //   "N CardName"        (mainboard)
    //   "Commander"         (section header → next lines become commander)
    //   ""                  (blank → reset to mainboard)
    m_deck.clear();
    m_commanderRules = nullptr;
    m_partnerRules   = nullptr;
    m_deckName = "Imported Deck";

    bool inCommander = false;
    std::istringstream ss(text);
    std::string line;
    while (std::getline(ss, line)) {
        // Trim
        while (!line.empty() && (line.front() == ' ' || line.front() == '\r')) line.erase(line.begin());
        while (!line.empty() && (line.back()  == ' ' || line.back()  == '\r')) line.pop_back();
        if (line.empty()) { inCommander = false; continue; }

        // Section headers
        std::string lo = line;
        std::transform(lo.begin(), lo.end(), lo.begin(), ::tolower);
        if (lo == "commander" || lo == "[commander]") { inCommander = true; continue; }
        if (lo == "deck" || lo == "main" || lo == "[main]" || lo == "mainboard") { inCommander = false; continue; }
        if (lo == "sideboard" || lo == "[sideboard]")  { inCommander = false; continue; }

        // Parse "N CardName" or "1x CardName"
        size_t space = line.find(' ');
        if (space == std::string::npos) continue;
        std::string numStr = line.substr(0, space);
        // Handle "1x" format
        if (!numStr.empty() && numStr.back() == 'x') numStr.pop_back();
        int n = 0;
        std::from_chars(numStr.data(), numStr.data() + numStr.size(), n);
        if (n <= 0) continue;
        std::string cardName = line.substr(space + 1);
        // Strip "(set/num)" suffixes from Arena format
        if (auto p = cardName.rfind(" ("); p != std::string::npos) cardName = cardName.substr(0, p);

        const auto* rules = m_db.find(cardName);
        if (!rules) continue;

        if (inCommander) {
            addCard(rules, 1);
            if (!m_commanderRules) m_commanderRules = rules;
            else if (!m_partnerRules) m_partnerRules = rules;
        } else {
            addCard(rules, n);
        }
    }
    m_deckRowsDirty = true;
    m_showPasteImport = false;
    m_pasteBuffer.clear();
}

void DeckEditorScreen::importFromUrl(const std::string& url) {
    // Convert known deck URL patterns to exportable plaintext URLs
    // Moxfield: https://www.moxfield.com/decks/DECKID -> https://api.moxfield.com/v2/decks/all/DECKID
    // Archidekt: https://archidekt.com/decks/DECKID -> text export not publicly available without login
    // We attempt to fetch the URL directly and parse whatever text we get.
    std::string fetchUrl = url;
    // Try to convert Moxfield deck URL to API endpoint
    if (url.find("moxfield.com/decks/") != std::string::npos) {
        auto pos = url.rfind('/');
        if (pos != std::string::npos) {
            std::string deckId = url.substr(pos + 1);
            fetchUrl = "https://api.moxfield.com/v2/decks/all/" + deckId;
        }
    }

    // Build wstring host and path
    std::wstring wHost, wPath;
    auto schemeEnd = fetchUrl.find("://");
    std::string rest = schemeEnd != std::string::npos ? fetchUrl.substr(schemeEnd + 3) : fetchUrl;
    auto slashPos = rest.find('/');
    std::string host = slashPos != std::string::npos ? rest.substr(0, slashPos) : rest;
    std::string path = slashPos != std::string::npos ? rest.substr(slashPos) : "/";
    wHost = std::wstring(host.begin(), host.end());
    wPath = std::wstring(path.begin(), path.end());

    std::string body = ui::CardImageDownloader::httpGet(wHost, wPath);
    if (body.empty()) {
        // Fall back to showing paste import with a hint
        m_showPasteImport = true;
        m_pasteBuffer.clear();
        return;
    }
    // Try to parse as decklist text
    importFromPastedList(body);
}

std::vector<std::string> DeckEditorScreen::getStapleRecommendations() const {
    // Universal Commander staples + color-specific recommendations
    // based on the commander's color identity.
    std::vector<std::string> recs;
    uint8_t ci = m_commanderRules ? m_commanderRules->manaCost.colorIdentity() : 0x1F;

    // Universal (colorless)
    static const char* kUniversal[] = {
        "Sol Ring", "Arcane Signet", "Command Tower", "Cultivate", "Kodama's Reach",
        "Swords to Plowshares", "Path to Exile", "Cyclonic Rift", "Rhystic Study",
        "Phyrexian Arena", "Smothering Tithe", "Thought Vessel",
        "Lightning Greaves", "Swiftfoot Boots", "Reliquary Tower",
    };
    for (const char* s : kUniversal)
        if (m_db.find(s)) recs.push_back(s);

    using namespace mtg::ManaAtom;
    if (ci & WHITE) { if (m_db.find("Generous Gift")) recs.push_back("Generous Gift"); }
    if (ci & BLUE)  { if (m_db.find("Counterspell"))  recs.push_back("Counterspell"); }
    if (ci & BLACK) { if (m_db.find("Demonic Tutor")) recs.push_back("Demonic Tutor"); }
    if (ci & RED)   { if (m_db.find("Chaos Warp"))    recs.push_back("Chaos Warp"); }
    if (ci & GREEN) { if (m_db.find("Rampant Growth")) recs.push_back("Rampant Growth"); }

    // Remove cards already in the deck
    recs.erase(std::remove_if(recs.begin(), recs.end(),
        [&](const std::string& n) {
            return std::any_of(m_deck.begin(), m_deck.end(),
                [&](const Entry& e) { return e.rules->name == n; });
        }), recs.end());
    return recs;
}

} // namespace ui
