# Mini Search Engine (C++17)

A local document search engine built around an inverted index, with BM25 relevance ranking.

## How it works

1. **Tokenize**: each document is lowercased and split into words.
2. **Index**: an inverted index maps every word to the documents containing it, along with the position of each occurrence. A query only touches the posting lists of its own words instead of scanning every document.
3. **Rank**: matching documents are scored with BM25 and the top results are returned.

### BM25 in one paragraph

Each query word contributes a score to each document that contains it. The score combines three things: how rare the word is across the corpus (IDF), how often it appears in the document (with diminishing returns, so 50 mentions are not worth 50x one mention), and the document's length relative to the average (so long documents don't win just by containing more words). Scores for all query words are added up. TF-IDF is also available for comparison.

Tuning constants: `k1 = 1.2` (term-frequency saturation) and `b = 0.75` (length normalization).

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
- [ ] Boolean queries (AND / OR / NOT)
- [ ] Phrase search (will use the stored positions)
- [ ] Incremental indexing (add / remove documents), save / load index
- [ ] Benchmarks: indexing throughput, query latency, memory on 1K / 10K / 100K documents

## Project layout

```
include/   headers (tokenizer.h, index.h)
src/       implementation; main.cpp is the CLI
tests/     GoogleTest unit tests (tokenizer, index, BM25)
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
```