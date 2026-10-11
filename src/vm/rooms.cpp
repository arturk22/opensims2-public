#include "rooms.hpp"

#include "errors.hpp"

namespace os2::vm {

// cTSObject::InterpValue, owner 0x17 (BV): the room is never checked (there
// is no error 0x28 here); the values are asked of the world:
//   0 ambient light: the room's light level scaled to 0-100 (not known here),
//   1 and 7 outside: the room manager's lookup (nTSWorld, misnamed
//     cRoomManager::cRoomManager) returns -1 for room -1 or a room the world
//     does not have, and -1 reads as "not outside" (0),
//   2 level, 3 area (quarter tiles / 4), 4 pool,
//   5 number of windows: objects whose room it is with category 0x16,
//   6 greenhouse (more than half its fences are greenhouse fences; none in
//     the base game's lots, 0),
//   past 7: error 0x29 "Bad room value requested.".
// Room 0 (a tile no room covers: fence lamps stand on such tiles) stays
// outside, as inferred before (370 failures in lot-run).
int16_t roomValue(const VM& vm, int16_t room, int16_t value) {
    if (value < 0 || value > 7)
        throw gameError(0x29, "room value " + std::to_string(value));
    if (value == 5) {
        int16_t n = 0;
        for (const auto& [id, o] : vm.objects())
            if (o.position.inWorld() && o.data[objdata::Room] == room && o.data[objdata::Category] == 0x16)
                ++n;
        return n;
    }
    if (value == 6)
        return 0;
    if (value == 7)
        value = 1;
    auto it = vm.rooms.find(room);
    if (it == vm.rooms.end()) {
        if (room == 0)
            return value == 1 ? 1 : 0;
        return 0; // the world has no such room
    }
    if (!(it->second.known & (1u << value)))
        throw VMError("room " + std::to_string(room) + ": value " + std::to_string(value) + " not known");
    return it->second.values[size_t(value)];
}

} // namespace os2::vm
