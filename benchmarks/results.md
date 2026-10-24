# Benchmark results

- Build: optimized (NDEBUG defined)
- Compiler: GCC 13.1.0
- Data: generated, seed 42, vocabulary 200,000 words (Zipf distribution), document length median 120 / mean about 150 words
- Queries: 1000 per type, top 10 results, single thread
- CPU / RAM: (add your machine here)

## Indexing

| Documents | Words | Text size | Index time | Docs/s | MB text/s | Unique terms | Peak memory* |
|---|---:|---:|---:|---:|---:|---:|---:|
| 1,000 | 0.15 M | 0.7 MB | 0.03 s | 28,890 | 19.4 | 38,872 | 11 MB |
| 10,000 | 1.51 M | 6.7 MB | 0.37 s | 27,317 | 18.3 | 143,972 | 84 MB |
| 100,000 | 15.27 M | 67.7 MB | 3.70 s | 27,061 | 18.3 | 199,949 | 706 MB |

*Peak process memory reached while building the index, minus what the process used before any document existed (the generator's word list). Includes allocator overhead and the temporary buffers used while indexing a document.

## Query latency (microseconds, lower is better)

### 1,000 documents

| Query | Mean | Median | 95th pct | 99th pct | Avg matching docs |
|---|---:|---:|---:|---:|---:|
| Single common word | 12.7 | 3.4 | 40.7 | 47.8 | - |
| Single rare word | 0.5 | 0.4 | 0.8 | 1.1 | - |
| Two words, any may match (BM25) | 21.9 | 21.0 | 46.2 | 53.0 | - |
| Two words, any may match (TF-IDF) | 21.1 | 20.9 | 43.7 | 50.3 | - |
| a AND b | 24.0 | 22.5 | 53.0 | 82.8 | 63.6 |
| "a b" (phrase) | 24.9 | 19.2 | 74.6 | 119.1 | 9.2 |
| (a OR b) AND NOT c | 31.7 | 31.6 | 57.8 | 67.3 | 332.9 |

BM25 and TF-IDF share 29% of their top-10 results on the two-word queries. (This shows how different the two rankings are, not which is better: generated text has no real relevance.)

### 10,000 documents

| Query | Mean | Median | 95th pct | 99th pct | Avg matching docs |
|---|---:|---:|---:|---:|---:|
| Single common word | 111.4 | 20.1 | 379.6 | 409.3 | - |
| Single rare word | 0.9 | 0.7 | 1.5 | 4.2 | - |
| Two words, any may match (BM25) | 202.6 | 187.8 | 440.1 | 512.0 | - |
| Two words, any may match (TF-IDF) | 202.6 | 185.4 | 452.2 | 617.8 | - |
| a AND b | 251.3 | 228.8 | 605.7 | 857.6 | 644.5 |
| "a b" (phrase) | 256.7 | 178.2 | 954.0 | 1460.5 | 109.5 |
| (a OR b) AND NOT c | 337.4 | 338.5 | 683.0 | 787.6 | 3334.5 |

BM25 and TF-IDF share 19% of their top-10 results on the two-word queries. (This shows how different the two rankings are, not which is better: generated text has no real relevance.)

### 100,000 documents

| Query | Mean | Median | 95th pct | 99th pct | Avg matching docs |
|---|---:|---:|---:|---:|---:|
| Single common word | 1342.7 | 201.4 | 5078.2 | 5587.4 | - |
| Single rare word | 2.9 | 1.6 | 7.0 | 29.7 | - |
| Two words, any may match (BM25) | 2503.1 | 2006.9 | 5851.4 | 6627.0 | - |
| Two words, any may match (TF-IDF) | 2495.2 | 2038.5 | 5900.7 | 6566.9 | - |
| a AND b | 3264.2 | 2649.9 | 8254.2 | 9661.8 | 6447.0 |
| "a b" (phrase) | 2881.3 | 1865.9 | 10135.0 | 15723.2 | 818.0 |
| (a OR b) AND NOT c | 4524.1 | 4014.2 | 9608.6 | 10909.9 | 33306.7 |

BM25 and TF-IDF share 10% of their top-10 results on the two-word queries. (This shows how different the two rankings are, not which is better: generated text has no real relevance.)

## Scaling (mean latency in microseconds)

| Query | 1,000 docs | 10,000 docs | 100,000 docs |
|---|---:|---:|---:|
| Single common word | 12.7 | 111.4 | 1342.7 |
| Single rare word | 0.5 | 0.9 | 2.9 |
| Two words, any may match (BM25) | 21.9 | 202.6 | 2503.1 |
| Two words, any may match (TF-IDF) | 21.1 | 202.6 | 2495.2 |
| a AND b | 24.0 | 251.3 | 3264.2 |
| "a b" (phrase) | 24.9 | 256.7 | 2881.3 |
| (a OR b) AND NOT c | 31.7 | 337.4 | 4524.1 |

## Saving, loading and incremental updates

| Documents | Build from files | Save | Index file | File / text size | Load | Re-sync, nothing changed | Re-sync, 3% changed | Speed-up vs rebuild |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 1,000 | 3.02 s | 10 ms | 0.7 MB | 111% | 26 ms | 22 ms | 135 ms | 22x |
| 10,000 | 2.40 s | 73 ms | 5.8 MB | 87% | 88 ms | 225 ms | 511 ms | 5x |

"3% changed" = 1% of files edited, 1% deleted, 1% new. "Build from files" includes reading every file from disk.
