#pragma once
#include <cstddef>
#include <filesystem>

#include "index.h"

namespace ms {

struct SyncStats {
    size_t added = 0;      // new files
    size_t updated = 0;    // files whose size or modification time changed
    size_t removed = 0;    // documents whose file no longer exists
    size_t unchanged = 0;  // files skipped because nothing changed
    bool changed() const { return added + updated + removed > 0; }
};

// Brings `index` in line with the .txt files under `root` (searched recursively):
//  - new files are indexed,
//  - files whose (size, modification time) stamp changed are re-indexed,
//  - documents whose file disappeared are removed,
//  - everything else is left alone and not even read.
// Documents are named by their path relative to `root`, with '/' separators,
// so an index keeps working if the folder is moved. Every document in the
// index is assumed to come from this folder.
// Throws std::runtime_error if the folder cannot be read.
SyncStats syncFolder(InvertedIndex& index, const std::filesystem::path& root);

}  // namespace ms
