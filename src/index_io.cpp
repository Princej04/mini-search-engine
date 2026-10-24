// Saving and loading an InvertedIndex.
//
// File layout (all integers are "varints": 7 bits per byte, high bit = more follows,
// so small numbers take 1 byte and the format does not depend on endianness):
//
//   "MSIX"  version byte (1)
//   numSlots
//   per slot:  alive byte (0/1); if alive: name (length + bytes), doc length, stamp
//   numTerms
//   per term:  term (length + bytes), numPostings,
//              per posting: docId delta, numPositions, position deltas
//
// Posting lists are sorted, so storing the *difference* from the previous entry
// gives small numbers that fit in one byte most of the time (delta encoding).
// Removed documents and their postings are not written.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>

#include "index.h"

namespace ms {

namespace {

constexpr char kMagic[4] = {'M', 'S', 'I', 'X'};
constexpr uint8_t kVersion = 1;

void putVarint(std::string& out, uint64_t v) {
    while (v >= 0x80) {
        out.push_back(static_cast<char>((v & 0x7F) | 0x80));
        v >>= 7;
    }
    out.push_back(static_cast<char>(v));
}

void putString(std::string& out, const std::string& s) {
    putVarint(out, s.size());
    out.append(s);
}

[[noreturn]] void corrupt(const char* why) {
    throw std::runtime_error(std::string("Corrupt index file: ") + why);
}

// Bounds-checked reader over an in-memory copy of the file.
class Reader {
public:
    explicit Reader(const std::string& data) : data_(data) {}

    size_t remaining() const { return data_.size() - pos_; }

    uint8_t byte() {
        if (pos_ >= data_.size()) corrupt("unexpected end of file");
        return static_cast<uint8_t>(data_[pos_++]);
    }

    uint64_t varint() {
        uint64_t v = 0;
        for (int shift = 0; shift < 64; shift += 7) {
            const uint8_t b = byte();
            v |= static_cast<uint64_t>(b & 0x7F) << shift;
            if (!(b & 0x80)) return v;
        }
        corrupt("number too long");
    }

    uint32_t u32() {
        const uint64_t v = varint();
        if (v > 0xFFFFFFFFull) corrupt("number out of range");
        return static_cast<uint32_t>(v);
    }

