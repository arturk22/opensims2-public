#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <fstream>
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
    void closeFile() { file_.close(); }
    bool fileOpen() const { return file_.is_open(); }

private:
    std::filesystem::path path_;
    std::ifstream file_;
    uint64_t fileSize_ = 0;
    DbpfHeader header_;
    std::vector<DbpfEntry> entries_;
    std::unordered_map<Tgi, size_t, TgiHash> byTgi_;
    // readConcurrent's descriptor (-1 until first used).
    struct SharedFd {
        std::unique_ptr<std::atomic<int>> fd = std::make_unique<std::atomic<int>>(-1);
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

// Builds a DBPF 1.1 package with index 7.1, the layout the game ships: 96-byte
// header, resource data in insertion order, a generated DIR listing the
// uncompressed size of every QFS-compressed resource, then the index.
// Duplicate TGIs are kept as given (the format allows them); DIR resources
// passed in are dropped, since the writer generates its own.
class DbpfWriter {
public:
    // Add uncompressed bytes. With `compress`, the resource is stored as QFS
    // unless that does not shrink it (or it exceeds QFS's 16 MiB limit).
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

// Key the game uses for the DIR resource.
constexpr Tgi kDirTgi{type::DIR, 0xE86B1EEF, 0x286B1F03, 0};

// Short tag for well-known Sims 2 resource types ("BHAV", "GMDC", ...), or empty.
std::string_view typeName(uint32_t type);

} // namespace os2
