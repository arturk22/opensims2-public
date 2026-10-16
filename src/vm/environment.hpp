#pragma once

// The environment motive (13), as the base game's cTSEnvironmentScoreManager
// computes it (Mac base vtable 0x15dddf0; docs/motives-mac.md §8):
//
// - Every room of the lot gets a score from its walls, floors and objects,
//   by "environment room": a room r > 0 is r + 0x10000, the outdoors
//   (room 0) is cut in cells of 5 x 5 tiles, cell cx + 256 cy
//   (GetEnvironmentRoomID). The rooms are the placement rooms (3ARY 0x09,
//   object data 29): a fenced yard is a room.
// - Walls: each side of each partition of wall graph 5 with a catalog
//   covering adds the covering's NicenessMultiplier x price and its
//   CrapScore to the room on that side, and counts 1. Floors: each tile of
//   the room (every level; a tile with several rooms in each) adds its floor
//   pattern's NicenessMultiplier x price and CrapScore, and 1 to the area.
//   Objects in the world (with a model, not people, a multi-tile's lead
//   only) add price (OBJD field 0x12) x object data 7 "Niceness Score" / 100
//   and data 86 "Crapness Score", each at least 0.
// - Scores: walls min(1, mean niceness / 14) x 25, floors min(1, mean
//   niceness / 23) x 25, objects clamp01(niceness / curve(sqrt(area))) x 50
//   with the curve of global STR# 0x2BC (1000 x); the room's score is their
//   sum within +-100 less every crap (GetBaseEnvironmentScore).
// - A Sim's environment: its room's score; outdoors the cells around it,
//   weighted by a tent two cells wide each way (the weights sum to 4: the
//   game does not normalize them); plus a light term from 0 (the floor's
//   light under the Sim at least 0.9) to -20 (0.55 or less); within +-100.
//   With no room (-1) it is 0.
//
// The game keeps the sums up to date with messages (an object moved, its
// niceness or crap changed); walls and floors are read on entering live
// mode. Here the walls and floors are read with the rooms (setLotRooms) and
// the objects' part is summed when the score is asked: the same sums.

#include "vm.hpp"

#include <map>
#include <optional>

namespace os2 {
struct Lot;
}

namespace os2::vm {

// The constructor's tuning (Mac base 0xc8342c, BV 0x5805a0; SetTuningConstant
// can change them, nothing in the base game does).
namespace envtuning {
constexpr float WallNicenessMax = 14.0f;   // +0x04
constexpr float FloorNicenessMax = 23.0f;  // +0x08
constexpr float FloorWeight = 0.25f;       // +0x0c
constexpr float WallWeight = 0.25f;        // +0x10
constexpr float LightWeight = 0.2f;        // +0x14
constexpr float LightDark = 0.55f;         // +0x18
constexpr float LightBright = 0.9f;        // +0x1c
constexpr float CellSize = 5.0f;           // +0x24, tiles
} // namespace envtuning

// Keys below this are outdoor cells (the score's FUN_00c8472c).
constexpr int32_t kEnvironmentIndoorBase = 0x10000;

// GetEnvironmentRoomID (base 0xc84620): -1 for room -1, r + 0x10000 for a
// room r > 0, the outdoor cell of (x, y) for room 0.
int32_t environmentRoomId(int16_t room, float x, float y);

// One room's sums and scores (cRoomInfo, base node +0x14..+0x48).
struct EnvironmentRoom {
    int windows = 0, category17 = 0; // objects of category (data 59) 0x16 and 0x17
    float wallNice = 0, wallCrap = 0, wallCount = 0, wallScore = 0;
    float floorNice = 0, floorCrap = 0, floorArea = 0, floorScore = 0;
    float objectNice = 0, objectCrap = 0, objectCount = 0, objectScore = 0;
};
using EnvironmentRooms = std::map<int32_t, EnvironmentRoom>;

// UpdateWallScores and UpdateFloorScores for the lot (vm.environmentArchitecture);
// setLotRooms calls it.
void setLotEnvironment(VM& vm, const Lot& lot);

// Every room with the objects' sums (UpdateObjectScores) and the scores
// (UpdateEnvScores) of now.
EnvironmentRooms environmentRooms(VM& vm);

// GetBaseEnvironmentScore (base 0xc84558): 0 for a key it does not have.
float baseEnvironmentScore(const EnvironmentRooms& rooms, int32_t key);

// The light's part of a Sim's score (base 0xc844e4) for an illuminance.
float environmentLightTerm(float illumination);

// GetEnvironmentScore (base 0xc841ac) of a Sim, from `rooms`.
float environmentScore(VM& vm, const Object& sim, const EnvironmentRooms& rooms);
float environmentScore(VM& vm, const Object& sim);

} // namespace os2::vm
