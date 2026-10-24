#include "index.h"

#include <algorithm>
#include <cmath>

#include "tokenizer.h"

namespace ms {

namespace {
// Compact automatically once this many removals are pending AND they make up
// at least a quarter of all ids. Keeps memory bounded under heavy churn.
constexpr size_t kAutoCompactMinRemovals = 32;
}  // namespace

uint32_t InvertedIndex::addDocument(const std::string& name, const std::string& text,
                                    uint64_t stamp) {
    removeDocument(name);  // replace semantics; does nothing if the name is new

    const std::vector<std::string> tokens = tokenize(text);

    const uint32_t docId = static_cast<uint32_t>(docNames_.size());
    docNames_.push_back(name);
    docLengths_.push_back(static_cast<uint32_t>(tokens.size()));
    docStamps_.push_back(stamp);
    alive_.push_back(1);
    nameToId_.emplace(name, docId);
    ++liveDocs_;
    totalLength_ += tokens.size();

    // Collect positions per term for this document first, then append one
    // Posting per term. Doc ids only increase, so posting lists stay sorted.
    std::unordered_map<std::string, std::vector<uint32_t>> local;
    for (uint32_t pos = 0; pos < tokens.size(); ++pos) {
        local[tokens[pos]].push_back(pos);
    }
    for (auto& [term, positions] : local) {
        index_[term].push_back(Posting{docId, std::move(positions)});
    }
    return docId;
}

bool InvertedIndex::removeDocument(const std::string& name) {
    const auto it = nameToId_.find(name);
    if (it == nameToId_.end()) return false;

    // `name` may be a reference into docNames_, so finish using it first.
    const uint32_t id = it->second;
    nameToId_.erase(it);

    alive_[id] = 0;
    totalLength_ -= docLengths_[id];
    docLengths_[id] = 0;
    docStamps_[id] = 0;
    std::string().swap(docNames_[id]);  // free the name's memory
    --liveDocs_;
    ++pendingDead_;

    if (pendingDead_ >= kAutoCompactMinRemovals && pendingDead_ * 4 >= docNames_.size()) {
        compact();
    }
    return true;
}

void InvertedIndex::compact() {
    if (pendingDead_ == 0) return;
    for (auto it = index_.begin(); it != index_.end();) {
        auto& list = it->second;
        list.erase(std::remove_if(list.begin(), list.end(),
                                  [this](const Posting& p) { return alive_[p.docId] == 0; }),
                   list.end());
        if (list.empty()) {
            it = index_.erase(it);
        } else {
            list.shrink_to_fit();
            ++it;
        }
    }
    pendingDead_ = 0;
}

std::optional<uint32_t> InvertedIndex::findDoc(const std::string& name) const {
    const auto it = nameToId_.find(name);
    if (it == nameToId_.end()) return std::nullopt;
    return it->second;
}

const std::vector<Posting>* InvertedIndex::postings(const std::string& term) const {
    auto it = index_.find(term);
    return it == index_.end() ? nullptr : &it->second;
}

std::unordered_map<uint32_t, double> InvertedIndex::scoreTerms(
    const std::vector<std::string>& terms, RankMode mode) const {
    std::unordered_map<uint32_t, double> scores;
    const double N = static_cast<double>(liveDocs_);
    const double avgdl = avgDocLength();

    for (const std::string& term : terms) {
        const auto* plist = postings(term);
        if (!plist) continue;

        // Document frequency must only count live documents, otherwise
        // removed-but-not-yet-compacted postings would skew the idf.
        size_t liveDf = plist->size();
        if (pendingDead_ > 0) {
            liveDf = static_cast<size_t>(std::count_if(
                plist->begin(), plist->end(),
                [this](const Posting& p) { return alive_[p.docId] != 0; }));
        }
        if (liveDf == 0) continue;
        const double df = static_cast<double>(liveDf);

        if (mode == RankMode::TfIdf) {
            const double idf = std::log(1.0 + N / df);  // rare terms weigh more
            for (const Posting& p : *plist) {
                if (!alive_[p.docId]) continue;
                const double tf = static_cast<double>(p.positions.size());
                scores[p.docId] += tf * idf;
            }
        } else {
            // BM25: idf * tf*(k1+1) / (tf + k1*(1 - b + b*dl/avgdl))
            // The "+1" inside the log keeps idf positive even for very common terms.
            const double idf = std::log(1.0 + (N - df + 0.5) / (df + 0.5));
            for (const Posting& p : *plist) {
                if (!alive_[p.docId]) continue;
                const double tf = static_cast<double>(p.positions.size());
                const double dl = static_cast<double>(docLengths_[p.docId]);
                const double norm = 1.0 - bm25_.b + bm25_.b * dl / avgdl;
                scores[p.docId] += idf * tf * (bm25_.k1 + 1.0) / (tf + bm25_.k1 * norm);
            }
        }
    }
    return scores;
}

void InvertedIndex::keepTopK(std::vector<SearchResult>& results, size_t topK) {
    auto better = [](const SearchResult& a, const SearchResult& b) {
        return a.score != b.score ? a.score > b.score : a.docId < b.docId;
    };
    if (results.size() > topK) {
        std::partial_sort(results.begin(), results.begin() + topK, results.end(), better);
        results.resize(topK);
    } else {
        std::sort(results.begin(), results.end(), better);
    }
}

std::vector<SearchResult> InvertedIndex::search(const std::string& query, size_t topK,
                                                RankMode mode) const {
    const auto scores = scoreTerms(tokenize(query), mode);
    std::vector<SearchResult> results;
    results.reserve(scores.size());
    for (const auto& [docId, score] : scores) results.push_back({docId, score});
    keepTopK(results, topK);
    return results;
}

std::vector<uint32_t> InvertedIndex::phraseMatch(const std::vector<std::string>& terms) const {
    std::vector<uint32_t> out;
    if (terms.empty()) return out;

    std::vector<const std::vector<Posting>*> lists;
    for (const std::string& t : terms) {
        const auto* p = postings(t);
        if (!p) return out;  // a missing word means no phrase can match
        lists.push_back(p);
    }

    // Step 1: walk all posting lists in lockstep to find documents that
    // contain every word (sorted-list intersection).
    // Step 2: in each such document, check the positions are consecutive.
    std::vector<size_t> it(lists.size(), 0);
    while (true) {
        uint32_t target = 0;
        for (size_t i = 0; i < lists.size(); ++i) {
            if (it[i] >= lists[i]->size()) return out;
            target = std::max(target, (*lists[i])[it[i]].docId);
        }

        bool aligned = true;
        for (size_t i = 0; i < lists.size(); ++i) {
            while (it[i] < lists[i]->size() && (*lists[i])[it[i]].docId < target) ++it[i];
            if (it[i] >= lists[i]->size()) return out;
            if ((*lists[i])[it[i]].docId != target) aligned = false;
        }
        if (!aligned) continue;  // some list jumped past target; recompute it

        if (alive_[target]) {
            // Every word occurs in `target`. Does word 0 at position p
            // have word i at position p + i, for every i?
            const auto& firstPositions = (*lists[0])[it[0]].positions;
            for (uint32_t start : firstPositions) {
                bool ok = true;
                for (size_t i = 1; i < lists.size() && ok; ++i) {
                    const auto& pos = (*lists[i])[it[i]].positions;
                    ok = std::binary_search(pos.begin(), pos.end(),
                                            start + static_cast<uint32_t>(i));
                }
                if (ok) {
                    out.push_back(target);
                    break;
                }
            }
        }
        for (size_t i = 0; i < lists.size(); ++i) ++it[i];
    }
}

std::vector<uint32_t> InvertedIndex::allDocIds() const {
    std::vector<uint32_t> ids;
    ids.reserve(liveDocs_);
    for (uint32_t i = 0; i < alive_.size(); ++i) {
        if (alive_[i]) ids.push_back(i);
    }
    return ids;
}

std::vector<SearchResult> InvertedIndex::rankDocs(const std::vector<std::string>& terms,
                                                  const std::vector<uint32_t>& candidates,
                                                  size_t topK, RankMode mode) const {
    const auto scores = scoreTerms(terms, mode);
    std::vector<SearchResult> results;
    results.reserve(candidates.size());
    for (uint32_t docId : candidates) {
        auto it = scores.find(docId);
        results.push_back({docId, it == scores.end() ? 0.0 : it->second});
    }
    keepTopK(results, topK);
    return results;
}

}  // namespace ms
