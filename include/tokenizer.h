#pragma once
#include <string>
#include <vector>

namespace ms {

// Lowercases the text and splits it on any non-alphanumeric character.
// "Hello, World-2024!" -> {"hello", "world", "2024"}
// Stop words are kept by default so word positions stay accurate for
// phrase search later. Call isStopWord() if you want to filter them.
std::vector<std::string> tokenize(const std::string& text);

bool isStopWord(const std::string& word);

}  // namespace ms
