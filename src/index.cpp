#include "index.h"

#include <algorithm>
#include <cmath>

#include "tokenizer.h"

namespace ms {

uint32_t InvertedIndex::addDocument(const std::string& name, const std::string& text) {
    const uint32_t docId = static_cast<uint32_t>(docNames_.size());
    docNames_.push_back(name);

    const std::vector<std::string> tokens = tokenize(text);
    docLengths_.push_back(static_cast<uint32_t>(tokens.size()));
    totalLength_ += tokens.size();

    // Collect positions per term for this document first, then append one
    // Posting per term. Doc ids increase, so posting lists stay sorted.
    std::unordered_map<std::string, std::vector<uint32_t>> local;
    for (uint32_t pos = 0; pos < tokens.size(); ++pos) {
        local[tokens[pos]].push_back(pos);
    }
    for (auto& [term, positions] : local) {
        index_[term].push_back(Posting{docId, std::move(positions)});
    }
    return docId;
}

const std::vector<Posting>* InvertedIndex::postings(const std::string& term) const {
    auto it = index_.find(term);
    return it == index_.end() ? nullptr : &it->second;
}

std::vector<SearchResult> InvertedIndex::search(const std::string& query, size_t topK,
                                                RankMode mode) const {
    std::unordered_map<uint32_t, double> scores;
    const double N = static_cast<double>(numDocs());
    const double avgdl = avgDocLength();

    for (const std::string& term : tokenize(query)) {
        const auto* plist = postings(term);
        if (!plist) continue;
        const double df = static_cast<double>(plist->size());

        if (mode == RankMode::TfIdf) {
            const double idf = std::log(1.0 + N / df);  // rare terms weigh more
            for (const Posting& p : *plist) {
                const double tf = static_cast<double>(p.positions.size());
                scores[p.docId] += tf * idf;
            }
        } else {
            // BM25: idf * tf*(k1+1) / (tf + k1*(1 - b + b*dl/avgdl))
            // The "+1" inside the log keeps idf positive even for very common terms.
            const double idf = std::log(1.0 + (N - df + 0.5) / (df + 0.5));
            for (const Posting& p : *plist) {
                const double tf = static_cast<double>(p.positions.size());
                const double dl = static_cast<double>(docLengths_[p.docId]);
                const double norm = 1.0 - bm25_.b + bm25_.b * dl / avgdl;
                scores[p.docId] += idf * tf * (bm25_.k1 + 1.0) / (tf + bm25_.k1 * norm);
            }
        }
    }

    std::vector<SearchResult> results;
    results.reserve(scores.size());
    for (const auto& [docId, score] : scores) results.push_back({docId, score});

    auto better = [](const SearchResult& a, const SearchResult& b) {
        return a.score != b.score ? a.score > b.score : a.docId < b.docId;
    };
    if (results.size() > topK) {
        std::partial_sort(results.begin(), results.begin() + topK, results.end(), better);
        results.resize(topK);
    } else {
        std::sort(results.begin(), results.end(), better);
    }
    return results;
}

}  // namespace ms