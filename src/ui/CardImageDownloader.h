#pragma once
#include <string>
#include <vector>

namespace ui {

struct DownloadEntry {
    std::string name;
    enum class State { Queued, Downloading, Done, Failed };
    State state   = State::Queued;
    int   percent = 0;  // 0-100 while Downloading; 100 when Done
};

// Downloads card images from Scryfall CDN using the oracle-cards bulk JSON.
// All public methods are thread-safe.
class CardImageDownloader {
public:
    // Create %APPDATA%\CitadelMTG\pics\cards if needed and return its path.
    static std::string initDir();

    // Return the download directory (empty until initDir() is called).
    static std::string dir();

    // Parse the Scryfall oracle-cards bulk JSON and build name→URL map.
    // Returns the number of cards indexed, or -1 on failure.
    static int loadOracle(const std::string& jsonPath);

    // Find the oracle JSON in well-known locations; load it. Returns card count.
    static int autoLoadOracle();

    // Ensure the oracle catalog is loaded; if not present anywhere on disk,
    // fetch Scryfall's bulk-data manifest, download the oracle-cards JSON
    // (~100 MB) into %APPDATA%/CitadelMTG/oracle-cards.json, and load it.
    // Blocking call — typically run on a worker thread. Returns the card
    // count on success, -1 on failure. fetchOracleProgress() reports 0..100
    // while running (and fetchOracleStatus() carries an error message on
    // failure).
    static int ensureOracleLoaded();
    static int  fetchOracleProgress();        // -1 idle, 0..100 active
    static std::string fetchOracleStatus();   // last status / error message

    // True if cardName has a downloaded image on disk.
    static bool isOnDisk(const std::string& cardName);

    // Full path to cardName's image, or "" if not present.
    static std::string imagePath(const std::string& cardName);

    // Make a card name safe as a Windows filename (replaces / : * etc.).
    static std::string sanitizeName(const std::string& name);

    // Fetch art for a TOKEN by name from Scryfall (the oracle bulk excludes
    // tokens, so the normal pipeline can't find them). Runs asynchronously and
    // de-duplicates, so it's safe to call every frame for a visible token.
    // `extraQuery` adds Scryfall search terms (e.g. "pow=2 tou=2") to pick the
    // right variant; it falls back to a name-only search if that finds nothing.
    static void fetchToken(const std::string& name,
                           const std::string& extraQuery = "");

    // Enqueue names for parallel download (4 workers). Clears previous batch.
    static void start(const std::vector<std::string>& names);

    // Stop all workers and join.
    static void stop();

    // True if workers are running or the queue is non-empty.
    static bool isActive();

    // Thread-safe copy of all entries in the current batch.
    static std::vector<DownloadEntry> snapshot();

    // Counts in the current batch.
    static int doneCount();
    static int totalCount();

    // Fetch rulings and oracle text for a card from Scryfall's API (blocking).
    // Returns a vector of ruling strings, or {"(offline)"} on failure.
    static std::vector<std::string> fetchRulings(const std::string& cardName);

    // Generic HTTP GET to a URL; returns the response body or empty string on failure.
    static std::string httpGet(const std::wstring& host, const std::wstring& path);
};

} // namespace ui
