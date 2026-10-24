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

## Benchmarks

```
cmake -S . -B build -G "MinGW Makefiles"
cmake --build build
./build/benchmark --out benchmarks/results.md
```

Use an optimized build (this project defaults to Release; with Visual Studio add `--config Release`). The program prints a warning if it was not built that way.

It generates its own documents (a 200,000-word vocabulary with a Zipf-like word distribution and log-normal document lengths, about 150 words on average), so it needs no downloads and every run is reproducible from the seed. For each size it reports:

- **Indexing:** time, documents per second, MB of text per second, unique terms, peak memory.
- **Queries:** mean, median, 95th and 99th percentile latency for single words, two-word ranked queries (BM25 and TF-IDF), `a AND b`, phrases, and `(a OR b) AND NOT c`.
- **Persistence:** time to build from files, save, and load; index file size; and the time to re-sync after 1% of files were edited, 1% deleted and 1% added, compared with a full rebuild.

Options: `--sizes 1000,10000,100000`, `--queries 1000`, `--sync-max 10000` (the file-based test writes that many files to your temp folder), `--no-sync`, `--seed 42`. The default 100,000-document run needs roughly 1 GB of RAM; use `--sizes 1000,10000` on a small machine.

### Results

Generated data, optimized build (GCC 13.1.0), single thread, 1,000 queries per type, top 10 results. Machine: (add your CPU and RAM here). Full tables: [`benchmarks/results.md`](benchmarks/results.md).

**Indexing**

| Documents | Words | Index time | Docs/s | Unique terms | Peak memory |
|---|---:|---:|---:|---:|---:|
| 1,000 | 0.15 M | 0.03 s | 28,890 | 38,872 | 11 MB |
| 10,000 | 1.51 M | 0.37 s | 27,317 | 143,972 | 84 MB |
| 100,000 | 15.27 M | 3.70 s | 27,061 | 199,949 | 706 MB |

**Mean query latency (microseconds)**

| Query | 1,000 docs | 10,000 docs | 100,000 docs |
|---|---:|---:|---:|
| Single common word | 12.7 | 111.4 | 1,342.7 |
| Single rare word | 0.5 | 0.9 | 2.9 |
| Two words, any may match (BM25) | 21.9 | 202.6 | 2,503.1 |
| `a AND b` | 24.0 | 251.3 | 3,264.2 |
| `"a b"` (phrase) | 24.9 | 256.7 | 2,881.3 |
| `(a OR b) AND NOT c` | 31.7 | 337.4 | 4,524.1 |

**Saving, loading and incremental updates**

| Documents | Build from files | Save | Index file | Load | Re-sync, nothing changed | Re-sync, 3% changed |
|---|---:|---:|---:|---:|---:|---:|
| 1,000 | 3.02 s* | 10 ms | 0.7 MB | 26 ms | 22 ms | 135 ms |
| 10,000 | 2.40 s | 73 ms | 5.8 MB | 88 ms | 225 ms | 511 ms |

\*The 1,000-document build time is an outlier (it was slower than the 10,000 case). It was the first file-based test in the run, so it most likely paid for cold disk caches and antivirus scanning of freshly written files. Read the 10,000-document row for the real picture.

**What the numbers show**

- **Indexing is linear.** Throughput stays near 27,000 documents (about 18 MB of text) per second at every size.
- **Memory is the weak spot.** 100,000 documents use about 706 MB, roughly 10 times the size of the text itself (about 7 KB per document). Each posting is a separate small vector, which costs a lot in per-object overhead.
- **Query time grows linearly with the corpus.** A common-word query goes from 13 us to 111 us to 1.3 ms as the corpus grows 10x each step, while a rare word stays under 3 us. The engine scores every matching document (a common word matches thousands), so cost follows the posting-list length, not the corpus size alone. Phrase and AND queries cost about the same as a plain two-word query.
- **Saved indexes pay off.** Loading the 10,000-document index (88 ms) is about 27 times faster than building it from the files (2.4 s). Re-syncing after 3% of the files changed took 511 ms instead of 2.4 s, about 5 times faster. Much of what remains is walking the folder and checking every file's size and modification time.

### Possible improvements

- **Faster queries:** accumulate scores in a flat array indexed by document id instead of a hash map, and keep the top 10 in a small heap instead of sorting every match.
- **Less memory:** store postings in flat arrays, and keep the delta + varint compression in memory instead of only on disk.
- **Faster AND and phrase queries:** skip pointers in posting lists, and process the rarest word first.
- **Faster indexing:** build in parallel on several threads and merge.
- **Better matching:** stemming, so "running" also matches "run".

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
- [x] Benchmark program: indexing throughput, query latency, memory on 1K / 10K / 100K documents, save / load / incremental re-sync
- [x] Benchmark results on real hardware (see Benchmarks below)

## Project layout

```
include/   headers (tokenizer.h, index.h, query.h, corpus.h)
src/       implementation: index.cpp (index + ranking), index_io.cpp (save/load),
           query.cpp (parser), corpus.cpp (folder sync); main.cpp is the CLI
tests/     GoogleTest unit tests (tokenizer, index, BM25, queries, incremental indexing)
benchmarks/ bench.cpp: the benchmark program (results go in benchmarks/results.md)
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
