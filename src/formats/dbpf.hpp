#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace os2 {

struct DbpfError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

namespace type {
constexpr uint32_t DIR = 0xE86B1EEF;
} // namespace type

// Type/Group/Instance(+high instance) key identifying a resource.
struct Tgi {
    uint32_t type = 0;
    uint32_t group = 0;
    uint32_t instance = 0;
    uint32_t instanceHi = 0;

    bool operator==(const Tgi&) const = default;
};

struct TgiHash {
    size_t operator()(const Tgi& k) const noexcept {
        uint64_t h = (uint64_t(k.type) << 32) ^ k.group;
        h ^= (uint64_t(k.instanceHi) << 32 | k.instance) * 0x9E3779B97F4A7C15ull;
        return size_t(h ^ (h >> 29));
    }
};

struct DbpfEntry {
    Tgi tgi;
    uint32_t offset = 0;
    uint32_t size = 0;             // size on disk
    std::optional<uint32_t> rawSize; // uncompressed size, if listed in DIR
    bool compressed() const { return rawSize.has_value(); }
};

struct DbpfHeader {
    uint32_t major = 0, minor = 0;
    uint32_t created = 0, modified = 0;
    uint32_t indexMajor = 0, indexCount = 0, indexOffset = 0, indexSize = 0;
    uint32_t holeCount = 0, holeOffset = 0, holeSize = 0;
    uint32_t indexMinor = 0; // stored value; 2 means index version 7.1
};

class DbpfPackage {
public:
    explicit DbpfPackage(const std::filesystem::path& path);

    const DbpfHeader& header() const { return header_; }
    const std::vector<DbpfEntry>& entries() const { return entries_; }
    const std::filesystem::path& path() const { return path_; }

    const DbpfEntry* find(const Tgi& tgi) const;

    // Bytes exactly as stored in the file.
    std::vector<uint8_t> readRaw(const DbpfEntry& e);
    // Bytes after QFS decompression (when the DIR lists the resource).
    std::vector<uint8_t> read(const DbpfEntry& e);
    // The same as read(), safe to call from several threads at once (and
    // alongside read()): positioned reads on a descriptor of its own, opened
    // on first use and kept.
    std::vector<uint8_t> readConcurrent(const DbpfEntry& e) const;

    // Release the file handle; the next read reopens it.
    void closeFile();
    bool fileOpen() const { return file_ >= 0; }

    DbpfPackage(const DbpfPackage&) = delete;
    DbpfPackage& operator=(const DbpfPackage&) = delete;
    DbpfPackage(DbpfPackage&&) noexcept;
    DbpfPackage& operator=(DbpfPackage&&) noexcept;
    ~DbpfPackage();

private:
    std::filesystem::path path_;
    std::intptr_t file_ = -1;
    uint64_t fileSize_ = 0;
    DbpfHeader header_;
    std::vector<DbpfEntry> entries_;
    std::unordered_map<Tgi, size_t, TgiHash> byTgi_;
    // readConcurrent's descriptor (-1 until first used).
    struct SharedFd {
        std::unique_ptr<std::atomic<std::intptr_t>> fd = std::make_unique<std::atomic<std::intptr_t>>(-1);
        SharedFd() = default;
        SharedFd(SharedFd&&) noexcept = default;
        SharedFd& operator=(SharedFd&& o) noexcept;
        ~SharedFd();
    };
    SharedFd shared_;

    void readHeader();
    void readIndex();
    void applyDirectory();
};

// Builds a DBPF 1.1 package as the game's cGZDBSegmentPackedFile writes one
// (docs/save-mac.md §9.2): 96-byte header with zero dates, resource data in
// insertion order, a generated DIR listing the uncompressed size of every
// QFS-compressed resource, then the index. The index takes the game's key
// type (DetermineKeyType): 24-byte entries and 2 at 0x3C (index 7.1, 20-byte
// DIR records) when some resource has a high instance, else 20-byte entries,
// 1 at 0x3C and 16-byte DIR records. The game writes in place, into holes;
// this writer always makes the whole file, as the game's compaction does
// (no holes; the order of the data and of the index is ours, which no reader
// depends on).
// Duplicate TGIs are kept as given (the format allows them); DIR resources
// passed in are dropped, since the writer generates its own.
class DbpfWriter {
public:
    // Add uncompressed bytes. With `compress`, the resource is stored as QFS
    // when the game's packed file would keep it compressed
    // (worthCompressing); the game compresses every record it writes, so the
    // save writers pass true.
    void add(const Tgi& tgi, std::vector<uint8_t> data, bool compress = false);
    // Add bytes exactly as they will be stored; a `rawSize` lists the resource
    // in the DIR as compressed (the bytes should then be a QFS stream).
    void addStored(const Tgi& tgi, std::vector<uint8_t> stored, std::optional<uint32_t> rawSize = {});

    size_t size() const { return resources_.size(); }
    bool contains(const Tgi& tgi) const;

    // Whole package as bytes.
    std::vector<uint8_t> build() const;
    void save(const std::filesystem::path& path) const;

private:
    struct Resource {
        Tgi tgi;
        std::vector<uint8_t> stored;
        std::optional<uint32_t> rawSize;
    };
    std::vector<Resource> resources_;
};

// The game's rule for a record it writes (cGZDBSegmentPackedFile::
// WriteRecordInternal and ShouldRecordBeCompressed, base 0x587004/0x589748,
// docs/save-mac.md §9.2): try only from 50 bytes to 16,000,000 and never for
// the DIR; keep the compressed bytes only under 0.8 of the raw size.
bool worthCompressing(uint32_t resourceType, size_t rawSize, size_t compressedSize);
constexpr size_t kMinCompressedRecord = 50, kMaxCompressedRecord = 16000000;

// Key the game uses for the DIR resource.
constexpr Tgi kDirTgi{type::DIR, 0xE86B1EEF, 0x286B1F03, 0};

// Short tag for well-known Sims 2 resource types ("BHAV", "GMDC", ...), or empty.
std::string_view typeName(uint32_t type);

} // namespace os2
