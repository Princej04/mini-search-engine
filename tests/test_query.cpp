#include <gtest/gtest.h>

#include <cstdint>
#include <initializer_list>
#include <vector>

#include "index.h"
#include "query.h"

namespace {

using Ids = std::vector<uint32_t>;

Ids ids(std::initializer_list<uint32_t> l) { return Ids(l); }

// doc 0: the quick brown fox jumps over the lazy dog
// doc 1: the lazy cat sleeps all day
// doc 2: quick brown dogs and quick cats
// doc 3: a fox and a dog became friends
ms::InvertedIndex makeIndex() {
    ms::InvertedIndex idx;
    idx.addDocument("d0", "the quick brown fox jumps over the lazy dog");
    idx.addDocument("d1", "the lazy cat sleeps all day");
    idx.addDocument("d2", "quick brown dogs and quick cats");
    idx.addDocument("d3", "a fox and a dog became friends");
    return idx;
}

Ids match(const ms::InvertedIndex& idx, const std::string& q) {
    return ms::booleanMatch(idx, q);
}

}  // namespace

// ---- phrase search ----

TEST(Phrase, ConsecutiveWordsMatch) {
    const auto idx = makeIndex();
    EXPECT_EQ(match(idx, "\"quick brown\""), ids({0, 2}));
    EXPECT_EQ(match(idx, "\"lazy dog\""), ids({0}));
    EXPECT_EQ(match(idx, "\"lazy cat\""), ids({1}));
}

TEST(Phrase, WordOrderMatters) {
    const auto idx = makeIndex();
    EXPECT_TRUE(match(idx, "\"brown quick\"").empty());
    EXPECT_TRUE(match(idx, "\"dog lazy\"").empty());
}

TEST(Phrase, WordsMustBeAdjacent) {
    const auto idx = makeIndex();
    // doc 0 has both words, but "fox" and "dog" are far apart.
    EXPECT_TRUE(match(idx, "\"fox dog\"").empty());
}

TEST(Phrase, StopWordsAreKeptInPhrases) {
    const auto idx = makeIndex();
    EXPECT_EQ(match(idx, "\"the lazy\""), ids({0, 1}));
    EXPECT_EQ(match(idx, "\"over the lazy dog\""), ids({0}));
}

TEST(Phrase, LongerPhraseAndMissingWord) {
    const auto idx = makeIndex();
    EXPECT_EQ(match(idx, "\"the quick brown fox\""), ids({0}));
    EXPECT_TRUE(match(idx, "\"quick purple\"").empty());
}

TEST(Phrase, RepeatedWordInPhrase) {
    ms::InvertedIndex idx;
    idx.addDocument("a", "cat cat dog");
    idx.addDocument("b", "cat dog cat");
    EXPECT_EQ(idx.phraseMatch({"cat", "cat"}), ids({0}));
    EXPECT_EQ(idx.phraseMatch({"dog", "cat"}), ids({1}));
    EXPECT_TRUE(idx.phraseMatch({"cat", "cat", "cat"}).empty());
}

TEST(Phrase, EmptyPhraseMatchesNothing) {
    const auto idx = makeIndex();
    EXPECT_TRUE(idx.phraseMatch({}).empty());
}

// ---- Boolean queries ----

TEST(Boolean, SingleWord) {
    const auto idx = makeIndex();
    EXPECT_EQ(match(idx, "fox"), ids({0, 3}));
    EXPECT_TRUE(match(idx, "zebra").empty());
}

TEST(Boolean, And) {
    const auto idx = makeIndex();
    EXPECT_EQ(match(idx, "fox AND dog"), ids({0, 3}));
    EXPECT_EQ(match(idx, "lazy AND dog"), ids({0}));
}

TEST(Boolean, ImplicitAnd) {
    const auto idx = makeIndex();
    EXPECT_EQ(match(idx, "fox dog"), ids({0, 3}));
    EXPECT_TRUE(match(idx, "fox cat").empty());
}

TEST(Boolean, Or) {
    const auto idx = makeIndex();
    EXPECT_EQ(match(idx, "fox OR cat"), ids({0, 1, 3}));
}

TEST(Boolean, Not) {
    const auto idx = makeIndex();
    EXPECT_EQ(match(idx, "lazy AND NOT cat"), ids({0}));
    EXPECT_EQ(match(idx, "NOT lazy"), ids({2, 3}));
    EXPECT_EQ(match(idx, "NOT NOT lazy"), ids({0, 1}));
}

