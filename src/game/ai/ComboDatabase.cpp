#include "ComboDatabase.h"
#include "../GameState.h"
#include "../Player.h"
#include "../Zone.h"
#include "../Card.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
#include <charconv>

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#endif

namespace mtg {

using json = nlohmann::json;

// ── JSON loading ──────────────────────────────────────────────────────────────

static ComboStep::Type parseStepType(std::string_view s) {
    if (s == "activate") return ComboStep::Type::Activate;
    if (s == "equip")    return ComboStep::Type::Equip;
    return ComboStep::Type::Cast;
}

void ComboDatabase::loadFromJson(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) {
        std::cout << "[ComboDatabase] file not found: " << path << '\n';
        return;
    }

    json root;
    try {
        f >> root;
    } catch (const json::parse_error& e) {
        std::cerr << "[ComboDatabase] JSON parse error: " << e.what() << '\n';
        return;
    }

    if (!root.is_array()) {
        std::cerr << "[ComboDatabase] expected JSON array at root\n";
        return;
    }

    for (const auto& obj : root) {
        try {
            ComboEntry entry;
            entry.id          = obj.value("id",          "");
            entry.description = obj.value("description", "");
            entry.condition   = obj.value("condition",   "");

            for (const auto& r : obj.at("required"))
                entry.required.push_back(r.get<std::string>());

            for (const auto& step : obj.at("sequence")) {
                ComboStep s;
                s.type           = parseStepType(step.value("type", "cast"));
                s.cardName       = step.value("card",          "");
                s.targetCardName = step.value("target_card",   "");
                s.targetFilter   = step.value("target_filter", "");
                s.abilityIndex   = step.value("ability_index", 0);
                entry.sequence.push_back(std::move(s));
            }

            if (!entry.id.empty() && !entry.required.empty())
                m_combos.push_back(std::move(entry));
        } catch (const std::exception& e) {
            std::cerr << "[ComboDatabase] skipping malformed entry: " << e.what() << '\n';
        }
    }

    std::cout << "[ComboDatabase] loaded " << m_combos.size() << " combos\n";
}

// ── Helpers ───────────────────────────────────────────────────────────────────

bool ComboDatabase::hasCard(const std::string& name,
                             uint8_t playerId,
                             const GameState& game) {
    const Player& p = game.player(playerId);
    for (const Card* c : p.hand().cards())
        if (c->rules->name == name) return true;
    for (const Card* c : game.battlefield().cards())
        if (c->controllerId == playerId && c->rules->name == name) return true;
    return false;
}

bool ComboDatabase::evalCondition(const std::string& cond,
                                   uint8_t playerId,
                                   const GameState& game) {
    if (cond.empty() || cond == "always") return true;

    const Player& me  = game.player(playerId);
    const Player& opp = game.player(playerId ^ 1);

    auto parseInt = [](std::string_view sv, int& out) -> bool {
        auto r = std::from_chars(sv.data(), sv.data() + sv.size(), out);
        return r.ec == std::errc{};
    };

    if (cond.rfind("own_life >= ", 0) == 0) {
        int n = 0;
        parseInt(std::string_view(cond).substr(12), n);
        return me.life() >= n;
    }
    if (cond.rfind("opp_life <= ", 0) == 0) {
        int n = 0;
        parseInt(std::string_view(cond).substr(12), n);
        return opp.life() <= n;
    }
    if (cond.rfind("own_creatures >= ", 0) == 0) {
        int n = 0;
        parseInt(std::string_view(cond).substr(17), n);
        int count = 0;
        for (const Card* c : game.battlefield().cards())
            if (c->controllerId == playerId && c->isCreature()) ++count;
        return count >= n;
    }
    if (cond.rfind("opp_creatures >= ", 0) == 0) {
        int n = 0;
        parseInt(std::string_view(cond).substr(17), n);
        int count = 0;
        for (const Card* c : game.battlefield().cards())
            if (c->controllerId != playerId && c->isCreature()) ++count;
        return count >= n;
    }
    return true; // unknown condition — don't block
}

// ── Main lookup ───────────────────────────────────────────────────────────────

// ── Commander Spellbook download ─────────────────────────────────────────────

