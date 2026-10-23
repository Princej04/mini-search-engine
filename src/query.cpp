#include "query.h"

#include <algorithm>
#include <cctype>
#include <iterator>

#include "tokenizer.h"

namespace ms {

namespace {

enum class Tok { LParen, RParen, And, Or, Not, Phrase, Word };

struct Token {
    Tok type;
    std::string text;  // for Phrase and Word
};

std::vector<Token> lex(const std::string& q) {
    std::vector<Token> toks;
    size_t i = 0;
    while (i < q.size()) {
        const char c = q[i];
        if (std::isspace(static_cast<unsigned char>(c))) {
            ++i;
        } else if (c == '(') {
            toks.push_back({Tok::LParen, ""});
            ++i;
        } else if (c == ')') {
            toks.push_back({Tok::RParen, ""});
            ++i;
        } else if (c == '"') {
            const size_t end = q.find('"', i + 1);
            if (end == std::string::npos) throw QueryError("Missing closing quote");
            toks.push_back({Tok::Phrase, q.substr(i + 1, end - i - 1)});
            i = end + 1;
        } else {
            size_t j = i;
            while (j < q.size() && !std::isspace(static_cast<unsigned char>(q[j])) &&
                   q[j] != '(' && q[j] != ')' && q[j] != '"') {
                ++j;
            }
            const std::string word = q.substr(i, j - i);
            if (word == "AND") toks.push_back({Tok::And, ""});
            else if (word == "OR") toks.push_back({Tok::Or, ""});
            else if (word == "NOT") toks.push_back({Tok::Not, ""});
            else toks.push_back({Tok::Word, word});
            i = j;
        }
    }
    return toks;
}

using Ids = std::vector<uint32_t>;

Ids intersect(const Ids& a, const Ids& b) {
    Ids out;
    std::set_intersection(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(out));
    return out;
}
Ids unite(const Ids& a, const Ids& b) {
    Ids out;
    std::set_union(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(out));
    return out;
}
Ids subtract(const Ids& a, const Ids& b) {
    Ids out;
    std::set_difference(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(out));
    return out;
}

// Recursive-descent parser that evaluates as it parses. Each function
// returns the sorted list of matching document ids for its piece of the query.
//
//   or      := and ( "OR" and )*
//   and     := unary ( ["AND"] unary )*      (AND may be left out)
//   unary   := "NOT" unary | primary
//   primary := "(" or ")" | "phrase" | word
class Parser {
public:
    Parser(const InvertedIndex& index, std::vector<Token> toks)
        : index_(index), toks_(std::move(toks)) {}

    Ids parse() {
        if (toks_.empty()) throw QueryError("Empty query");
        Ids result = parseOr();
        if (pos_ != toks_.size()) throw QueryError("Unexpected " + describe(toks_[pos_]));
        return result;
    }

    // Words that appear outside any NOT; used for ranking the matches.
    std::vector<std::string> positiveTerms() const {
        std::vector<std::string> t = positive_;
        std::sort(t.begin(), t.end());
        t.erase(std::unique(t.begin(), t.end()), t.end());
        return t;
    }

private:
    bool peekIs(Tok t) const { return pos_ < toks_.size() && toks_[pos_].type == t; }

    bool startsOperand() const {
        return peekIs(Tok::Not) || peekIs(Tok::LParen) || peekIs(Tok::Phrase) ||
               peekIs(Tok::Word);
    }

    static std::string describe(const Token& t) {
        switch (t.type) {
            case Tok::LParen: return "'('";
            case Tok::RParen: return "')'";
            case Tok::And: return "AND";
            case Tok::Or: return "OR";
            case Tok::Not: return "NOT";
            default: return "'" + t.text + "'";
        }
    }

    Ids parseOr() {
        Ids result = parseAnd();
        while (peekIs(Tok::Or)) {
            ++pos_;
            result = unite(result, parseAnd());
        }
        return result;
    }

    Ids parseAnd() {
        Ids result = parseUnary();
        while (true) {
            if (peekIs(Tok::And)) {
                ++pos_;
                result = intersect(result, parseUnary());
            } else if (startsOperand()) {
                result = intersect(result, parseUnary());  // implicit AND
            } else {
                break;
            }
        }
        return result;
    }

    Ids parseUnary() {
        if (peekIs(Tok::Not)) {
            ++pos_;
            negated_ = !negated_;  // words under NOT should not boost ranking
            Ids inner = parseUnary();
            negated_ = !negated_;
            return subtract(index_.allDocIds(), inner);
        }
        return parsePrimary();
    }

    Ids parsePrimary() {
        if (pos_ >= toks_.size()) throw QueryError("Query ended unexpectedly");
        const Token& t = toks_[pos_];

        if (t.type == Tok::LParen) {
            ++pos_;
            Ids inner = parseOr();
            if (!peekIs(Tok::RParen)) throw QueryError("Missing closing parenthesis");
            ++pos_;
            return inner;
        }
        if (t.type == Tok::Phrase || t.type == Tok::Word) {
            ++pos_;
            // "dog-cat" or a quoted phrase becomes several tokens -> phrase match.
            const std::vector<std::string> terms = tokenize(t.text);
            if (!negated_) positive_.insert(positive_.end(), terms.begin(), terms.end());
            return index_.phraseMatch(terms);
        }
        throw QueryError("Unexpected " + describe(t));
    }

    const InvertedIndex& index_;
    std::vector<Token> toks_;
    size_t pos_ = 0;
    bool negated_ = false;
    std::vector<std::string> positive_;
};

}  // namespace

bool looksBoolean(const std::string& query) {
    for (const Token& t : lex(query)) {
        if (t.type != Tok::Word) return true;  // operator, quote or parenthesis
    }
    return false;
}

std::vector<uint32_t> booleanMatch(const InvertedIndex& index, const std::string& query) {
    Parser parser(index, lex(query));
    return parser.parse();
}

std::vector<SearchResult> runQuery(const InvertedIndex& index, const std::string& query,
                                   size_t topK, RankMode mode) {
    if (!looksBoolean(query)) return index.search(query, topK, mode);

    Parser parser(index, lex(query));
    const Ids matches = parser.parse();
    return index.rankDocs(parser.positiveTerms(), matches, topK, mode);
}

}  // namespace ms
