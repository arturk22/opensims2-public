#pragma once

// Buy mode's object placement in the VM: buying an object from the catalog
// onto the lot, moving and selling placed objects, as the Mac binary's
// cTSUserToolObject does (docs/buy-build-mac.md §2-§5). Plain C++ (the
// app's Buy panel and s2pkg drive it).
//
// TryPlace: first a free container slot of an object on the tile that
// takes the object (FindFirstAvailableSlot); otherwise the position tests
// in the binary's order, each with its error code (placementerror): in the
// lot, a level of the lot, a floor under it unless in the air (od 42 bit
// 0x200), the floor/terrain od 42 allows, the wall flags od 13 (STR# 0xE5:
// bits 0-3 wall required in front / right / behind / left, 8-11 wall not
// allowed there) and no wall through the object (unless od 42 bit 0x400),
// then the objects on its tiles in a common exclusive layer (od 63, STR#
// 0xFB: 1 floor, 2 wall, 4 no other object of the same type, 8 ceiling; 0
// counts as floor) unless od 8 lets them through (0x02 allow object
// intersection, 0x04 object and person, 0x10 person), and the height test
// (od 4 bit 0 = on the floor). The VM entry points run as the binary runs
// them (§4): 9 placement on a position change, 10 pickup out of the world,
// 11 user placement on the click (if od 40 bit 0x200), 12 user pickup, 50
// user delete, 49 delete and 3 cleanup when destroyed.
//
// Multi-tile objects: each part's OBJD "sub index" holds its tile offset,
// x in the low byte and y in the high byte (signed) in the object's frame;
// direction d turns it clockwise by 90 degrees per 2 (checked against the
// saved lots: a double bed facing 0 has B at +y and C at +x, a loveseat
// facing 2 has (1,0) at +x).
//
// Inferred or left out (TODO.md): od 4 compared with the slot's height
// bit (FreeSO's rule for The Sims 1) and the weight against the support
// strength; Sims block placement; slopes, water, pools, ceilings, room
// placement (od 30), diagonal wall flags, the footprints inside a tile and
// the "allow intersection" tree (entry 5) are not tested.

#include "vm.hpp"

#include <optional>
#include <string>
#include <vector>