#ifdef _WIN32
// HTTPS GET on backend.commanderspellbook.com. Returns body or empty on failure.
static std::string spellbookGet(const std::wstring& path) {
    HINTERNET hSession = WinHttpOpen(L"CitadelMTG/1.0 ComboDB",
                                     WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                     WINHTTP_NO_PROXY_NAME,
                                     WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return {};
    auto close = [](HINTERNET h) { if (h) WinHttpCloseHandle(h); };

    HINTERNET hConnect = WinHttpConnect(hSession,
                                         L"backend.commanderspellbook.com",
                                         INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET hRequest = hConnect
        ? WinHttpOpenRequest(hConnect, L"GET", path.c_str(),
                              nullptr, WINHTTP_NO_REFERER,
                              WINHTTP_DEFAULT_ACCEPT_TYPES,
                              WINHTTP_FLAG_SECURE)
        : nullptr;
    if (!hRequest) { close(hConnect); close(hSession); return {}; }

    DWORD timeout = 15000;
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
#endif

int ComboDatabase::downloadFromSpellbook(const std::string& cachePath,
                                          int maxCombos) {
#ifdef _WIN32
    json out = json::array();
    int written = 0;
    int offset  = 0;
    constexpr int kPageSize = 100;          // API caps page size at 100
    const bool unlimited = (maxCombos <= 0);

    while (unlimited || written < maxCombos) {
        std::wstring path = L"/variants/?limit=" +
                            std::to_wstring(kPageSize) +
                            L"&offset=" + std::to_wstring(offset);
        std::cout << "[ComboDatabase] fetching variants offset=" << offset
                  << "...\n";
        std::string body = spellbookGet(path);
        if (body.empty()) {
            std::cerr << "[ComboDatabase] fetch failed at offset " << offset
                      << " — keeping " << written << " combos collected so far.\n";
            break;
        }
        json page;
        try { page = json::parse(body); }
        catch (const std::exception& e) {
            std::cerr << "[ComboDatabase] JSON parse failed: " << e.what() << "\n";
            break;
        }
        if (!page.contains("results") || !page["results"].is_array()) break;
        const auto& results = page["results"];
        if (results.empty()) break;

        for (const auto& v : results) {
            if (!unlimited && written >= maxCombos) break;
            ComboEntry entry;
            if (v.contains("id"))
                entry.id = std::to_string(v["id"].is_number()
                    ? v["id"].get<long long>()
                    : 0);
            if (entry.id.empty() && v.contains("identity"))
                entry.id = v["identity"].get<std::string>();

            // Description: prefer the Spellbook "description" field, falling
            // back to a comma-joined produces list so the AI/UX can at least
            // explain what the combo does.
            if (v.contains("description") && v["description"].is_string())
                entry.description = v["description"].get<std::string>();
            if (entry.description.empty() && v.contains("produces") &&
                v["produces"].is_array()) {
                std::string desc;
                for (const auto& p : v["produces"]) {
                    if (!p.is_object() || !p.contains("feature")) continue;
                    const auto& feat = p["feature"];
                    if (feat.is_object() && feat.contains("name") &&
                        feat["name"].is_string()) {
                        if (!desc.empty()) desc += ", ";
                        desc += feat["name"].get<std::string>();
                    }
                }
                entry.description = std::move(desc);
            }

            // Required cards live in `uses[].card.name`. Each entry can also
            // carry a quantity, but for AI presence-check we only need the
            // distinct card names.
            if (v.contains("uses") && v["uses"].is_array()) {
                for (const auto& u : v["uses"]) {
                    if (!u.is_object()) continue;
                    if (!u.contains("card")) continue;
                    const auto& card = u["card"];
                    if (card.is_object() && card.contains("name") &&
                        card["name"].is_string())
                        entry.required.push_back(card["name"].get<std::string>());
                }
            }
            if (entry.id.empty() || entry.required.empty()) continue;

            // Emit JSON in the same schema loadFromJson expects.
            json jentry;
            jentry["id"]          = entry.id;
            jentry["description"] = entry.description;
            jentry["required"]    = entry.required;
            jentry["condition"]   = "";        // unknown — leave open
            jentry["sequence"]    = json::array();  // no auto-execute
            out.push_back(std::move(jentry));
            ++written;
        }

        // Stop if Spellbook says there's no next page.
        if (!page.contains("next") || page["next"].is_null()) break;
        offset += kPageSize;
    }

    if (written == 0) return 0;

    std::ofstream f(cachePath);
    if (!f.is_open()) {
        std::cerr << "[ComboDatabase] cannot write cache: " << cachePath << "\n";
        return 0;
    }
    f << out.dump(2);
    f.close();
    std::cout << "[ComboDatabase] downloaded " << written
              << " combos → " << cachePath << "\n";

    // Reload from the newly-written file so callers see the fresh data.
    m_combos.clear();
    loadFromJson(cachePath);
    return written;
#else
    (void)cachePath; (void)maxCombos;
    std::cout << "[ComboDatabase] download is Windows-only (WinHTTP).\n";
    return 0;
#endif
}

const ComboEntry* ComboDatabase::findAssembled(uint8_t playerId,
                                                const GameState& game) const {
    for (const ComboEntry& entry : m_combos) {
        if (!evalCondition(entry.condition, playerId, game)) continue;

        bool allPresent = true;
        for (const std::string& name : entry.required) {
            if (!hasCard(name, playerId, game)) { allPresent = false; break; }
        }
        if (allPresent) return &entry;
    }
    return nullptr;
}

} // namespace mtg