TEST(Boolean, ParenthesesAndPrecedence) {
    const auto idx = makeIndex();
    EXPECT_EQ(match(idx, "(fox OR cat) AND lazy"), ids({0, 1}));
    // AND binds tighter than OR: fox OR (cat AND lazy)
    EXPECT_EQ(match(idx, "fox OR cat AND lazy"), ids({0, 1, 3}));
    EXPECT_EQ(match(idx, "NOT (fox OR cat)"), ids({2}));
}

TEST(Boolean, PhraseCombinedWithOperators) {
    const auto idx = makeIndex();
    EXPECT_EQ(match(idx, "\"quick brown\" AND NOT dog"), ids({2}));
    EXPECT_EQ(match(idx, "\"quick brown\" OR \"became friends\""), ids({0, 2, 3}));
}

TEST(Boolean, HyphenatedWordBecomesPhrase) {
    ms::InvertedIndex idx;
    idx.addDocument("a", "a well-known fact");
    idx.addDocument("b", "known well");
    EXPECT_EQ(match(idx, "well-known"), ids({0}));
}

TEST(Boolean, MalformedQueriesThrow) {
    const auto idx = makeIndex();
    EXPECT_THROW(match(idx, "fox AND"), ms::QueryError);
    EXPECT_THROW(match(idx, "AND fox"), ms::QueryError);
    EXPECT_THROW(match(idx, "fox OR OR cat"), ms::QueryError);
    EXPECT_THROW(match(idx, "(fox"), ms::QueryError);
    EXPECT_THROW(match(idx, "fox)"), ms::QueryError);
    EXPECT_THROW(match(idx, "\"unclosed"), ms::QueryError);
    EXPECT_THROW(match(idx, "NOT"), ms::QueryError);
    EXPECT_THROW(match(idx, "()"), ms::QueryError);
}

TEST(Boolean, LooksBoolean) {
    EXPECT_FALSE(ms::looksBoolean("cat dog"));
    EXPECT_FALSE(ms::looksBoolean("and or not"));  // lowercase are ordinary words
    EXPECT_TRUE(ms::looksBoolean("cat AND dog"));
    EXPECT_TRUE(ms::looksBoolean("\"lazy dog\""));
    EXPECT_TRUE(ms::looksBoolean("(cat)"));
    EXPECT_TRUE(ms::looksBoolean("NOT cat"));
}

// ---- runQuery: ranking on top of matching ----

TEST(RunQuery, PlainQueryStillMatchesAnyWord) {
    const auto idx = makeIndex();
    const auto results = ms::runQuery(idx, "fox cat");
    // fox: docs 0, 3   cat: doc 1   -> three documents, not zero
    EXPECT_EQ(results.size(), 3u);
}

TEST(RunQuery, BooleanResultsAreOnlyMatchingDocuments) {
    const auto idx = makeIndex();
    const auto results = ms::runQuery(idx, "quick AND brown");
    ASSERT_EQ(results.size(), 2u);
    // Doc 2 mentions "quick" twice in a shorter text, so BM25 ranks it first.
    EXPECT_EQ(results[0].docId, 2u);
    EXPECT_EQ(results[1].docId, 0u);
    EXPECT_GT(results[0].score, results[1].score);
}

TEST(RunQuery, NegatedWordsDoNotAffectScore) {
    const auto idx = makeIndex();
    const auto withNot = ms::runQuery(idx, "lazy AND NOT cat");
    const auto plain = ms::runQuery(idx, "lazy AND dog");
    ASSERT_EQ(withNot.size(), 1u);
    ASSERT_EQ(plain.size(), 1u);
    EXPECT_EQ(withNot[0].docId, 0u);
    // Both rank doc 0 by "lazy" alone vs "lazy"+"dog": the NOT query has the lower score.
    EXPECT_GT(withNot[0].score, 0.0);
    EXPECT_LT(withNot[0].score, plain[0].score);
}

TEST(RunQuery, PureNotQueryReturnsMatchesWithZeroScore) {
    const auto idx = makeIndex();
    const auto results = ms::runQuery(idx, "NOT lazy");
    ASSERT_EQ(results.size(), 2u);
    EXPECT_EQ(results[0].docId, 2u);
    EXPECT_EQ(results[1].docId, 3u);
}

TEST(RunQuery, TopKLimitsBooleanResults) {
    ms::InvertedIndex idx;
    for (int i = 0; i < 20; ++i) idx.addDocument("d" + std::to_string(i), "apple pie");
    EXPECT_EQ(ms::runQuery(idx, "apple AND pie", 5).size(), 5u);
}

TEST(RunQuery, MalformedBooleanQueryThrows) {
    const auto idx = makeIndex();
    EXPECT_THROW(ms::runQuery(idx, "fox AND"), ms::QueryError);
}
