#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace ms {

// One entry in a term's posting list: which document, and where in it.
struct Posting {
    uint32_t docId;
    std::vector<uint32_t> positions;  // word offsets; tf = positions.size()
};

struct SearchResult {
    uint32_t docId;
    double score;
};

class InvertedIndex {
public:
    // Indexes one document and returns its id.
    uint32_t addDocument(const std::string& name, const std::string& text);

    size_t numDocs() const { return docNames_.size(); }
    size_t numTerms() const { return index_.size(); }
    const std::string& docName(uint32_t docId) const { return docNames_.at(docId); }
    uint32_t docLength(uint32_t docId) const { return docLengths_.at(docId); }

    // Posting list for a term, or nullptr if the term is not indexed.
    const std::vector<Posting>* postings(const std::string& term) const;

    // Ranked search (TF-IDF for now; BM25 is the next step).
    // A document matches if it contains at least one query term.
    std::vector<SearchResult> search(const std::string& query, size_t topK = 10) const;

private:
    std::unordered_map<std::string, std::vector<Posting>> index_;
    std::vector<std::string> docNames_;
    std::vector<uint32_t> docLengths_;  // number of tokens per document
};

}  // namespace ms
