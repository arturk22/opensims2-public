#include "dbpf.hpp"

#include "binreader.hpp"
#include "qfs.hpp"

#include <cstring>

namespace os2 {

DbpfPackage::DbpfPackage(const std::filesystem::path& path) : path_(path), file_(path, std::ios::binary) {
    if (!file_)
        throw DbpfError("cannot open " + path.string());
    file_.seekg(0, std::ios::end);
    fileSize_ = uint64_t(file_.tellg());
    readHeader();
    readIndex();
    applyDirectory();
}

void DbpfPackage::readHeader() {
    uint8_t h[96];
    if (fileSize_ < sizeof h)
        throw DbpfError(path_.string() + ": file too small for DBPF header");
    file_.seekg(0);
    file_.read(reinterpret_cast<char*>(h), sizeof h);
    if (std::memcmp(h, "DBPF", 4) != 0)
        throw DbpfError(path_.string() + ": bad magic");

    header_.major = le32(h + 0x04);
    header_.minor = le32(h + 0x08);
    header_.created = le32(h + 0x18);
    header_.modified = le32(h + 0x1C);
    header_.indexMajor = le32(h + 0x20);
    header_.indexCount = le32(h + 0x24);
    header_.indexOffset = le32(h + 0x28);
    header_.indexSize = le32(h + 0x2C);
    header_.holeCount = le32(h + 0x30);
    header_.holeOffset = le32(h + 0x34);
    header_.holeSize = le32(h + 0x38);
    header_.indexMinor = le32(h + 0x3C);

    if (header_.major != 1)
        throw DbpfError(path_.string() + ": unsupported DBPF version " + std::to_string(header_.major));
    if (header_.indexMajor != 7)
        throw DbpfError(path_.string() + ": unsupported index version " + std::to_string(header_.indexMajor));
}

void DbpfPackage::readIndex() {
    if (header_.indexCount == 0)
        return;
    const uint32_t entrySize = header_.indexSize / header_.indexCount;
    if (entrySize != 20 && entrySize != 24)
        throw DbpfError(path_.string() + ": unexpected index entry size " + std::to_string(entrySize));
    if (uint64_t(header_.indexOffset) + header_.indexSize > fileSize_)
        throw DbpfError(path_.string() + ": index outside file");

    std::vector<uint8_t> idx(header_.indexSize);
    file_.seekg(header_.indexOffset);
    file_.read(reinterpret_cast<char*>(idx.data()), std::streamsize(idx.size()));

    entries_.reserve(header_.indexCount);
    for (uint32_t i = 0; i < header_.indexCount; ++i) {
        const uint8_t* p = idx.data() + size_t(i) * entrySize;
        DbpfEntry e;
        e.tgi.type = le32(p);
        e.tgi.group = le32(p + 4);
        e.tgi.instance = le32(p + 8);
        size_t o = 12;
        if (entrySize == 24) {
            e.tgi.instanceHi = le32(p + 12);
            o = 16;
        }
        e.offset = le32(p + o);
        e.size = le32(p + o + 4);
        if (uint64_t(e.offset) + e.size > fileSize_)
            throw DbpfError(path_.string() + ": resource outside file");
        byTgi_.emplace(e.tgi, entries_.size());
        entries_.push_back(e);
    }
}

void DbpfPackage::applyDirectory() {
    const DbpfEntry* dir = nullptr;
    for (const auto& e : entries_)
        if (e.tgi.type == type::DIR) {
            dir = &e;
            break;
        }
    if (!dir)
        return;

    const auto data = readRaw(*dir);
    const bool wide = header_.indexMinor == 2; // 7.1: entries carry instanceHi
    const size_t recSize = wide ? 20 : 16;
    for (size_t p = 0; p + recSize <= data.size(); p += recSize) {
        Tgi k;
        k.type = le32(&data[p]);
        k.group = le32(&data[p + 4]);
        k.instance = le32(&data[p + 8]);
        if (wide)
            k.instanceHi = le32(&data[p + 12]);
        const uint32_t raw = le32(&data[p + recSize - 4]);
        auto it = byTgi_.find(k);
        if (it != byTgi_.end())
            entries_[it->second].rawSize = raw;
    }
}

const DbpfEntry* DbpfPackage::find(const Tgi& tgi) const {
    auto it = byTgi_.find(tgi);
    return it == byTgi_.end() ? nullptr : &entries_[it->second];
}

std::vector<uint8_t> DbpfPackage::readRaw(const DbpfEntry& e) {
    std::vector<uint8_t> buf(e.size);
    if (!file_.is_open()) {
        file_.open(path_, std::ios::binary);
        if (!file_)
            throw DbpfError("cannot reopen " + path_.string());
    }
    file_.clear();
    file_.seekg(e.offset);
    file_.read(reinterpret_cast<char*>(buf.data()), std::streamsize(buf.size()));
    if (!file_)
        throw DbpfError(path_.string() + ": short read");
    return buf;
}

std::vector<uint8_t> DbpfPackage::read(const DbpfEntry& e) {
    auto raw = readRaw(e);
    if (!e.compressed() || !qfsIsCompressed(raw))
        return raw;
    auto out = qfsDecompress(raw);
    if (out.size() != *e.rawSize)
        throw DbpfError(path_.string() + ": decompressed size differs from DIR");
    return out;
}

std::string_view typeName(uint32_t t) {
    switch (t) {
    case 0x00000000: return "UI";
    case 0x0C560F39: return "BINX";
    case 0x1C4A276C: return "TXTR";
    case 0x2026960B: return "AUDIO";
    case 0x42434F4E: return "BCON";
    case 0x42484156: return "BHAV";
    case 0x43545353: return "CTSS";
    case 0x46574156: return "FWAV";
    case 0x474C4F42: return "GLOB";
    case 0x49596978: return "TXMT";
    case 0x4C697E5A: return "MMAT";
    case 0x4D51F042: return "CINE";
    case 0x4E524546: return "NREF";
    case 0x4F424A44: return "OBJD";
    case 0x4F424A66: return "OBJf";
    case 0x50414C54: return "PALT";
    case 0x534C4F54: return "SLOT";
    case 0x53545223: return "STR#";
    case 0x54505250: return "TPRP";
    case 0x54524353: return "TRCN";
    case 0x54544142: return "TTAB";
    case 0x54544173: return "TTAs";
    case 0x7BA3838C: return "GMND";
    case 0x856DDBAC: return "IMG";
    case 0xAC4F8687: return "GMDC";
    case 0xC9C81B9B: return "LAMB";
    case 0xCCCEF852: return "LxNR";
    case 0xC9C81BA3: return "LDIR";
    case 0xC9C81BA9: return "LPNT";
    case 0xC9C81BAD: return "LSPT";
    case 0xAC506764: return "3IDR";
    case 0xE519C933: return "CRES";
    case 0xE86B1EEF: return "DIR";
    case 0xEBCF3E27: return "GZPS";
    case 0xED534136: return "LIFO";
    case 0xFB00791E: return "ANIM";
    case 0xFC6EB1F7: return "SHPE";
    default: return {};
    }
}

} // namespace os2
