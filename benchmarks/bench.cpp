// Benchmark for the mini search engine.
//
//   benchmark [--sizes 1000,10000,100000] [--queries 1000] [--sync-max 10000]
//             [--no-sync] [--seed 42] [--out results.md]
//
// Documents are generated, not downloaded: a fixed vocabulary with a Zipf-like word
// distribution (a few words are very common, most are rare, as in natural language;
// the exact tail is not tuned to match any real corpus) and log-normal document
// lengths. Document i is a pure function of (seed, i), so
// every run is reproducible and the text never has to be kept in memory.
//
// For each size it measures:
//   - indexing time and throughput, and peak process memory
//   - query latency (mean / median / 95th / 99th percentile) per query type
//   - how much BM25 and TF-IDF disagree on the top 10
//   - saving, loading, and re-syncing a folder after 3% of files changed

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "corpus.h"
#include "index.h"
#include "query.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <psapi.h>
#elif defined(__unix__) || defined(__APPLE__)
#include <sys/resource.h>
#endif

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

// ---------------------------------------------------------------- utilities

double secondsSince(Clock::time_point t) {
    return std::chrono::duration<double>(Clock::now() - t).count();
}
double microsSince(Clock::time_point t) {
    return std::chrono::duration<double, std::micro>(Clock::now() - t).count();
}

std::string num(double v, int decimals = 1) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*f", decimals, v);
    return buf;
}

std::string withCommas(uint64_t v) {
    std::string s = std::to_string(v);
    for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) s.insert(static_cast<size_t>(i), ",");
    return s;
}

// Peak memory used by this process so far, in bytes (0 if unknown).
size_t peakMemoryBytes() {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) return pmc.PeakWorkingSetSize;
    return 0;
#elif defined(__APPLE__)
    struct rusage ru;
    if (getrusage(RUSAGE_SELF, &ru) == 0) return static_cast<size_t>(ru.ru_maxrss);  // bytes
    return 0;
#elif defined(__unix__)
    struct rusage ru;
    if (getrusage(RUSAGE_SELF, &ru) == 0) return static_cast<size_t>(ru.ru_maxrss) * 1024;  // KB
    return 0;
#else
    return 0;
#endif
}

std::string compilerName() {
#if defined(__clang__)
    return std::string("Clang ") + __clang_version__;
#elif defined(__GNUC__)
    return std::string("GCC ") + __VERSION__;
#elif defined(_MSC_VER)
    return "MSVC " + std::to_string(_MSC_VER);
#else
    return "unknown compiler";
#endif
}

