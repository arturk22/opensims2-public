// Buy mode's object placement (placement.hpp).

#include "placement.hpp"

#include "hood_bridge.hpp"

#include <algorithm>
#include <cmath>
#include <set>

namespace os2::vm {

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
    for (const auto& [other, x] : vm.objects())
        if (other == lead || x.data[objdata::LeadTileObjectId] == lead)
            out.push_back(other);
    if (std::find(out.begin(), out.end(), id) == out.end())
        out.push_back(id);
    return out;
}

Placement checkPlacement(VM& vm, ObjectId id, TilePos tile, int direction) {
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
    for (ObjectId part : group) {
        const Object* o = vm.object(part);
        const uint16_t sub = o->definition && o->definition->masterId() ? o->definition->subIndex() : 0;
        const auto off = partOffset(sub, p.direction);
        p.parts.push_back({part, {tile.x + off[0] - leadOff[0], tile.y + off[1] - leadOff[1], tile.level}});
    }
    std::set<std::pair<int, int>> tiles;
    for (const auto& part : p.parts)
        tiles.insert({part.tile.x, part.tile.y});

    // Step 4 of TryPlace: a free slot of an object on the tile that takes
    // this one (single-tile objects): the allowed heights hold the slot's
    // height bit (FreeSO's rule for The Sims 1) and the surface is strong
    // enough. The first such slot wins (FindFirstAvailableSlot).
    if (group.size() == 1) {
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
        if (!w.contains(t))
            return fail(placementerror::LevelOutOfBounds, "level out of the lot");
        if (t.level > w.firstLevel() && !w.hasFloor(t) && !(pf & 0x200) && !(pf2 & 0x04))
            return fail(placementerror::NoFloor, "no floor");
        if (pf && !(pf & 0x03) && !(pf & 0x200))
            return fail(placementerror::NoFloor, "not allowed on the floor");
        // TestPlacementWithRespectToWalls (§3.3): the wall flags (od 13) on
        // the four sides, and no wall through the object (unless p & 0x400).
        if (w.diagonalWall(t) && !(pf & 0x400))
            return fail(placementerror::WallIntersect, "diagonal wall");
        if (!(pf & 0x400))
            for (int k = 0; k < 4; ++k) {
                const auto s = directionStep(k * 2);
                if (tiles.count({t.x + s[0], t.y + s[1]}) && w.wallBetween(t, s[0], s[1]))
                    return fail(placementerror::WallIntersect, "a wall cuts the object");
            }
        const int16_t wp = o.data[objdata::WallPlacementFlags];
        static const char* const sides[4] = {"in front", "on the right", "behind", "on the left"};
        for (int side = 0; side < 4; ++side) {
            const auto s = sideStep(p.direction, side);
            const bool wall = w.wallBetween(t, s[0], s[1]);
            if ((wp & (1 << side)) && !wall)
                return fail(placementerror::NeedsWall, std::string("needs a wall ") + sides[side]);
            if ((wp & (0x100 << side)) && wall)
                return fail(placementerror::WallIntersect, std::string("no wall allowed ") + sides[side]);
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

void applyPlacement(VM& vm, ObjectId id, const Placement& p) {
    for (const auto& part : p.parts) {
        Object* o = vm.object(part.id);
        if (!o)
            continue;
        o->data[objdata::Direction] = int16_t(p.direction);
        o->position = {float(part.tile.x) + 0.5f, float(part.tile.y) + 0.5f, part.tile.level};
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
    vm.planner.reset();
}

ObjectId createGhost(VM& vm, uint32_t group, uint16_t definitionInstance) {
    std::vector<ObjectId> before;
    for (const auto& [id, o] : vm.objects())
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

void deleteGroup(VM& vm, ObjectId id) {
    for (ObjectId part : groupOf(vm, id)) {
        // What it holds goes with it.
        std::vector<ObjectId> held;
        for (const auto& [other, x] : vm.objects())
            if (x.data[objdata::ContainerId] == part && other != part)
                held.push_back(other);
        for (ObjectId h : held)
            deleteGroup(vm, h);
        runEntry(vm, part, entry::Delete);
        runEntry(vm, part, entry::Cleanup);
        vm.threads.erase(part);
        vm.removeObject(part);
    }
    vm.planner.reset();
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

bool buy(VM& vm, ObjectId ghost, const Placement& p, int price, std::string* why) {
    auto refuse = [&](const std::string& r) {
        if (why)
            *why = r;
        return false;
    };
    if (!p.ok)
        return refuse(p.reason.empty() ? "does not fit" : p.reason);
    if (funds(vm) < price)
        return refuse("not enough funds");
    applyPlacement(vm, ghost, p);
    runUserPlacement(vm, ghost);
    addFunds(vm, -price);
    for (const auto& part : p.parts)
        vm.startMain(part.id);
    return true;
}

int sell(VM& vm, ObjectId id) {
    const int value = saleValue(vm, id);
    const auto group = groupOf(vm, id);
    if (group.size() == 1)
        runEntry(vm, id, entry::UserDelete);
    pickUp(vm, id);
    addFunds(vm, value);
    return value;
}

void dayPassed(VM& vm) {
    for (const auto& [id, o] : vm.objects()) {
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
}

int saleValue(VM& vm, ObjectId id) {
    const Object* o = vm.object(id);
    if (!o)
        return 0;
    const Object* lead = vm.object(multiTileGroup(*o));
    return std::max<int>(0, (lead ? lead : o)->data[objdata::CurrentValue]);
}

} // namespace os2::vm
