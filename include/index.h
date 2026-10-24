#pragma once
#include <cstdint>
#include <optional>
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
    // ---- adding, updating and removing documents ----

    // Indexes a document and returns its id. Names are unique: if a live
    // document with this name already exists it is replaced (it gets a new id).
    // `stamp` is an opaque version number chosen by the caller (for files:
    // a hash of size + modification time); it is stored and saved, and lets
    // the caller tell whether a document changed since it was indexed.
    uint32_t addDocument(const std::string& name, const std::string& text,
                         uint64_t stamp = 0);

    // Removes a document by name. Returns false if there is no such document.
    // Ids of other documents never change. The removed document is hidden from
    // every query immediately; its postings are physically dropped later by
    // compact() (which also runs automatically once enough removals pile up).
    bool removeDocument(const std::string& name);

    // Physically deletes the postings of removed documents and any term that
    // is left with no documents. Never changes query results.
    void compact();

    // ---- lookups ----

    size_t numDocs() const { return liveDocs_; }             // live documents
    size_t numSlots() const { return docNames_.size(); }     // ids ever handed out
    size_t numRemoved() const { return pendingDead_; }       // removed, postings not yet purged
    size_t numTerms() const { return index_.size(); }

    bool isAlive(uint32_t docId) const { return docId < alive_.size() && alive_[docId] != 0; }
    std::optional<uint32_t> findDoc(const std::string& name) const;

    // For a removed document: empty name, length 0, stamp 0.
    const std::string& docName(uint32_t docId) const { return docNames_.at(docId); }
    uint32_t docLength(uint32_t docId) const { return docLengths_.at(docId); }
    uint64_t docStamp(uint32_t docId) const { return docStamps_.at(docId); }

    double avgDocLength() const {
        return liveDocs_ == 0 ? 0.0 : static_cast<double>(totalLength_) / liveDocs_;
    }

    // Posting list for a term, or nullptr if the term is not indexed.
    // May still contain postings of removed documents until compact() runs.
    const std::vector<Posting>* postings(const std::string& term) const;

    // ---- searching ----

    // Ranked search. A document matches if it contains at least one query term.
    // BM25 is the default; TfIdf is kept so the two can be compared.
    std::vector<SearchResult> search(const std::string& query, size_t topK = 10,
                                     RankMode mode = RankMode::BM25) const;

    // Phrase search: ids (ascending) of documents where `terms` appear as
    // consecutive words, in order. Uses the stored word positions.
    // One term = every document containing it. No terms = no documents.
    std::vector<uint32_t> phraseMatch(const std::vector<std::string>& terms) const;

    // Ids (ascending) of all live documents.
    std::vector<uint32_t> allDocIds() const;

    // Scores only the given candidate documents against `terms` and returns
    // the best `topK`. Candidates that contain none of the terms get score 0.
    std::vector<SearchResult> rankDocs(const std::vector<std::string>& terms,
                                       const std::vector<uint32_t>& candidates,
                                       size_t topK = 10,
                                       RankMode mode = RankMode::BM25) const;

    void setBm25Params(const Bm25Params& p) { bm25_ = p; }

    // ---- persistence (implemented in index_io.cpp) ----

    // Writes the index to `path` (compact binary format; removed documents
    // are not written). Throws std::runtime_error on I/O failure.
    void save(const std::string& path) const;

    // Reads an index written by save(). Throws std::runtime_error if the file
    // is missing, truncated or corrupt.
    static InvertedIndex load(const std::string& path);

private:
    // docId -> score summed over all `terms`.
    std::unordered_map<uint32_t, double> scoreTerms(const std::vector<std::string>& terms,
                                                    RankMode mode) const;
    static void keepTopK(std::vector<SearchResult>& results, size_t topK);

    std::unordered_map<std::string, std::vector<Posting>> index_;

    // One slot per document id. A removed document keeps its slot (alive_ = 0)
    // so ids of other documents never shift.
    std::vector<std::string> docNames_;
    std::vector<uint32_t> docLengths_;  // number of tokens per document
    std::vector<uint64_t> docStamps_;
    std::vector<uint8_t> alive_;
    std::unordered_map<std::string, uint32_t> nameToId_;  // live documents only

    size_t liveDocs_ = 0;
    size_t pendingDead_ = 0;    // removals whose postings are still in index_
    uint64_t totalLength_ = 0;  // sum of live document lengths
    Bm25Params bm25_;
};

}  // namespace ms
