#include "CardImageDownloader.h"
#include <filesystem>
#include <fstream>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <chrono>
#include <deque>
#include <unordered_map>
#include <vector>
#include <iostream>
#include <cstdio>
#include <algorithm>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>

namespace ui {

namespace fs = std::filesystem;

namespace {

// ── Globals ───────────────────────────────────────────────────────────────────

std::mutex              g_mutex;
std::condition_variable g_cv;

std::string g_dir;

// name (sanitized) → CDN URL
std::unordered_map<std::string, std::string> g_oracle;

// Current download batch
std::vector<DownloadEntry> g_entries;
std::unordered_map<std::string, size_t> g_nameIdx;  // name → index in g_entries
std::deque<size_t>         g_queue;                 // indices of Queued entries

std::vector<std::thread> g_workers;
bool g_running = false;
constexpr int kWorkerCount = 4;

// Active WinHTTP session handles — allows stop() to cancel in-flight I/O.
// Protected by g_sessionsMu (separate from g_mutex to avoid deadlock).
std::mutex g_sessionsMu;
std::unordered_map<std::thread::id, HINTERNET> g_activeSessions;

// Bulk-data fetch progress (-1 idle, 0..100 active) and status text.
std::mutex  g_bulkMu;
int         g_bulkPct = -1;
std::string g_bulkStatus;

// ── Helpers ───────────────────────────────────────────────────────────────────

std::wstring toWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<size_t>(n - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

// Extract the value of the first JSON string field matching "key":"..." after pos.
std::string jsonStr(const std::string& line, const std::string& key, size_t pos = 0) {
    std::string search = "\"" + key + "\":\"";
    size_t p = line.find(search, pos);
    if (p == std::string::npos) return {};
    p += search.size();
    std::string val;
    val.reserve(128);
    while (p < line.size() && line[p] != '"') {
        if (line[p] == '\\') { ++p; if (p < line.size()) val += line[p]; }
        else val += line[p];
        ++p;
    }
    return val;
}

// Find the first "image_uris":{...,"normal":"URL"...} in the line and return the URL.
std::string extractNormalUrl(const std::string& line) {
    size_t iuPos = line.find("\"image_uris\":{");
    if (iuPos == std::string::npos) return {};
    return jsonStr(line, "normal", iuPos);
}

// ── Downloader ────────────────────────────────────────────────────────────────

// Download one card by index. Registers the WinHTTP session in g_activeSessions
// so that stop() can cancel blocking I/O immediately.
bool downloadOne(size_t idx) {
    std::string name, url;
    {
        std::lock_guard lk(g_mutex);
        if (!g_running) return false;
        name = g_entries[idx].name;
        auto it = g_oracle.find(CardImageDownloader::sanitizeName(name));
        if (it == g_oracle.end()) {
            g_entries[idx].state = DownloadEntry::State::Failed;
            return false;
        }
        url = it->second;
    }

    fs::path outPath = fs::path(g_dir) / (CardImageDownloader::sanitizeName(name) + ".full.jpg");
    {
        std::error_code ec;
        if (fs::exists(outPath, ec)) {
            std::lock_guard lk(g_mutex);
            g_entries[idx].state   = DownloadEntry::State::Done;
            g_entries[idx].percent = 100;
            return true;
        }
    }

    // Parse URL: "https://cards.scryfall.io/normal/..."
    const std::string prefix = "https://";
    if (url.size() <= prefix.size()) return false;
    std::string rest = url.substr(prefix.size());
    auto slashPos = rest.find('/');
    if (slashPos == std::string::npos) return false;
    std::string host = rest.substr(0, slashPos);
    std::string path = rest.substr(slashPos);

    HINTERNET hSession = WinHttpOpen(
        L"CitadelMTG/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return false;

    // Register so stop() can cancel this download.
    auto tid = std::this_thread::get_id();
    {
        std::lock_guard lk(g_sessionsMu);
        g_activeSessions[tid] = hSession;
    }

    // Helper: unregister and close handles.
    // If stop() already closed hSession (entry removed), skip closing it.
    auto cleanup = [&](HINTERNET hReq, HINTERNET hConn) {
        bool sessionStillOurs = false;
        {
            std::lock_guard lk(g_sessionsMu);
            auto it = g_activeSessions.find(tid);
            if (it != g_activeSessions.end()) {
                g_activeSessions.erase(it);
                sessionStillOurs = true;
            }
        }
        if (hReq)  WinHttpCloseHandle(hReq);
        if (hConn) WinHttpCloseHandle(hConn);
        if (sessionStillOurs) WinHttpCloseHandle(hSession);
        // else: stop() already closed hSession and all derived handles.
    };

    HINTERNET hConnect = WinHttpConnect(hSession, toWide(host).c_str(),
                                        INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET hRequest = hConnect
        ? WinHttpOpenRequest(hConnect, L"GET", toWide(path).c_str(),
                             nullptr, WINHTTP_NO_REFERER,
                             WINHTTP_DEFAULT_ACCEPT_TYPES,
                             WINHTTP_FLAG_SECURE)
        : nullptr;

    if (!hRequest) { cleanup(hRequest, hConnect); return false; }

    // 5-second timeouts — fast enough for CDN, short enough for clean shutdown.
    DWORD dwTimeout = 5000;
    WinHttpSetOption(hRequest, WINHTTP_OPTION_RECEIVE_TIMEOUT, &dwTimeout, sizeof(dwTimeout));
    WinHttpSetOption(hRequest, WINHTTP_OPTION_CONNECT_TIMEOUT, &dwTimeout, sizeof(dwTimeout));
    WinHttpSetOption(hRequest, WINHTTP_OPTION_SEND_TIMEOUT,    &dwTimeout, sizeof(dwTimeout));

    if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(hRequest, nullptr)) {
        cleanup(hRequest, hConnect); return false;
    }

    DWORD status = 0, statusSz = sizeof(status);
    WinHttpQueryHeaders(hRequest,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSz, WINHTTP_NO_HEADER_INDEX);
    if (status != 200) { cleanup(hRequest, hConnect); return false; }

    DWORD contentLen = 0, clSz = sizeof(contentLen);
    WinHttpQueryHeaders(hRequest,
        WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &contentLen, &clSz, WINHTTP_NO_HEADER_INDEX);

    fs::path tmpPath = outPath.string() + ".tmp";
    bool ok = false;
    {
        std::ofstream ofs(tmpPath, std::ios::binary);
        if (ofs) {
            DWORD avail = 0;
            size_t total = 0;
            while (WinHttpQueryDataAvailable(hRequest, &avail) && avail > 0) {
                std::vector<char> buf(avail);
                DWORD bytesRead = 0;
                if (!WinHttpReadData(hRequest, buf.data(), avail, &bytesRead) || bytesRead == 0)
                    break;
                ofs.write(buf.data(), static_cast<std::streamsize>(bytesRead));
                total += bytesRead;
                if (contentLen > 0) {
                    std::lock_guard lk(g_mutex);
                    g_entries[idx].percent =
                        static_cast<int>(std::min<size_t>(total, contentLen) * 100 / contentLen);
                }
            }
            ok = (total >= 1024);
            if (!ok) {
                ofs.close();
                std::error_code ec; fs::remove(tmpPath, ec);
            }
        }
    }

    cleanup(hRequest, hConnect);

    if (ok) {
        std::error_code ec;
        fs::rename(tmpPath, outPath, ec);
        if (ec) { fs::remove(tmpPath, ec); return false; }
    }
    return ok;
}

void workerFunc() {
    while (true) {
        size_t idx;
        {
            std::unique_lock lk(g_mutex);
            g_cv.wait(lk, [] { return !g_queue.empty() || !g_running; });
            if (!g_running && g_queue.empty()) break;
            if (g_queue.empty()) continue;
            idx = g_queue.front();
            g_queue.pop_front();
            g_entries[idx].state = DownloadEntry::State::Downloading;
        }

        bool ok = downloadOne(idx);

        {
            std::lock_guard lk(g_mutex);
            g_entries[idx].state   = ok ? DownloadEntry::State::Done
                                        : DownloadEntry::State::Failed;
            g_entries[idx].percent = ok ? 100 : 0;
        }
    }
}

} // namespace

// ── Public API ────────────────────────────────────────────────────────────────

std::string CardImageDownloader::sanitizeName(const std::string& name) {
    std::string out;
    out.reserve(name.size());
    for (char c : name) {
        if (c == '/' || c == '\\' || c == ':' || c == '*' ||
            c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
            out += '_';
        else
            out += c;
    }
    return out;
}

std::string CardImageDownloader::initDir() {
    std::lock_guard lk(g_mutex);
    if (!g_dir.empty()) return g_dir;
    const char* appdata = std::getenv("APPDATA");
    if (!appdata) return {};
    fs::path p = fs::path(appdata) / "CitadelMTG" / "pics" / "cards";
    std::error_code ec;
    fs::create_directories(p, ec);
    if (!ec) g_dir = p.string();
    return g_dir;
}

std::string CardImageDownloader::dir() {
    std::lock_guard lk(g_mutex);
    return g_dir;
}

int CardImageDownloader::loadOracle(const std::string& jsonPath) {
    std::ifstream ifs(jsonPath);
    if (!ifs) return -1;

    std::unordered_map<std::string, std::string> newOracle;
    newOracle.reserve(30000);

    std::string line;
    while (std::getline(ifs, line)) {
        if (line.empty() || line[0] == '[' || line[0] == ']') continue;

        std::string name = jsonStr(line, "name");
        if (name.empty()) continue;

        std::string url = extractNormalUrl(line);
        if (url.empty()) continue;

        // Index by sanitized full name
        newOracle[sanitizeName(name)] = url;

        // For transform/modal-DFC cards "A // B", also index by the front face name
        auto slashPos = name.find(" // ");
        if (slashPos != std::string::npos) {
            std::string frontName = name.substr(0, slashPos);
            newOracle.emplace(sanitizeName(frontName), url);
        }

        // Also index by the first card_face name (transform cards)
        size_t cfPos = line.find("\"card_faces\":[{");
        if (cfPos != std::string::npos) {
            std::string faceName = jsonStr(line, "name", cfPos + 14);
            if (!faceName.empty() && faceName != name)
                newOracle.emplace(sanitizeName(faceName), url);
        }
    }

    int count = static_cast<int>(newOracle.size());
    {
        std::lock_guard lk(g_mutex);
        g_oracle = std::move(newOracle);
    }
    std::cout << "[Oracle] Indexed " << count << " card images.\n";
    return count;
}

int CardImageDownloader::autoLoadOracle() {
    if (const char* p = std::getenv("CITADEL_ORACLE")) {
        if (fs::exists(p)) return loadOracle(p);
    }
    // 1) %APPDATA%\CitadelMTG\oracle-cards.json (the canonical cache slot)
    if (const char* apd = std::getenv("APPDATA")) {
        fs::path p = fs::path(apd) / "CitadelMTG" / "oracle-cards.json";
        if (fs::exists(p)) return loadOracle(p.string());
    }
    // 2) project root / cwd, for dev convenience
    for (const char* dir : {R"(Z:\forgeraw\mtg-project)", "."}) {
        std::error_code ec;
        for (auto& e : fs::directory_iterator(dir, ec)) {
            auto fn = e.path().filename().string();
            if (fn.find("oracle-cards") != std::string::npos &&
                fn.size() > 5 && fn.substr(fn.size() - 5) == ".json")
                return loadOracle(e.path().string());
        }
    }
    return -1;
}

// ── Bulk-data fetch ──────────────────────────────────────────────────────────
// Download Scryfall's oracle-cards JSON dump (~100 MB) to the per-user cache
// slot. Uses the bulk-data manifest at api.scryfall.com to find the current
// download URI (the filename includes a date stamp, so it changes weekly).

namespace {

void setBulkStatus(int pct, std::string text) {
    std::lock_guard lk(g_bulkMu);
    g_bulkPct    = pct;
    g_bulkStatus = std::move(text);
}

// Download host+path to localPath while pumping percent into g_bulkPct.
// Returns true on success and (size > 1 MB).
bool downloadLargeFile(const std::wstring& host, const std::wstring& path,
                       const fs::path& localPath) {
    HINTERNET hSess = WinHttpOpen(L"CitadelMTG/1.0",
                                   WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                   WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSess) return false;
    HINTERNET hConn = WinHttpConnect(hSess, host.c_str(),
                                      INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET hReq  = hConn
        ? WinHttpOpenRequest(hConn, L"GET", path.c_str(),
                             nullptr, WINHTTP_NO_REFERER,
                             WINHTTP_DEFAULT_ACCEPT_TYPES,
                             WINHTTP_FLAG_SECURE)
        : nullptr;

    auto closeAll = [&] {
        if (hReq)  WinHttpCloseHandle(hReq);
        if (hConn) WinHttpCloseHandle(hConn);
        if (hSess) WinHttpCloseHandle(hSess);
    };

    if (!hReq) { closeAll(); return false; }

    // 60-second timeouts — bulk file is 100 MB on a slow CDN.
    DWORD t = 60000;
    WinHttpSetOption(hReq, WINHTTP_OPTION_RECEIVE_TIMEOUT, &t, sizeof(t));
    WinHttpSetOption(hReq, WINHTTP_OPTION_CONNECT_TIMEOUT, &t, sizeof(t));
    WinHttpSetOption(hReq, WINHTTP_OPTION_SEND_TIMEOUT,    &t, sizeof(t));

    if (!WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(hReq, nullptr)) {
        closeAll(); return false;
    }

    DWORD status = 0, statusSz = sizeof(status);
    WinHttpQueryHeaders(hReq,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSz, WINHTTP_NO_HEADER_INDEX);
    if (status != 200) { closeAll(); return false; }

    long long contentLen = 0;
    DWORD clVal = 0, clSz = sizeof(clVal);
    if (WinHttpQueryHeaders(hReq,
            WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &clVal, &clSz, WINHTTP_NO_HEADER_INDEX))
        contentLen = clVal;

    fs::path tmpPath = localPath.string() + ".tmp";
    bool ok = false;
    long long total = 0;
    {
        std::ofstream ofs(tmpPath, std::ios::binary);
        if (ofs) {
            DWORD avail = 0;
            while (WinHttpQueryDataAvailable(hReq, &avail) && avail > 0) {
                std::vector<char> buf(avail);
                DWORD bytesRead = 0;
                if (!WinHttpReadData(hReq, buf.data(), avail, &bytesRead) ||
                    bytesRead == 0) break;
                ofs.write(buf.data(), static_cast<std::streamsize>(bytesRead));
                total += bytesRead;
                if (contentLen > 0) {
                    int pct = static_cast<int>(std::min<long long>(
                        total * 100 / contentLen, 100));
                    setBulkStatus(pct,
                        "Downloading Scryfall catalog (" +
                        std::to_string(total / (1024 * 1024)) + " / " +
                        std::to_string(contentLen / (1024 * 1024)) + " MB)…");
                }
            }
            ok = (total > 1024 * 1024);   // ≥ 1 MB sanity check
        }
    }
    closeAll();

    if (ok) {
        std::error_code ec;
        fs::remove(localPath, ec);
        fs::rename(tmpPath, localPath, ec);
        if (ec) { fs::remove(tmpPath, ec); return false; }
    } else {
        std::error_code ec; fs::remove(tmpPath, ec);
    }
    return ok;
}

} // namespace

int CardImageDownloader::ensureOracleLoaded() {
    {
        std::lock_guard lk(g_mutex);
        if (!g_oracle.empty()) return static_cast<int>(g_oracle.size());
    }
    // Try local files first.
    int n = autoLoadOracle();
    if (n > 0) return n;

    // Need to fetch. Resolve cache path.
    const char* apd = std::getenv("APPDATA");
    if (!apd) {
        setBulkStatus(-1, "APPDATA not set; can't cache oracle catalog");
        return -1;
    }
    fs::path cache = fs::path(apd) / "CitadelMTG" / "oracle-cards.json";
    std::error_code ec;
    fs::create_directories(cache.parent_path(), ec);

    // 1) Fetch the bulk-data manifest.
    setBulkStatus(0, "Fetching Scryfall bulk-data manifest…");
    std::string manifest = httpGet(L"api.scryfall.com", L"/bulk-data");
    if (manifest.empty()) {
        setBulkStatus(-1, "Could not reach api.scryfall.com");
        return -1;
    }

    // 2) Locate the oracle_cards entry, then read download_uri AFTER it.
    auto p = manifest.find("\"type\":\"oracle_cards\"");
    if (p == std::string::npos) {
        setBulkStatus(-1, "oracle_cards not found in manifest");
        return -1;
    }
    std::string url = jsonStr(manifest, "download_uri", p);
    if (url.empty()) {
        setBulkStatus(-1, "download_uri missing in manifest");
        return -1;
    }

    // 3) Split url into host + path.
    const std::string prefix = "https://";
    if (url.size() <= prefix.size()) { setBulkStatus(-1, "bad URL"); return -1; }
    std::string rest = url.substr(prefix.size());
    auto slash = rest.find('/');
    if (slash == std::string::npos) { setBulkStatus(-1, "bad URL"); return -1; }
    std::wstring host = toWide(rest.substr(0, slash));
    std::wstring path = toWide(rest.substr(slash));

    // 4) Download.
    setBulkStatus(0, "Downloading Scryfall catalog…");
    if (!downloadLargeFile(host, path, cache)) {
        setBulkStatus(-1, "Download failed");
        return -1;
    }

    // 5) Parse + index.
    setBulkStatus(100, "Loading catalog into memory…");
    int loaded = loadOracle(cache.string());
    if (loaded > 0)
        setBulkStatus(-1, "Catalog ready (" + std::to_string(loaded) + " cards)");
    else
        setBulkStatus(-1, "Catalog file is malformed");
    return loaded;
}

int CardImageDownloader::fetchOracleProgress() {
    std::lock_guard lk(g_bulkMu);
    return g_bulkPct;
}

std::string CardImageDownloader::fetchOracleStatus() {
    std::lock_guard lk(g_bulkMu);
    return g_bulkStatus;
}

bool CardImageDownloader::isOnDisk(const std::string& cardName) {
    if (g_dir.empty()) return false;
    std::error_code ec;
    return fs::exists(fs::path(g_dir) / (sanitizeName(cardName) + ".full.jpg"), ec);
}

std::string CardImageDownloader::imagePath(const std::string& cardName) {
    if (g_dir.empty()) return {};
    fs::path p = fs::path(g_dir) / (sanitizeName(cardName) + ".full.jpg");
    std::error_code ec;
    if (fs::exists(p, ec)) return p.string();
    return {};
}

void CardImageDownloader::start(const std::vector<std::string>& names) {
    {
        std::lock_guard lk(g_mutex);

        // Build fresh batch
        g_entries.clear();
        g_nameIdx.clear();
        g_queue.clear();

        g_entries.reserve(names.size());
        for (auto& n : names) {
            if (n.empty()) continue;
            if (g_nameIdx.count(n)) continue;
            size_t idx = g_entries.size();
            g_entries.push_back({n, DownloadEntry::State::Queued, 0});
            g_nameIdx[n] = idx;
            g_queue.push_back(idx);
        }

        if (!g_running) {
            g_running = true;
            int workers = std::min(kWorkerCount, static_cast<int>(g_queue.size()));
            g_workers.clear();
            for (int i = 0; i < workers; ++i)
                g_workers.emplace_back(workerFunc);
        }
    }
    g_cv.notify_all();
}

void CardImageDownloader::stop() {
    {
        std::lock_guard lk(g_mutex);
        g_running = false;
    }
    // Cancel all in-flight WinHTTP sessions. Closing a session handle immediately
    // aborts any blocking call (WinHttpReadData, WinHttpReceiveResponse, etc.) on
    // that session and all handles derived from it, causing those calls to return
    // with an error. Workers then exit cleanly without waiting for timeouts.
    {
        std::lock_guard lk(g_sessionsMu);
        for (auto& [tid, hSess] : g_activeSessions)
            if (hSess) WinHttpCloseHandle(hSess);
        g_activeSessions.clear();
    }
    g_cv.notify_all();
    for (auto& t : g_workers) if (t.joinable()) t.join();
    g_workers.clear();
}

bool CardImageDownloader::isActive() {
    std::lock_guard lk(g_mutex);
    if (!g_running) return false;
    if (!g_queue.empty()) return true;
    for (auto& e : g_entries)
        if (e.state == DownloadEntry::State::Downloading) return true;
    return false;
}

std::vector<DownloadEntry> CardImageDownloader::snapshot() {
    std::lock_guard lk(g_mutex);
    return g_entries;
}

int CardImageDownloader::doneCount() {
    std::lock_guard lk(g_mutex);
    return static_cast<int>(std::count_if(g_entries.begin(), g_entries.end(),
        [](const DownloadEntry& e) { return e.state == DownloadEntry::State::Done; }));
}

int CardImageDownloader::totalCount() {
    std::lock_guard lk(g_mutex);
    return static_cast<int>(g_entries.size());
}

// ── Rulings / errata fetcher ──────────────────────────────────────────────────

std::string CardImageDownloader::httpGet(const std::wstring& host, const std::wstring& path) {
#ifdef _WIN32
    HINTERNET hSess = WinHttpOpen(L"CitadelMTG/1.0",
                                   WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                   WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSess) return {};
    HINTERNET hConn = WinHttpConnect(hSess, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET hReq  = hConn ? WinHttpOpenRequest(hConn, L"GET", path.c_str(),
                                                   nullptr, WINHTTP_NO_REFERER,
                                                   WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                   WINHTTP_FLAG_SECURE) : nullptr;
    // Scryfall requires both User-Agent (set via WinHttpOpen) and Accept header.
    static const wchar_t* kHeaders = L"Accept: application/json\r\n";
    std::string body;
    if (hReq && WinHttpSendRequest(hReq, kHeaders, static_cast<DWORD>(-1L),
                                    WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
             && WinHttpReceiveResponse(hReq, nullptr)) {
        DWORD toRead = 0;
        while (WinHttpQueryDataAvailable(hReq, &toRead) && toRead > 0) {
            std::string chunk(toRead, '\0');
            DWORD read = 0;
            WinHttpReadData(hReq, chunk.data(), toRead, &read);
            body.append(chunk, 0, read);
        }
    }
    if (hReq)  WinHttpCloseHandle(hReq);
    if (hConn) WinHttpCloseHandle(hConn);
    if (hSess) WinHttpCloseHandle(hSess);
    return body;
#else
    return {};
#endif
}

void CardImageDownloader::fetchToken(const std::string& name,
                                     const std::string& extraQuery) {
#ifdef _WIN32
    if (g_dir.empty() || name.empty()) return;
    // Already downloaded?
    {
        std::error_code ec;
        if (fs::exists(fs::path(g_dir) / (sanitizeName(name) + ".full.jpg"), ec)) return;
    }
    // Attempt each token name at most once per session (called every frame).
    {
        static std::mutex s_mu;
        static std::unordered_map<std::string, char> s_attempted;
        std::lock_guard<std::mutex> lk(s_mu);
        if (!s_attempted.emplace(name, 1).second) return;
    }

    std::thread([name, extraQuery]() {
        auto enc = [](const std::string& s) {
            std::wstring e;
            for (char ch : s) {
                if (std::isalnum(static_cast<unsigned char>(ch))) e += static_cast<wchar_t>(ch);
                else if (ch == ' ') e += L'+';
                else { wchar_t b[8]; swprintf(b, 8, L"%%%02X", static_cast<unsigned char>(ch)); e += b; }
            }
            return e;
        };
        // Scryfall token search: is:token plus the exact token name, optionally
        // narrowed by power/toughness/colour so we pick the right variant.
        auto search = [&](const std::string& extra) -> std::string {
            std::string q = "is:token !\"" + name + "\"";
            if (!extra.empty()) q += " " + extra;
            std::wstring path = L"/cards/search?q=" + enc(q) + L"&unique=cards";
            return extractNormalUrl(httpGet(L"api.scryfall.com", path));
        };
        std::string url = search(extraQuery);
        if (url.empty() && !extraQuery.empty()) url = search("");   // fall back to name only

        if (url.empty()) return;
        const std::string prefix = "https://";
        if (url.size() <= prefix.size()) return;
        std::string rest = url.substr(prefix.size());
        auto sp = rest.find('/');
        if (sp == std::string::npos) return;
        std::wstring host(rest.begin(), rest.begin() + static_cast<long>(sp));
        std::wstring path(rest.begin() + static_cast<long>(sp), rest.end());

        std::string bytes = httpGet(host, path);
        if (bytes.size() < 1024) return;   // too small to be a real JPEG

        fs::path out = fs::path(g_dir) / (sanitizeName(name) + ".full.jpg");
        fs::path tmp = out.string() + ".tmp";
        { std::ofstream ofs(tmp, std::ios::binary);
          if (!ofs) return;
          ofs.write(bytes.data(), static_cast<std::streamsize>(bytes.size())); }
        std::error_code ec;
        fs::rename(tmp, out, ec);
        if (ec) fs::remove(tmp, ec);
    }).detach();
#else
    (void)name; (void)extraQuery;
#endif
}

std::vector<std::string> CardImageDownloader::fetchRulings(const std::string& cardName) {
    // URL-encode the card name for Scryfall fuzzy search
    std::wstring encoded;
    for (char c : cardName) {
        if (std::isalnum((unsigned char)c)) encoded += (wchar_t)c;
        else if (c == ' ') encoded += L'+';
        else { wchar_t buf[8]; swprintf(buf, 8, L"%%%02X", (unsigned char)c); encoded += buf; }
    }

    // Fetch rulings from Scryfall named endpoint
    std::wstring rulingsPath = L"/cards/named?fuzzy=" + encoded + L"&format=json";
    std::string cardJson = httpGet(L"api.scryfall.com", rulingsPath);

    std::vector<std::string> result;
    if (cardJson.empty()) { result.push_back("(Could not reach Scryfall — check internet)"); return result; }

    // Simple JSON text extraction — find "oracle_text" value
    auto findField = [&](const std::string& key) -> std::string {
        std::string search = "\"" + key + "\":\"";
        auto pos = cardJson.find(search);
        if (pos == std::string::npos) return {};
        pos += search.size();
        std::string val;
        for (size_t i = pos; i < cardJson.size(); ++i) {
            if (cardJson[i] == '"' && (i == 0 || cardJson[i-1] != '\\')) break;
            if (cardJson[i] == '\\' && i + 1 < cardJson.size()) {
                if (cardJson[i+1] == 'n') { val += '\n'; ++i; }
                else if (cardJson[i+1] == '"') { val += '"'; ++i; }
                else val += cardJson[i];
            } else val += cardJson[i];
        }
        return val;
    };

    std::string oracle = findField("oracle_text");
    if (!oracle.empty()) result.push_back("Oracle: " + oracle);

    std::string legalities = findField("legalities");
    // Extract commander legality
    auto cmdPos = cardJson.find("\"commander\":\"");
    if (cmdPos != std::string::npos) {
        cmdPos += 13;
        std::string leg;
        for (size_t i = cmdPos; i < cardJson.size() && cardJson[i] != '"'; ++i) leg += cardJson[i];
        result.push_back("Commander: " + leg);
    }

    // Fetch rulings separately
    auto scryfallId = findField("id");
    if (!scryfallId.empty()) {
        std::wstring sid(scryfallId.begin(), scryfallId.end());
        std::string rulingsJson = httpGet(L"api.scryfall.com", L"/cards/" + sid + L"/rulings");
        // Extract ruling "comment" fields
        size_t p = 0;
        while ((p = rulingsJson.find("\"comment\":\"", p)) != std::string::npos) {
            p += 11;
            std::string comment;
            for (size_t i = p; i < rulingsJson.size(); ++i) {
                if (rulingsJson[i] == '"' && (i == 0 || rulingsJson[i-1] != '\\')) break;
                comment += rulingsJson[i];
            }
            if (!comment.empty()) result.push_back("• " + comment);
        }
    }

    if (result.empty()) result.push_back("(No rulings found)");
    return result;
}

} // namespace ui
