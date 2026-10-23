#include <gtest/gtest.h>

#include "index.h"

TEST(Index, BuildsPostingsWithPositions) {
    ms::InvertedIndex idx;
    idx.addDocument("a", "the cat sat on the mat");
    const auto* plist = idx.postings("the");
    ASSERT_NE(plist, nullptr);
    ASSERT_EQ(plist->size(), 1u);
    const std::vector<uint32_t> expected = {0, 4};
    EXPECT_EQ((*plist)[0].positions, expected);
    EXPECT_EQ(idx.docLength(0), 6u);
}

TEST(Index, UnknownTermHasNoPostings) {
    ms::InvertedIndex idx;
    idx.addDocument("a", "hello world");
    EXPECT_EQ(idx.postings("missing"), nullptr);
}

TEST(Index, RanksMoreRelevantDocumentFirst) {
    ms::InvertedIndex idx;
    idx.addDocument("none", "dogs and birds");
    idx.addDocument("once", "a cat and a dog");
    idx.addDocument("often", "cat cat cat story");
    const auto results = idx.search("cat");
    ASSERT_EQ(results.size(), 2u);
    EXPECT_EQ(idx.docName(results[0].docId), "often");
    EXPECT_EQ(idx.docName(results[1].docId), "once");
}

TEST(Index, RareTermsOutweighCommonTerms) {
    ms::InvertedIndex idx;
    idx.addDocument("d0", "common common word");
    idx.addDocument("d1", "common word");
    idx.addDocument("d2", "common rare");
    const auto results = idx.search("common rare");
    ASSERT_FALSE(results.empty());
    EXPECT_EQ(idx.docName(results[0].docId), "d2");
}

TEST(Index, TopKLimitsResults) {
    ms::InvertedIndex idx;
    for (int i = 0; i < 20; ++i) idx.addDocument("d" + std::to_string(i), "apple");
    EXPECT_EQ(idx.search("apple", 5).size(), 5u);
}
