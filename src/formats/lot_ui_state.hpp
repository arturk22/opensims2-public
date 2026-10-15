#pragma once

// The UI state saved with a lot (cTSMainToolbar::Write/Read, BV
// 0x1aacf8/0x1aadf0; docs/game-flow-mac.md §7.6): resource 0x8B0C79D6,
// instance 2, of the lot package: a 64-byte name, then little-endian u32s:
// the version (2 in the base game's lots, 3 in BV), "paused", the floor
// (GetCurrentLevel: 1 is the ground) and the wall mode
// (cITSMainToolbar::eWallMode: 0 down, 1 cutaway, 2 up, 3 up with the roof).
// Read takes version 0 as paused with one more u32 before the floor, and the
// version 2 one after the wall mode, both thrown away.
// cTSMainToolbar::ApplySavedUIState puts them back after the lot loads.
// Plain C++.

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace os2 {

class DbpfPackage;

constexpr uint32_t kTypeLotUiState = 0x8B0C79D6;

struct LotUiState {
    uint32_t version = 2;
    bool paused = false;
    int32_t level = 1;    // the game's floor (1: the ground)
    int32_t wallMode = 1; // cutaway
};

// Nothing when the bytes are too short.
std::optional<LotUiState> parseLotUiState(std::span<const uint8_t> bytes);
// The lot package's 0x8B0C79D6 (any instance; the game's is 2), if any.
std::optional<LotUiState> readLotUiState(DbpfPackage& pkg);
// The bytes cTSMainToolbar::Write gives, as the base game's lots hold them
// (version 2, with its fifth u32 0), after `name` (the 64-byte name field).
std::vector<uint8_t> writeLotUiState(const LotUiState& s, std::span<const uint8_t> name = {});

} // namespace os2
