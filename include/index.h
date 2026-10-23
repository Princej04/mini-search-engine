#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace ms {

// One entry in a term's posting list: which document, and where in it.
struct Posting {
    uint32_t docId;
    std::vector<uint32_t> positions;  // word offsets, ascending; tf = positions.size()
};

struct SearchResult {
    uint32_t docId;
    double score;
};

enum class RankMode { TfIdf, BM25 };

// Standard BM25 tuning constants.
struct Bm25Params {
    double k1 = 1.2;   // how quickly repeated words stop adding score (saturation)
    double b = 0.75;   // how strongly long documents are penalized (0 = not at all, 1 = fully)
};

class InvertedIndex {
public:
    // Indexes one document and returns its id.
    uint32_t addDocument(const std::string& name, const std::string& text);

    size_t numDocs() const { return docNames_.size(); }
    size_t numTerms() const { return index_.size(); }
    const std::string& docName(uint32_t docId) const { return docNames_.at(docId); }
    uint32_t docLength(uint32_t docId) const { return docLengths_.at(docId); }
    double avgDocLength() const {
        return docNames_.empty() ? 0.0 : static_cast<double>(totalLength_) / docNames_.size();
    }

    // Posting list for a term, or nullptr if the term is not indexed.
    const std::vector<Posting>* postings(const std::string& term) const;

    // Ranked search. A document matches if it contains at least one query term.
    // BM25 is the default; TfIdf is kept so the two can be compared.
    std::vector<SearchResult> search(const std::string& query, size_t topK = 10,
                                     RankMode mode = RankMode::BM25) const;

    // Phrase search: ids (ascending) of documents where `terms` appear as
    // consecutive words, in order. Uses the stored word positions.
    // One term = every document containing it. No terms = no documents.
    std::vector<uint32_t> phraseMatch(const std::vector<std::string>& terms) const;

    // All document ids, ascending.
    std::vector<uint32_t> allDocIds() const;

    // Scores only the given candidate documents against `terms` and returns
    // the best `topK`. Candidates that contain none of the terms get score 0.
    std::vector<SearchResult> rankDocs(const std::vector<std::string>& terms,
                                       const std::vector<uint32_t>& candidates,
                                       size_t topK = 10,
                                       RankMode mode = RankMode::BM25) const;

    void setBm25Params(const Bm25Params& p) { bm25_ = p; }

private:
    // docId -> score summed over all `terms`.
    std::unordered_map<uint32_t, double> scoreTerms(const std::vector<std::string>& terms,
                                                    RankMode mode) const;
    static void keepTopK(std::vector<SearchResult>& results, size_t topK);

    std::unordered_map<std::string, std::vector<Posting>> index_;
    std::vector<std::string> docNames_;
    std::vector<uint32_t> docLengths_;  // number of tokens per document
    uint64_t totalLength_ = 0;          // sum of all document lengths
    Bm25Params bm25_;
};

}  // namespace ms
