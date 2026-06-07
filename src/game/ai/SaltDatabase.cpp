#include "SaltDatabase.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#endif

namespace mtg {

using json = nlohmann::json;

std::unordered_map<std::string, float> SaltDatabase::s_scores;

// ── Static fallback ───────────────────────────────────────────────────────────
// Sourced from EDHREC top-100 salt list (scores as of 2026).

static const std::vector<std::pair<std::string, float>> kStaticSalt = {
    // Top tier
    {"Stasis",                   3.06f},
    {"Winter Orb",               2.96f},
    {"Vivi Ornitier",            2.81f},
    {"Tergrid, God of Fright",   2.80f},
    {"Rhystic Study",            2.73f},
    {"Cyclonic Rift",            2.66f},
    {"Expropriate",              2.58f},
    {"Smothering Tithe",         2.50f},
    {"Teferi, Time Raveler",     2.44f},
    {"Mana Drain",               2.38f},
    {"Drannith Magistrate",      2.32f},
    {"Opposition Agent",         2.25f},
    {"Narset, Parter of Veils",  2.19f},
    {"Dockside Extortionist",    2.12f},
    {"Thassa's Oracle",          2.06f},
    {"Nadu, Winged Wisdom",      2.00f},
    // High tier
    {"Grand Arbiter Augustin IV",1.92f},
    {"Armageddon",               1.85f},
    {"Ruination",                1.78f},
    {"Vorinclex, Voice of Hunger",1.72f},
    {"Jin-Gitaxias, Core Augur", 1.65f},
    {"Mindslaver",               1.59f},
    {"Sunder",                   1.53f},
    {"Consecrated Sphinx",       1.47f},
    {"Elesh Norn, Grand Cenobite",1.41f},
    {"Blightsteel Colossus",     1.36f},
    {"Possessed Portal",         1.30f},
    {"Knowledge Pool",           1.25f},
    {"Torment of Hailfire",      1.20f},
    {"Humility",                 1.15f},
    {"Leovold, Emissary of Trest",1.10f},
    // Mid tier
    {"Blood Moon",               1.05f},
    {"Back to Basics",           1.00f},
    {"Possibility Storm",        0.95f},
    {"Obliterate",               0.90f},
    {"Contamination",            0.85f},
    {"Living End",               0.80f},
    {"Aetherflux Reservoir",     0.75f},
    {"Nekusar, the Mindrazer",   0.70f},
    {"Decree of Annihilation",   0.65f},
    {"Jokulhaups",               0.62f},
    {"Infernal Darkness",        0.58f},
    {"Scrambleverse",            0.54f},
    {"Painful Quandary",         0.50f},
    // Lower tier
    {"Spreading Plague",         0.45f},
    {"Gaddock Teeg",             0.42f},
    {"Grave Pact",               0.38f},
    {"Etali, Primal Storm",      0.35f},
    {"Boros Charm",              0.32f},
    {"Aven Mindcensor",          0.28f},
    {"Price of Progress",        0.25f},
    {"Wound Reflection",         0.22f},
};

void SaltDatabase::loadStaticFallback() {
    for (const auto& [name, score] : kStaticSalt)
        s_scores.emplace(name, score);
}

// ── JSON cache I/O ────────────────────────────────────────────────────────────

int SaltDatabase::load(const std::string& cachePath) {
    loadStaticFallback(); // always start with static data

    std::ifstream f(cachePath);
    if (!f.is_open()) return static_cast<int>(s_scores.size());

    json root;
    try { f >> root; }
    catch (...) { return static_cast<int>(s_scores.size()); }

    if (!root.is_object()) return static_cast<int>(s_scores.size());

    int added = 0;
    for (auto& [name, val] : root.items()) {
        float score = val.is_number() ? val.get<float>() : 0.f;
        if (score > 0.f) { s_scores[name] = score; ++added; }
    }
    std::cout << "[SaltDatabase] loaded " << added << " entries from cache ("
              << s_scores.size() << " total)\n";
    return static_cast<int>(s_scores.size());
}

void SaltDatabase::saveCache(const std::string& cachePath) {
    json root = json::object();
    for (const auto& [name, score] : s_scores)
        root[name] = score;

    std::ofstream f(cachePath);
    if (f.is_open()) {
        f << root.dump(2);
        std::cout << "[SaltDatabase] cached " << s_scores.size()
                  << " scores → " << cachePath << '\n';
    }
}

float SaltDatabase::get(const std::string& cardName) {
    if (s_scores.empty()) loadStaticFallback();
    auto it = s_scores.find(cardName);
    return it != s_scores.end() ? it->second : 0.0f;
}

// ── EDHREC fetch (Windows only) ──────────────────────────────────────────────

#ifdef _WIN32

// Generic GET https://<host><path>. Returns empty on any failure or non-200.
static std::string httpsGet(const std::wstring& host, const std::wstring& path) {
    HINTERNET hSession = WinHttpOpen(L"CitadelMTG/1.0 SaltDB",
                                     WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                     WINHTTP_NO_PROXY_NAME,
                                     WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return {};
    auto close = [](HINTERNET h) { if (h) WinHttpCloseHandle(h); };

    HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(),
                                        INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET hRequest = hConnect
        ? WinHttpOpenRequest(hConnect, L"GET", path.c_str(),
                              nullptr, WINHTTP_NO_REFERER,
                              WINHTTP_DEFAULT_ACCEPT_TYPES,
                              WINHTTP_FLAG_SECURE)
        : nullptr;
    if (!hRequest) { close(hConnect); close(hSession); return {}; }

    DWORD timeout = 8000;
    WinHttpSetOption(hRequest, WINHTTP_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof(timeout));
    WinHttpSetOption(hRequest, WINHTTP_OPTION_CONNECT_TIMEOUT, &timeout, sizeof(timeout));
    WinHttpSetOption(hRequest, WINHTTP_OPTION_SEND_TIMEOUT,    &timeout, sizeof(timeout));

    if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                             WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(hRequest, nullptr)) {
        close(hRequest); close(hConnect); close(hSession); return {};
    }
    DWORD status = 0, statusSize = sizeof(status);
    WinHttpQueryHeaders(hRequest,
                         WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                         WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
                         WINHTTP_NO_HEADER_INDEX);
    if (status != 200) {
        close(hRequest); close(hConnect); close(hSession); return {};
    }
    std::string body;
    DWORD avail = 0;
    while (WinHttpQueryDataAvailable(hRequest, &avail) && avail > 0) {
        std::string chunk(avail, '\0');
        DWORD read = 0;
        WinHttpReadData(hRequest, chunk.data(), avail, &read);
        body.append(chunk.data(), read);
    }
    close(hRequest); close(hConnect); close(hSession);
    return body;
}

// EDHREC publishes a single salt page at /top/salt but it's heavily truncated.
// The format-bucketed variants (year/month/week) and the popular-cards pages
// often contain hundreds of distinct cards with embedded salt scores in their
// `label` text — together they massively expand coverage beyond the static
// fallback.
static std::string fetchEdhrecPath(const std::wstring& path) {
    return httpsGet(L"json.edhrec.com", path);
}

// Try to parse salt score from a label string like "Salt Score: 3.06\n16546 decks".
// Returns 0.f if the pattern is not found.
static float parseSaltLabel(const std::string& label) {
    static const std::string kPrefix = "Salt Score: ";
    auto pos = label.find(kPrefix);
    if (pos == std::string::npos) return 0.f;
    pos += kPrefix.size();
    float val = 0.f;
    try { val = std::stof(label.substr(pos)); } catch (...) {}
    return val;
}

// Walk any JSON value recursively; collect entries that have a "name" and
// either a direct "salt" number or a "label" containing "Salt Score: X.XX".
// This handles both the old EDHREC format (salt field) and the current format
// (label field with embedded score).
static void extractSaltEntries(const json& node,
                                std::unordered_map<std::string, float>& out,
                                int depth = 0) {
    if (depth > 10) return;
    if (node.is_object()) {
        auto nameIt  = node.find("name");
        auto saltIt  = node.find("salt");
        auto labelIt = node.find("label");

        if (nameIt != node.end() && nameIt->is_string()) {
            float score = 0.f;
            if (saltIt != node.end() && saltIt->is_number()) {
                score = saltIt->get<float>();
            } else if (labelIt != node.end() && labelIt->is_string()) {
                score = parseSaltLabel(labelIt->get<std::string>());
            }
            if (score > 0.f)
                out[nameIt->get<std::string>()] = score;
        }

        for (auto& [k, v] : node.items())
            extractSaltEntries(v, out, depth + 1);
    } else if (node.is_array()) {
        for (const auto& elem : node)
            extractSaltEntries(elem, out, depth + 1);
    }
}

int SaltDatabase::fetchAndCache(const std::string& cachePath) {
    loadStaticFallback();

    // Only the dedicated salt pages embed "Salt Score: X.XX" in their card
    // labels. The general /top/* pages (cards, creatures, year, …) label cards
    // with deck-inclusion counts ("In 12345 decks") and carry NO salt data, so
    // querying them is pure noise. As of 2026 EDHREC also returns 403 for the
    // /salt/{year,month,week} buckets — they're kept here only in case EDHREC
    // restores them; a 403 yields an empty body and is skipped harmlessly.
    // Net result: salt coverage is the ~Top 100 from salt.json plus whatever
    // buckets happen to be live, merged over the static fallback.
    static const wchar_t* kPaths[] = {
        L"/pages/top/salt.json",
        L"/pages/top/salt/year.json",
        L"/pages/top/salt/month.json",
        L"/pages/top/salt/week.json",
    };

    int updated      = 0;   // entries added or whose score changed
    int fetchedTotal = 0;   // distinct salt entries parsed from live pages
    int pagesOk      = 0;   // pages that returned parseable salt data
    for (const wchar_t* path : kPaths) {
        // Convert path to ASCII for the log line (paths are pure ASCII).
        std::string asciiPath;
        for (const wchar_t* p = path; *p; ++p)
            asciiPath += static_cast<char>(*p);
        std::cout << "[SaltDatabase] fetching " << asciiPath << "...\n";
        std::string body = fetchEdhrecPath(path);
        if (body.empty()) continue;
        try {
            json root = json::parse(body);
            std::unordered_map<std::string, float> fetched;
            extractSaltEntries(root, fetched);
            if (fetched.empty()) continue;   // 200 but no salt labels — skip
            ++pagesOk;
            fetchedTotal += static_cast<int>(fetched.size());
            int changedThisPage = 0;
            for (auto& [name, score] : fetched) {
                auto it = s_scores.find(name);
                // Refresh live scores even when not strictly higher, so the
                // DB tracks EDHREC's current numbers rather than a stale peak.
                if (it == s_scores.end() || it->second != score) {
                    s_scores[name] = score;
                    ++changedThisPage;
                }
            }
            updated += changedThisPage;
            std::cout << "  parsed " << fetched.size() << " salt scores ("
                      << changedThisPage << " new/changed)\n";
        } catch (const std::exception& e) {
            std::cerr << "  JSON parse failed: " << e.what() << "\n";
        }
    }

    // A successful fetch that produced no *changes* (cache already current) is
    // still a success — only report failure when no page yielded salt data.
    if (pagesOk > 0) {
        std::cout << "[SaltDatabase] fetched " << fetchedTotal
                  << " salt scores from " << pagesOk << " EDHREC page(s), "
                  << updated << " new/changed (" << s_scores.size()
                  << " total in DB)\n";
        saveCache(cachePath);
    } else {
        std::cout << "[SaltDatabase] no salt data fetched (offline or EDHREC "
                     "format changed). Using current cache + static fallback.\n";
    }
    return static_cast<int>(s_scores.size());
}

#else  // non-Windows

int SaltDatabase::fetchAndCache(const std::string& cachePath) {
    loadStaticFallback();
    std::cout << "[SaltDatabase] WinHTTP not available on this platform "
                 "— using static fallback\n";
    (void)cachePath;
    return static_cast<int>(s_scores.size());
}

#endif

} // namespace mtg
