#include "tokenizer.h"

#include <cctype>
#include <unordered_set>

namespace ms {

std::vector<std::string> tokenize(const std::string& text) {
    std::vector<std::string> tokens;
    std::string current;
    for (unsigned char c : text) {
        if (std::isalnum(c)) {
            current.push_back(static_cast<char>(std::tolower(c)));
        } else if (!current.empty()) {
            tokens.push_back(std::move(current));
            current.clear();
        }
    }
    if (!current.empty()) tokens.push_back(std::move(current));
    return tokens;
}

bool isStopWord(const std::string& word) {
    static const std::unordered_set<std::string> kStopWords = {
        "a", "an", "and", "are", "as", "at", "be", "by", "for", "from",
        "has", "he", "in", "is", "it", "its", "of", "on", "that", "the",
        "to", "was", "were", "will", "with"};
    return kStopWords.count(word) > 0;
}

}  // namespace ms
