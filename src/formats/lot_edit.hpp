#pragma once

// Build-mode changes to a lot's geometry (Lot, lot.hpp), as the game's
// wall, floor and wallpaper tools make them (docs/buy-build-mac.md §7):
// walls go into every wall graph their type belongs to, floors into the
// quarter-tile floor array, wall coverings into the wall layer, and the
// rooms are worked out again from the walls. No prices or rules here
// (vm/build_edit.hpp); levels are the lot's (0 = the ground).

#include "lot.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace os2 {

// Quarter tiles (Lot::floors, Lot::rooms): west (-x), north (+y), east
// (+x), south (-y).
enum Quarter : int { QWest = 0, QNorth = 1, QEast = 2, QSouth = 3 };

// The wall from grid point (ax, ay) to (bx, by) on `level`, either way
// round; null if none.
const Lot::Wall* findWall(const Lot& lot, int level, int ax, int ay, int bx, int by);
// Adds a wall one tile long (an edge, |dx| + |dy| == 1, or a diagonal,
// |dx| == |dy| == 1) of `type` with the given coverings (right, left),
// to Lot::walls and to the wall graphs of its type. Returns its ID; none if
// there is a wall there already or the segment is not one tile long.
std::optional<uint32_t> addWall(Lot& lot, int level, int ax, int ay, int bx, int by, uint32_t type,
                                std::array<uint16_t, 2> patterns);
// Two rooms a removed wall separated, for updateRooms: the rooms on the
// right and left of the graph's line (seen from its a to its b) in the room
// array of graph `graph` (5, 6 or 7).
struct RoomMerge {
    uint32_t graph = 0;
    int32_t right = 0, left = 0;
};
// cWallGraph::SetRoomsPostAttach (Mac BV 0xc1f1bc, docs/buy-build-mac.md
// §7.3, §7.25) for the line (ax, ay)-(bx, by) just added to graph `g` on
// `level`, over the graph's room array (5 -> 3ARY 0x09, 6 -> 0x0A, 7 ->
// 0x0B): a line that closes no cycle changes nothing; one that splits a room
// gives the first free ID to its left side (the left normal (-dy, dx)), or,
// splitting the outdoors (0), to the side its cycle encloses. addWall calls
// it for every graph the wall goes into.
void postAttachRooms(Lot& lot, Lot::WallGraph& g, int level, int ax, int ay, int bx, int by);
// Takes the wall there out of Lot::walls and every graph. Returns what it
// was. `merges` gets the rooms it separated in each room graph.
std::optional<Lot::Wall> removeWall(Lot& lot, int level, int ax, int ay, int bx, int by,
                                    std::vector<RoomMerge>* merges = nullptr);

// The reference of a pattern name in a string map, added if missing (new
// references from `first` up: floors keep 0 for no floor).
uint16_t patternRef(std::map<uint16_t, std::string>& map, const std::string& name, uint16_t first);

// The floor of quarter `q` of tile (x, y): a floor pattern reference, 0
// for none. Out of range: no change, false.
bool setFloor(Lot& lot, int level, int x, int y, int q, uint16_t pattern);

// The rotation of quarter `q`'s floor pattern (Lot::floorRotations: 1, 2,
// 4 or 8); 1 when the lot has none.
bool setFloorRotation(Lot& lot, int level, int x, int y, int q, uint8_t rotation);
uint8_t floorRotation(const Lot& lot, int level, int x, int y, int q);

// Fence arches (fencearches.txt "fenceArch N", PartitionType 2): cPartition
// Manager::AddWallToGraph puts them in their own wall graph (0x13) and in no
// other, so they sit on a fence's edge beside it. None of the shipped lots
// has one: the arch's style is taken as the base game's only arch (26),
// inferred. Edges between two grid points, along the grid.
struct FenceArch {
    int level = 0, ax = 0, ay = 0, bx = 0, by = 0;
};
std::vector<FenceArch> fenceArches(const Lot& lot);
bool hasFenceArch(const Lot& lot, int level, int ax, int ay, int bx, int by);
bool addFenceArch(Lot& lot, int level, int ax, int ay, int bx, int by);
bool removeFenceArch(Lot& lot, int level, int ax, int ay, int bx, int by);

// Adds the underground level a pool needs (minLevel - 1): its grid points
// 3 below the ground, no floors or rooms; the wall graphs' level range
// follows. False if the lot has one already.
bool addLowerLevel(Lot& lot);
// The 3ARY 0x14 deck/foundation/pool markers (Lot::deckFoundationStyles) as
// cTSUserToolUndoableDeckFoundation::SetDeckFoundationStyle writes them (Mac
// BV 0x41817c, docs/buy-build-mac.md §7.25): quarter `q` of tile (x, y) on
// `level` gets `style` (the deckfoundations.txt product: 0 foundation, 1 and
// 2 decks, 3 pool; -1 none). A lot without the array gets one (a plane a
// level, from the lowest to the one under the top, as the saved lots keep
// it). Returns the marker it had; kNoMarkerPlane off the arrays (the top
// level has none).
constexpr int32_t kNoMarkerPlane = -2;
int32_t setDeckFoundationMarker(Lot& lot, int level, int x, int y, int q, int32_t style);
// After a level was added: the marker planes again from the lowest level to
// the one under the top, the new planes without markers (a lot without the
// array keeps none).
void fitDeckFoundationMarkers(Lot& lot);

// Adds a level on top (a dormer's roof over the attic, MakeDormer: WorldDB
// vtable 0x1C): its grid points 3 above the highest, no floors or rooms.
void addUpperLevel(Lot& lot);

// Rooms again from the walls of each room array's wall graph (rooms 5 ->
// 3ARY 0x09, 6 -> 0x0A, 7 -> 0x0B): quarter tiles no wall separates share
// a room. A room keeps the ID most of its quarters had (the larger room
// keeps it when one is split); new rooms get the next ID. Quarters that
// had no room and have no floor stay out of every room (the saved lots
// leave the floorless quarters of the upper levels out). The lines of the
// graphs get the rooms on their sides and the graphs their room lists.
// Rooms joined by removed walls (`merges`, in the order the walls went) keep
// the right-hand room's ID, or become room 0 if either side was 0
// (cWallGraph::SetRoomsPreDetach, docs/buy-build-mac.md §7.3).
void updateRooms(Lot& lot, const std::vector<RoomMerge>& merges = {});

// The room (3ARY 0x09, or the room array `rooms` of 0x0A/0x0B) on one side
// of a wall: 0 its right (seen from a to b), 1 its left; 0 for none.
int32_t wallSideRoom(const Lot& lot, const Lot::Wall& wall, int side, uint32_t rooms = 0x09);

// Rooms of a graph from scratch, for checking the algorithm against a
// saved lot: for each quarter its component (an index), same layout as
// Lot::rooms.
std::vector<std::array<uint32_t, 4>> roomComponents(const Lot& lot, const Lot::WallGraph& graph);

} // namespace os2
