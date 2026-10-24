#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "corpus.h"
#include "index.h"
#include "query.h"

namespace fs = std::filesystem;

namespace {

// A scratch folder that is wiped before and after each test.
struct TempDir {
    fs::path path;
    explicit TempDir(const std::string& name)
        : path(fs::temp_directory_path() / ("ms_test_" + name)) {
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

void writeFile(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << text;
}

std::string readAll(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::map<std::string, double> scoresByName(const ms::InvertedIndex& idx, const std::string& q,
                                           ms::RankMode mode) {
    std::map<std::string, double> out;
    for (const auto& r : idx.search(q, 1000, mode)) out[idx.docName(r.docId)] = r.score;
    return out;
}

void expectSameScores(const std::map<std::string, double>& a,
                      const std::map<std::string, double>& b) {
    EXPECT_EQ(a.size(), b.size());
    for (const auto& kv : a) {
        const auto it = b.find(kv.first);
        if (it == b.end()) {
            EXPECT_TRUE(false);
            continue;
        }
        EXPECT_NEAR(kv.second, it->second, 1e-12);
    }
}

}  // namespace

// ---- removing and replacing documents ----

TEST(Remove, HidesDocumentFromEveryKindOfQuery) {
    ms::InvertedIndex idx;
    idx.addDocument("a", "the quick brown fox");
    idx.addDocument("b", "the quick brown dog");
    ASSERT_TRUE(idx.removeDocument("a"));

    EXPECT_EQ(idx.numDocs(), 1u);
    EXPECT_FALSE(idx.findDoc("a").has_value());
    EXPECT_FALSE(idx.isAlive(0));

    EXPECT_TRUE(idx.search("fox").empty());                   // ranked
    EXPECT_EQ(idx.search("quick").size(), 1u);
    EXPECT_TRUE(ms::runQuery(idx, "\"brown fox\"").empty());  // phrase
    EXPECT_EQ(ms::runQuery(idx, "\"quick brown\"").size(), 1u);
    EXPECT_EQ(ms::runQuery(idx, "NOT dog").size(), 0u);       // NOT only sees live docs
    EXPECT_EQ(ms::runQuery(idx, "quick AND NOT fox").size(), 1u);
}

TEST(Remove, UnknownNameReturnsFalse) {
    ms::InvertedIndex idx;
    idx.addDocument("a", "hello");
    EXPECT_FALSE(idx.removeDocument("nope"));
    EXPECT_EQ(idx.numDocs(), 1u);
}

TEST(Remove, OtherDocumentIdsStayStable) {
    ms::InvertedIndex idx;
    idx.addDocument("a", "one");
    idx.addDocument("b", "two");
    idx.addDocument("c", "three");
    idx.removeDocument("b");
    EXPECT_EQ(idx.docName(2), "c");
    EXPECT_EQ(*idx.findDoc("c"), 2u);
    EXPECT_EQ(idx.addDocument("d", "four"), 3u);  // ids are never reused
    EXPECT_EQ(idx.docName(1), "");                // removed slot is empty
}

TEST(Remove, ScoresMatchAFreshlyBuiltIndex) {
    // Removing a document must leave idf and average length exactly as if the
    // document had never been indexed. Checked before and after compaction.
    const std::vector<std::pair<std::string, std::string>> docs = {
        {"a", "the cat sat on the mat"},
        {"b", "dogs chase cats and cats chase mice"},
        {"c", "a cat and a dog and a bird"},
        {"d", "birds sing in the morning"}};

    ms::InvertedIndex removed;
    for (const auto& d : docs) removed.addDocument(d.first, d.second);
    removed.removeDocument("b");

    ms::InvertedIndex fresh;
    for (const auto& d : docs) {
        if (d.first != "b") fresh.addDocument(d.first, d.second);
    }

    const std::vector<std::string> queries = {"cat", "cat dog bird", "chase", "the"};
    const std::vector<ms::RankMode> modes = {ms::RankMode::BM25, ms::RankMode::TfIdf};

    EXPECT_DOUBLE_EQ(removed.avgDocLength(), fresh.avgDocLength());
    for (const auto& q : queries) {
        for (auto mode : modes) {
            expectSameScores(scoresByName(removed, q, mode), scoresByName(fresh, q, mode));
        }
    }

    removed.compact();
    for (const auto& q : queries) {
        for (auto mode : modes) {
            expectSameScores(scoresByName(removed, q, mode), scoresByName(fresh, q, mode));
        }
    }
}

TEST(Replace, AddingSameNameReplacesTheDocument) {
    ms::InvertedIndex idx;
    const uint32_t first = idx.addDocument("a", "old words here");
    const uint32_t second = idx.addDocument("a", "new content now");
    EXPECT_NE(first, second);
    EXPECT_EQ(idx.numDocs(), 1u);
    EXPECT_TRUE(idx.search("old").empty());
    EXPECT_EQ(idx.search("new").size(), 1u);
    EXPECT_EQ(idx.docName(second), "a");
}

TEST(Replace, StampIsStoredAndReplaced) {
    ms::InvertedIndex idx;
    const uint32_t a = idx.addDocument("a", "hello", 111);
    EXPECT_EQ(idx.docStamp(a), 111u);
    const uint32_t a2 = idx.addDocument("a", "hello again", 222);
    EXPECT_EQ(idx.docStamp(a2), 222u);
}

TEST(Compact, DropsPostingsAndEmptyTerms) {
    ms::InvertedIndex idx;
    idx.addDocument("x", "unique1 shared");
    idx.addDocument("y", "shared");
    idx.removeDocument("x");

    EXPECT_EQ(idx.numRemoved(), 1u);
    EXPECT_TRUE(idx.search("unique1").empty());  // hidden even before compaction

    idx.compact();
    EXPECT_EQ(idx.numRemoved(), 0u);
    EXPECT_EQ(idx.postings("unique1"), nullptr);
    EXPECT_EQ(idx.numTerms(), 1u);
    EXPECT_EQ(idx.search("shared").size(), 1u);
}

TEST(Compact, RunsAutomaticallyWhenManyDocumentsAreRemoved) {
    ms::InvertedIndex idx;
    for (int i = 0; i < 100; ++i) {
        idx.addDocument("d" + std::to_string(i), "alpha beta u" + std::to_string(i));
    }
    for (int i = 0; i < 40; ++i) idx.removeDocument("d" + std::to_string(i));

    EXPECT_EQ(idx.numDocs(), 60u);
    EXPECT_LT(idx.numRemoved(), 40u);  // some compaction already happened
    EXPECT_EQ(idx.search("alpha", 1000).size(), 60u);
    EXPECT_TRUE(idx.search("u5").empty());
    EXPECT_EQ(idx.search("u50").size(), 1u);
}

TEST(Remove, EmptyingTheIndexIsSafeAndItCanBeReused) {
    ms::InvertedIndex idx;
    idx.addDocument("a", "some words");
    idx.removeDocument("a");
    EXPECT_EQ(idx.numDocs(), 0u);
    EXPECT_DOUBLE_EQ(idx.avgDocLength(), 0.0);
    EXPECT_TRUE(idx.search("some").empty());
    EXPECT_TRUE(ms::runQuery(idx, "NOT zebra").empty());

    idx.addDocument("b", "fresh words");
    EXPECT_EQ(idx.search("fresh").size(), 1u);
}

// ---- saving and loading ----

TEST(Persistence, RoundTripKeepsEverything) {
    TempDir dir("roundtrip");
    const std::string file = (dir.path / "index.bin").string();

    // A document longer than 128 words, so positions need multi-byte numbers.
    std::string longText;
    for (int i = 0; i < 300; ++i) longText += "filler ";
    longText += "needle needle";

    ms::InvertedIndex idx;
    idx.addDocument("a", "the quick brown fox jumps over the lazy dog", 11);
    idx.addDocument("b", "the lazy cat sleeps all day", 22);
    idx.addDocument("gone", "will be removed", 33);
    idx.addDocument("long", longText, 44);
    idx.addDocument("c", "quick brown dogs and quick cats", 55);
    idx.removeDocument("gone");
    idx.save(file);
    idx.compact();  // the saved file never contains removed documents

    const ms::InvertedIndex loaded = ms::InvertedIndex::load(file);

    EXPECT_EQ(loaded.numDocs(), idx.numDocs());
    EXPECT_EQ(loaded.numSlots(), idx.numSlots());
    EXPECT_EQ(loaded.numTerms(), idx.numTerms());
    EXPECT_DOUBLE_EQ(loaded.avgDocLength(), idx.avgDocLength());

    // Same ids, names, lengths and stamps for every slot.
    for (uint32_t id = 0; id < idx.numSlots(); ++id) {
        EXPECT_EQ(loaded.isAlive(id), idx.isAlive(id));
        EXPECT_EQ(loaded.docName(id), idx.docName(id));
        EXPECT_EQ(loaded.docLength(id), idx.docLength(id));
        EXPECT_EQ(loaded.docStamp(id), idx.docStamp(id));
    }

    // Same query results, including phrases and Boolean queries.
    const std::vector<std::string> queries = {"quick", "lazy cat", "filler", "will"};
    for (const auto& q : queries) {
        for (auto mode : {ms::RankMode::BM25, ms::RankMode::TfIdf}) {
            expectSameScores(scoresByName(idx, q, mode), scoresByName(loaded, q, mode));
        }
    }
    EXPECT_EQ(ms::booleanMatch(loaded, "\"quick brown\""), ms::booleanMatch(idx, "\"quick brown\""));
    EXPECT_EQ(ms::booleanMatch(loaded, "\"needle needle\""), ms::booleanMatch(idx, "\"needle needle\""));
    EXPECT_EQ(ms::booleanMatch(loaded, "\"needle needle\"").size(), 1u);
    EXPECT_EQ(ms::booleanMatch(loaded, "lazy AND NOT dog"), ms::booleanMatch(idx, "lazy AND NOT dog"));
}

TEST(Persistence, LoadedIndexKeepsWorkingIncrementally) {
    TempDir dir("continue");
    const std::string file = (dir.path / "index.bin").string();

    ms::InvertedIndex idx;
    idx.addDocument("a", "alpha beta");
    idx.addDocument("b", "beta gamma");
    idx.save(file);

    ms::InvertedIndex loaded = ms::InvertedIndex::load(file);
    EXPECT_EQ(loaded.addDocument("c", "gamma delta"), 2u);  // continues after the last id
    EXPECT_TRUE(loaded.removeDocument("a"));
    EXPECT_EQ(loaded.numDocs(), 2u);
    EXPECT_EQ(loaded.search("gamma").size(), 2u);
    EXPECT_TRUE(loaded.search("alpha").empty());
}

TEST(Persistence, SavingOverAnExistingFileWorks) {
    TempDir dir("overwrite");
    const std::string file = (dir.path / "index.bin").string();

    ms::InvertedIndex idx;
    idx.addDocument("a", "first version");
    idx.save(file);
    idx.addDocument("b", "second version");
    idx.save(file);

    const ms::InvertedIndex loaded = ms::InvertedIndex::load(file);
    EXPECT_EQ(loaded.numDocs(), 2u);
    EXPECT_FALSE(fs::exists(file + ".tmp"));
}

TEST(Persistence, EmptyIndexRoundTrips) {
    TempDir dir("empty");
    const std::string file = (dir.path / "index.bin").string();
    ms::InvertedIndex idx;
    idx.save(file);
    const ms::InvertedIndex loaded = ms::InvertedIndex::load(file);
    EXPECT_EQ(loaded.numDocs(), 0u);
    EXPECT_TRUE(loaded.search("anything").empty());
}

TEST(Persistence, MissingFileThrows) {
    TempDir dir("missing");
    EXPECT_THROW(ms::InvertedIndex::load((dir.path / "nope.bin").string()), std::runtime_error);
}

TEST(Persistence, GarbageFileThrows) {
    TempDir dir("garbage");
    const fs::path file = dir.path / "index.bin";
    writeFile(file, "this is definitely not an index file");
    EXPECT_THROW(ms::InvertedIndex::load(file.string()), std::runtime_error);
    writeFile(file, "");
    EXPECT_THROW(ms::InvertedIndex::load(file.string()), std::runtime_error);
}

TEST(Persistence, TruncatedFileThrows) {
    TempDir dir("truncated");
    const fs::path file = dir.path / "index.bin";

    ms::InvertedIndex idx;
    for (int i = 0; i < 20; ++i) {
        idx.addDocument("doc" + std::to_string(i), "some words repeat repeat and word" + std::to_string(i));
    }
    idx.save(file.string());

    const std::string full = readAll(file);
    ASSERT_TRUE(full.size() > 20);
    // Cutting the file at several points must always be detected, never crash.
    for (size_t cut : {full.size() - 1, full.size() / 2, size_t{10}, size_t{5}}) {
        writeFile(file, full.substr(0, cut));
        EXPECT_THROW(ms::InvertedIndex::load(file.string()), std::runtime_error);
    }
}

TEST(Persistence, CorruptedBytesNeverCrash) {
    TempDir dir("corrupt");
    const fs::path file = dir.path / "index.bin";

    ms::InvertedIndex idx;
    for (int i = 0; i < 10; ++i) {
        idx.addDocument("doc" + std::to_string(i), "alpha beta gamma alpha delta " + std::to_string(i));
    }
    idx.save(file.string());
    const std::string good = readAll(file);

    // Flip each byte in turn: loading must either succeed or throw
    // std::runtime_error, and must never crash or hang.
    for (size_t i = 0; i < good.size(); ++i) {
        std::string bad = good;
        bad[i] = static_cast<char>(bad[i] ^ 0xFF);
        writeFile(file, bad);
        try {
            const ms::InvertedIndex loaded = ms::InvertedIndex::load(file.string());
            loaded.search("alpha");
            ms::runQuery(loaded, "\"alpha beta\" OR NOT gamma");
        } catch (const std::runtime_error&) {
        }
    }
    EXPECT_TRUE(true);
}

// ---- syncing a folder ----

TEST(Sync, AddUpdateRemoveUnchanged) {
    TempDir dir("sync");
    writeFile(dir.path / "a.txt", "apples and oranges");
    writeFile(dir.path / "sub" / "b.txt", "bananas are yellow");
    writeFile(dir.path / "ignored.md", "not a text file");

    ms::InvertedIndex idx;
    auto s1 = ms::syncFolder(idx, dir.path);
    EXPECT_EQ(s1.added, 2u);
    EXPECT_EQ(s1.unchanged, 0u);
    EXPECT_EQ(idx.numDocs(), 2u);
    EXPECT_TRUE(idx.findDoc("a.txt").has_value());
    EXPECT_TRUE(idx.findDoc("sub/b.txt").has_value());  // relative, '/' separators
    EXPECT_TRUE(s1.changed());

    // Nothing changed: nothing is re-read.
    auto s2 = ms::syncFolder(idx, dir.path);
    EXPECT_EQ(s2.added, 0u);
    EXPECT_EQ(s2.updated, 0u);
    EXPECT_EQ(s2.removed, 0u);
    EXPECT_EQ(s2.unchanged, 2u);
    EXPECT_FALSE(s2.changed());

    // Change one file (different length), add one, delete one.
    writeFile(dir.path / "a.txt", "apples and oranges and also pears");
    writeFile(dir.path / "c.txt", "cherries");
    fs::remove(dir.path / "sub" / "b.txt");

    auto s3 = ms::syncFolder(idx, dir.path);
    EXPECT_EQ(s3.added, 1u);
    EXPECT_EQ(s3.updated, 1u);
    EXPECT_EQ(s3.removed, 1u);
    EXPECT_EQ(s3.unchanged, 0u);
    EXPECT_EQ(idx.numDocs(), 2u);

    EXPECT_EQ(idx.search("pears").size(), 1u);      // updated content is searchable
    EXPECT_EQ(idx.search("cherries").size(), 1u);   // new file is searchable
    EXPECT_TRUE(idx.search("bananas").empty());     // deleted file is gone
}

TEST(Sync, WorksOnTopOfASavedIndex) {
    TempDir dir("sync_saved");
    writeFile(dir.path / "docs" / "one.txt", "first document");
    writeFile(dir.path / "docs" / "two.txt", "second document");
    const std::string indexFile = (dir.path / "index.bin").string();

    {
        ms::InvertedIndex idx;
        ms::syncFolder(idx, dir.path / "docs");
        idx.save(indexFile);
    }

    writeFile(dir.path / "docs" / "three.txt", "third document");
    ms::InvertedIndex idx = ms::InvertedIndex::load(indexFile);
    const auto s = ms::syncFolder(idx, dir.path / "docs");
    EXPECT_EQ(s.added, 1u);
    EXPECT_EQ(s.unchanged, 2u);  // the two old files were not re-read
    EXPECT_EQ(idx.numDocs(), 3u);
    EXPECT_EQ(idx.search("document", 10).size(), 3u);
}

TEST(Sync, MissingFolderThrows) {
    TempDir dir("sync_missing");
    ms::InvertedIndex idx;
    EXPECT_THROW(ms::syncFolder(idx, dir.path / "does_not_exist"), std::runtime_error);
}
