#pragma once
// ── Soft assertion helpers ────────────────────────────────────────────────────
// GAME_ASSERT(condition, message): in Debug builds, logs and can break.
// In Release builds the condition is still evaluated (may emit no code with -O2).
// Usage:
//   GAME_ASSERT(card != nullptr, "castSpell called with null card");
//   GAME_ASSERT(life >= 0, "negative life total: " + std::to_string(life));

#include <string>
#include <iostream>

namespace mtg {

inline void softAssertFail(const char* file, int line,
                            const char* expr, const std::string& msg) {
    std::cerr << "[ASSERT FAIL] " << file << ":" << line
              << "  (" << expr << ")";
    if (!msg.empty()) std::cerr << "  — " << msg;
    std::cerr << '\n';
#ifdef _WIN32
    // Write to game_log.txt so crash is recoverable
    if (const char* ap = std::getenv("APPDATA")) {
        std::string logPath = std::string(ap) + "\\CitadelMTG\\game_log.txt";
        if (FILE* f = std::fopen(logPath.c_str(), "a")) {
            std::fprintf(f, "[ASSERT FAIL] %s:%d (%s) — %s\n",
                         file, line, expr, msg.c_str());
            std::fclose(f);
        }
    }
#endif
#ifndef NDEBUG
    // Break in debug builds so devs can inspect the call stack
    std::abort();
#endif
}

} // namespace mtg

// Macros — always evaluate the condition (no undefined behaviour from empty macro)
#define GAME_ASSERT(cond, ...) \
    do { if (!(cond)) { \
        ::mtg::softAssertFail(__FILE__, __LINE__, #cond, \
            [&]{ std::string _m; auto _args = {__VA_ARGS__}; \
                 for (auto& a : _args) _m += a; return _m; }()); \
    } } while(0)

#define GAME_ASSERT_MSG(cond, msg) \
    do { if (!(cond)) ::mtg::softAssertFail(__FILE__, __LINE__, #cond, (msg)); } while(0)
