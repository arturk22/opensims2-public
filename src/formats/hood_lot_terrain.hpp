#pragma once

// The neighborhood terrain under the lots (docs/neighborhood-mac.md §4):
// cTSNHoodTerrain::FlattenTerrainUnderLot (BV 0x4d1b10) sets every vertex of
// a lot's rectangle (lot info vtable 0x30, neighborhood tiles) to the lot's
// elevation (vtable 0x40), except vertices another lot also touches (the lots
// within one tile of the vertex) and those the occupant manager's vertex test
// (vtable 0x28 with 2) refuses, taken here to be the vertices of road tiles
// (inferred: the roads keep their saved quads). The game calls it when a lot
// is placed or moved; the shipped neighborhoods are saved flattened already,
// so this mostly closes small seams. Plain C++.

#include "hood_terrain.hpp"
#include "neighborhood.hpp"

#include <vector>

namespace os2::hood {

// Returns how many vertices moved.
size_t flattenTerrainUnderLots(Terrain& terrain, const std::vector<Lot>& lots, const Occupants& occupants);

} // namespace os2::hood
