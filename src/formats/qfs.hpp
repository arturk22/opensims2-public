#pragma once

#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

namespace os2 {

struct QfsError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// True if `data` looks like a DBPF-wrapped QFS stream:
// [u32 compressed size LE][0x10 0xFB][u24 uncompressed size BE]...
bool qfsIsCompressed(std::span<const uint8_t> data);

// Uncompressed size stored in a DBPF QFS header.
uint32_t qfsUncompressedSize(std::span<const uint8_t> data);

// Decompress a DBPF-wrapped QFS (RefPack) stream.
std::vector<uint8_t> qfsDecompress(std::span<const uint8_t> data);

// Compress into a DBPF-wrapped QFS stream (simple greedy LZ, used for tests and writing).
std::vector<uint8_t> qfsCompress(std::span<const uint8_t> data);

} // namespace os2
