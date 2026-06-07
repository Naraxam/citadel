#pragma once
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>

namespace ui {

enum class GameResult : uint8_t { Win, Draw, Loss };

class WinRateTracker {
public:
    static constexpr int kRecentMax = 1000;

    explicit WinRateTracker(std::filesystem::path savePath);

    void record(const std::string& name, GameResult r);

    float winRate(const std::string& name, int lastN) const;

    void save() const;

    // Erase all in-memory stats and overwrite the on-disk file with empty JSON.
    void clear();

    // Snapshot of raw {wins, losses, draws} counts for every tracked key.
    // Used to compute per-session deltas in training reports.
    using RawStats = std::array<int, 3>; // [0]=wins [1]=losses [2]=draws
    std::unordered_map<std::string, RawStats> snapshot() const;

private:
    struct Stats {
        int wins   = 0;
        int losses = 0;
        int draws  = 0;
        std::deque<uint8_t> recent;
    };

    std::filesystem::path m_path;
    mutable std::mutex    m_mu;
    std::unordered_map<std::string, Stats> m_data;

    void load();
};

} // namespace ui
