#pragma once
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "index.h"

namespace ms {

// Thrown for malformed queries, e.g. "fox AND" or "(fox OR cat".
struct QueryError : std::invalid_argument {
    using std::invalid_argument::invalid_argument;
};

// Query syntax
//   word                a single word
//   "two words"         phrase: the words must appear next to each other, in order
//   a AND b             both       (writing a b with nothing between also means AND)
//   a OR b              either
//   NOT a               documents that do not contain a
//   ( ... )             grouping
// Operators must be UPPERCASE. Precedence: NOT, then AND, then OR.

// True if the query uses any operator, quote or parenthesis.
bool looksBoolean(const std::string& query);

// Ids (ascending) of the documents that satisfy a Boolean query.
// Throws QueryError if the query is malformed.
std::vector<uint32_t> booleanMatch(const InvertedIndex& index, const std::string& query);

// Main entry point for the CLI.
//  - Plain query ("cat dog"):  ranked search, any word may match (as before).
//  - Boolean query:            documents must satisfy the expression; the
//                              survivors are ranked by the words that are not negated.
// Throws QueryError if a Boolean query is malformed.
std::vector<SearchResult> runQuery(const InvertedIndex& index, const std::string& query,
                                   size_t topK = 10, RankMode mode = RankMode::BM25);

}  // namespace ms
