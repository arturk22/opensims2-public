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
    auto fail = [&](std::string why) {
        p.ok = false;
        p.reason = std::move(why);
        return p;
    };
    if (!lead || !lead->definition)
        return fail("no such object");
    if (!vm.world)
        return fail("no lot");
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

    for (const auto& part : p.parts) {
        const Object& o = *vm.object(part.id);
        const TilePos t = part.tile;
        if (!w.contains(t))
            return fail("off the lot");
        // Floors: above the ground a tile needs one; the ground is floor or
        // terrain, which the placement flags (42: 1 floor, 2 terrain) allow.
        const int16_t pf = o.data[objdata::PlacementFlags];
        if (t.level > w.firstLevel() && !w.hasFloor(t))
            return fail("no floor");
        if (pf && !(pf & 0x03))
            return fail("not allowed on the floor");
        if (w.diagonalWall(t))
            return fail("diagonal wall");
        // No wall between two tiles of the object.
        for (int k = 0; k < 4; ++k) {
            const auto s = directionStep(k * 2);
            if (tiles.count({t.x + s[0], t.y + s[1]}) && w.wallBetween(t, s[0], s[1]))
                return fail("a wall cuts the object");
        }
        // Walls the object needs or does not allow on its sides (13).
        const int16_t wp = o.data[objdata::WallPlacementFlags];
        static const char* const sides[4] = {"in front", "on the right", "behind", "on the left"};
        for (int side = 0; side < 4; ++side) {
            const auto s = sideStep(p.direction, side);
            const bool wall = w.wallBetween(t, s[0], s[1]);
            if ((wp & (1 << side)) && !wall)
                return fail(std::string("needs a wall ") + sides[side]);
            if ((wp & (0x100 << side)) && wall)
                return fail(std::string("no wall allowed ") + sides[side]);
        }
        // Other objects on the tile.
        const int16_t myLayers = layers(o);
        const bool meLetsPeople = o.data[objdata::Flags] & (kAllowObjectAndPerson | kAllowPerson);
        for (ObjectId other : vm.objectsOnTile(t)) {
            if (mine.count(other))
                continue;
            const Object& x = *vm.object(other);
            if (x.person) {
                if (!meLetsPeople)
                    return fail("a Sim is in the way");
                continue;
            }
            if (!(layers(x) & myLayers))
                continue;
            if ((x.data[objdata::Flags] | o.data[objdata::Flags]) & (kAllowObjectIntersection | kAllowObjectAndPerson))
                continue;
            // A surface: a free container slot of a height this object
            // allows, strong enough for it (single-tile objects only).
            const int16_t heights = o.data[objdata::AllowedHeightFlags];
            if (group.size() == 1 && p.container == 0) {
                const auto slots = containerSlots(vm, x);
                for (size_t i = 0; i < slots.size(); ++i) {
                    const int h = slots[i]->height;
                    if (h < 1 || h > 16 || !(heights & (1 << (h - 1))))
                        continue;
                    if (vm.containedIn(other, int16_t(i)))
                        continue;
                    if (o.data[objdata::Weight] >= x.data[objdata::SupportStrength])
                        continue;
                    p.container = other;
                    p.slot = int(i);
                    break;
                }
                if (p.container == other)
                    continue;
            }
            return fail("\"" + (x.definition ? x.definition->fileName : std::string("?")) + "\" is in the way");
        }
    }
    // An object that does not stand on the floor needs a surface.
    if (!p.container && !(lead->data[objdata::AllowedHeightFlags] & 1))
        return fail("needs a surface");
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
    for (ObjectId part : groupOf(vm, leadId))
        runEntry(vm, part, 0); // Init
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
    for (const auto& part : p.parts)
        runEntry(vm, part.id, entry::UserPlacement);
    addFunds(vm, -price);
    if (Object* lead = vm.object(ghost); lead && lead->data[objdata::CurrentValue] == 0)
        lead->data[objdata::CurrentValue] = int16_t(std::min(price, 32767));
    for (const auto& part : p.parts)
        vm.startMain(part.id);
    return true;
}

int saleValue(VM& vm, ObjectId id) {
    const Object* o = vm.object(id);
    if (!o)
        return 0;
    const Object* lead = vm.object(multiTileGroup(*o));
    return std::max<int>(0, (lead ? lead : o)->data[objdata::CurrentValue]);
}

} // namespace os2::vm
