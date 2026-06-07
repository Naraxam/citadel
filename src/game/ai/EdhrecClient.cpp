#include "EdhrecClient.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <iostream>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#endif

namespace mtg::EdhrecClient {

using json = nlohmann::json;

// ── slugify ──────────────────────────────────────────────────────────────────
// EDHREC URL slugs are lowercase ASCII with non-alphanumerics collapsed to "-"
// and leading/trailing/double dashes stripped. The Python helper proved this
// matches the live URL scheme for both commanders and tags.
std::string slugify(const std::string& name) {
    std::string out;
    out.reserve(name.size());
    bool lastDash = true;  // suppress leading dashes
    for (unsigned char c : name) {
        if (c >= 'A' && c <= 'Z') {
            out += static_cast<char>(c - 'A' + 'a');
            lastDash = false;
        } else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            out += static_cast<char>(c);
            lastDash = false;
        } else {
            // Apostrophes get DELETED (EDHREC: "K'rrik" → "krrik"), everything
            // else becomes a single dash. Diacritics on a UTF-8 byte stream
            // pass through as multi-byte > 127 — drop them entirely.
            if (c == '\'' || c == '`' || c > 127) continue;
            if (!lastDash) { out += '-'; lastDash = true; }
        }
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    return out;
}

// ── WinHTTP fetch ────────────────────────────────────────────────────────────
#ifdef _WIN32

// Fetches https://json.edhrec.com<path> and returns the body. Empty on any
// failure or non-200 response.
static std::string fetchJson(const std::string& path) {
    const std::wstring host = L"json.edhrec.com";
    std::wstring wpath(path.begin(), path.end());

    HINTERNET hSession = WinHttpOpen(L"CitadelMTG/1.0 EdhrecClient",
                                     WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                     WINHTTP_NO_PROXY_NAME,
                                     WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return {};
    auto close = [](HINTERNET h) { if (h) WinHttpCloseHandle(h); };

    HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(),
                                         INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET hRequest = hConnect
        ? WinHttpOpenRequest(hConnect, L"GET", wpath.c_str(),
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

    // Reject anything that isn't 200 — 404s on tag-specific decks are common
    // and the caller falls back to the generic deck endpoint.
    DWORD status = 0;
    DWORD statusSize = sizeof(status);
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

#else
static std::string fetchJson(const std::string&) { return {}; }
#endif

// ── Commander → tags ─────────────────────────────────────────────────────────
std::optional<std::vector<TagInfo>>
fetchCommanderTags(const std::string& commanderName) {
    std::string slug = slugify(commanderName);
    if (slug.empty()) return std::nullopt;

    std::string body = fetchJson("/pages/commanders/" + slug + ".json");
    if (body.empty()) return std::nullopt;

    std::vector<TagInfo> tags;
    try {
        json root = json::parse(body);
        // EDHREC structure: root.panels.taglinks = [ {label, slug, count, …}, … ]
        // Be defensive about each step in case the schema shifts.
        if (!root.contains("panels")) return tags;
        const auto& panels = root["panels"];
        if (!panels.contains("taglinks")) return tags;
        const auto& links = panels["taglinks"];
        if (!links.is_array()) return tags;
        for (const auto& t : links) {
            TagInfo info;
            if (t.contains("value") && t["value"].is_string())
                info.name = t["value"].get<std::string>();
            else if (t.contains("label") && t["label"].is_string())
                info.name = t["label"].get<std::string>();
            if (t.contains("slug") && t["slug"].is_string())
                info.slug = t["slug"].get<std::string>();
            if (t.contains("count") && t["count"].is_number())
                info.count = t["count"].get<int>();
            if (!info.slug.empty()) {
                if (info.name.empty()) info.name = info.slug;
                tags.push_back(std::move(info));
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "[EdhrecClient] tag JSON parse failed: " << e.what() << "\n";
        return std::nullopt;
    }
    // Sort by inclusion count desc so the most-relevant archetypes lead.
    std::sort(tags.begin(), tags.end(),
              [](const TagInfo& a, const TagInfo& b){ return a.count > b.count; });
    return tags;
}

// ── Commander + tag → average deck ───────────────────────────────────────────
static std::optional<std::vector<DeckLine>>
parseDeckBody(const std::string& body) {
    if (body.empty()) return std::nullopt;
    try {
        json root = json::parse(body);
        if (!root.contains("deck")) return std::nullopt;
        const auto& deck = root["deck"];
        if (!deck.is_array() || deck.empty()) return std::nullopt;
        std::vector<DeckLine> out;
        out.reserve(deck.size());
        for (const auto& entry : deck) {
            if (!entry.is_string()) continue;
            // Each line is "N CardName" (Forge-style).
            std::string s = entry.get<std::string>();
            auto sp = s.find(' ');
            if (sp == std::string::npos) continue;
            int n = 0;
            try { n = std::stoi(s.substr(0, sp)); } catch (...) { continue; }
            if (n <= 0) continue;
            std::string name = s.substr(sp + 1);
            // Strip leading whitespace and any trailing CR.
            while (!name.empty() && (name.front() == ' ' || name.front() == '\t')) name.erase(0, 1);
            while (!name.empty() && (name.back() == '\r' || name.back() == '\n')) name.pop_back();
            if (name.empty()) continue;
            out.push_back({n, std::move(name)});
        }
        if (out.empty()) return std::nullopt;
        return out;
    } catch (const std::exception& e) {
        std::cerr << "[EdhrecClient] deck JSON parse failed: " << e.what() << "\n";
        return std::nullopt;
    }
}

std::optional<std::vector<DeckLine>>
fetchAverageDeck(const std::string& commanderName,
                  const std::string& tagSlug) {
    std::string cmdSlug = slugify(commanderName);
    if (cmdSlug.empty()) return std::nullopt;

    // Prefer the tag-specific deck; fall back to the commander's generic one.
    if (!tagSlug.empty()) {
        std::string body = fetchJson("/pages/average-decks/" + cmdSlug + "/" + tagSlug + ".json");
        if (auto deck = parseDeckBody(body)) return deck;
    }
    std::string body = fetchJson("/pages/average-decks/" + cmdSlug + ".json");
    return parseDeckBody(body);
}

}  // namespace mtg::EdhrecClient
