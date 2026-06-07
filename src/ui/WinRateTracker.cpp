#include "WinRateTracker.h"
#include <nlohmann/json.hpp>
#include <fstream>

namespace ui {

WinRateTracker::WinRateTracker(std::filesystem::path savePath)
    : m_path(std::move(savePath))
{
    load();
}

void WinRateTracker::record(const std::string& name, GameResult r) {
    std::lock_guard lk(m_mu);
    auto& s = m_data[name];
    if      (r == GameResult::Win)  ++s.wins;
    else if (r == GameResult::Loss) ++s.losses;
    else                            ++s.draws;
    s.recent.push_back(static_cast<uint8_t>(r));
    if (static_cast<int>(s.recent.size()) > kRecentMax)
        s.recent.pop_front();
}

float WinRateTracker::winRate(const std::string& name, int lastN) const {
    std::lock_guard lk(m_mu);
    auto it = m_data.find(name);
    if (it == m_data.end()) return -1.f;
    const Stats& s = it->second;

    if (lastN == 0) {
        int total = s.wins + s.losses + s.draws;
        return total ? static_cast<float>(s.wins) / total : -1.f;
    }

    int n = std::min(lastN, static_cast<int>(s.recent.size()));
    if (n == 0) return -1.f;

    int offset = static_cast<int>(s.recent.size()) - n;
    int wins = 0;
    for (int i = offset; i < static_cast<int>(s.recent.size()); ++i)
        if (static_cast<GameResult>(s.recent[i]) == GameResult::Win) ++wins;
    return static_cast<float>(wins) / n;
}

void WinRateTracker::save() const {
    std::lock_guard lk(m_mu);
    nlohmann::json j;
    for (const auto& [name, s] : m_data) {
        auto& e     = j[name];
        e["wins"]   = s.wins;
        e["losses"] = s.losses;
        e["draws"]  = s.draws;
        e["recent"] = nlohmann::json::array();
        for (uint8_t v : s.recent)
            e["recent"].push_back(v);
    }
    std::ofstream f(m_path);
    if (f) f << j.dump(2);
}

std::unordered_map<std::string, WinRateTracker::RawStats>
WinRateTracker::snapshot() const {
    std::lock_guard lk(m_mu);
    std::unordered_map<std::string, RawStats> out;
    out.reserve(m_data.size());
    for (const auto& [k, s] : m_data)
        out[k] = { s.wins, s.losses, s.draws };
    return out;
}

void WinRateTracker::clear() {
    std::lock_guard lk(m_mu);
    m_data.clear();
    std::ofstream f(m_path, std::ios::trunc);
    if (f) f << "{}";
}

void WinRateTracker::load() {
    std::ifstream f(m_path);
    if (!f) return;
    try {
        nlohmann::json j;
        f >> j;
        for (auto& [name, e] : j.items()) {
            Stats s;
            s.wins   = e.value("wins",   0);
            s.losses = e.value("losses", 0);
            s.draws  = e.value("draws",  0);
            if (e.contains("recent"))
                for (auto v : e["recent"])
                    s.recent.push_back(v.template get<uint8_t>());
            while (static_cast<int>(s.recent.size()) > kRecentMax)
                s.recent.pop_front();
            m_data.emplace(name, std::move(s));
        }
    } catch (...) {}
}

} // namespace ui
