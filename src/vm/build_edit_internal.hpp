#pragma once

// What the build editor's sources share: build_edit.cpp, build_edit_walls.cpp,
// build_edit_ground.cpp and build_edit_stairs.cpp (internal to them, not an
// API; the functions are described where they are defined, in
// build_edit.cpp).

#include "formats/lot.hpp"

#include <array>
#include <cstdint>
#include <string>

namespace os2::vm::build_edit_internal {

constexpr uint32_t kWallType = Lot::Normal; // walls.txt "wall 1", the wall tool's
const char* const kDefaultWallPattern = "blank"; // its defaultPattern

// "§<v>".
std::string money(int v);
// "(x,y)".
std::string point(int x, int y);
// A street floor pattern: nothing is built over it.
bool street(const std::string& pattern);
// Whether the wall `w` stands on the section a (either way round).
bool sameSection(const std::array<int, 4>& a, const Lot::Wall& w);
// The partitions round the raised tiles (foundation, decks).
bool raisedEdge(uint32_t type);
// FUN_009ac8d0 (BV): the wall style's product may be deleted by the level
// room tool (cTSProduct vtable 0x154 with 4: walls.txt "deleteTool
// levelRoom", the table at 0x28660d0 partition 1, roof 2, levelroom 4,
// modularstairs 8): the foundation (23), 16, the deck skirts (24, 4) and the
// pool's wall (29).
bool levelRoomDeletes(uint32_t type);

} // namespace os2::vm::build_edit_internal
