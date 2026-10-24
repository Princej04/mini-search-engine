#include "corpus.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;

namespace ms {

namespace {

bool readFile(const fs::path& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

// Cheap "has this file changed?" fingerprint: size mixed with modification time.
uint64_t stampOf(uintmax_t size, fs::file_time_type mtime) {
    const uint64_t t = static_cast<uint64_t>(mtime.time_since_epoch().count());
    return (static_cast<uint64_t>(size) * 0x9E3779B97F4A7C15ull) ^ t;
}

}  // namespace

SyncStats syncFolder(InvertedIndex& index, const fs::path& root) {
    std::error_code ec;
    std::vector<fs::path> files;
    fs::recursive_directory_iterator it(root, ec), end;
    if (ec) throw std::runtime_error("Cannot read folder: " + root.string());
    for (; it != end; it.increment(ec)) {
        if (ec) throw std::runtime_error("Error while reading folder: " + root.string());
        std::error_code ec2;
        if (it->is_regular_file(ec2) && it->path().extension() == ".txt") {
            files.push_back(it->path());
        }
    }
    // Sorted so document ids come out in the same order on every run.
    std::sort(files.begin(), files.end());

    SyncStats stats;
    std::unordered_set<std::string> seen;

    for (const fs::path& path : files) {
        const std::string name = path.lexically_relative(root).generic_string();
        // Mark as seen before anything can fail, so an unreadable file keeps its
        // old version in the index instead of being treated as deleted.
        seen.insert(name);

        std::error_code e1, e2;
        const uintmax_t size = fs::file_size(path, e1);
        const fs::file_time_type mtime = fs::last_write_time(path, e2);
        if (e1 || e2) continue;
        const uint64_t stamp = stampOf(size, mtime);

        const std::optional<uint32_t> existing = index.findDoc(name);
        if (existing && index.docStamp(*existing) == stamp) {
            ++stats.unchanged;
            continue;
        }

        std::string text;
        if (!readFile(path, text)) continue;
        index.addDocument(name, text, stamp);  // replaces the old version if there is one
        if (existing) ++stats.updated;
        else ++stats.added;
    }

    std::vector<std::string> gone;
    for (uint32_t id = 0; id < index.numSlots(); ++id) {
        if (index.isAlive(id) && seen.count(index.docName(id)) == 0) {
            gone.push_back(index.docName(id));
        }
    }
    for (const std::string& name : gone) {
        if (index.removeDocument(name)) ++stats.removed;
    }
    return stats;
}

}  // namespace ms
