#pragma once

// Playing the neighborhood: families moving into lots and out of them,
// bulldozing an empty lot, and the money rules behind them, as the Mac
// binary has them (docs/neighborhood-mac.md §10):
//
// - The price of a lot (cTSNeighborhood::GetLotFileInfo, BV 0x4d8d8c) comes
//   from the lot file's simulator record (SIMI): lot value (+0xA4) + the lot's
//   objects (+0xA8) + the family's objects (+0xAC, left out once the family
//   was evicted: lot flag 8) + factor x the field +0xB0 (+ the business value,
//   BV only), +0xB0 being the architecture value (cTSSimulator::GetArchValue)
//   and the factor the catalog's refund rate, 0.8. Without a lot file: nTSWorld::LotValue(w, h) (lot tiles) =
//   round(w x h x 6.5 / 100) x 100 - 1000. The same record says whether the
//   lot has a house (global 22 "Lot Has House" != 0) and whether a family may
//   move in (global 21 "Inhibit Move In" == 0).
// - Whether a family can move in (cTSFamilyManager::CanMoveFamilyIntoLot,
//   BV 0x4fb804): see MoveInCheck. The family needs a member
//   (CountMembers(1)), and, unless the lot inhibits move-in, funds of at
//   least the price.
// - Moving in (cTSFamilyManager::MoveIn, BV 0x4fb730): the family's lot is
//   set and its "new lot" flag (FAMI flags bit 0x04, cTSFamily::SetNewLot)
//   raised. The price is charged when the lot loads for the family
//   (cTSLoadLotController::LoadLot, step 0x1B: funds -= GetLotFileInfo's
//   price unless the lot is of type 2): see formats/lot_arrivals.hpp.
// - Moving out (cTSGameStateController::EvictFamily, BV 0x139740): the family
//   gets the lot's price back (unless the lot is a dorm or a business type,
//   2/3), its lot becomes 0 and the lot info gets flag 8. The lot file is
//   left alone: the next time the lot loads, CleanupThingsRelatedToEvicted-
//   Family removes the family's Sims and what cTSObject::IsDeletedByEvict
//   picks, all the Buy-mode furniture without "stays after evict"
//   (formats/lot_evict.hpp), and the next save clears the flag; until then
//   the lot's price leaves out the family's objects.
// - Bulldozing (cTSUserToolBulldozeLot): only a lot nobody lives in (STR#
//   0x83 #29); the lot info, its name and its file go.
//
// Everything writes only the given neighborhood folder (the user's copy:
// app/user_data.hpp); each file is written under a temporary name and
// renamed over the old one. Plain C++.

#include "neighborhood.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace os2::hood {

// What GetLotFileInfo reads for a lot.
struct LotFileInfo {
    bool fromFile = false;   // a lot file with a SIMI (else the world value by size)
    int64_t price = 0;       // what moving in costs / moving out gives back
    bool hasHouse = false;   // global 22 "Lot Has House"
    bool canMoveIn = true;   // global 21 "Inhibit Move In" == 0
    // The parts (SIMI fields).
    int64_t lotValue = 0, lotObjects = 0, familyObjects = 0, architecture = 0; // +0xA4, +0xA8, +0xAC, +0xB0
};

// nTSWorld::LotValue (BV 0x1635f4): width and depth in lot tiles.
int64_t worldLotValue(int width, int depth);
LotFileInfo lotFileInfo(const Lot& lot);

// cTSFamilyManager::CanMoveFamilyIntoLot's answers.
enum class MoveInCheck {
    Ok = 0,
    NoFamily = 1,
    NoLot = 2,
    Community = 3,    // "Families cannot move into community lots."
    Occupied = 5,     // the lot has a family ("This lot is already occupied.")
    CantAfford = 7,   // "The %s family cannot afford this lot."
    NoMembers = 9,
};
MoveInCheck canMoveIn(const Neighborhood& hood, uint32_t family, uint32_t lot);
const char* moveInCheckName(MoveInCheck c);

// The families in the family bin (cTSFamilyManager::GetHomelessFamilyIDs,
// BV 0x4fbfe0): lot < 1 and ID 1..0x7FD8 (the ones above are the NPCs' and
// townies').
std::vector<uint32_t> homelessFamilies(const Neighborhood& hood);
// The family living on a lot (0: none).
uint32_t familyOnLot(const Neighborhood& hood, uint32_t lot);
// Net worth (cTSFamilyManager::GetFamilyNetWorth): funds + its lot's price.
int64_t netWorth(const Neighborhood& hood, uint32_t family);

struct MoveResult {
    bool ok = false;
    std::string error;
    int64_t price = 0;
    int32_t fundsBefore = 0, fundsAfter = 0;
    size_t simsRemoved = 0, objectsRemoved = 0; // eviction cleanup
    std::vector<std::string> lines;             // what was written
};

// `folder` is the neighborhood folder (refused when it is inside `forbidden`,
// the game's data).
MoveResult moveIn(const std::filesystem::path& folder, uint32_t family, uint32_t lot,
                  const std::filesystem::path& forbidden = {});
MoveResult moveOut(const std::filesystem::path& folder, uint32_t lot, const std::filesystem::path& forbidden = {});
MoveResult bulldoze(const std::filesystem::path& folder, uint32_t lot, const std::filesystem::path& forbidden = {});

} // namespace os2::hood
