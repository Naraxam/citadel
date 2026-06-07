// std::getenv() below trips MSVC's C4996 deprecation, which is an error in this
// target. The rest of the codebase uses std::getenv freely; silence it here too.
#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "CardDb.h"
#include "CardScriptParser.h"
#include <miniz.h>
#include <iostream>
#include <cstdlib>

namespace mtg {

void CardDb::loadFromDirectory(const std::filesystem::path& dir) {
    namespace fs = std::filesystem;
    std::error_code ec;
    int files = 0, loaded = 0, failed = 0, dfcs = 0;
    for (const auto& entry : fs::recursive_directory_iterator(dir, ec)) {
        if (ec) { ec.clear(); continue; }
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".txt") continue;
        ++files;

        auto faces = CardScriptParser::parseBothFaces(entry.path());
        if (faces.empty()) { ++failed; continue; }
        if (faces.size() > 1) ++dfcs;
        for (auto& rules : faces) {
            std::string name = rules.name;
            m_cards.insert_or_assign(std::move(name), std::move(rules));
        }
        ++loaded;
    }
    std::cout << "CardDb: loaded " << loaded << "/" << files
              << " cards from directory (" << dfcs << " DFCs, "
              << failed << " parse errors).\n";
}

void CardDb::loadFromZip(const std::filesystem::path& zipPath) {
    // NOTE on startup speed: parsing ~32k scripts here is ~7s in a Debug build.
    // Two "obvious" speedups were tried and rejected: (1) multi-threaded parsing
    // REGRESSED the Debug build ~4x because the locked debug-CRT heap serialises
    // the heavy per-card allocations under contention; (2) a serialised binary
    // CardRules cache is too fragile (pointer members + format/version drift) to
    // risk on a working engine. The GUI already hides this cost behind an async
    // loading screen, so the app stays responsive. Kept simple + single-threaded.
    mz_zip_archive zip{};
    std::string pathStr = zipPath.string();

    if (!mz_zip_reader_init_file(&zip, pathStr.c_str(), 0)) {
        std::cerr << "CardDb: failed to open ZIP: " << zipPath << '\n';
        return;
    }

    mz_uint numFiles = mz_zip_reader_get_num_files(&zip);
    int loaded = 0;

    for (mz_uint i = 0; i < numFiles; ++i) {
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&zip, i, &stat)) continue;

        // Only process .txt files
        std::string filename(stat.m_filename);
        if (filename.size() < 4 ||
            filename.compare(filename.size() - 4, 4, ".txt") != 0) continue;

        // Extract file content into memory
        size_t size = 0;
        void* data  = mz_zip_reader_extract_to_heap(&zip, i, &size, 0);
        if (!data) continue;

        std::string_view text(static_cast<const char*>(data), size);
        auto faces = CardScriptParser::parseBothFacesText(text);
        mz_free(data);

        for (auto& rules : faces) {
            std::string name = rules.name;
            m_cards.insert_or_assign(std::move(name), std::move(rules));
        }
        if (!faces.empty()) ++loaded;
    }

    mz_zip_reader_end(&zip);
    int totalCards = static_cast<int>(m_cards.size());
    std::cout << "CardDb: loaded " << loaded << " scripts from ZIP, "
              << totalCards << " total cards in database.\n";
}

std::filesystem::path CardDb::customCardsDir() {
    namespace fs = std::filesystem;
    if (const char* custom = std::getenv("CITADEL_CUSTOM_CARDS"); custom && *custom)
        return fs::path(custom);
    if (const char* appdata = std::getenv("APPDATA"); appdata && *appdata)
        return fs::path(appdata) / "CitadelMTG" / "customcards";
    return {};
}

void CardDb::loadCustomCards() {
    namespace fs = std::filesystem;
    fs::path dir = customCardsDir();
    std::error_code ec;
    if (dir.empty() || !fs::exists(dir, ec) || !fs::is_directory(dir, ec))
        return;
    size_t before = m_cards.size();
    std::cout << "[CardDb] Loading custom-card overlay from " << dir << '\n';
    loadFromDirectory(dir);  // insert_or_assign → overlay overrides base cards
    std::cout << "[CardDb] Custom overlay: net +" << (m_cards.size() - before)
              << " new card name(s) (existing names were overridden in place).\n";
}

const CardRules* CardDb::find(std::string_view name) const noexcept {
    auto it = m_cards.find(std::string(name));
    return it != m_cards.end() ? &it->second : nullptr;
}

void CardDb::wireBackFaces() {
    int wired = 0, broken = 0;
    for (auto& [name, rules] : m_cards) {
        if (rules.altName.empty()) continue;
        auto it = m_cards.find(rules.altName);
        if (it != m_cards.end()) {
            rules.backFace = &it->second;
            ++wired;
        } else {
            // Back-face card referenced by altName was not found in the DB
            std::cerr << "[CardDb] WARNING: DFC back-face not found: \""
                      << rules.altName << "\" (front: \"" << name << "\")\n";
            ++broken;
        }
    }
    if (wired > 0)
        std::cout << "[CardDb] Wired " << wired << " DFC back-face pair(s)";
    if (broken > 0)
        std::cout << " (" << broken << " unresolved altName reference(s))";
    if (wired > 0 || broken > 0)
        std::cout << ".\n";
}

bool CardDb::reloadCard(const std::filesystem::path& cardsDir, std::string_view cardName) {
    namespace fs = std::filesystem;
    // Build expected filename: replace spaces with underscores, append .txt
    std::string fname(cardName);
    for (char& c : fname) if (c == ' ') c = '_';
    fname += ".txt";

    // Search for the file recursively
    fs::path found;
    std::error_code ec;
    for (auto& entry : fs::recursive_directory_iterator(cardsDir, ec)) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().filename().string() == fname) { found = entry.path(); break; }
    }
    if (found.empty()) return false;

    auto parsed = CardScriptParser::parseFile(found);
    if (!parsed) return false;

    std::string key(cardName);
    m_cards[key] = std::move(*parsed);
    wireBackFaces();  // re-wire back faces after update
    return true;
}

} // namespace mtg