namespace os2::vm {

namespace objdata {
constexpr int16_t AllowedHeightFlags = 4;
constexpr int16_t WallAdjacencyFlags = 5;
constexpr int16_t WallPlacementFlags = 13;
constexpr int16_t Weight = 27;
constexpr int16_t SupportStrength = 28;
constexpr int16_t CurrentValue = 41;
constexpr int16_t FlagField2 = 40; // bit 0x200: the user placement tree runs
constexpr int16_t PlacementFlags = 42;
constexpr int16_t ExclusivePlacementFlags = 63;
constexpr int16_t PlacementFlags2 = 82;
} // namespace objdata

// OBJf entry points of placement (STR# 0xF5).
namespace entry {
constexpr size_t Placement = 9;
constexpr size_t Pickup = 10;
constexpr size_t UserPlacement = 11;
constexpr size_t UserPickup = 12;
constexpr size_t Cleanup = 3;
constexpr size_t Delete = 49;
constexpr size_t UserDelete = 50;
} // namespace entry

// The tile offset of a multi-tile part (sub index) facing `direction`.
std::array<int, 2> partOffset(uint16_t subIndex, int direction);

// Placement error codes (cTSSimSystem::PlacementErrorString): the text is
// string code - 2 of UIText.package's STR# 0x89 ("Local fora dos limites",
// ...); the list codes (0x19, 0x8C-0x91) show its "Sua colocação só pode
// ser em um destes lugares:" header (string 90) and the allowed places.
namespace placementerror {
constexpr int OutOfBounds = 2, LevelOutOfBounds = 3, NeedsWall = 7, WallIntersect = 8, Objects = 0x0C,
              NoPlace = 0x0E, Weight = 0x0F, PlaceList = 0x19, NoFloor = 0x8E;
// cTSSimSystem::PlacementErrorString (Mac BV 0x58a6d4): the code's text,
// as a 1-based entry of UIText STR# 0x89; a code out of its table is 1
// "Location out of bounds"; 0 for the codes whose entry is empty in the base
// game (0x14, 0x1E, 0x46, ...: expansion texts). docs/buy-build-mac.md §7.10.
int stringNumber(int code);
// The 0-based index of a code's text in STR# 0x89 (-1: no text).
inline int textIndex(int code) { return stringNumber(code) - 1; }
} // namespace placementerror

struct Placement {
    bool ok = false;
    int error = 0;      // placementerror code when not ok
    std::string reason; // why not, in English (empty when ok)
    int level = 1, direction = 0;
    // Each part and the tile it goes to.
    struct Part {
        ObjectId id = 0;
        TilePos tile;
        // Off the tile's centre (Build mode's modular stairs: a step every
        // quarter tile, at its own height): where the part goes instead.
        std::optional<Position> at;
    };
    std::vector<Part> parts;
    // Into a container slot of an object (counters, tables): the container
    // and the slot number (counting the SLOT's container entries).
    ObjectId container = 0;
    int slot = -1;
};

// The step to the tile on a side of an object facing `direction`: 0 front,
// 1 right, 2 behind, 3 left (the order of the wall flags, STR# 0xE5).
std::array<int, 2> sideStep(int direction, int side);

// The objects of `id`'s multi-tile group (just `id` for a single-tile one).
std::vector<ObjectId> groupOf(VM& vm, ObjectId id);

// Whether the object `id` (the lead of its group; it may be in the world:
// its own tiles do not count) fits with its lead part on `tile` facing
// `direction` (0, 2, 4, 6), and how. Without `slots` the position tests
// alone (TSObjectCanSetPosition, BV 0x8ae264 -> TestPlaceInWorld: no free
// container slot is looked for, as Create New Object Instance asks).
// With `footprints` the objects' test is the binary's
// TestPlacementWithRespectToObject (BV 0x8ac158) by footprints: the parts
// must already stand where they are tested (their footprints are taken
// where they are; Create New Object Instance places the new object first).
Placement checkPlacement(VM& vm, ObjectId id, TilePos tile, int direction, bool slots = true,
                         bool footprints = false);

// Where the group of `id` is now (its lead's direction, level, container
// slot and each part's tile), for putting it back with applyPlacement.
Placement currentPlacement(VM& vm, ObjectId id);

// Puts the group where `p` says (positions, direction, container slot) and
// runs the placement tree of each part. Does not check.
void applyPlacement(VM& vm, ObjectId id, const Placement& p);
// The user placement tree (entry 11) of the parts whose od 40 has 0x200.
void runUserPlacement(VM& vm, ObjectId id);
// Takes the group out of the world (the pickup tree runs).
void pickUp(VM& vm, ObjectId id);

// A new object from a catalog OBJD, out of the world, as
// MakeNewOutOfWorldObject makes it: each part's Init runs and its current
// value (od 41) becomes its OBJD price (no Main yet; JustBorn and Reset
// are not modelled): the ghost Buy mode moves around. Returns the lead.
ObjectId createGhost(VM& vm, uint32_t group, uint16_t definitionInstance);
// Test aid (s2pkg pie --create, the app's --vm-create): "GUID[:attr=value,
// ...]" made as createGhost makes it, with those attributes (e.g. a party
// controller, "0x2C91C16A:2=1", for the birthday cinematic). Throws when
// the GUID has no OBJD.
ObjectId createTestObject(VM& vm, const std::string& spec);
// Removes a group from the VM at once (a ghost given up, an object sold
// at the end of the session): the delete (49) and cleanup (3) trees run.
void deleteGroup(VM& vm, ObjectId id);

// Places a ghost as bought: the user placement tree, then the parts' Mains
// start; the current family pays `price` (false, nothing done, when the
// funds are short or it does not fit).
bool buy(VM& vm, ObjectId ghost, const Placement& p, int price, std::string* why = nullptr);

// What selling gives back: the lead's current value (object data 41).
int saleValue(VM& vm, ObjectId id);

// Selling a placed object (the remove handler of the undoable, §2): the
// user delete tree (entry 50, single-tile objects; the group handles
// multi-tile ones), out of the world, and the family gets its sale value
// back. The object is not destroyed: Buy mode keeps it for undo until the
// session ends (deleteGroup). Returns the refund.
int sell(VM& vm, ObjectId id);

// A staircase step's railing model (RailingObjectModel, BV 0x523d10; the
// same code in the modular stairs' undoable, 0x3ff17c): the object's OBJf
// entry 48 (the stairs' "Function - Get Model Type") run as a check tree
// (cTSObject::RunTree 0x30), its temp 0 when it returns true, else 1.
// 1 = no railing, 2 / 3 = a railing on one side (cTSUserToolStairsRailing:
// left or right), 4 = both sides.
int railingObjectModel(VM& vm, ObjectId id);
// RailingCost (BV 0x523da4): the railing's price (VM::railingPrice) once for
// models 2 and 3, twice for 4, nothing for 0 and 1. ComputeStats adds it to
// a staircase's value (types 0xB / 0xC) and RemoveFlight to every part's
// refund.
int railingCost(VM& vm, ObjectId id);
// cTSUserToolUndoableObjectModelSwap: the object's OBJf entry 47 (the
// stairs' "Function - Set Model by Type": graphic = OBJD field 0x48 +
// 3 x (model - 1), which picks the model of STR# 0x85) with the model as
// its parameter 0.
void setRailingModel(VM& vm, ObjectId id, int model);
// The Railing Tool's side (cTSUserToolStairsRailing::OnAnyMouseEvent): for
// modular stairs (types 11, 12) PointIsOnLeftSideOfObject at the point under
// the mouse, for a staircase object (type 10) its negation (left of the
// object's facing).
bool railingSide(VM& vm, ObjectId id, float x, float y);

// A day passed (cTSObject::DayPassed, docs/buy-build-mac.md §2): every
// object not "self depreciating" (OBJD field 37) loses its "initial
// depreciation" (field 35) when it has one, else its "daily depreciation"
// (36), down to the "depreciation limit" (38); multi-tile objects by
// their lead.
void dayPassed(VM& vm);

// cTSLot::ComputeAndStoreLotData (BV 0x46fbd4) with cEdithObjectModule::
// ComputeStats (BV 0x52c638; docs/neighborhood-mac.md §11.1): the current
// value (od 41) of every lead object in the world, not of type 7 nor with
// catalog field 40 bit 0x400, goes to the family's objects when
// cTSObject::IsDeletedByEvict (a Buy mode object, OBJD field 0x45 "build
// mode type" 0 -- of the catalog definition, a multi-tile part's master's:
// a door's parts have 0, its master 8 -- without "stays after evict", od 43
// bit 0x10), else to the
// lot's objects when it is a Build mode object or of type 0xB/0xC (then
// plus its railing, railingCost); the decompiled
// ComputeStats adds a Build mode object to the lot and a Buy mode one only
// for those two types (the round-10 RE note's table reads it the other
// way). Then the simulator's
// setters: globals 48/49 = (lot + family objects) / 1000 and the rest,
// 46/47 = the lot value / 1000 and the rest (the land, kept from the
// SIMI: the lot's size does not change in play). The game runs it on the
// day's turn (message 0x89) and when the lot changes (0x8B); here at the
// day's turn, after buying, selling or deleting, and when a lot loads.
void computeLotData(VM& vm);

// The current family's funds (the Budget globals, else the family's).
int32_t funds(VM& vm);
void addFunds(VM& vm, int32_t amount);

// Buy and build cost nothing on a lot with no family: every buy/build
// undoable's Init (Mac BV 0x9b47a4) and cTSUserToolBuyBuild's (0x9b36e0) set
// "no charge" when cTSGameStateController::CurrentFamily() (vtable 0x38) is
// null, unless the UI bridge has a selected person (cTSUIBridge::
// GetSelectedPerson, vtable 0x24; here global 3 naming a Sim on the lot,
// inferred mapping). Then cTSUserToolUndoable::TotalCost (0x9b5568) is 0, so
// nothing is charged or refunded (selling gives nothing back either), and
// cTSUserToolObject::TryDrop (0x2111b4) skips the funds check.
bool noCharge(VM& vm);
// cTSUserToolBuyBuild::CanAfford (0x9b38fc): cost <= 0 or <= the funds;
// with no charge the cost is 0.
bool canAfford(VM& vm, int64_t cost);
// Funds += amount unless there is no charge (the buy/build tools' charges,
// refunds and their undo/redo; cheats that set the funds use addFunds).
void chargeFunds(VM& vm, int32_t amount);

} // namespace os2::vm
