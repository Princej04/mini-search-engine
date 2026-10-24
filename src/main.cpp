#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "corpus.h"
#include "index.h"
#include "query.h"

namespace fs = std::filesystem;

using Clock = std::chrono::steady_clock;

static double msSince(Clock::time_point t) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}

static void printHelp() {
    std::cout << "Queries:\n"
                 "  cat dog                  ranked search, any word may match\n"
                 "  \"quick brown\"            phrase (words next to each other)\n"
                 "  cat AND dog              both words\n"
                 "  cat OR dog               either word\n"
                 "  cat AND NOT dog          cat but not dog\n"
                 "  (cat OR dog) AND \"lazy\"  grouping (operators must be UPPERCASE)\n"
                 "Commands:\n"
                 "  :add <file.txt>          index one more file\n"
                 "  :remove <name>           remove a document (name as printed in results)\n"
                 "  :save [file]             save the index\n"
                 "  :stats                   show index statistics\n"
                 "  :bm25 / :tfidf           switch ranking\n"
                 "  :help    quit\n";
}

static void printStats(const ms::InvertedIndex& index) {
    std::cout << index.numDocs() << " documents, " << index.numTerms()
              << " unique terms, average length " << index.avgDocLength()
              << " words, " << index.numRemoved() << " removals waiting for compaction\n";
}

static bool saveIndex(const ms::InvertedIndex& index, const std::string& path) {
    try {
        const auto t = Clock::now();
        index.save(path);
        std::error_code ec;
        const auto bytes = fs::file_size(path, ec);
        std::cout << "Saved index to " << path;
        if (!ec) std::cout << " (" << bytes / 1024 << " KB)";
        std::cout << " in " << msSince(t) << " ms\n";
        return true;
    } catch (const std::exception& e) {
        std::cout << "Could not save: " << e.what() << "\n";
        return false;
    }
}

int main(int argc, char** argv) {
    fs::path folder;
    std::string indexPath;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--index" && i + 1 < argc) indexPath = argv[++i];
        else if (folder.empty()) folder = arg;
        else { folder.clear(); break; }  // unexpected extra argument -> show usage
    }
    if (folder.empty()) {
        std::cerr << "Usage: " << argv[0] << " <folder-with-txt-files> [--index <index-file>]\n"
                  << "  With --index, the index is loaded from the file if it exists, brought up to\n"
                  << "  date (only new, changed and deleted files are processed) and saved again.\n";
        return 1;
    }
    if (!fs::is_directory(folder)) {
        std::cerr << "Not a directory: " << folder << "\n";
        return 1;
    }

    ms::InvertedIndex index;
    bool loaded = false;
    if (!indexPath.empty() && fs::exists(indexPath)) {
        try {
            const auto t = Clock::now();
            index = ms::InvertedIndex::load(indexPath);
            loaded = true;
            std::cout << "Loaded " << index.numDocs() << " documents from " << indexPath
                      << " in " << msSince(t) << " ms\n";
        } catch (const std::exception& e) {
            std::cout << e.what() << "\nRebuilding the index from scratch.\n";
        }
    }

    ms::SyncStats stats;
    try {
        const auto t = Clock::now();
        stats = ms::syncFolder(index, folder);
        std::cout << "Synced " << folder.string() << ": " << stats.added << " added, "
                  << stats.updated << " updated, " << stats.removed << " removed, "
                  << stats.unchanged << " unchanged in " << msSince(t) << " ms\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
    printStats(index);

    bool dirty = false;
    if (!indexPath.empty() && (!loaded || stats.changed())) saveIndex(index, indexPath);
    printHelp();

    ms::RankMode mode = ms::RankMode::BM25;
    std::string line;
    while (std::cout << "\nsearch> " && std::getline(std::cin, line)) {
        if (line == "quit" || line == "exit") break;
        if (line.empty()) continue;

        if (line == ":help") { printHelp(); continue; }
        if (line == ":stats") { printStats(index); continue; }
        if (line == ":bm25") { mode = ms::RankMode::BM25; std::cout << "Ranking: BM25\n"; continue; }
        if (line == ":tfidf") { mode = ms::RankMode::TfIdf; std::cout << "Ranking: TF-IDF\n"; continue; }

        if (line.rfind(":save", 0) == 0) {
            std::string path = line.size() > 6 ? line.substr(6) : indexPath;
            if (path.empty()) std::cout << "Usage: :save <file>  (or start with --index <file>)\n";
            else if (saveIndex(index, path)) dirty = false;
            continue;
        }
        if (line.rfind(":add ", 0) == 0) {
            const fs::path file = line.substr(5);
            std::ifstream in(file, std::ios::binary);
            if (!in) { std::cout << "Cannot open " << file << "\n"; continue; }
            std::ostringstream ss;
            ss << in.rdbuf();
            const bool replaced = index.findDoc(file.generic_string()).has_value();
            index.addDocument(file.generic_string(), ss.str());
            dirty = true;
            std::cout << (replaced ? "Replaced " : "Added ") << file.generic_string() << "\n";
            continue;
        }
        if (line.rfind(":remove ", 0) == 0) {
            if (index.removeDocument(line.substr(8))) { dirty = true; std::cout << "Removed.\n"; }
            else std::cout << "No document with that name.\n";
            continue;
        }

        try {
            const auto q0 = Clock::now();
            const auto results = ms::runQuery(index, line, 10, mode);
            const double qus = msSince(q0) * 1000.0;

            if (results.empty()) std::cout << "No results.\n";
            for (const auto& r : results) {
                std::cout << "  " << r.score << "  " << index.docName(r.docId) << "\n";
            }
            std::cout << "(" << qus << " us)\n";
        } catch (const ms::QueryError& e) {
            std::cout << "Query error: " << e.what() << "\n";
        }
    }

    if (dirty && !indexPath.empty()) saveIndex(index, indexPath);
    return 0;
}
