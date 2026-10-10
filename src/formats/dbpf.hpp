#pragma once

#include <cstdint>
#include <filesystem>
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

    void readHeader();
    void readIndex();
    void applyDirectory();
};

// Short tag for well-known Sims 2 resource types ("BHAV", "GMDC", ...), or empty.
std::string_view typeName(uint32_t type);

} // namespace os2
