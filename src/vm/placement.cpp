// Buy mode's object placement (placement.hpp).

#include "placement.hpp"

#include "hood_bridge.hpp"
#include "formats/build_catalog.hpp"
#include "formats/resindex.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>
#include <tuple>

namespace os2::vm {

int placementerror::stringNumber(int code) {
    struct Entry {
        uint8_t code, number;
    };
    // The function's table, code -> 1-based STR# 0x89 entry.
    static constexpr Entry kTable[] = {{0x01, 74}, {0x03, 2}, {0x04, 3}, {0x06, 5}, {0x07, 6}, {0x08, 7}, {0x0a, 9}, {0x0b, 10}, {0x0c, 11}, {0x0e, 13}, {0x0f, 14}, {0x10, 15}, {0x11, 16}, {0x12, 17}, {0x13, 32}, {0x16, 21}, {0x17, 22}, {0x18, 23}, {0x19, 91}, {0x1a, 25}, {0x1b, 26}, {0x1c, 63}, {0x21, 32}, {0x22, 33}, {0x23, 34}, {0x24, 35}, {0x25, 91}, {0x26, 37}, {0x27, 38}, {0x28, 39}, {0x29, 40}, {0x2a, 41}, {0x2b, 42}, {0x2c, 43}, {0x2d, 44}, {0x2e, 45}, {0x2f, 8}, {0x30, 97}, {0x31, 4}, {0x32, 91}, {0x33, 91}, {0x34, 91}, {0x35, 91}, {0x37, 91}, {0x38, 93}, {0x39, 38}, {0x3a, 38}, {0x3b, 98}, {0x41, 7}, {0x43, 7}, {0x44, 6}, {0x48, 32}, {0x49, 17}, {0x4a, 76}, {0x4b, 75}, {0x4c, 47}, {0x4d, 48}, {0x4e, 49}, {0x4f, 50}, {0x50, 94}, {0x52, 32}, {0x53, 17}, {0x58, 85}, {0x5a, 51}, {0x5b, 52}, {0x5c, 53}, {0x61, 54}, {0x63, 55}, {0x64, 56}, {0x65, 57}, {0x66, 58}, {0x68, 59}, {0x69, 60}, {0x6a, 61}, {0x6b, 55}, {0x6c, 62}, {0x6d, 63}, {0x6e, 64}, {0x6f, 65}, {0x70, 66}, {0x71, 67}, {0x72, 68}, {0x73, 69}, {0x74, 69}, {0x75, 70}, {0x76, 7}, {0x77, 71}, {0x78, 72}, {0x79, 79}, {0x7a, 11}, {0x7b, 73}, {0x7c, 73}, {0x7f, 11}, {0x80, 73}, {0x81, 12}, {0x82, 76}, {0x83, 11}, {0x84, 7}, {0x85, 77}, {0x86, 78}, {0x87, 95}, {0x88, 79}, {0x89, 86}, {0x8a, 90}, {0x8b, 89}, {0x8c, 91}, {0x8d, 91}, {0x8e, 91}, {0x8f, 91}, {0x90, 91}, {0x91, 91}, {0x92, 96}};
    static constexpr uint8_t kEmpty[] = {0x14, 0x15, 0x1d, 0x1e, 0x1f, 0x3c, 0x3d, 0x3e, 0x3f, 0x46, 0x47, 0x54, 0x55, 0x56, 0x57,
                                         0x5d, 0x5e, 0x5f, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0x9b, 0x9c, 0x9d, 0xa5};
    for (const Entry& e : kTable)
        if (e.code == code)
            return e.number;
    for (uint8_t e : kEmpty)
        if (e == code)
            return 0;
    return 1;
}

namespace {

// Object data 8 "flags" bits (STR# 0x8E).
constexpr int16_t kAllowObjectIntersection = 0x02, kAllowObjectAndPerson = 0x04, kAllowPerson = 0x10;
// Exclusive placement layers (STR# 0xFB).
constexpr int16_t kLayerFloor = 0x01, kLayerWall = 0x02, kLayerCeiling = 0x08;

int16_t layers(const Object& o) {
    const int16_t l = o.data[objdata::ExclusivePlacementFlags] & (kLayerFloor | kLayerWall | kLayerCeiling);
    return l ? l : kLayerFloor;
}

int normDir(int d) { return ((d % 8) + 8) % 8; }

// Container slots of an object's SLOT, in slot-number order.
std::vector<const Slots::Slot*> containerSlots(VM& vm, const Object& o) {
    std::vector<const Slots::Slot*> out;
    if (!o.definition)
        return out;
    if (const Slots* s = vm.code().slots(o.group, o.definition->slotId()))
        for (const auto& slot : s->slots)
            if (slot.type == 0)
                out.push_back(&slot);
    return out;
}

void runEntry(VM& vm, ObjectId id, size_t function) {
    try {
        vm.runFunction(id, function);
    } catch (const VMError&) {
        // A tree that fails does not stop the placement (as with Inits).
    }
}

} // namespace

// The step to the tile on a side of an object facing `direction`: 0 front,
// 1 right, 2 behind, 3 left (the order of the wall flags, STR# 0xE5).
std::array<int, 2> sideStep(int direction, int side) {
    const auto f = directionStep(direction);
    switch (side) {
    case 0: return f;
    case 1: return {f[1], -f[0]}; // clockwise of the front
    case 2: return {-f[0], -f[1]};
    default: return {-f[1], f[0]};
    }
}


void runUserPlacement(VM& vm, ObjectId id) {
    for (ObjectId part : groupOf(vm, id))
        if (const Object* o = vm.object(part); o && (o->data[objdata::FlagField2] & 0x200))
            runEntry(vm, part, entry::UserPlacement);
}

std::array<int, 2> partOffset(uint16_t subIndex, int direction) {
    int x = int8_t(subIndex & 0xFF), y = int8_t(subIndex >> 8);
    for (int i = 0; i < normDir(direction) / 2; ++i) {
        const int nx = y, ny = -x; // a quarter turn clockwise
        x = nx, y = ny;
    }
    return {x, y};
}

std::vector<ObjectId> groupOf(VM& vm, ObjectId id) {
    const Object* o = vm.object(id);
    if (!o)
        return {};
    const ObjectId lead = multiTileGroup(*o);
    std::vector<ObjectId> out;
    for (const auto& [other, x] : vm.eachObject())
        if (other == lead || x.data[objdata::LeadTileObjectId] == lead)
            out.push_back(other);
    if (std::find(out.begin(), out.end(), id) == out.end())
        out.push_back(id);
    return out;
}

Placement checkPlacement(VM& vm, ObjectId id, TilePos tile, int direction, bool slots, bool footprints) {
    Placement p;
    p.level = tile.level;
    p.direction = normDir(direction);
    const Object* lead = vm.object(id);
    auto fail = [&](int code, std::string why) {
        p.ok = false;
        p.error = code;
        p.reason = std::move(why);
        return p;
    };
    if (!lead || !lead->definition)
        return fail(placementerror::NoPlace, "no such object");
    if (!vm.world)
        return fail(placementerror::OutOfBounds, "no lot");
    const World& w = *vm.world;
    const std::vector<ObjectId> group = groupOf(vm, id);
    const std::set<ObjectId> mine(group.begin(), group.end());
    const uint16_t leadSub = lead->definition->subIndex() == 0xFFFF ? 0 : lead->definition->subIndex();
    const auto leadOff = partOffset(leadSub, p.direction);
    // OBJD field 0xB0 "level offset": parts on the levels above (the
    // upper half of a staircase).
    const int leadLevel = int16_t(lead->definition->field(0xB0));
    for (ObjectId part : group) {
        const Object* o = vm.object(part);
        const uint16_t sub = o->definition && o->definition->masterId() ? o->definition->subIndex() : 0;
        const auto off = partOffset(sub, p.direction);
        const int dl = o->definition ? int16_t(o->definition->field(0xB0)) - leadLevel : 0;
        p.parts.push_back({part, {tile.x + off[0] - leadOff[0], tile.y + off[1] - leadOff[1], tile.level + dl}});
    }
    std::set<std::tuple<int, int, int>> tiles;
    for (const auto& part : p.parts)
        tiles.insert({part.tile.x, part.tile.y, part.tile.level});

    // Step 4 of TryPlace: a free slot of an object on the tile that takes
    // this one (single-tile objects): the allowed heights hold the slot's
    // height bit (FreeSO's rule for The Sims 1) and the surface is strong
    // enough. The first such slot wins (FindFirstAvailableSlot).
    if (slots && group.size() == 1) {
        const int16_t heights = lead->data[objdata::AllowedHeightFlags];
        bool tooHeavy = false;
        for (ObjectId other : vm.objectsOnTile(tile)) {
            const Object& x = *vm.object(other);
            if (mine.count(other) || x.person)
                continue;
            const auto slots = containerSlots(vm, x);
            for (size_t i = 0; i < slots.size() && !p.container; ++i) {
                const int h = slots[i]->height;
                if (h < 1 || h > 16 || !(heights & (1 << (h - 1))))
                    continue;
                if (const ObjectId held = vm.containedIn(other, int16_t(i)); held && !mine.count(held))
                    continue;
                if (lead->data[objdata::Weight] >= x.data[objdata::SupportStrength]) {
                    tooHeavy = true;
                    continue;
                }
                p.container = other, p.slot = int(i);
            }
            if (p.container)
                break;
        }
        if (p.container) {
            p.ok = true;
            return p;
        }
        if (tooHeavy && !(heights & 1))
            return fail(placementerror::Weight, "the surface cannot hold it");
    }

    for (const auto& part : p.parts) {
        const Object& o = *vm.object(part.id);
        const TilePos t = part.tile;
        const int16_t pf = o.data[objdata::PlacementFlags], pf2 = o.data[objdata::PlacementFlags2];
        // TestPlacementCommon (docs/buy-build-mac.md §3.2): 1 in the lot,
        // 2 a level of the lot, 3 a floor under it (not in the air), 7 the
        // floor or terrain the placement flags allow (on the ground the
        // world cannot tell floor from terrain: either flag will do).
        if (t.x < 0 || t.y < 0 || !w.contains({t.x, t.y, w.firstLevel()}))
            return fail(placementerror::OutOfBounds, "out of the lot");
        // TSObjectIsInLockedArea / IsOnLockedTile (BV 0x8a9d00, 0x8a66b0): a
        // tile on the lot that is locked (the road strip, cIWorldDB slot 0xc4)
        // -> 0x02, unless the object's placement flags (od 42) have 0x40.
        // (Objects whose AllowOnlyOnLockedTile bit is set, cTSObject +0xea
        // bit 10, must stand on locked tiles instead: not mapped here.)
        if (!(pf & 0x40) && w.locked(t))
            return fail(placementerror::OutOfBounds, "on a locked tile (the road strip)");
        if (!w.contains(t))
            return fail(placementerror::LevelOutOfBounds, "level out of the lot");
        // (The ground, VM level 1, is terrain even when a pool gives the lot
        // a level under it.)
        if (t.level != 1 && t.level > w.firstLevel() && !w.hasFloor(t) && !(pf & 0x200) && !(pf2 & 0x04))
            return fail(placementerror::NoFloor, "no floor");
        // 5 a pool tile needs p & 0x800 (0x8F); pool objects need no floor there.
        const bool inPool = w.pool(t);
        // (Parts in the air, p & 0x200, or on the water, 0x4000, may hang over it: inferred.)
        if (inPool && !(pf & (0x800 | 0x200 | 0x4000)))
            return fail(0x8F, "not in a pool");
        if (pf && !(pf & 0x03) && !(pf & 0x200) && !(inPool && (pf & 0x800)))
            return fail(placementerror::NoFloor, inPool || !(pf & 0x800) ? "not allowed on the floor" : "only in a pool");
        // TestPlacementWithRespectToWalls (§3.3): the wall flags (od 13) on
        // the four sides, and no wall through the object (unless p & 0x400).
        if (w.diagonalWall(t) && !(pf & 0x400))
            return fail(placementerror::WallIntersect, "diagonal wall");
        // Doors and windows are wall portals (IsWallPortal, §3.3): the wall
        // runs between their parts.
        const bool portal = o.definition && (o.definition->objectType() == kObjectTypeDoor ||
                                             o.definition->objectType() == kObjectTypeWindow);
        if (!(pf & 0x400) && !portal)
            for (int k = 0; k < 4; ++k) {
                const auto s = directionStep(k * 2);
                if (tiles.count({t.x + s[0], t.y + s[1], t.level}) && w.wallBetween(t, s[0], s[1], true))
                    return fail(placementerror::WallIntersect, "a wall cuts the object");
            }
        const int16_t wp = o.data[objdata::WallPlacementFlags];
        static const char* const sides[4] = {"in front", "on the right", "behind", "on the left"};
        for (int side = 0; side < 4; ++side) {
            const auto s = sideStep(p.direction, side);
            // For pool objects (p & 0x800) the pool's edge is a wall (the
            // pool walls stand a level down; "Must be against a pool wall").
            const bool poolEdge = (pf & 0x800) && w.pool(t) != w.pool({t.x + s[0], t.y + s[1], t.level});
            const bool wall = w.wallBetween(t, s[0], s[1], true) || poolEdge;
            if ((wp & (1 << side)) && !wall)
                return fail(placementerror::NeedsWall, std::string("needs a wall ") + sides[side]);
            if ((wp & (0x100 << side)) && wall)
                return fail(placementerror::WallIntersect, std::string("no wall allowed ") + sides[side]);
        }
        // TestPlacementWithRespectToObjects / TestPlacementWithRespectToObject
        // (BV 0x8ace04, 0x8ac158) by footprints: each object on the tiles
        // round the part (not the object's own parts) whose footprint
        // (TSObjectWorldSpaceFootprint; a person's box of half width 0.1)
        // crosses the part's. A person is in the way unless the part lets
        // people through (CanIntersectPeople: od 8 & 0x14); another object
        // unless either lets objects through (CanIntersectObjects: od 8 &
        // 6) or the two are on different layers (FootprintBlocksCeiling, od
        // 42 bit 0x1000, against FootprintBlocksFloor, the bit clear). Not
        // done: the "allow intersection" trees (entry 5) both ways, the
        // doors' circle and footprint tests, the roof clearance.
        if (footprints) {
            const auto own = vm.routingFootprint(o);
            constexpr float kHalf = 0.1f; // TSObjectBaseFootprintWidth / 2
            const bool letsPeople = o.data[objdata::Flags] & 0x14;
            const bool letsObjects = o.data[objdata::Flags] & 0x06;
            const bool ceiling = o.data[objdata::PlacementFlags] & 0x1000;
            auto crosses = [&](const std::vector<std::array<float, 4>>& other) {
                for (const auto& a : own)
                    for (const auto& b : other)
                        if (a[0] < a[2] && a[1] < a[3] && b[0] < b[2] && b[1] < b[3] && a[0] < b[2] && b[0] < a[2] &&
                            a[1] < b[3] && b[1] < a[3])
                            return true;
                return false;
            };
            for (int dx = -1; dx <= 1; ++dx)
                for (int dy = -1; dy <= 1; ++dy)
                    for (ObjectId other : vm.objectsOnTile({t.x + dx, t.y + dy, t.level})) {
                        if (mine.count(other))
                            continue;
                        const Object& x = *vm.object(other);
                        if (x.person) {
                            if (letsPeople || ceiling)
                                continue;
                            const float px = x.position.x, py = x.position.y;
                            if (crosses({{px - kHalf, py - kHalf, px + kHalf, py + kHalf}}))
                                return fail(placementerror::Objects, "a Sim is in the way");
                            continue;
                        }
                        if (letsObjects || (x.data[objdata::Flags] & 0x06) ||
                            ceiling != bool(x.data[objdata::PlacementFlags] & 0x1000))
                            continue;
                        if (crosses(vm.routingFootprint(x)))
                            return fail(placementerror::Objects, "\"" + (x.definition ? x.definition->fileName : std::string("?")) +
                                                                     "\" is in the way");
                    }
            continue;
        }
        // TestPlacementWithRespectToObjects (§3.4): objects on the tile in
        // a common exclusive layer (od 63), unless od 8 lets them through.
        const int16_t myLayers = layers(o);
        const bool meLetsPeople = o.data[objdata::Flags] & (kAllowObjectAndPerson | kAllowPerson);
        for (ObjectId other : vm.objectsOnTile(t)) {
            if (mine.count(other))
                continue;
            const Object& x = *vm.object(other);
            if (x.person) {
                if (!meLetsPeople)
                    return fail(placementerror::Objects, "a Sim is in the way");
                continue;
            }
            const bool sameType = (o.data[objdata::ExclusivePlacementFlags] & 0x04) && x.definition &&
                                  o.definition && x.definition->objectType() == o.definition->objectType();
            if (!(layers(x) & myLayers) && !sameType)
                continue;
            if (!sameType &&
                ((x.data[objdata::Flags] | o.data[objdata::Flags]) & (kAllowObjectIntersection | kAllowObjectAndPerson)))
                continue;
            return fail(placementerror::Objects,
                        "\"" + (x.definition ? x.definition->fileName : std::string("?")) + "\" is in the way");
        }
    }
    // The height test: on the floor needs bit 0 of od 4; otherwise only a
    // slot will do ("Must place in one of these places").
    if (!(lead->data[objdata::AllowedHeightFlags] & 1))
        return fail(placementerror::PlaceList, "needs a surface");
    p.ok = true;
    return p;
}

Placement currentPlacement(VM& vm, ObjectId id) {
    Placement p;
    const Object* lead = vm.object(id);
    if (!lead)
        return p;
    p.ok = lead->position.inWorld();
    p.level = lead->position.level;
    p.direction = lead->data[objdata::Direction];
    p.container = lead->data[objdata::ContainerId];
    p.slot = p.container ? lead->data[objdata::SlotNumber] : -1;
    for (ObjectId part : groupOf(vm, id))
        if (const Object* o = vm.object(part))
            p.parts.push_back({part, VM::tileOf(*o)});
    return p;
}

void applyPlacement(VM& vm, ObjectId id, const Placement& p) {
    for (const auto& part : p.parts) {
        Object* o = vm.object(part.id);
        if (!o)
            continue;
        o->data[objdata::Direction] = int16_t(p.direction);
        o->position = part.at ? *part.at : Position{float(part.tile.x) + 0.5f, float(part.tile.y) + 0.5f, part.tile.level};
        if (p.container) {
            o->data[objdata::ContainerId] = p.container;
            o->data[objdata::SlotNumber] = int16_t(p.slot);
            if (const Object* c = vm.object(p.container))
                o->position = c->position;
        } else {
            o->data[objdata::ContainerId] = 0;
            o->data[objdata::SlotNumber] = -1;
        }
    }
    // The pool ladder's and diving board's portals follow it (build mode
    // does the same, build_edit.cpp).
    vm.refreshObjectPortals();
    vm.planner.reset();
    for (const auto& part : p.parts)
        runEntry(vm, part.id, entry::Placement);
    (void)id;
}

void pickUp(VM& vm, ObjectId id) {
    for (ObjectId part : groupOf(vm, id)) {
        Object* o = vm.object(part);
        if (!o)
            continue;
        o->position = Position{};
        o->data[objdata::ContainerId] = 0;
        o->data[objdata::SlotNumber] = -1;
        runEntry(vm, part, entry::Pickup);
    }
    vm.refreshObjectPortals();
    vm.planner.reset();
}

ObjectId createGhost(VM& vm, uint32_t group, uint16_t definitionInstance) {
    std::vector<ObjectId> before;
    for (const auto& [id, o] : vm.eachObject())
        before.push_back(id);
    Object& lead = vm.instantiate(group, definitionInstance);
    const ObjectId leadId = lead.id;
    for (ObjectId part : groupOf(vm, leadId)) {
        Object* o = vm.object(part);
        o->position = Position{};
        o->data[objdata::ContainerId] = 0;
        o->data[objdata::SlotNumber] = -1;
    }
    // As cEdithObjectModule::MakeNewOutOfWorldObject (docs/primitives-mac.md
    // §4c): the Init of each part, then the current value = the OBJD's price.
    for (ObjectId part : groupOf(vm, leadId)) {
        runEntry(vm, part, 0); // Init
        if (Object* o = vm.object(part); o && o->definition)
            o->data[objdata::CurrentValue] = int16_t(std::min<int>(o->definition->price(), 32767));
    }
    return leadId;
}

ObjectId createTestObject(VM& vm, const std::string& spec) {
    const auto colon = spec.find(':');
    const uint32_t guid = uint32_t(std::stoul(spec.substr(0, colon), nullptr, 0));
    const auto at = vm.code().definitionByGuid(guid);
    if (!at)
        throw std::runtime_error("no OBJD with GUID " + spec.substr(0, colon));
    const ObjectId id = createGhost(vm, at->first, at->second);
    Object& o = *vm.object(id);
    for (size_t a = colon == std::string::npos ? spec.size() : colon + 1, b; a < spec.size(); a = b + 1) {
        b = spec.find(',', a);
        if (b == std::string::npos)
            b = spec.size();
        const std::string kv = spec.substr(a, b - a);
        const auto eq = kv.find('=');
        const size_t attr = std::stoul(kv.substr(0, eq));
        if (o.attributes.size() <= attr)
            o.attributes.resize(attr + 1);
        o.attributes[attr] = int16_t(std::stoi(kv.substr(eq + 1)));
    }
    return id;
}

void deleteGroup(VM& vm, ObjectId id) {
    for (ObjectId part : groupOf(vm, id)) {
        // What it holds goes with it.
        std::vector<ObjectId> held;
        for (const auto& [other, x] : vm.eachObject())
            if (x.data[objdata::ContainerId] == part && other != part)
                held.push_back(other);
        for (ObjectId h : held)
            deleteGroup(vm, h);
        runEntry(vm, part, entry::Delete);
        runEntry(vm, part, entry::Cleanup);
        vm.threads.erase(part);
        vm.removeObject(part);
    }
    vm.refreshObjectPortals();
    vm.planner.reset();
    computeLotData(vm);
}

int32_t funds(VM& vm) {
    const int16_t family = vm.globals[global::CurrentFamily];
    if (family && vm.families.count(family))
        return familyFunds(vm, family);
    return int32_t(vm.globals[global::BudgetDiv10000]) * 10000 + vm.globals[global::BudgetMod10000];
}

void addFunds(VM& vm, int32_t amount) {
    const int32_t now = funds(vm) + amount;
    const int16_t family = vm.globals[global::CurrentFamily];
    if (family && vm.families.count(family)) {
        setFamilyFunds(vm, family, now);
        return;
    }
    vm.globals[global::BudgetDiv10000] = int16_t(now / 10000);
    vm.globals[global::BudgetMod10000] = int16_t(now % 10000);
}

bool noCharge(VM& vm) {
    // Global 9 names the lot's family (LoadLot); a lot run without its
    // neighborhood still has one when it names one.
    if (vm.globals[global::CurrentFamily] != 0)
        return false;
    const int16_t selected = vm.globals[global::SelectedPersonId];
    return !(selected > 0 && vm.object(ObjectId(selected)));
}

bool canAfford(VM& vm, int64_t cost) {
    return cost <= 0 || noCharge(vm) || cost <= funds(vm);
}

void chargeFunds(VM& vm, int32_t amount) {
    if (amount && !noCharge(vm))
        addFunds(vm, amount);
}

bool buy(VM& vm, ObjectId ghost, const Placement& p, int price, std::string* why) {
    auto refuse = [&](const std::string& r) {
        if (why)
            *why = r;
        return false;
    };
    if (!p.ok)
        return refuse(p.reason.empty() ? "does not fit" : p.reason);
    if (!canAfford(vm, price))
        return refuse("not enough funds");
    applyPlacement(vm, ghost, p);
    runUserPlacement(vm, ghost);
    chargeFunds(vm, -price);
    for (const auto& part : p.parts)
        vm.startMain(part.id);
    computeLotData(vm);
    return true;
}

int sell(VM& vm, ObjectId id) {
    const int value = saleValue(vm, id);
    const auto group = groupOf(vm, id);
    if (group.size() == 1)
        runEntry(vm, id, entry::UserDelete);
    pickUp(vm, id);
    if (noCharge(vm))
        return computeLotData(vm), 0; // TotalCost is 0: nothing back
    addFunds(vm, value);
    computeLotData(vm);
    return value;
}

int railingObjectModel(VM& vm, ObjectId id) {
    try {
        if (auto t = vm.runFunction(id, 48); t && t->status == Thread::Status::Finished && t->result)
            return t->temps[0];
    } catch (const VMError&) {
    }
    return 1;
}

int railingCost(VM& vm, ObjectId id) {
    if (!vm.railingPrice)
        if (ResourceIndex* index = vm.code().index())
            vm.railingPrice = railingToolPrice(*index);
    const int model = railingObjectModel(vm, id);
    if (model == 4)
        return 2 * vm.railingPrice;
    return model > 1 && model < 4 ? vm.railingPrice : 0;
}

void setRailingModel(VM& vm, ObjectId id, int model) {
    const int16_t args[1] = {int16_t(model)};
    try {
        vm.runFunction(id, 47, 1000, args);
    } catch (const VMError&) {
    }
}

bool railingSide(VM& vm, ObjectId id, float x, float y) {
    const Object* o = vm.object(id);
    if (!o || !o->definition)
        return false;
    // The binary compares one coordinate by the facing (0: x, 2: y, 4: x,
    // 6: y, its own axes); here the side of the facing's line the point is
    // on, left = counter-clockwise of the facing (inferred mapping of the
    // axes).
    const auto [fx, fy] = directionStep(o->data[objdata::Direction]);
    const bool left = -float(fy) * (x - o->position.x) + float(fx) * (y - o->position.y) > 0;
    return o->definition->objectType() == 10 ? !left : left;
}

void dayPassed(VM& vm) {
    for (const auto& [id, o] : vm.eachObject()) {
        if (!o.definition || o.person || multiTileGroup(o) != id)
            continue;
        const ObjectDefinition& d = *o.definition;
        if (d.field(0x8A) != 0) // self depreciating
            continue;
        const int dep = d.field(0x86) ? int16_t(d.field(0x86)) : int16_t(d.field(0x88));
        const int limit = int16_t(d.field(0x8C));
        if (dep == 0 && limit == 0)
            continue;
        Object* x = vm.object(id);
        int v = x->data[objdata::CurrentValue] - dep;
        if (v < limit)
            v = limit;
        x->data[objdata::CurrentValue] = int16_t(v);
    }
    computeLotData(vm); // cTSLot on the day's message 0x89
}

void computeLotData(VM& vm) {
    constexpr int16_t kMovementFlags = 43; // bit 0x10: stays after evict (STR# 0xCB #4)
    int32_t family = 0, lot = 0;
    for (const auto& [id, o] : vm.eachObject()) {
        if (!o.definition || o.person || multiTileGroup(o) != id)
            continue;
        // In the world: placed, or in a slot of something in the world.
        const Object* at = &o;
        for (int depth = 0; at && !at->position.inWorld() && at->data[objdata::ContainerId] && depth < 8; ++depth)
            at = vm.object(at->data[objdata::ContainerId]);
        if (!at || !at->position.inWorld())
            continue;
        const ObjectDefinition& d = *o.definition;
        const uint16_t type = d.objectType();
        if (type == 7 || (d.field(0x90) & 0x400))
            continue;
        const int value = o.data[objdata::CurrentValue];
        // GetBuildModeType: field 0x45 of the catalog definition, a
        // multi-tile part's master's (a door's parts have 0, the master 8).
        const ObjectDefinition* catalog = &d;
        if (const uint16_t master = d.masterId())
            if (auto instance = vm.code().masterDefinition(o.group, master))
                if (const ObjectDefinition* md = vm.code().definition(o.group, *instance))
                    catalog = md;
        const bool buyMode = catalog->field(0xCA) == 0;
        if (buyMode && !(o.data[kMovementFlags] & 0x10))
            family += value; // IsDeletedByEvict
        else if (!buyMode || type == 0xB || type == 0xC)
            lot += value + (type == 0xB || type == 0xC ? railingCost(vm, id) : 0); // stairs: + RailingCost
    }
    auto& v = vm.lotValues;
    if (!v.known)
        v.lot = int32_t(vm.globals[46]) * 1000 + vm.globals[47];
    v.lotObjects = lot;
    v.familyObjects = family;
    v.known = true;
    const int32_t total = lot + family;
    vm.globals[48] = int16_t(total / 1000);
    vm.globals[49] = int16_t(total - 1000 * (total / 1000));
    vm.globals[46] = int16_t(v.lot / 1000);
    vm.globals[47] = int16_t(v.lot - 1000 * (v.lot / 1000));
}

int saleValue(VM& vm, ObjectId id) {
    const Object* o = vm.object(id);
    if (!o)
        return 0;
    const Object* lead = vm.object(multiTileGroup(*o));
    return std::max<int>(0, (lead ? lead : o)->data[objdata::CurrentValue]);
}

} // namespace os2::vm
