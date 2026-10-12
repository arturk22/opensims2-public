#include "dbpf.hpp"

#include "binreader.hpp"
#include "qfs.hpp"

#include <cstring>

#include <fcntl.h>
#include <unistd.h>

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

DbpfPackage::SharedFd& DbpfPackage::SharedFd::operator=(SharedFd&& o) noexcept {
    if (this != &o) {
        if (fd && *fd >= 0)
            ::close(*fd);
        fd = std::move(o.fd);
    }
    return *this;
}

DbpfPackage::SharedFd::~SharedFd() {
    if (fd && *fd >= 0)
        ::close(*fd);
}

std::vector<uint8_t> DbpfPackage::readConcurrent(const DbpfEntry& e) const {
    std::vector<uint8_t> raw(e.size);
    int fd = shared_.fd->load(std::memory_order_acquire);
    if (fd < 0) {
        const int mine = ::open(path_.c_str(), O_RDONLY | O_CLOEXEC);
        if (mine < 0)
            throw DbpfError("cannot reopen " + path_.string());
        int expected = -1;
        if (shared_.fd->compare_exchange_strong(expected, mine))
            fd = mine;
        else
            ::close(mine), fd = expected; // another thread opened it first
    }
    size_t got = 0;
    while (got < raw.size()) {
        const ssize_t n = ::pread(fd, raw.data() + got, raw.size() - got, off_t(e.offset + got));
        if (n <= 0)
            break;
        got += size_t(n);
    }
    if (got != raw.size())
        throw DbpfError(path_.string() + ": short read");
    if (!e.compressed() || !qfsIsCompressed(raw))
        return raw;
    auto out = qfsDecompress(raw);
    if (out.size() != *e.rawSize)
        throw DbpfError(path_.string() + ": decompressed size differs from DIR");
    return out;
}

namespace {

void putLe32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(uint8_t(v));
    out.push_back(uint8_t(v >> 8));
    out.push_back(uint8_t(v >> 16));
    out.push_back(uint8_t(v >> 24));
}

void setLe32(std::vector<uint8_t>& out, size_t at, uint32_t v) {
    out[at] = uint8_t(v);
    out[at + 1] = uint8_t(v >> 8);
    out[at + 2] = uint8_t(v >> 16);
    out[at + 3] = uint8_t(v >> 24);
}

} // namespace

bool DbpfWriter::contains(const Tgi& tgi) const {
    for (const auto& r : resources_)
        if (r.tgi == tgi)
            return true;
    return false;
}

void DbpfWriter::add(const Tgi& tgi, std::vector<uint8_t> data, bool compress) {
    if (compress && !data.empty() && data.size() <= 0xFFFFFF) {
        auto packed = qfsCompress(data);
        if (packed.size() < data.size()) {
            const auto raw = uint32_t(data.size());
            addStored(tgi, std::move(packed), raw);
            return;
        }
    }
    addStored(tgi, std::move(data));
}

void DbpfWriter::addStored(const Tgi& tgi, std::vector<uint8_t> stored, std::optional<uint32_t> rawSize) {
    if (tgi.type == type::DIR)
        return;
    resources_.push_back({tgi, std::move(stored), rawSize});
}

std::vector<uint8_t> DbpfWriter::build() const {
    std::vector<uint8_t> dir; // 7.1 records: type, group, instance, instanceHi, raw size
    for (const auto& r : resources_) {
        if (!r.rawSize)
            continue;
        putLe32(dir, r.tgi.type);
        putLe32(dir, r.tgi.group);
        putLe32(dir, r.tgi.instance);
        putLe32(dir, r.tgi.instanceHi);
        putLe32(dir, *r.rawSize);
    }

    std::vector<uint8_t> out(96);
    std::memcpy(out.data(), "DBPF", 4);
    std::vector<uint8_t> index;
    auto place = [&](const Tgi& k, const std::vector<uint8_t>& bytes) {
        if (out.size() + bytes.size() > 0xFFFFFFFFull)
            throw DbpfError("package larger than 4 GiB");
        putLe32(index, k.type);
        putLe32(index, k.group);
        putLe32(index, k.instance);
        putLe32(index, k.instanceHi);
        putLe32(index, uint32_t(out.size()));
        putLe32(index, uint32_t(bytes.size()));
        out.insert(out.end(), bytes.begin(), bytes.end());
    };
    for (const auto& r : resources_)
        place(r.tgi, r.stored);
    const bool hasDir = !dir.empty();
    if (hasDir)
        place(kDirTgi, dir);

    const size_t count = resources_.size() + hasDir;
    if (out.size() + index.size() > 0xFFFFFFFFull)
        throw DbpfError("package larger than 4 GiB");
    setLe32(out, 0x04, 1); // DBPF 1.1
    setLe32(out, 0x08, 1);
    setLe32(out, 0x20, 7); // index 7.1
    setLe32(out, 0x24, uint32_t(count));
    setLe32(out, 0x28, uint32_t(out.size()));
    setLe32(out, 0x2C, uint32_t(index.size()));
    setLe32(out, 0x3C, 2);
    out.insert(out.end(), index.begin(), index.end());
    return out;
}

void DbpfWriter::save(const std::filesystem::path& path) const {
    const auto bytes = build();
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f)
        throw DbpfError("cannot create " + path.string());
    f.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    if (!f)
        throw DbpfError(path.string() + ": write failed");
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
