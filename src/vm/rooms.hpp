#pragma once

// Room values (data owner 0x17 "room [temp 0]"), as cTSObject::InterpValue
// computes them in the Mac binary.

#include "vm.hpp"

namespace os2::vm {

int16_t roomValue(const VM& vm, int16_t room, int16_t value);

// The OBJf entry "room changed" (7).
constexpr size_t kRoomChangedFunction = 7;

// cTSObject::OnRoomChanged (BV 0xb0093c; docs/primitives-mac.md §4i), run
// when an object's moves are done (OnFinishEditing, BV 0xb01a44) and for
// every object of a room whose map changed (UpdateObjectsInRoom): nothing
// when its room (object data 29) is the one last told (notifiedRoom) unless
// it is a diagonal door; else it is noted and an object that is not a portal
// runs its entry 7 with {room, -1, 0, 0}, a portal (PortalOnRoomChanged) with
// its two rooms when they changed (PortalGetRooms), itself as stack object.
void onRoomChanged(VM& vm, ObjectId id);
// The same for the object and everything it holds (FinishEditing goes down
// the containment slots), after their rooms were set.
void finishMove(VM& vm, ObjectId id);
// cTSObject::PortalCalculateRooms (BV 0xe221e4): the rooms on the two sides
// of a door part that is a portal; false when it is not one or a side has no
// room. The binary asks the room (definition 2) at each of the portal's
// slots; here the door's tile and the tile behind it, the wall between
// (inferred; the saved doors of N001_Lot11 agree: attribute 3 = the front
// room, 4 = the back one, on the 0,0 part, and the 0,1 part, not a portal,
// has its own room and -1).
// Every object whose room is not the one last told (it moved, or its
// container did): onRoomChanged. The VM runs it at the end of each tick, for
// the moves the trees and the engine made without telling.
void notifyRoomChanges(VM& vm);
bool portalRooms(const VM& vm, const Object& o, int16_t& a, int16_t& b);

} // namespace os2::vm