uint64_t splitmix64(uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

// Uniform in [0, 1). Built from raw mt19937_64 output (whose sequence is fixed by the
// C++ standard) so datasets are identical on every compiler and platform.
double uniform01(std::mt19937_64& rng) {
    return static_cast<double>(rng() >> 11) * (1.0 / 9007199254740992.0);
}

// ---------------------------------------------------------------- data generator

class Corpus {
public:
    Corpus(size_t vocabSize, uint64_t seed) : seed_(seed) {
        words_.reserve(vocabSize);
        cumulative_.reserve(vocabSize);
        double sum = 0.0;
        for (size_t rank = 0; rank < vocabSize; ++rank) {
            words_.push_back(makeWord(rank));
            sum += 1.0 / static_cast<double>(rank + 1);  // Zipf: frequency ~ 1 / rank
            cumulative_.push_back(sum);
        }
        total_ = sum;
    }

    size_t vocabSize() const { return words_.size(); }
    const std::string& word(size_t rank) const { return words_[rank]; }

    // A random word rank; low ranks (common words) are much more likely.
    size_t sampleRank(std::mt19937_64& rng) const {
        const double u = uniform01(rng) * total_;
        const auto it = std::upper_bound(cumulative_.begin(), cumulative_.end(), u);
        return std::min(static_cast<size_t>(it - cumulative_.begin()), words_.size() - 1);
    }

    // Word ranks of document `id` (always the same for the same id and seed).
    std::vector<uint32_t> docRanks(uint32_t id) const {
        std::mt19937_64 rng(splitmix64(seed_ + id));
        // Log-normal length (median 120 words, mean about 150) via Box-Muller.
        const double u1 = std::max(uniform01(rng), 1e-12);
        const double u2 = uniform01(rng);
        const double z = std::sqrt(-2.0 * std::log(u1)) * std::cos(6.283185307179586 * u2);
        const double len = std::exp(std::log(120.0) + 0.7 * z);
        const size_t n = static_cast<size_t>(std::min(3000.0, std::max(20.0, len)));
        std::vector<uint32_t> ranks(n);
        for (uint32_t& r : ranks) r = static_cast<uint32_t>(sampleRank(rng));
        return ranks;
    }

    std::string render(const std::vector<uint32_t>& ranks) const {
        std::string text;
        text.reserve(ranks.size() * 7);
        for (size_t i = 0; i < ranks.size(); ++i) {
            if (i) text.push_back(' ');
            text += words_[ranks[i]];
        }
        return text;
    }

    std::string doc(uint32_t id) const { return render(docRanks(id)); }

private:
    // Rank -> a unique pronounceable pseudo-word: common words come out short.
    static std::string makeWord(size_t rank) {
        static const char kCons[] = "bcdfghjklmnprstvwxyz";  // 20
        static const char kVowels[] = "aeiou";               // 5
        std::string w;
        size_t n = rank;
        do {
            const size_t d = n % 100;
            w.push_back(kCons[d / 5]);
            w.push_back(kVowels[d % 5]);
            n /= 100;
        } while (n > 0);
        return w;
    }

    uint64_t seed_;
    std::vector<std::string> words_;
    std::vector<double> cumulative_;
    double total_ = 0.0;
};

std::string docName(uint32_t id) { return "doc" + std::to_string(id) + ".txt"; }

// ---------------------------------------------------------------- measuring

struct Latency {
    double mean = 0, p50 = 0, p95 = 0, p99 = 0;  // microseconds
};

volatile size_t g_sink = 0;  // keeps the compiler from optimizing queries away

template <class F>
Latency measure(const std::vector<std::string>& queries, F run) {
    const size_t warm = std::min<size_t>(queries.size(), 50);
    for (size_t i = 0; i < warm; ++i) g_sink = g_sink + run(queries[i]);

    std::vector<double> us;
    us.reserve(queries.size());
    for (const std::string& q : queries) {
        const auto t = Clock::now();
        const size_t r = run(q);
        us.push_back(microsSince(t));
        g_sink = g_sink + r;
    }
    std::sort(us.begin(), us.end());
    Latency l;
    if (us.empty()) return l;
    double sum = 0;
    for (double v : us) sum += v;
    l.mean = sum / static_cast<double>(us.size());
    auto pct = [&](double p) { return us[static_cast<size_t>(p * static_cast<double>(us.size() - 1))]; };
    l.p50 = pct(0.50);
    l.p95 = pct(0.95);
    l.p99 = pct(0.99);
    return l;
}

struct QuerySets {
    std::vector<std::string> single, rare, twoWords, andQ, phrase, complexQ;
};

QuerySets makeQueries(const Corpus& c, size_t count, uint32_t numDocs, uint64_t seed) {
    std::mt19937_64 rng(splitmix64(seed ^ 0xABCDEF));
    auto commonWord = [&] { return c.word(c.sampleRank(rng)); };
    auto distinctWord = [&](const std::string& other) {
        std::string w = commonWord();
        while (w == other) w = commonWord();
        return w;
    };

    QuerySets q;
    for (size_t i = 0; i < count; ++i) {
        q.single.push_back(commonWord());

        const size_t lo = std::min<size_t>(1000, c.vocabSize() - 1);
        const size_t span = c.vocabSize() - lo;
        q.rare.push_back(c.word(lo + static_cast<size_t>(uniform01(rng) * static_cast<double>(span)) % span));

        const std::string a = commonWord();
        const std::string b = distinctWord(a);
        const std::string d = distinctWord(a);
        q.twoWords.push_back(a + " " + b);
        q.andQ.push_back(a + " AND " + b);
        q.complexQ.push_back("(" + a + " OR " + b + ") AND NOT " + d);

        // A phrase that really occurs: two adjacent words from a random document.
        const uint32_t id = static_cast<uint32_t>(rng() % numDocs);
        const auto ranks = c.docRanks(id);
        const size_t pos = static_cast<size_t>(rng() % (ranks.size() - 1));
        q.phrase.push_back("\"" + c.word(ranks[pos]) + " " + c.word(ranks[pos + 1]) + "\"");
    }
    return q;
}

double avgMatches(const ms::InvertedIndex& idx, const std::vector<std::string>& queries) {
    const size_t n = std::min<size_t>(queries.size(), 200);
    if (n == 0) return 0;
    double total = 0;
    for (size_t i = 0; i < n; ++i) total += static_cast<double>(ms::booleanMatch(idx, queries[i]).size());
    return total / static_cast<double>(n);
}

// Average fraction of the BM25 top 10 that TF-IDF also puts in its top 10.
double rankingOverlap(const ms::InvertedIndex& idx, const std::vector<std::string>& queries) {
    double total = 0;
    size_t used = 0;
    for (size_t i = 0; i < std::min<size_t>(queries.size(), 200); ++i) {
        const auto a = idx.search(queries[i], 10, ms::RankMode::BM25);
        const auto b = idx.search(queries[i], 10, ms::RankMode::TfIdf);
        if (a.empty()) continue;
        size_t common = 0;
        for (const auto& x : a) {
            for (const auto& y : b) {
                if (x.docId == y.docId) { ++common; break; }
            }
        }
        total += static_cast<double>(common) / static_cast<double>(a.size());
        ++used;
    }
    return used ? total / static_cast<double>(used) : 0;
}

// ---------------------------------------------------------------- results

struct QueryRow {
    std::string name;
    Latency lat;
    double matches = -1;  // average number of matching documents; -1 = not applicable
};

struct SyncResult {
    bool ran = false;
    bool consistent = true;
    double fullBuildSec = 0, saveSec = 0, loadSec = 0, noChangeMs = 0, changedMs = 0;
    uint64_t rawBytes = 0, fileBytes = 0;
    size_t changedFiles = 0;
};

struct SizeResult {
    size_t n = 0;
    double words = 0;
    uint64_t textBytes = 0;
    double indexSec = 0;
    size_t terms = 0;
    size_t peakDeltaBytes = 0;
    std::vector<QueryRow> rows;
    double overlap = 0;
    SyncResult sync;
};

SyncResult benchSync(const Corpus& corpus, size_t n) {
    SyncResult r;
    const fs::path base = fs::temp_directory_path();
    const fs::path dir = base / ("ms_bench_" + std::to_string(n));
    const fs::path indexFile = base / ("ms_bench_" + std::to_string(n) + ".idx");
    std::error_code ec;
    fs::remove_all(dir, ec);

    auto pathOf = [&](uint32_t id) { return dir / std::to_string(id / 1000) / docName(id); };
    auto write = [&](uint32_t id, const std::string& text) {
        const fs::path p = pathOf(id);
        fs::create_directories(p.parent_path());
        std::ofstream out(p, std::ios::binary | std::ios::trunc);
        out << text;
    };

    for (uint32_t id = 0; id < n; ++id) {
        const std::string text = corpus.doc(id);
        r.rawBytes += text.size();
        write(id, text);
    }

    ms::InvertedIndex idx;
    auto t = Clock::now();
    ms::syncFolder(idx, dir);
    r.fullBuildSec = secondsSince(t);

    t = Clock::now();
    idx.save(indexFile.string());
    r.saveSec = secondsSince(t);
    r.fileBytes = static_cast<uint64_t>(fs::file_size(indexFile, ec));

    t = Clock::now();
    ms::InvertedIndex loaded = ms::InvertedIndex::load(indexFile.string());
    r.loadSec = secondsSince(t);
    if (loaded.numDocs() != idx.numDocs()) r.consistent = false;

    t = Clock::now();
    const ms::SyncStats same = ms::syncFolder(loaded, dir);
    r.noChangeMs = secondsSince(t) * 1000.0;
    if (same.changed() || same.unchanged != n) r.consistent = false;

    // Change 1% of the files, delete 1%, add 1%.
    const size_t k = std::max<size_t>(1, n / 100);
    r.changedFiles = 3 * k;
    for (uint32_t id = 0; id < k; ++id) write(id, corpus.doc(id) + " benchmark edit");
    for (uint32_t id = static_cast<uint32_t>(k); id < 2 * k; ++id) fs::remove(pathOf(id), ec);
    for (uint32_t id = static_cast<uint32_t>(n); id < n + k; ++id) write(id, corpus.doc(id));

    t = Clock::now();
    const ms::SyncStats st = ms::syncFolder(loaded, dir);
    r.changedMs = secondsSince(t) * 1000.0;
    if (st.updated != k || st.removed != k || st.added != k) r.consistent = false;

    r.ran = true;
    fs::remove_all(dir, ec);
    fs::remove(indexFile, ec);
    return r;
}

// Indexing and queries for one size. `baselinePeak` is the process's peak memory before
// any index existed; sizes must run in increasing order and before any save/load test
// (those use several indexes at once), so that the peak reached while building this
// index is the highest the process has seen.
SizeResult runSize(const Corpus& corpus, size_t n, size_t queryCount, uint64_t seed,
                   size_t baselinePeak) {
    SizeResult res;
    res.n = n;

    // ---- indexing
    std::cerr << "[" << withCommas(n) << " docs] indexing...\n";
    ms::InvertedIndex idx;
    const size_t peakBefore = baselinePeak;
    double indexSeconds = 0;
    for (uint32_t id = 0; id < n; ++id) {
        const std::string text = corpus.doc(id);  // generation time is not counted
        res.textBytes += text.size();
        const auto t = Clock::now();
        idx.addDocument(docName(id), text);
        indexSeconds += secondsSince(t);
    }
    const size_t peakAfter = peakMemoryBytes();
    res.indexSec = indexSeconds;
    res.terms = idx.numTerms();
    res.words = idx.avgDocLength() * static_cast<double>(idx.numDocs());
    res.peakDeltaBytes = peakAfter > peakBefore ? peakAfter - peakBefore : 0;

    // ---- queries
    std::cerr << "[" << withCommas(n) << " docs] querying...\n";
    const QuerySets q = makeQueries(corpus, queryCount, static_cast<uint32_t>(n), seed);
    auto ranked = [&](ms::RankMode mode) {
        return [&idx, mode](const std::string& s) { return ms::runQuery(idx, s, 10, mode).size(); };
    };
    auto plain = ranked(ms::RankMode::BM25);

    res.rows.push_back({"Single common word", measure(q.single, plain), -1});
    res.rows.push_back({"Single rare word", measure(q.rare, plain), -1});
    res.rows.push_back({"Two words, any may match (BM25)", measure(q.twoWords, plain), -1});
    res.rows.push_back({"Two words, any may match (TF-IDF)",
                        measure(q.twoWords, ranked(ms::RankMode::TfIdf)), -1});
    res.rows.push_back({"a AND b", measure(q.andQ, plain), avgMatches(idx, q.andQ)});
    res.rows.push_back({"\"a b\" (phrase)", measure(q.phrase, plain), avgMatches(idx, q.phrase)});
    res.rows.push_back({"(a OR b) AND NOT c", measure(q.complexQ, plain), avgMatches(idx, q.complexQ)});
    res.overlap = rankingOverlap(idx, q.twoWords);
    return res;
}

// ---------------------------------------------------------------- report

void tableRow(std::ostream& os, const std::vector<std::string>& cells) {
    os << "|";
    for (const auto& c : cells) os << " " << c << " |";
    os << "\n";
}

void tableHeader(std::ostream& os, const std::vector<std::string>& cells) {
    tableRow(os, cells);
    os << "|";
    for (size_t i = 0; i < cells.size(); ++i) os << (i == 0 ? "---|" : "---:|");
    os << "\n";
}

std::string report(const std::vector<SizeResult>& results, const Corpus& corpus, uint64_t seed,
                   size_t queryCount) {
    std::ostringstream os;
    os << "# Benchmark results\n\n";
#ifdef NDEBUG
    os << "- Build: optimized (NDEBUG defined)\n";
#else
    os << "- **WARNING: this is not an optimized build. Numbers are not meaningful.** "
          "Rebuild with `-DCMAKE_BUILD_TYPE=Release` (or `--config Release` for Visual Studio).\n";
#endif
    os << "- Compiler: " << compilerName() << "\n";
    os << "- Data: generated, seed " << seed << ", vocabulary " << withCommas(corpus.vocabSize())
       << " words (Zipf distribution), document length median 120 / mean about 150 words\n";
    os << "- Queries: " << queryCount << " per type, top 10 results, single thread\n";
    os << "- CPU / RAM: (add your machine here)\n\n";

    os << "## Indexing\n\n";
    tableHeader(os, {"Documents", "Words", "Text size", "Index time", "Docs/s", "MB text/s",
                     "Unique terms", "Peak memory*"});
    for (const auto& r : results) {
        const double mb = static_cast<double>(r.textBytes) / (1024.0 * 1024.0);
        const double peakMB = static_cast<double>(r.peakDeltaBytes) / (1024.0 * 1024.0);
        tableRow(os, {withCommas(r.n), num(r.words / 1e6, 2) + " M", num(mb, 1) + " MB",
                      num(r.indexSec, 2) + " s", withCommas(static_cast<uint64_t>(static_cast<double>(r.n) / r.indexSec)),
                      num(mb / r.indexSec, 1), withCommas(r.terms),
                      r.peakDeltaBytes ? num(peakMB, 0) + " MB" : "n/a"});
    }
    os << "\n*Peak process memory reached while building the index, minus what the process "
          "used before any document existed (the generator's word list). Includes allocator "
          "overhead and the temporary buffers used while indexing a document.\n\n";

    os << "## Query latency (microseconds, lower is better)\n\n";
    for (const auto& r : results) {
        os << "### " << withCommas(r.n) << " documents\n\n";
        tableHeader(os, {"Query", "Mean", "Median", "95th pct", "99th pct", "Avg matching docs"});
        for (const auto& row : r.rows) {
            tableRow(os, {row.name, num(row.lat.mean), num(row.lat.p50), num(row.lat.p95),
                          num(row.lat.p99), row.matches < 0 ? "-" : num(row.matches, 1)});
        }
        os << "\nBM25 and TF-IDF share " << num(r.overlap * 100, 0)
           << "% of their top-10 results on the two-word queries. (This shows how different "
              "the two rankings are, not which is better: generated text has no real relevance.)\n\n";
    }

    os << "## Scaling (mean latency in microseconds)\n\n";
    std::vector<std::string> head = {"Query"};
    for (const auto& r : results) head.push_back(withCommas(r.n) + " docs");
    tableHeader(os, head);
    if (!results.empty()) {
        for (size_t i = 0; i < results.front().rows.size(); ++i) {
            std::vector<std::string> cells = {results.front().rows[i].name};
            for (const auto& r : results) cells.push_back(num(r.rows[i].lat.mean));
            tableRow(os, cells);
        }
    }

    bool anySync = false;
    for (const auto& r : results) anySync = anySync || r.sync.ran;
    if (anySync) {
        os << "\n## Saving, loading and incremental updates\n\n";
        tableHeader(os, {"Documents", "Build from files", "Save", "Index file", "File / text size",
                         "Load", "Re-sync, nothing changed", "Re-sync, 3% changed", "Speed-up vs rebuild"});
        for (const auto& r : results) {
            if (!r.sync.ran) continue;
            const auto& s = r.sync;
            tableRow(os, {withCommas(r.n), num(s.fullBuildSec, 2) + " s", num(s.saveSec * 1000, 0) + " ms",
                          num(static_cast<double>(s.fileBytes) / (1024.0 * 1024.0), 1) + " MB",
                          num(100.0 * static_cast<double>(s.fileBytes) / static_cast<double>(s.rawBytes), 0) + "%",
                          num(s.loadSec * 1000, 0) + " ms", num(s.noChangeMs, 0) + " ms",
                          num(s.changedMs, 0) + " ms",
                          num(s.fullBuildSec * 1000.0 / std::max(s.changedMs, 0.001), 0) + "x"});
            if (!s.consistent) os << "\n**Warning: the sync counts for " << r.n << " documents were not as expected.**\n";
        }
        os << "\n\"3% changed\" = 1% of files edited, 1% deleted, 1% new. \"Build from files\" "
              "includes reading every file from disk.\n";
    }
    return os.str();
}

// ---------------------------------------------------------------- main

void usage(const char* prog) {
    std::cerr << "Usage: " << prog << " [options]\n"
              << "  --sizes a,b,c    document counts to test (default 1000,10000,100000)\n"
              << "  --queries N      queries per type (default 1000)\n"
              << "  --sync-max N     largest size for the file-based save/load/sync test (default 10000)\n"
              << "  --no-sync        skip the file-based test (it writes files to the temp folder)\n"
              << "  --seed S         random seed (default 42)\n"
              << "  --out FILE       also write the report to FILE (markdown)\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<size_t> sizes = {1000, 10000, 100000};
    size_t queryCount = 1000;
    size_t syncMax = 10000;
    bool doSync = true;
    uint64_t seed = 42;
    std::string outFile;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        try {
            if (a == "--sizes") {
                const char* v = next();
                if (!v) { usage(argv[0]); return 1; }
                sizes.clear();
                std::stringstream ss(v);
                std::string item;
                while (std::getline(ss, item, ',')) {
                    const long long n = std::stoll(item);
                    if (n < 1) throw std::invalid_argument("size");
                    sizes.push_back(static_cast<size_t>(n));
                }
            } else if (a == "--queries") {
                const char* v = next();
                if (!v) { usage(argv[0]); return 1; }
                queryCount = static_cast<size_t>(std::stoll(v));
            } else if (a == "--sync-max") {
                const char* v = next();
                if (!v) { usage(argv[0]); return 1; }
                syncMax = static_cast<size_t>(std::stoll(v));
            } else if (a == "--seed") {
                const char* v = next();
                if (!v) { usage(argv[0]); return 1; }
                seed = static_cast<uint64_t>(std::stoull(v));
            } else if (a == "--out") {
                const char* v = next();
                if (!v) { usage(argv[0]); return 1; }
                outFile = v;
            } else if (a == "--no-sync") {
                doSync = false;
            } else {
                usage(argv[0]);
                return a == "--help" ? 0 : 1;
            }
        } catch (const std::exception&) {
            std::cerr << "Bad value for " << a << "\n";
            usage(argv[0]);
            return 1;
        }
    }
    if (sizes.empty() || queryCount == 0) { usage(argv[0]); return 1; }
    std::sort(sizes.begin(), sizes.end());
    sizes.erase(std::unique(sizes.begin(), sizes.end()), sizes.end());

#ifndef NDEBUG
    std::cerr << "WARNING: not an optimized build; the numbers will be much too slow.\n";
#endif

    const Corpus corpus(200000, seed);
    const size_t baselinePeak = peakMemoryBytes();

    std::vector<SizeResult> results;
    for (size_t n : sizes) results.push_back(runSize(corpus, n, queryCount, seed, baselinePeak));

    // The file-based test runs last: it holds several indexes at once, which would
    // distort the memory numbers above.
    if (doSync) {
        for (SizeResult& r : results) {
            if (r.n > syncMax) continue;
            std::cerr << "[" << withCommas(r.n) << " docs] save / load / re-sync...\n";
            r.sync = benchSync(corpus, r.n);
        }
    }

    const std::string text = report(results, corpus, seed, queryCount);
    std::cout << "\n" << text;

    if (!outFile.empty()) {
        std::error_code ec;
        const fs::path p(outFile);
        if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
        std::ofstream out(p, std::ios::binary | std::ios::trunc);
        out << text;
        if (!out) {
            std::cerr << "Could not write " << outFile << "\n";
            return 1;
        }
        std::cerr << "Report written to " << outFile << "\n";
    }
    return 0;
}
