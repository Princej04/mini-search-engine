#include <gtest/gtest.h>

#include <string>

#include "index.h"

namespace {

std::string repeat(const std::string& word, int n) {
    std::string s;
    for (int i = 0; i < n; ++i) s += word + " ";
    return s;
}

double scoreOf(const ms::InvertedIndex& idx, const std::string& query,
               const std::string& docName, ms::RankMode mode) {
    for (const auto& r : idx.search(query, 100, mode)) {
        if (idx.docName(r.docId) == docName) return r.score;
    }
    return 0.0;
}

}  // namespace

TEST(BM25, AverageDocumentLength) {
    ms::InvertedIndex idx;
    idx.addDocument("a", "one two");           // 2 words
    idx.addDocument("b", "one two three four");  // 4 words
    EXPECT_DOUBLE_EQ(idx.avgDocLength(), 3.0);
}

TEST(BM25, RepeatedWordsSaturate) {
    // Two documents of equal length (100 words): one with "cat" 10 times,
    // one with "cat" 50 times. TF-IDF scores the second exactly 5x higher.
    // BM25 must score it far less than 5x higher (diminishing returns).
    ms::InvertedIndex idx;
    idx.addDocument("some", repeat("cat", 10) + repeat("filler", 90));
    idx.addDocument("many", repeat("cat", 50) + repeat("filler", 50));
    idx.addDocument("other", "dog");

    const double bm25Some = scoreOf(idx, "cat", "some", ms::RankMode::BM25);
    const double bm25Many = scoreOf(idx, "cat", "many", ms::RankMode::BM25);
    const double tfidfSome = scoreOf(idx, "cat", "some", ms::RankMode::TfIdf);
    const double tfidfMany = scoreOf(idx, "cat", "many", ms::RankMode::TfIdf);

    EXPECT_GT(bm25Many, bm25Some);              // more mentions still helps
    EXPECT_LT(bm25Many / bm25Some, 2.0);        // but not 5x
    EXPECT_NEAR(tfidfMany / tfidfSome, 5.0, 1e-9);
}

TEST(BM25, ShorterDocumentWinsWithSameTermFrequency) {
    // Both documents mention "cat" once; the short one is more focused.
    ms::InvertedIndex idx;
    idx.addDocument("long", "cat " + repeat("filler", 50));
    idx.addDocument("short", "cat");
    idx.addDocument("other", "dog");

    const auto results = idx.search("cat");
    ASSERT_EQ(results.size(), 2u);
    EXPECT_EQ(idx.docName(results[0].docId), "short");
}

TEST(BM25, RareTermsStillOutweighCommonTerms) {
    ms::InvertedIndex idx;
    idx.addDocument("d0", "common common word");
    idx.addDocument("d1", "common word");
    idx.addDocument("d2", "common rare");
    const auto results = idx.search("common rare");
    ASSERT_FALSE(results.empty());
    EXPECT_EQ(idx.docName(results[0].docId), "d2");
}

TEST(BM25, ScoresArePositiveEvenForTermInEveryDocument) {
    ms::InvertedIndex idx;
    idx.addDocument("a", "same words here");
    idx.addDocument("b", "same words there");
    for (const auto& r : idx.search("same")) EXPECT_GT(r.score, 0.0);
}