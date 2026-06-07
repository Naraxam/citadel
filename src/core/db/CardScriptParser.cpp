#include "CardScriptParser.h"
#include <charconv>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>

// Local helper: parse a protection-type clause into a bitmask byte.
// Defined here so we don't create a dependency between CardRules and KeywordAbility.
static uint8_t parseProtectionType(std::string_view s) noexcept {
    if (s.find("artifact")    != std::string_view::npos) return 0x01;
    if (s.find("enchantment") != std::string_view::npos) return 0x02;
    if (s.find("creature")    != std::string_view::npos) return 0x04;
    if (s.find("instant")     != std::string_view::npos) return 0x08;
    if (s.find("sorcery")     != std::string_view::npos) return 0x10;
    if (s.find("monocolored") != std::string_view::npos) return 0x20;
    if (s.find("everything")  != std::string_view::npos) return 0xFF;
    return 0;
}

namespace mtg {

std::optional<CardRules> CardScriptParser::parseFile(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file) return std::nullopt;

    std::vector<std::string> lines;
    std::string line;
    while (std::getline(file, line)) {
        // Strip trailing carriage return (Windows line endings in the Forge repo)
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (!line.empty())
            lines.push_back(std::move(line));
    }
    return parseLines(lines);
}

std::optional<CardRules> CardScriptParser::parseText(std::string_view text) {
    std::vector<std::string> lines;
    std::string line;
    for (char c : text) {
        if (c == '\n') {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty()) lines.push_back(std::move(line));
            line.clear();
        } else {
            line += c;
        }
    }
    if (!line.empty()) lines.push_back(std::move(line));
    return parseLines(lines);
}

// ── DFC helpers ───────────────────────────────────────────────────────────────

static std::vector<std::string> readAllLines(const std::filesystem::path& path) {
    std::ifstream file(path);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) lines.push_back(std::move(line));
    }
    return lines;
}

static std::vector<std::string> textToLines(std::string_view text) {
    std::vector<std::string> lines;
    std::string line;
    for (char c : text) {
        if (c == '\n') {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty()) lines.push_back(std::move(line));
            line.clear();
        } else {
            line += c;
        }
    }
    if (!line.empty()) lines.push_back(std::move(line));
    return lines;
}

std::vector<CardRules> CardScriptParser::parseBothFaces(const std::filesystem::path& path) {
    return parseBothFacesLines(readAllLines(path));
}

std::vector<CardRules> CardScriptParser::parseBothFacesText(std::string_view text) {
    return parseBothFacesLines(textToLines(text));
}

std::vector<CardRules> CardScriptParser::parseBothFacesLines(const std::vector<std::string>& lines) {
    // Find the "ALTERNATE" separator (bare line, no colon)
    size_t altIdx = lines.size();
    for (size_t i = 0; i < lines.size(); ++i) {
        if (lines[i] == "ALTERNATE") { altIdx = i; break; }
    }

    std::vector<std::string> frontLines(lines.begin(), lines.begin() + (ptrdiff_t)altIdx);
    auto front = parseLines(frontLines);
    if (!front) return {};

    if (altIdx == lines.size()) {
        // Single-faced card
        return { std::move(*front) };
    }

    std::vector<std::string> backLines(lines.begin() + (ptrdiff_t)altIdx + 1, lines.end());
    auto back = parseLines(backLines);
    if (!back) return { std::move(*front) };

    // Wire names so CardDb::wireBackFaces() can link the two faces
    front->altName = back->name;
    back->altName  = front->name;

    return { std::move(*front), std::move(*back) };
}

