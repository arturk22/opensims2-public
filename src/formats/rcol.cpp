#include "rcol.hpp"

#include "scenegraph.hpp"

namespace os2 {

namespace {

constexpr uint32_t kVersionFlag = 0xFFFF0001;
constexpr uint32_t kMaxCount = 0x10000;

bool isClassChar(uint8_t c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }

// Offset where a block header "len, className, typeId" starts at or after
// `from`, or npos. The class name always starts with 'c'.
size_t findBlockHeader(std::span<const uint8_t> d, size_t from, uint32_t typeId) {
    for (size_t j = from + 2; j + 4 <= d.size(); ++j) {
        if (le32(&d[j]) != typeId)
            continue;
        // Walk back over the class name to its length byte.
        size_t k = j;
        while (k > from && isClassChar(d[k - 1]) && j - k < 64)
            --k;
        // The name may itself end in characters that look like a length; try each split.
        for (size_t start = k; start < j; ++start)
            if (start > from && d[start - 1] == j - start && d[start] == 'c')
                return start - 1;
    }
    return std::string::npos;
}

bool isBlockHeaderAt(std::span<const uint8_t> d, size_t at, uint32_t typeId) {
    if (at >= d.size())
        return false;
    const size_t len = d[at];
    return len > 0 && at + 1 + len + 4 <= d.size() && d[at + 1] == 'c' && le32(&d[at + 1 + len]) == typeId;
}

} // namespace

bool rcolLooksValid(std::span<const uint8_t> data) {
    if (data.size() < 8)
        return false;
    uint32_t first = le32(data.data());
    return first == kVersionFlag || first < 256;
}

Rcol::Rcol(std::span<const uint8_t> data) {
    BinReader r(data);
    uint32_t count = r.u32();
    if (count == kVersionFlag) {
        hasInstanceHi = true;
        count = r.u32();
    }
    if (count > kMaxCount)
        throw FormatError("RCOL: bad link count");
    links.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        Tgi t;
        t.group = r.u32();
        t.instance = r.u32();
        if (hasInstanceHi)
            t.instanceHi = r.u32();
        t.type = r.u32();
        links.push_back(t);
    }

    const uint32_t blockCount = r.u32();
    if (blockCount == 0 || blockCount > kMaxCount)
        throw FormatError("RCOL: bad block count");
    std::vector<uint32_t> types(blockCount);
    for (auto& t : types)
        t = r.u32();

    blocks.reserve(blockCount);
    for (uint32_t i = 0; i < blockCount; ++i) {
        Block b;
        b.className = r.str7();
        b.typeId = r.u32();
        if (b.typeId != types[i])
            throw FormatError("RCOL: block " + std::to_string(i) + " type differs from the type list");
        const size_t start = r.pos();
        size_t end = data.size();
        if (i + 1 < blockCount) {
            // Known classes are measured by their parser; the header search is a
            // fallback, since embedded blocks can repeat the next block's type id.
            const size_t measured = measureRcolBlock(b.className, data.subspan(start));
            if (measured != std::string::npos && isBlockHeaderAt(data, start + measured, types[i + 1]))
                end = start + measured;
            else
                end = findBlockHeader(data, start, types[i + 1]);
            if (end == std::string::npos)
                throw FormatError("RCOL: cannot find block " + std::to_string(i + 1));
        }
        b.body = data.subspan(start, end - start);
        r.seek(end);
        blocks.push_back(std::move(b));
    }
}

const Rcol::Block* Rcol::find(std::string_view className) const {
    for (const auto& b : blocks)
        if (b.className == className)
            return &b;
    return nullptr;
}

void readEmbeddedHeader(BinReader& r, std::string_view expected) {
    const auto at = r.pos();
    const auto name = r.str7();
    if (name != expected)
        throw FormatError("expected embedded " + std::string(expected) + ", got '" + name + "' at offset " +
                          std::to_string(at));
    r.u32(); // block id, always 0 for embedded blocks
}

SgResource SgResource::read(BinReader& r) {
    readEmbeddedHeader(r, "cSGResource");
    SgResource s;
    s.version = r.u32();
    s.fileName = r.str7();
    return s;
}

ObjectGraphNode ObjectGraphNode::read(BinReader& r) {
    readEmbeddedHeader(r, "cObjectGraphNode");
    ObjectGraphNode n;
    n.version = r.u32();
    const uint32_t count = r.u32();
    if (count > kMaxCount)
        throw FormatError("cObjectGraphNode: bad extension count");
    for (uint32_t i = 0; i < count; ++i) {
        Extension e;
        e.enabled = r.u8();
        e.dependent = r.u8();
        e.index = r.u32();
        n.extensions.push_back(e);
    }
    if (n.version == 4)
        n.fileName = r.str7();
    return n;
}

ReferentNode ReferentNode::read(BinReader& r) {
    readEmbeddedHeader(r, "cReferentNode");
    ReferentNode n;
    n.version = r.u32();
    n.graph = ObjectGraphNode::read(r);
    return n;
}

CompositionTreeNode CompositionTreeNode::read(BinReader& r) {
    readEmbeddedHeader(r, "cCompositionTreeNode");
    CompositionTreeNode n;
    n.version = r.u32();
    n.graph = ObjectGraphNode::read(r);
    return n;
}

} // namespace os2
