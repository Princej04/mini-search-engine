#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "index.h"

namespace fs = std::filesystem;

static std::string readFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <folder-with-txt-files>\n";
        return 1;
    }
    const fs::path root = argv[1];
    if (!fs::is_directory(root)) {
        std::cerr << "Not a directory: " << root << "\n";
        return 1;
    }

    ms::InvertedIndex index;
    const auto t0 = std::chrono::steady_clock::now();
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        if (entry.is_regular_file() && entry.path().extension() == ".txt") {
            index.addDocument(entry.path().string(), readFile(entry.path()));
        }
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    std::cout << "Indexed " << index.numDocs() << " documents, " << index.numTerms()
              << " unique terms in " << ms << " ms\n";

    std::string line;
    while (std::cout << "\nsearch> " && std::getline(std::cin, line)) {
        if (line == "quit" || line == "exit") break;
        if (line.empty()) continue;

        const auto q0 = std::chrono::steady_clock::now();
        const auto results = index.search(line, 10);
        const auto q1 = std::chrono::steady_clock::now();
        const double qus = std::chrono::duration<double, std::micro>(q1 - q0).count();

        if (results.empty()) std::cout << "No results.\n";
        for (const auto& r : results) {
            std::cout << "  " << r.score << "  " << index.docName(r.docId) << "\n";
        }
        std::cout << "(" << qus << " us)\n";
    }
    return 0;
}
