// Object ownership: the object-to-neighbor relationships whose variable 1
// counts how often a Sim used the object (cTSPerson::UpdateObjectOwnership),
// read back by Relationship's flag 0x08, the autonomy's ownership bias and
// Set to Next 32 "Owner of Object".

#pragma once

#include "vm.hpp"

namespace os2::vm {

// OBJD "Object ownership flags" (editor label 25, file offset 0x72): bit 0
// makes the object count its users.
constexpr size_t kObjdOwnershipFlags = 0x72;

// cTSPerson::UpdateObjectOwnership (Mac BV 0xad2ebc), run when Idle for
// Input starts an interaction: for an object (the leader of a multi-tile
// one) whose OBJD ownership flags have bit 0, the relationship from the
// object to the Sim's neighbor ID is created with two variables (0, 0) if
// missing and variable 1 goes up by one, to at most 100. Only for Sims of
// the current family and objects that are not Sims (the binary asks the
// interaction's person and the object's kind; inferred).
void updateObjectOwnership(VM& vm, ObjectId sim, ObjectId object);

// Set to Next 32 "Owner of Object (Object in Temp0, Threshold in Temp1)
// returns Neighbor ID" (TrySetToNext, Mac BV): the next neighbor ID after
// `after` (0 or less: from the first) whose ownership relationship with the
// object has variable 1 >= `threshold` (compared unsigned); -1 when there is
// none (the binary writes the table's end marker, -1, into the target and
// returns false).
int16_t nextObjectOwner(const VM& vm, ObjectId object, int16_t after, int16_t threshold);

} // namespace os2::vm