std::optional<CardRules> CardScriptParser::parseLines(const std::vector<std::string>& lines) {
    CardRules rules;
    bool hasName = false;

    // Pre-pass: collect LEVEL band blocks (they don't use the colon-key format)
    // Format: "LEVEL X-Y" or "LEVEL X+" on one line, P/T on the next line.
    for (size_t li = 0; li + 1 < lines.size(); ++li) {
        std::string_view sv = lines[li];
        if (sv.size() < 8 || sv.substr(0, 6) != "LEVEL ") continue;
        std::string_view range = sv.substr(6);  // "X-Y" or "X+"
        CardRules::LevelBand band;
        band.minLevel = 0; band.maxLevel = 9999;
        auto dash = range.find('-');
        auto plus = range.find('+');
        if (dash != std::string_view::npos) {
            std::from_chars(range.data(), range.data() + dash, band.minLevel);
            std::from_chars(range.data() + dash + 1,
                            range.data() + range.size(), band.maxLevel);
        } else if (plus != std::string_view::npos) {
            std::from_chars(range.data(), range.data() + plus, band.minLevel);
            band.maxLevel = 9999;
        } else {
            continue;  // unrecognised format
        }
        // Next line should be P/T like "2/3"
        std::string_view pt = lines[li + 1];
        auto slash = pt.find('/');
        if (slash != std::string_view::npos) {
            band.power     = std::string(pt.substr(0, slash));
            band.toughness = std::string(pt.substr(slash + 1));
            ++li;  // skip the P/T line
        }
        rules.levelBands.push_back(std::move(band));
    }

    for (const auto& line : lines) {
        // Skip LEVEL band lines (already parsed above)
        if (line.size() >= 6 && std::string_view(line).substr(0, 6) == "LEVEL ") continue;

        // Split on the first ':' only — values may contain colons (SVar bodies, Oracle text)
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;

        std::string_view key   = std::string_view(line).substr(0, colon);
        std::string_view value = std::string_view(line).substr(colon + 1);

        if (key == "Name") {
            rules.name = value;
            hasName = true;

        } else if (key == "ManaCost") {
            rules.manaCost = ManaCost::parse(value);

        } else if (key == "Types") {
            rules.type = CardType::parse(value);

        } else if (key == "PT") {
            auto slash = value.find('/');
            if (slash != std::string_view::npos) {
                rules.power     = value.substr(0, slash);
                rules.toughness = value.substr(slash + 1);
            }

        } else if (key == "SplitName") {
            rules.splitName = std::string(value);
            rules.hasSplit  = true;
        } else if (key == "SplitCost") {
            rules.splitCost = ManaCost::parse(value);
            rules.hasSplit  = true;
        } else if (key == "SplitOracle") {
            rules.splitOracleText = std::string(value);
        } else if (key == "SplitPT") {
            auto sl = value.find('/');
            if (sl != std::string_view::npos) {
                rules.splitPower     = std::string(value.substr(0, sl));
                rules.splitToughness = std::string(value.substr(sl + 1));
            }
        } else if (key == "AdventureName") {
            rules.adventureName = std::string(value);
            rules.hasAdventure  = true;
        } else if (key == "AdventureCost") {
            rules.adventureCost = ManaCost::parse(value);
            rules.hasAdventure  = true;
        } else if (key == "K") {
            rules.keywords.emplace_back(value);
            // ── Parse type-based protection and colour-specific hexproof ─────────
            {
                std::string_view v = value;
                std::string vl(v);
                std::transform(vl.begin(), vl.end(), vl.begin(),
                    [](unsigned char c){ return static_cast<char>(::tolower(c)); });
                if (vl.substr(0, 15) == "protection from") {
                    uint8_t typeBits = parseProtectionType(vl);
                    if (typeBits) rules.protectionTypeMask |= typeBits;
                    // Color protection is handled by parseKeyword → buildKeywordMask
                }
                if (vl.substr(0, 13) == "hexproof from") {
                    std::string_view rest = vl;
                    rest.remove_prefix(14);  // "hexproof from "
                    uint8_t mask = 0;
                    if (rest.find("white") != std::string_view::npos) mask |= 0x01;
                    if (rest.find("blue")  != std::string_view::npos) mask |= 0x02;
                    if (rest.find("black") != std::string_view::npos) mask |= 0x04;
                    if (rest.find("red")   != std::string_view::npos) mask |= 0x08;
                    if (rest.find("green") != std::string_view::npos) mask |= 0x10;
                    rules.hexproofFromColor |= mask;
                }
            }
            if (value.size() > 10 && value.substr(0, 10) == "Flashback:") {
                rules.flashbackCost = ManaCost::parse(value.substr(10));
                rules.hasFlashback  = true;
            }
            else if (value.size() > 7 && value.substr(0, 7) == "Kicker:") {
                rules.kickerCost = ManaCost::parse(value.substr(7));
                rules.hasKicker  = true;
            } else if (value.size() > 8 && value.substr(0, 8) == "Buyback:") {
                rules.buybackCost = ManaCost::parse(value.substr(8));
                rules.hasBuyback  = true;
            } else if (value == "Convoke") {
                rules.hasConvoke = true;
            } else if (value == "Retrace") {
                rules.hasRetrace = true;
            } else if (value.size() > 8 && value.substr(0, 8) == "Madness:") {
                rules.madnessCost = ManaCost::parse(value.substr(8));
                rules.hasMadness  = true;
            } else if (value == "Improvise") {
                rules.hasImprovise = true;
            } else if (value == "Cascade") {
                rules.hasCascade = true;
            } else if (value.size() > 10 && value.substr(0, 10) == "Prototype:") {
                // K:Prototype:COST:P/T  e.g. K:Prototype:1 G:3/4
                auto protoRest = value.substr(10);
                auto protoColon = protoRest.rfind(':');
                if (protoColon != std::string_view::npos) {
                    rules.prototypeCost = ManaCost::parse(protoRest.substr(0, protoColon));
                    auto pt = protoRest.substr(protoColon + 1);
                    auto slash = pt.find('/');
                    if (slash != std::string_view::npos) {
                        rules.prototypePower     = std::string(pt.substr(0, slash));
                        rules.prototypeToughness = std::string(pt.substr(slash + 1));
                    }
                    rules.hasPrototype = true;
                }
            } else if (value.size() > 8 && value.substr(0, 8) == "Disguise") {
                // K:Disguise:COST or K:Disguise (uses morph mechanics + ward 2)
                rules.hasDisguise = true;
                rules.hasMorph    = true;  // Disguise is a Morph variant
                if (value.size() > 9 && value[8] == ':')
                    rules.disguiseCost = ManaCost::parse(value.substr(9));
                rules.hasWard  = true;
                rules.wardCost = ManaCost::parse("2");  // ward 2 while face-down
            } else if (value == "Companion") {
                rules.hasCompanion = true;
            } else if (value == "Epic") {
                rules.hasEpic = true;
            } else if (value == "Banding") {
                rules.hasBanding = true;
            } else if (value == "Haunt") {
                rules.hasHaunt = true;
            } else if (value == "Background") {
                rules.hasBackground = true;
            } else if (value == "Choose a Background") {
                rules.choosesBackground = true;
            } else if (value == "Case") {
                rules.hasCase = true;
            } else if (value == "Split Second") {
                rules.hasSplitSecond = true;
            } else if (value == "Prowess") {
                rules.hasProwess = true;
            } else if (value.size() > 5 && value.substr(0, 5) == "Ward:") {
                rules.wardCost = ManaCost::parse(value.substr(5));
                rules.hasWard  = true;
            } else if (value.size() > 11 && value.substr(0, 11) == "etbCounter:") {
                // K:etbCounter:P1P1:X[:optional extra fields]
                auto rest = value.substr(11);
                auto c1   = rest.find(':');
                if (c1 != std::string_view::npos) {
                    std::string ctype  = std::string(rest.substr(0, c1));
                    auto rest2 = rest.substr(c1 + 1);
                    auto c2    = rest2.find(':');
                    std::string amount = std::string(c2 == std::string_view::npos
                                                     ? rest2 : rest2.substr(0, c2));
                    rules.etbCounters.push_back({std::move(ctype), std::move(amount)});
                }
            } else if (value.size() > 14 && value.substr(0, 14) == "CastSurcharge:") {
                int n = 0;
                std::from_chars(value.data() + 14, value.data() + value.size(), n);
                rules.castSurcharge = std::max(0, n);
            } else if (value.size() > 5 && value.substr(0, 5) == "Crew:") {
                int cost = 0;
                std::from_chars(value.data() + 5, value.data() + value.size(), cost);
                rules.hasCrew  = true;
                rules.crewCost = cost;
            } else if (value.size() > 8 && value.substr(0, 8) == "Chapter:") {
                // K:Chapter:N:SVar1,SVar2,...SVar3  (one SVar per chapter)
                auto chRest = value.substr(8);
                auto chSep  = chRest.find(':');
                if (chSep != std::string_view::npos) {
                    int maxChap = 0;
                    std::from_chars(chRest.data(), chRest.data() + chSep, maxChap);
                    CardRules::SagaEntry se;
                    se.maxChapter = maxChap;
                    // Parse comma-separated SVar list
                    auto svList = chRest.substr(chSep + 1);
                    while (!svList.empty()) {
                        auto comma = svList.find(',');
                        se.chapterSVars.emplace_back(
                            comma == std::string_view::npos ? svList : svList.substr(0, comma));
                        svList = comma == std::string_view::npos
                                 ? std::string_view{} : svList.substr(comma + 1);
                    }
                    rules.saga = std::move(se);
                }
            } else if (value.size() > 20 && value.substr(0, 20) == "ETBReplacement:Copy:") {
                // K:ETBReplacement:Copy:SVar[:Optional]
                auto ecRest = value.substr(20);
                auto ecSep  = ecRest.find(':');
                std::string svarName = std::string(ecSep == std::string_view::npos
                                                   ? ecRest : ecRest.substr(0, ecSep));
                bool optional = ecSep != std::string_view::npos &&
                                ecRest.substr(ecSep + 1) == "Optional";
                rules.etbCopy = CardRules::ETBCopyEntry{ std::move(svarName), optional };
            } else if (value.size() > 21 && value.substr(0, 21) == "ETBReplacement:Other:") {
                // K:ETBReplacement:Other:SVar[:Optional] — run the named SVar's
                // effect as the permanent enters (e.g. ChooseCT → choose a type).
                auto eoRest = value.substr(21);
                auto eoSep  = eoRest.find(':');
                rules.etbOtherSVar = std::string(eoSep == std::string_view::npos
                                                 ? eoRest : eoRest.substr(0, eoSep));
            } else if (value.size() > 8 && value.substr(0, 8) == "Cycling:") {
                rules.cyclingCost = ManaCost::parse(value.substr(8));
                rules.hasCycling  = true;
            } else if (value.size() > 12 && value.substr(0, 12) == "TypeCycling:") {
                // Format: "TypeCycling:Basic:1" → cyclingType="Basic", cost="1"
                auto tcRest = value.substr(12);
                auto tcSep  = tcRest.rfind(':');
                if (tcSep != std::string_view::npos) {
                    rules.cyclingType     = std::string(tcRest.substr(0, tcSep));
                    rules.typeCyclingCost = ManaCost::parse(tcRest.substr(tcSep + 1));
                    rules.hasTypeCycling  = true;
                }
            } else if (value.size() > 7 && value.substr(0, 7) == "Dredge:") {
                int n = 0;
                std::from_chars(value.data() + 7, value.data() + value.size(), n);
                rules.hasDredge    = true;
                rules.dredgeAmount = n;
            } else if (value.size() > 6 && value.substr(0, 6) == "Evoke:") {
                rules.evokeCost = ManaCost::parse(value.substr(6));
                rules.hasEvoke  = true;
            } else if (value == "Delve") {
                rules.hasDelve = true;
            } else if (value.size() > 12 && value.substr(0, 12) == "Bloodthirst:") {
                int n = 0;
                std::from_chars(value.data() + 12, value.data() + value.size(), n);
                rules.hasBloodthirst    = true;
                rules.bloodthirstAmount = n;
            } else if (value.size() > 12 && value.substr(0, 12) == "Annihilator:") {
                int n = 0;
                std::from_chars(value.data() + 12, value.data() + value.size(), n);
                rules.hasAnnihilator   = true;
                rules.annihilatorCount = n;
            } else if (value == "Annihilator") {
                rules.hasAnnihilator   = true;
                rules.annihilatorCount = 1;
            } else if (value.size() > 6 && value.substr(0, 6) == "Morph:") {
                rules.morphCost = ManaCost::parse(value.substr(6));
                rules.hasMorph  = true;
            } else if (value.size() > 10 && value.substr(0, 10) == "Megamorph:") {
                rules.megamorphCost = ManaCost::parse(value.substr(10));
                rules.hasMegamorph  = true;
            } else if (value.size() > 5 && value.substr(0, 5) == "Dash:") {
                rules.dashCost = ManaCost::parse(value.substr(5));
                rules.hasDash  = true;
            } else if (value.size() > 6 && value.substr(0, 6) == "Blitz:") {
                rules.blitzCost = ManaCost::parse(value.substr(6));
                rules.hasBlitz  = true;
            } else if (value == "Myriad") {
                rules.hasMyriad = true;
            } else if (value == "Cipher") {
                rules.hasCipher = true;
            } else if (value.size() > 9 && value.substr(0, 9) == "Casualty ") {
                rules.hasCasualty = true;
                int n = 1;
                std::from_chars(value.data() + 9, value.data() + value.size(), n);
                rules.casualtyAmount = std::max(1, n);
            } else if (value.size() > 10 && value.substr(0, 10) == "Replicate:") {
                rules.replicateCost = ManaCost::parse(value.substr(10));
                rules.hasReplicate  = true;
            } else if (value.size() > 7 && value.substr(0, 7) == "Bestow:") {
                rules.bestowCost = ManaCost::parse(value.substr(7));
                rules.hasBestow  = true;
            } else if (value.size() > 10 && value.substr(0, 10) == "Transmute:") {
                rules.transmuteCost = ManaCost::parse(value.substr(10));
                rules.hasTransmute  = true;
            } else if (value.size() > 5 && value.substr(0, 5) == "Meld:") {
                // K:Meld:PartnerName:ResultName
                rules.hasMeld = true;
                auto meldRest = value.substr(5);
                auto meldColon = meldRest.find(':');
                if (meldColon != std::string_view::npos) {
                    rules.meldPartner = std::string(meldRest.substr(0, meldColon));
                    rules.meldResult  = std::string(meldRest.substr(meldColon + 1));
                }
            } else if (value == "Flanking") {
                rules.hasFlanking = true;
            } else if (value.size() > 8 && value.substr(0, 8) == "Rampage ") {
                rules.hasRampage = true;
                int n = 1;
                std::from_chars(value.data() + 8, value.data() + value.size(), n);
                rules.rampageAmount = std::max(1, n);
            } else if (value == "Boast") {
                rules.hasBoast = true;
            } else if (value.size() > 7 && value.substr(0, 7) == "Strive:") {
                rules.striveCost = ManaCost::parse(value.substr(7));
                rules.hasStrive  = true;
            } else if (value.size() > 8 && value.substr(0, 8) == "Fortify:") {
                rules.fortifyCost = ManaCost::parse(value.substr(8));
                rules.hasFortify  = true;
            } else if (value.size() > 10 && value.substr(0, 10) == "Reinforce:") {
                // Format: Reinforce:N:COST  or  Reinforce N-COST
                auto colon2 = value.find(':', 10);
                if (colon2 != std::string_view::npos) {
                    int n = 1;
                    std::from_chars(value.data() + 10, value.data() + colon2, n);
                    rules.reinforceAmount = std::max(1, n);
                    rules.reinforceCost   = ManaCost::parse(value.substr(colon2 + 1));
                }
                rules.hasReinforce = true;
            } else if (value.size() > 8 && value.substr(0, 8) == "Amplify ") {
                rules.hasAmplify = true;
                int n = 1;
                std::from_chars(value.data() + 8, value.data() + value.size(), n);
                rules.amplifyAmount = std::max(1, n);
            } else if (value.size() > 7 && value.substr(0, 7) == "Fading ") {
                rules.hasFading = true;
                int n = 0;
                std::from_chars(value.data() + 7, value.data() + value.size(), n);
                rules.fadingAmount = std::max(0, n);
            } else if (value.size() > 10 && value.substr(0, 10) == "Vanishing ") {
                rules.hasVanishing = true;
                int n = 0;
                std::from_chars(value.data() + 10, value.data() + value.size(), n);
                rules.vanishingAmount = std::max(0, n);
            } else if (value == "Battle Cry") {
                rules.hasBattleCry = true;
            } else if (value == "Conspire") {
                rules.hasConspire = true;
            } else if (value.size() > 6 && value.substr(0, 6) == "Graft ") {
                rules.hasGraft = true;
                int n = 0;
                std::from_chars(value.data() + 6, value.data() + value.size(), n);
                rules.graftCount = std::max(1, n);
            } else if (value == "Manifest") {
                rules.hasManifest = true;
            } else if (value == "Sunburst") {
                rules.hasSunburst = true;
            } else if (value.size() > 9 && value.substr(0, 9) == "Forecast:") {
                rules.forecastCost = ManaCost::parse(value.substr(9));
                rules.hasForecast  = true;
            } else if (value == "Phasing") {
                rules.hasPhasing = true;
            } else if (value.size() > 8 && value.substr(0, 8) == "Disturb:") {
                rules.disturbCost = ManaCost::parse(value.substr(8));
                rules.hasDisturb  = true;
            } else if (value == "Bargain") {
                rules.hasBargain = true;
            } else if (value.size() >= 7 && value.substr(0, 7) == "Backup ") {
                rules.hasBackup = true;
                int n = 1;
                std::from_chars(value.data() + 7, value.data() + value.size(), n);
                rules.backupAmount = std::max(1, n);
            } else if (value.size() > 7 && value.substr(0, 7) == "Encore:") {
                rules.encoreCost = ManaCost::parse(value.substr(7));
                rules.hasEncore  = true;
            } else if (value.size() >= 7 && value.substr(0, 7) == "Connive") {
                rules.hasConnive = true;
                int n = 1;
                if (value.size() > 8 && value[7] == ':')
                    std::from_chars(value.data() + 8, value.data() + value.size(), n);
                rules.conniveAmount = std::max(1, n);
            } else if (value.size() > 7 && value.substr(0, 7) == "Renown:") {
                int n = 0;
                std::from_chars(value.data() + 7, value.data() + value.size(), n);
                rules.hasRenown    = true;
                rules.renownAmount = n;
            } else if (value == "Mentor") {
                rules.hasMentor = true;
            } else if (value.size() > 6 && value.substr(0, 6) == "Surge:") {
                rules.surgeCost = ManaCost::parse(value.substr(6));
                rules.hasSurge  = true;
            } else if (value.size() > 10 && value.substr(0, 10) == "Spectacle:") {
                rules.spectacleCost = ManaCost::parse(value.substr(10));
                rules.hasSpectacle  = true;
            } else if (value.size() > 8 && value.substr(0, 8) == "Miracle:") {
                rules.miracleCost = ManaCost::parse(value.substr(8));
                rules.hasMiracle  = true;
            } else if (value.size() > 9 && value.substr(0, 9) == "Overload:") {
                rules.overloadCost = ManaCost::parse(value.substr(9));
                rules.hasOverload  = true;
            } else if (value.size() > 7 && value.substr(0, 7) == "Embalm:") {
                rules.embalmCost = ManaCost::parse(value.substr(7));
                rules.hasEmbalm  = true;
            } else if (value.size() > 11 && value.substr(0, 11) == "Eternalize:") {
                rules.eternalizeCost = ManaCost::parse(value.substr(11));
                rules.hasEternalize  = true;
            } else if (value.size() > 9 && value.substr(0, 9) == "Foretell:") {
                rules.foretellCost = ManaCost::parse(value.substr(9));
                rules.hasForetell  = true;
            } else if (value == "Rebound") {
                rules.hasRebound = true;
            } else if (value.size() > 8 && value.substr(0, 8) == "Suspend:") {
                // K:Suspend:N:COST  or  K:Suspend:X:XMin1 X cost
                auto rest = value.substr(8);
                auto sep  = rest.find(':');
                if (sep != std::string_view::npos) {
                    auto countPart = rest.substr(0, sep);
                    auto costPart  = rest.substr(sep + 1);
                    // Strip any "XMin1 " prefix from cost (Aeon Chronicler style)
                    auto costClean = costPart;
                    if (costClean.substr(0, 6) == "XMin1 ")
                        costClean = costClean.substr(6);
                    rules.hasSuspend   = true;
                    rules.suspendCost  = ManaCost::parse(costClean);
                    if (countPart == "X") {
                        rules.suspendCountIsX = true;
                        rules.suspendCount    = 0;
                    } else {
                        std::from_chars(countPart.data(),
                                        countPart.data() + countPart.size(),
                                        rules.suspendCount);
                    }
                }
            } else if (value == "Riot") {
                rules.hasRiot = true;
            } else if (value.size() > 7 && value.substr(0, 7) == "Emerge:") {
                rules.emergeCost = ManaCost::parse(value.substr(7));
                rules.hasEmerge  = true;
            } else if (value.size() > 8 && value.substr(0, 8) == "Afflict:") {
                int n = 0;
                std::from_chars(value.data() + 8, value.data() + value.size(), n);
                rules.hasAfflict    = true;
                rules.afflictAmount = n;
            } else if (value.size() > 9 && value.substr(0, 9) == "Champion:") {
                rules.hasChampion    = true;
                rules.championFilter = std::string(value.substr(9));
            } else if (value.size() > 7 && value.substr(0, 7) == "Modular") {
                if (value.size() > 8 && value[7] == ':') {
                    int n = 0;
                    std::from_chars(value.data() + 8, value.data() + value.size(), n);
                    rules.hasModular   = true;
                    rules.modularCount = n;
                    // Modular enters with N +1/+1 counters (reuses the etbCounter path)
                    rules.etbCounters.push_back({"P1P1", std::to_string(n)});
                }
            } else if (value.size() > 8 && value.substr(0, 8) == "Unearth:") {
                rules.unearthCost = ManaCost::parse(value.substr(8));
                rules.hasUnearth  = true;
            } else if (value.size() > 7 && value.substr(0, 7) == "Escape:") {
                // K:Escape:COST ExileFromGrave<N/...>
                // Split on " ExileFromGrave<" to separate the mana cost from the exile count.
                auto rest      = value.substr(7);
                auto graveMark = rest.find(" ExileFromGrave<");
                if (graveMark != std::string_view::npos) {
                    rules.escapeCost = ManaCost::parse(rest.substr(0, graveMark));
                    auto countStr    = rest.substr(graveMark + 16); // skip " ExileFromGrave<"
                    int n = 0;
                    std::from_chars(countStr.data(), countStr.data() + countStr.size(), n);
                    rules.escapeExile = n;
                } else {
                    // Fallback: treat everything as the cost with no exile count
                    rules.escapeCost = ManaCost::parse(rest);
                    rules.escapeExile = 0;
                }
                rules.hasEscape = true;
            } else if (value == "Jump-start") {
                rules.hasJumpStart = true;
            } else if (value.size() > 10 && value.substr(0, 10) == "Fabricate:") {
                int n = 0;
                std::from_chars(value.data() + 10, value.data() + value.size(), n);
                rules.hasFabricate    = true;
                rules.fabricateAmount = n;
            } else if (value == "Extort") {
                rules.hasExtort = true;
            } else if (value.size() > 10 && value.substr(0, 10) == "Soulshift:") {
                int n = 0;
                std::from_chars(value.data() + 10, value.data() + value.size(), n);
                rules.hasSoulshift    = true;
                rules.soulshiftAmount = n;
            } else if (value.size() > 8 && value.substr(0, 8) == "Tribute:") {
                int n = 0;
                std::from_chars(value.data() + 8, value.data() + value.size(), n);
                rules.hasTribute    = true;
                rules.tributeAmount = n;
            } else if (value.size() > 9 && value.substr(0, 9) == "Scavenge:") {
                rules.scavengeCost = ManaCost::parse(value.substr(9));
                rules.hasScavenge  = true;
            } else if (value == "Heroic") {
                rules.hasHeroic = true;
            } else if (value == "Inspired") {
                rules.hasInspired = true;
            } else if (value == "Evolve") {
                rules.hasEvolve = true;
            } else if (value.size() > 12 && value.substr(0, 12) == "Monstrosity:") {
                // K:Monstrosity:N:COST  e.g.  "Monstrosity:3:4 G G"
                auto mRest = value.substr(12);
                auto mSep  = mRest.find(':');
                if (mSep != std::string_view::npos) {
                    int n = 0;
                    std::from_chars(mRest.data(), mRest.data() + mSep, n);
                    rules.hasMonstrosity   = true;
                    rules.monstrosityCount = n;
                    rules.monstrosityCost  = ManaCost::parse(mRest.substr(mSep + 1));
                }
            } else if (value.size() > 5 && value.substr(0, 5) == "Echo:") {
                rules.echoCost = ManaCost::parse(value.substr(5));
                rules.hasEcho  = true;
            } else if (value.size() > 19 &&
                       (value.substr(0, 19) == "Cumulative upkeep:" ||
                        value.substr(0, 19) == "Cumulative Upkeep:")) {
                // K:Cumulative upkeep:COST   or   K:Cumulative upkeep:AddCounter<N/TYPE>:desc
                auto cuRest = value.substr(19);
                if (cuRest.size() > 13 && cuRest.substr(0, 13) == "AddCounter<1/") {
                    // Counter-based variant: "AddCounter<1/M1M1>:Put a -1/-1 counter…"
                    auto cEnd = cuRest.find('>');
                    if (cEnd != std::string_view::npos) {
                        rules.cumulativeUpkeepCounterType = std::string(cuRest.substr(13, cEnd - 13));
                        rules.cumulativeUpkeepIsCounter   = true;
                    }
                } else {
                    rules.cumulativeUpkeepCost = ManaCost::parse(cuRest);
                }
                rules.hasCumulativeUpkeep = true;
            } else if (value.size() > 9 && value.substr(0, 9) == "Affinity:") {
                rules.affinityType = std::string(value.substr(9));
                rules.hasAffinity  = true;
            } else if (value.size() > 6 && value.substr(0, 6) == "Toxic:") {
                int n = 0;
                std::from_chars(value.data() + 6, value.data() + value.size(), n);
                rules.hasToxic    = true;
                rules.toxicAmount = n;
            } else if (value == "Exalted") {
                rules.hasExalted = true;
            } else if (value == "Training") {
                rules.hasTraining = true;
            } else if (value.size() > 9 && value.substr(0, 9) == "Ninjutsu:") {
                rules.ninjutsuCost = ManaCost::parse(value.substr(9));
                rules.hasNinjutsu  = true;
            } else if (value.size() > 9 && value.substr(0, 9) == "Level up:") {
                // K:Level up:COST  (note lowercase "up")
                rules.levelUpCost = ManaCost::parse(value.substr(9));
                rules.hasLevelUp  = true;
            }

        } else if (key == "A") {
            rules.abilityLines.emplace_back(value);
            // Detect Channel abilities (AB$ lines with ActivationZone$ Hand)
            if (value.find("ActivationZone$ Hand") != std::string_view::npos)
                rules.hasChannel = true;

        } else if (key == "T") {
            rules.triggerLines.emplace_back(value);

        } else if (key == "S") {
            rules.staticAbilityLines.emplace_back(value);

        } else if (key == "R") {
            rules.replacementLines.emplace_back(value);
            // Detect "can't be countered" — common shorthand check:
            // R:Event$ Countered | ValidCard$ Card.Self | ReplaceWith$ Nothing
            if (value.find("Countered") != std::string_view::npos &&
                value.find("Self")      != std::string_view::npos)
                rules.cantBeCountered = true;

        } else if (key == "SVar") {
            // Format: SVar:VarName:body
            auto varColon = value.find(':');
            if (varColon != std::string_view::npos) {
                std::string varName(value.substr(0, varColon));
                std::string varBody(value.substr(varColon + 1));
                rules.svars.emplace(std::move(varName), std::move(varBody));
            }

        } else if (key == "Loyalty") {
            std::from_chars(value.data(), value.data() + value.size(),
                            rules.initialLoyalty);

        } else if (key == "Oracle") {
            rules.oracleText = unescape(value);

        } else if (key == "ALTNAME") {
            rules.altName = value;

        }
        // DeckHas, DeckHints, AlternateMode, etc. are metadata not needed for gameplay
    }

    if (!hasName) return std::nullopt;
    return rules;
}

std::string CardScriptParser::unescape(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size() && s[i + 1] == 'n') {
            out += '\n';
            ++i;
        } else {
            out += s[i];
        }
    }
    return out;
}

} // namespace mtg
