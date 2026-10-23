#include <gtest/gtest.h>

#include "tokenizer.h"

TEST(Tokenizer, LowercasesAndSplitsOnPunctuation) {
    const std::vector<std::string> expected = {"hello", "world", "2024"};
    EXPECT_EQ(ms::tokenize("Hello, World-2024!"), expected);
}

TEST(Tokenizer, EmptyAndSymbolOnlyInput) {
    EXPECT_TRUE(ms::tokenize("").empty());
    EXPECT_TRUE(ms::tokenize("  ... ,, !! ").empty());
}

TEST(Tokenizer, StopWords) {
    EXPECT_TRUE(ms::isStopWord("the"));
    EXPECT_FALSE(ms::isStopWord("search"));
}
