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

} // namespace os2::vm::build_edit_internal
