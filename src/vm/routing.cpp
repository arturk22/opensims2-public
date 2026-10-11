// Positions in the world: occupancy of the lot's tiles.
#include "vm.hpp"

#include <algorithm>
#include <cmath>
#include <set>

namespace os2::vm {

bool VM::blocksRouting(const Object& o) {
    if (!o.position.inWorld() || o.person || o.data[objdata::ContainerId] != 0)
        return false;
    if (o.definition) {
        switch (o.definition->objectType()) {
        case kObjectTypeDoor:
        case kObjectTypeWindow:
        case kObjectTypeStairs:
        case kObjectTypeModularStairs:
        case kObjectTypeModularStairsPortal: return false;
        default: break;
        }
    }
    return (o.data[objdata::Flags] & (0x01 | 0x04 | 0x10)) == 0;
}

BlockedGrid VM::blockedTiles(int level, std::span<const ObjectId> ignore) {
    if (!world)
        throw VMError("no world (positions need a lot)");
    std::set<ObjectId> skip;
    for (ObjectId id : ignore)
        if (const Object* o = object(id))
            skip.insert(multiTileGroup(*o));
    BlockedGrid grid(size_t(world->width()) * world->height(), 0);
    for (const auto& [id, o] : objects_) {
        if (o.position.level != level || !blocksRouting(o) || skip.count(multiTileGroup(o)))
            continue;
        const TilePos t = tileOf(o);
        if (world->contains(t))
            grid[world->index(t.x, t.y)] = 1;
    }
    return grid;
}

bool VM::tileFree(TilePos t, std::span<const ObjectId> ignore) {
    if (!world)
        throw VMError("no world (positions need a lot)");
    if (!world->walkable(t))
        return false;
    return !blockedTiles(t.level, ignore)[world->index(t.x, t.y)];
}

std::vector<ObjectId> VM::objectsOnTile(TilePos t) const {
    std::vector<ObjectId> out;
    for (const auto& [id, o] : objects_)
        if (o.position.inWorld() && o.data[objdata::ContainerId] == 0 && tileOf(o) == t)
            out.push_back(id);
    return out;
}

} // namespace os2::vm

namespace os2::vm {

// Routing slots (SLOT type 3) say where a Sim stands to use an object. Read
// as FreeSO's VMSlotParser reads The Sims 1's, with the Sims 2 values (see
// Slots::Slot): flags bits 0-7 are the allowed directions around the object
// (bit n = direction n relative to the object's own; 0x55 the four sides,
// 0xFF all eight), 0x200 makes them absolute, 0x800 ignores rooms, 0x1000
// snaps the facing, 0x10000 uses the average position of a multi-tile
// object; the distances are in tiles; "facing" is -3 anywhere, -2 towards the
// object, -1 away from it, 0-7 a direction relative to the object's
// (inferred: the global SLOT's eight offset slots, 0.7 tiles around the
// object, all face away from it that way, as a Sim stands to sit down). A slot
// with an offset or without directions is a point (offsets are in tiles in
// the object's model space, whose forward axis is +Y); in the Sims 2 most
// object slots are points with no offset: the position comes from a bone of
// the model ("No bone on model corresponding to name in routing slot"), which
// is taken here as the object's own tile. Goals are whole tiles: a range of
// distances becomes the tiles whose centres are within it (half a tile of
// slack), in an allowed direction, with no wall in between and in the
// object's room; each costs how far it is from the optimal distance.
VM::RouteGoals VM::routingGoals(const Slots::Slot& slot, const Object& target) {
    RouteGoals out;
    out.level = target.position.level;
    float cx = target.position.x, cy = target.position.y;
    if ((slot.flags & 0x10000) && multiTileGroup(target)) {
        float sx = 0, sy = 0;
        int n = 0;
        for (const auto& [id, o] : objects_)
            if (multiTileGroup(o) == multiTileGroup(target) && o.position.inWorld())
                sx += o.position.x, sy += o.position.y, ++n;
        if (n)
            cx = sx / float(n), cy = sy / float(n);
    }
    const int objDir = ((target.data[objdata::Direction] % 8) + 8) % 8;
    const uint32_t dirs = slot.flags & 0xFF;
    const bool absolute = slot.flags & 0x200, ignoreRooms = slot.flags & 0x800;
    auto facingAt = [&](float px, float py) {
        if (slot.facing >= 0)
            return (objDir + slot.facing) % 8;
        if (slot.facing == -2 || slot.facing == -1) {
            const int d = slot.facing == -2 ? directionOf(cx - px, cy - py) : directionOf(px - cx, py - cy);
            return d < 0 ? objDir : d;
        }
        return -1;
    };
    const TilePos centre = tileOf(target);

    const float ox = slot.offset[0], oy = slot.offset[1];
    if (dirs == 0 || ox != 0 || oy != 0) {
        const double yaw = (90.0 - 45.0 * objDir) * M_PI / 180.0;
        const float px = cx + float(ox * std::cos(yaw) - oy * std::sin(yaw));
        const float py = cy + float(ox * std::sin(yaw) + oy * std::cos(yaw));
        out.goals.push_back({int(std::floor(px)), int(std::floor(py)), 0});
        out.points.push_back({px, py});
        out.facings.push_back(facingAt(px, py));
        out.onTarget = out.goals.back().x == centre.x && out.goals.back().y == centre.y;
        return out;
    }
    if (!world)
        return out;
    const float minD = slot.minDistance, maxD = std::max(slot.maxDistance, slot.minDistance);
    const float optimal = slot.optimalDistance > 0 ? slot.optimalDistance : minD;
    const int reach = int(std::ceil(maxD)) + 1;
    for (int x = centre.x - reach; x <= centre.x + reach; ++x)
        for (int y = centre.y - reach; y <= centre.y + reach; ++y) {
            const TilePos t{x, y, out.level};
            if (!world->contains(t) || (x == centre.x && y == centre.y))
                continue;
            const float px = float(x) + 0.5f, py = float(y) + 0.5f;
            const float d = std::hypot(px - cx, py - cy);
            if (d < minD - 0.5f || d > maxD + 0.5f)
                continue;
            int dir = directionOf(px - cx, py - cy);
            if (!absolute)
                dir = ((dir - objDir) % 8 + 8) % 8;
            if (!(dirs & (1u << dir)))
                continue;
            if (!ignoreRooms && world->room(t) != world->room(centre))
                continue;
            if (!world->lineOfSight(centre, t))
                continue;
            out.goals.push_back({x, y, std::fabs(d - optimal)});
            out.points.push_back({px, py});
            out.facings.push_back(facingAt(px, py));
        }
    return out;
}

} // namespace os2::vm
