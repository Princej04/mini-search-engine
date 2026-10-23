# Mini Search Engine (C++17)

A local document search engine built around an inverted index.

## Build and run

```
cmake -S . -B build
cmake --build build
./build/search data/sample        # index a folder of .txt files, then type queries
ctest --test-dir build            # run the unit tests
```

On Windows (Visual Studio) the binaries are in `build/Release/` or `build/Debug/`.

## Current status

- [x] Tokenizer (lowercase, split on non-alphanumerics)
- [x] Inverted index with word positions
- [x] TF-IDF ranked search
- [x] CLI and unit tests
- [ ] BM25 ranking
- [ ] Boolean queries (AND / OR / NOT)
- [ ] Phrase search (uses the stored positions)
- [ ] Incremental indexing (add / remove documents), save / load index
- [ ] Benchmarks: indexing throughput, query latency, memory on 1K / 10K / 100K docs

## Layout

```
include/   headers
src/       implementation, main.cpp is the CLI
tests/     GoogleTest unit tests
data/      sample documents
```