    std::string str() {
        const uint64_t n = varint();
        if (n > remaining()) corrupt("bad string length");
        std::string s(data_.data() + pos_, static_cast<size_t>(n));
        pos_ += static_cast<size_t>(n);
        return s;
    }

private:
    const std::string& data_;
    size_t pos_ = 0;
};

}  // namespace

void InvertedIndex::save(const std::string& path) const {
    auto liveCount = [this](const std::vector<Posting>& list) {
        size_t n = 0;
        for (const Posting& p : list) n += alive_[p.docId] ? 1 : 0;
        return n;
    };

    std::string buf;
    buf.append(kMagic, sizeof(kMagic));
    buf.push_back(static_cast<char>(kVersion));

    putVarint(buf, docNames_.size());
    for (size_t id = 0; id < docNames_.size(); ++id) {
        buf.push_back(alive_[id] ? 1 : 0);
        if (alive_[id]) {
            putString(buf, docNames_[id]);
            putVarint(buf, docLengths_[id]);
            putVarint(buf, docStamps_[id]);
        }
    }

    size_t termCount = 0;
    for (const auto& kv : index_) {
        if (liveCount(kv.second) > 0) ++termCount;
    }
    putVarint(buf, termCount);

    for (const auto& [term, list] : index_) {
        const size_t n = liveCount(list);
        if (n == 0) continue;
        putString(buf, term);
        putVarint(buf, n);
        uint32_t prevDoc = 0;
        bool first = true;
        for (const Posting& p : list) {
            if (!alive_[p.docId]) continue;
            putVarint(buf, first ? p.docId : p.docId - prevDoc);
            prevDoc = p.docId;
            first = false;
            putVarint(buf, p.positions.size());
            uint32_t prevPos = 0;
            for (size_t i = 0; i < p.positions.size(); ++i) {
                putVarint(buf, i == 0 ? p.positions[i] : p.positions[i] - prevPos);
                prevPos = p.positions[i];
            }
        }
    }

    // Write to a temporary file first, then rename, so a crash or full disk
    // cannot leave a half-written index in place of a good one.
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error("Cannot write index file: " + tmp);
        out.write(buf.data(), static_cast<std::streamsize>(buf.size()));
        out.flush();
        if (!out) throw std::runtime_error("Failed while writing index file: " + tmp);
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {  // some platforms refuse to rename over an existing file
        std::filesystem::remove(path, ec);
        std::filesystem::rename(tmp, path, ec);
        if (ec) throw std::runtime_error("Cannot replace index file: " + path);
    }
}

InvertedIndex InvertedIndex::load(const std::string& path) {
    std::string data;
    {
        std::ifstream in(path, std::ios::binary);
        if (!in) throw std::runtime_error("Cannot open index file: " + path);
        in.seekg(0, std::ios::end);
        const std::streamoff size = in.tellg();
        if (size < 0) throw std::runtime_error("Cannot read index file: " + path);
        in.seekg(0, std::ios::beg);
        data.resize(static_cast<size_t>(size));
        if (size > 0) in.read(&data[0], size);
        if (!in) throw std::runtime_error("Cannot read index file: " + path);
    }

    Reader r(data);
    for (char expected : kMagic) {
        if (r.remaining() == 0 || static_cast<char>(r.byte()) != expected) {
            corrupt("not a mini-search index (bad header)");
        }
    }
    if (r.byte() != kVersion) corrupt("unsupported version");

    InvertedIndex idx;

    const uint64_t slots = r.varint();
    if (slots > r.remaining() || slots > 0xFFFFFFFFull) corrupt("bad document count");
    idx.docNames_.resize(static_cast<size_t>(slots));
    idx.docLengths_.assign(static_cast<size_t>(slots), 0);
    idx.docStamps_.assign(static_cast<size_t>(slots), 0);
    idx.alive_.assign(static_cast<size_t>(slots), 0);

    for (size_t id = 0; id < slots; ++id) {
        const uint8_t flag = r.byte();
        if (flag > 1) corrupt("bad document flag");
        if (flag == 0) continue;
        std::string name = r.str();
        const uint32_t len = r.u32();
        const uint64_t stamp = r.varint();
        if (!idx.nameToId_.emplace(name, static_cast<uint32_t>(id)).second) {
            corrupt("duplicate document name");
        }
        idx.docNames_[id] = std::move(name);
        idx.docLengths_[id] = len;
        idx.docStamps_[id] = stamp;
        idx.alive_[id] = 1;
        ++idx.liveDocs_;
        idx.totalLength_ += len;
    }

    const uint64_t terms = r.varint();
    if (terms > r.remaining()) corrupt("bad term count");
    idx.index_.reserve(static_cast<size_t>(terms));

    for (uint64_t t = 0; t < terms; ++t) {
        std::string term = r.str();
        const uint64_t n = r.varint();
        if (n == 0 || n > r.remaining()) corrupt("bad posting count");

        std::vector<Posting> list;
        list.reserve(static_cast<size_t>(n));
        uint64_t prevDoc = 0;
        for (uint64_t k = 0; k < n; ++k) {
            const uint64_t delta = r.varint();
            if (delta > slots || (k > 0 && delta == 0)) corrupt("bad document id");
            const uint64_t doc = (k == 0) ? delta : prevDoc + delta;
            if (doc >= slots || !idx.alive_[static_cast<size_t>(doc)]) corrupt("bad document id");
            prevDoc = doc;
            const uint32_t docLen = idx.docLengths_[static_cast<size_t>(doc)];

            const uint64_t npos = r.varint();
            if (npos == 0 || npos > r.remaining()) corrupt("bad position count");
            Posting p;
            p.docId = static_cast<uint32_t>(doc);
            p.positions.reserve(static_cast<size_t>(npos));
            uint64_t prevPos = 0;
            for (uint64_t j = 0; j < npos; ++j) {
                const uint64_t d = r.varint();
                if (d >= docLen || (j > 0 && d == 0)) corrupt("bad word position");
                const uint64_t pos = (j == 0) ? d : prevPos + d;
                if (pos >= docLen) corrupt("bad word position");
                p.positions.push_back(static_cast<uint32_t>(pos));
                prevPos = pos;
            }
            list.push_back(std::move(p));
        }
        if (!idx.index_.emplace(std::move(term), std::move(list)).second) {
            corrupt("duplicate term");
        }
    }

    if (r.remaining() != 0) corrupt("unexpected data at end of file");
    return idx;
}

}  // namespace ms
