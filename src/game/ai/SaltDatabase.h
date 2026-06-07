#pragma once
#include <string>
#include <unordered_map>

namespace mtg {

// Stores per-card "salt" scores sourced from EDHREC.
// Salt measures how much opponents dislike playing against a card (0.0–2.5+).
// Higher salt → higher perceived threat → AI removes it first.
//
// Usage:
//   SaltDatabase::load("data/salt_scores.json");   // load from cache
//   SaltDatabase::fetchAndCache("data/salt_scores.json");  // fetch EDHREC + write cache
//   float s = SaltDatabase::get("Cyclonic Rift");  // 0.0 if unknown
class SaltDatabase {
public:
    // Load salt scores from a local JSON cache file.
    // Returns the number of entries loaded (0 if file missing or malformed).
    static int load(const std::string& cachePath);

    // Fetch salt scores from EDHREC, merge with the static fallback, and write
    // the result to cachePath.  Returns the number of entries stored.
    // On Windows uses WinHTTP; on other platforms only the static fallback is used.
    static int fetchAndCache(const std::string& cachePath);

    // Returns the salt score for cardName, or 0.0f if not in the database.
    static float get(const std::string& cardName);

    static bool empty() noexcept { return s_scores.empty(); }
    static int  size()  noexcept { return static_cast<int>(s_scores.size()); }

private:
    static std::unordered_map<std::string, float> s_scores;

    // Load the bundled static list of well-known high-salt cards.
    static void loadStaticFallback();

    // Save the current score map to cachePath as JSON.
    static void saveCache(const std::string& cachePath);
};

} // namespace mtg
