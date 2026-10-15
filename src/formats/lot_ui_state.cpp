#include "lot_ui_state.hpp"

#include "dbpf.hpp"
#include "log_once.hpp"

#include <algorithm>
#include <exception>
#include <string>

namespace os2 {

namespace {

constexpr size_t kNameSize = 64;

uint32_t u32At(std::span<const uint8_t> b, size_t at) {
    return uint32_t(b[at]) | uint32_t(b[at + 1]) << 8 | uint32_t(b[at + 2]) << 16 | uint32_t(b[at + 3]) << 24;
}

void put32(std::vector<uint8_t>& out, uint32_t v) {
    for (int k = 0; k < 4; ++k)
        out.push_back(uint8_t(v >> (8 * k)));
}

} // namespace

std::optional<LotUiState> parseLotUiState(std::span<const uint8_t> bytes) {
    size_t at = kNameSize;
    auto next = [&]() -> std::optional<uint32_t> {
        if (at + 4 > bytes.size())
            return std::nullopt;
        const uint32_t v = u32At(bytes, at);
        at += 4;
        return v;
    };
    LotUiState s;
    const auto version = next(), paused = next();
    if (!version || !paused)
        return std::nullopt;
    s.version = *version;
    s.paused = *version == 0 || *paused != 0;
    if (*version == 0 && !next()) // version 0: one more u32 before the floor
        return std::nullopt;
    const auto level = next(), wall = next();
    if (!level || !wall)
        return std::nullopt;
    s.level = int32_t(*level);
    s.wallMode = int32_t(*wall);
    return s;
}

std::optional<LotUiState> readLotUiState(DbpfPackage& pkg) {
    for (const auto& e : pkg.entries())
        if (e.tgi.type == kTypeLotUiState) {
            try {
                const auto bytes = pkg.read(e);
                return parseLotUiState(bytes);
            } catch (const std::exception& err) {
                logOnce("lot UI state", "read", err);
                return std::nullopt;
            }
        }
    return std::nullopt;
}

std::vector<uint8_t> writeLotUiState(const LotUiState& s, std::span<const uint8_t> name) {
    std::vector<uint8_t> out(kNameSize, 0);
    std::copy_n(name.begin(), std::min(name.size(), kNameSize), out.begin());
    put32(out, 2);
    put32(out, s.paused ? 1 : 0);
    put32(out, uint32_t(s.level));
    put32(out, uint32_t(s.wallMode));
    put32(out, 0);
    return out;
}

} // namespace os2
