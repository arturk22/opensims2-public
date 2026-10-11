#pragma once

// Buy mode's object placement in the VM: buying an object from the catalog
// onto the lot, moving and selling placed objects, with the game's
// placement rules. Plain C++ (the app's Buy panel and s2pkg drive it).
//
// What an object needs is what its Init leaves in its object data, labelled
// in the game's SimAntics editor (objects.package, group 0x7FE59FD0):
//   4  allowed height flags (STR# 0xCA): bit 0 on the floor; the bits of a
//      container slot's height (Slot::height h: bit h - 1) say which
//      surfaces it may stand on, as in The Sims 1 (FreeSO's GetObjPlace):
//      a phone 0x008E goes on tables and counters, a counter sink 0x0100
//      only into the counter's sink slot (height 9);
//   5  wall adjacency flags (STR# 0xD0, set by the engine: wall on left 1,
//      right 2, in front 3, behind 4 as bit numbers + 1);
//   8  flags (STR# 0x8E): bit 1 allow object intersection, 2 allow object
//      and person intersection, 4 allow person intersection;
//   13 wall placement flags (STR# 0xE5): bits 0-3 wall required in front /
//      on right / behind / on left, 8-11 wall not allowed there;
//   27 weight and 28 support strength (a surface holds lighter objects);
//   41 current value (what selling gives back);
//   42 placement flags (TS1 layout: 1 on floor, 2 on terrain, 0x200 in air);
//   63 exclusive placement flags (STR# 0xFB): 1 floor, 2 wall, 4 no other
//      object of the same type, 8 ceiling: objects only collide within a
//      layer, so a painting hangs over a chair (0 counts as floor).
// The OBJf entry points the editor documents (STR# 0xF5) run as the game
// says: 9 "placement" whenever the position changes, 10 "pickup" when it
// leaves the world, 11 "user placement" when the player clicks to place it,
// 12 "user pickup" before the player picks it up.
//
// Multi-tile objects: each part's OBJD "sub index" holds its tile offset,
// x in the low byte and y in the high byte (signed) in the object's frame;
// direction d turns it clockwise by 90 degrees per 2 (checked against the
// saved lots: a double bed facing 0 has B at +y and C at +x, a loveseat
// facing 2 has (1,0) at +x).
//
// Inferred (TODO.md): the slot rule and the weights as FreeSO has them for
// The Sims 1; objects that block routing block placement; Sims block it;
// a part's tiles may not have a wall between them; walls are checked on the
// tile's own edges; the "allow intersection" tree (OBJf 5) is not run.

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
constexpr int16_t PlacementFlags = 42;
constexpr int16_t ExclusivePlacementFlags = 63;
} // namespace objdata

// OBJf entry points of placement (STR# 0xF5).
namespace entry {
constexpr size_t Placement = 9;
constexpr size_t Pickup = 10;
constexpr size_t UserPlacement = 11;
constexpr size_t UserPickup = 12;
} // namespace entry

// The tile offset of a multi-tile part (sub index) facing `direction`.
std::array<int, 2> partOffset(uint16_t subIndex, int direction);

struct Placement {
    bool ok = false;
    std::string reason; // why not (empty when ok)
    int level = 1, direction = 0;
    // Each part and the tile it goes to.
    struct Part {
        ObjectId id = 0;
        TilePos tile;
    };
    std::vector<Part> parts;
    // Into a container slot of an object (counters, tables): the container
    // and the slot number (counting the SLOT's container entries).
    ObjectId container = 0;
    int slot = -1;
};

// The objects of `id`'s multi-tile group (just `id` for a single-tile one).
std::vector<ObjectId> groupOf(VM& vm, ObjectId id);

// Whether the object `id` (the lead of its group; it may be in the world:
// its own tiles do not count) fits with its lead part on `tile` facing
// `direction` (0, 2, 4, 6), and how.
Placement checkPlacement(VM& vm, ObjectId id, TilePos tile, int direction);

// Puts the group where `p` says (positions, direction, container slot) and
// runs the placement tree of each part. Does not check.
void applyPlacement(VM& vm, ObjectId id, const Placement& p);
// Takes the group out of the world (the pickup tree runs).
void pickUp(VM& vm, ObjectId id);

// A new object from a catalog OBJD, out of the world, with its Init run
// (no Main yet): the ghost Buy mode moves around. Returns the lead.
ObjectId createGhost(VM& vm, uint32_t group, uint16_t definitionInstance);
// Removes a group from the VM at once (a ghost given up, an object sold).
void deleteGroup(VM& vm, ObjectId id);

// Places a ghost as bought: the user placement tree, then the parts' Mains
// start; the current family pays `price` (false, nothing done, when the
// funds are short or it does not fit). Its current value starts at the
// price when the Init left none.
bool buy(VM& vm, ObjectId ghost, const Placement& p, int price, std::string* why = nullptr);

// What selling gives back: the lead's current value (object data 41).
int saleValue(VM& vm, ObjectId id);

// The current family's funds (the Budget globals, else the family's).
int32_t funds(VM& vm);
void addFunds(VM& vm, int32_t amount);

} // namespace os2::vm
