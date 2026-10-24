# Mini Search Engine (C++17)

A local document search engine built around an inverted index, with BM25 relevance ranking.

## How it works

1. **Tokenize**: each document is lowercased and split into words.
2. **Index**: an inverted index maps every word to the documents containing it, along with the position of each occurrence. A query only touches the posting lists of its own words instead of scanning every document.
3. **Rank**: matching documents are scored with BM25 and the top results are returned.

### BM25 in one paragraph

Each query word contributes a score to each document that contains it. The score combines three things: how rare the word is across the corpus (IDF), how often it appears in the document (with diminishing returns, so 50 mentions are not worth 50x one mention), and the document's length relative to the average (so long documents don't win just by containing more words). Scores for all query words are added up. TF-IDF is also available for comparison.

Tuning constants: `k1 = 1.2` (term-frequency saturation) and `b = 0.75` (length normalization).

## Query syntax

| Query | Meaning |
|---|---|
| `cat dog` | ranked search, any word may match (best matches first) |
| `"quick brown"` | phrase: the words must be next to each other, in this order |
| `cat AND dog` | documents containing both (writing `cat dog` inside a Boolean query also means AND) |
| `cat OR dog` | documents containing either |
| `cat AND NOT dog` | cat, but not dog |
| `(fox OR cat) AND "lazy dog"` | grouping |

Operators must be UPPERCASE. Precedence is NOT, then AND, then OR. For Boolean queries the matching documents are still ranked with BM25, using the words that are not negated. A malformed query such as `fox AND` prints an error instead of crashing.

### How phrase search works

Each posting stores the word offsets of every occurrence. To find `"quick brown"`: intersect the posting lists of `quick` and `brown` (both sorted by document id, so one linear walk), then in each shared document check that `brown` appears at the position right after some `quick`. For longer phrases, word `i` must appear at `start + i`.

### How Boolean queries work

Each word or phrase turns into a sorted list of document ids. `AND` is a sorted-list intersection, `OR` is a union, and `NOT` subtracts from the list of all documents. A small recursive-descent parser handles precedence and parentheses.

## Incremental indexing

```
./build/search data/sample --index sample.idx
```

- **First run:** indexes every `.txt` file under the folder and saves `sample.idx`.
- **Later runs:** loads the saved index, then compares each file's size and modification time with the stamp stored in the index. Only new files are indexed, changed files are re-indexed, and deleted files are removed. Unchanged files are not even read.
- In the prompt: `:add <file>`, `:remove <name>`, `:save`, `:stats`. Changes made with these are saved on exit.

How it works:

- **Removal uses tombstones.** A removed document keeps its id slot (so other ids never change) and is skipped by every query immediately. Its postings are physically deleted later by `compact()`, which also runs automatically once enough removals pile up. This is the same idea real engines use (mark deleted now, merge later), because deleting from the middle of every posting list per removal would be slow.
- **Statistics stay correct.** Document counts, document frequencies and average length only count live documents, so BM25 scores after a removal are identical to a freshly built index (there is a test for exactly this).
- **Updating = remove + add.** Adding a document whose name already exists replaces it.
- **Saved format.** Posting lists are sorted, so each document id and word position is stored as the *difference* from the previous one, written as a variable-length integer (1 byte for numbers under 128). Saving writes a temporary file and renames it, so a crash cannot corrupt an existing index. Loading validates everything and throws on a truncated or corrupt file instead of crashing (tested by flipping every byte of a file in turn, also under AddressSanitizer).

Limitation: change detection uses size and modification time, so a file edited without changing either would not be noticed.

## Build and run

Requirements: a C++17 compiler (GCC/MinGW or MSVC) and CMake 3.14+. GoogleTest is downloaded automatically.

```
cmake -S . -B build
cmake --build build
./build/search data/sample        # index a folder of .txt files, then type queries
ctest --test-dir build            # run the unit tests
```

Windows with MinGW:
```
cmake -S . -B build -G "MinGW Makefiles"
cmake --build build
.\build\search.exe data\sample
```

Windows with Visual Studio: build with `--config Release`; the binary is in `build\Release\`.

To build without the tests: `cmake -S . -B build -DBUILD_TESTS=OFF`.

## Example

```
Indexed 3 documents, 81 unique terms in 1.1 ms

search> inverted index
  2.68  data/sample/search_engines.txt
(1.3 us)
```

## Status

- [x] Tokenizer (lowercase, split on non-alphanumerics)
- [x] Inverted index with word positions
- [x] TF-IDF ranking
- [x] BM25 ranking (default), selectable via `RankMode`
- [x] CLI with indexing time and per-query latency
- [x] Unit tests (GoogleTest)
- [x] Boolean queries (AND / OR / NOT, parentheses)
- [x] Phrase search (uses the stored word positions)
- [x] Incremental indexing: add, update and remove documents without rebuilding
- [x] Save / load the index (delta + varint compressed), re-sync a folder processing only what changed
- [ ] Benchmarks: indexing throughput, query latency, memory on 1K / 10K / 100K documents

## Project layout

```
include/   headers (tokenizer.h, index.h, query.h, corpus.h)
src/       implementation: index.cpp (index + ranking), index_io.cpp (save/load),
           query.cpp (parser), corpus.cpp (folder sync); main.cpp is the CLI
tests/     GoogleTest unit tests (tokenizer, index, BM25, queries, incremental indexing)
data/      sample documents
```

## API sketch

```cpp
ms::InvertedIndex index;
index.addDocument("a.txt", "the cat sat on the mat");
index.addDocument("b.txt", "dogs are loyal");

auto results = index.search("cat", 10);                       // BM25
auto tfidf   = index.search("cat", 10, ms::RankMode::TfIdf);  // for comparison
for (auto& r : results) std::cout << index.docName(r.docId) << " " << r.score << "\n";

// Phrase and Boolean queries (include "query.h"):
auto hits = ms::runQuery(index, "(cat OR dog) AND \"sat on\" AND NOT bird");

// Incremental indexing:
index.addDocument("a.txt", "new text");   // adds, or replaces an existing "a.txt"
index.removeDocument("b.txt");            // hidden from queries immediately
index.save("my.idx");                     // compressed binary file
auto again = ms::InvertedIndex::load("my.idx");
ms::syncFolder(again, "data/books");      // only new / changed / deleted files are processed
```
