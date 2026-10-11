// Positions in the world: occupancy of the lot's tiles.
#include "vm.hpp"

#include <algorithm>
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
