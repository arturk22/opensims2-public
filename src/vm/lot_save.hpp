#pragma once

#include "lot.hpp"

#include "formats/lot_objects.hpp"

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace os2::vm {

// Saving a lot from the VM: the inverse of loadLot + resumeLotObjects
// (lot_resume.hpp), in the game's format (LotObjects::save, docs/save-mac.md).
//
// captureLot starts from the lot as it was read (`saved`) and puts the VM's
// state into it: for each object of the module that still exists, its
// position and level, attributes, semi-attributes, object data, object
// arrays, temporary token, timers, its Main thread's temps and tree stack
// (frame by frame: tree, node, stack object, the object it runs as (the
// thread's owner) and on (the code owner), parameters and locals, and the
// BHAV's save type = the code owner's), and of the Sims their person data,
// motives (the exact floats when the motive engine has them) and the
// running interaction (the person's slot at +0x5D4) and queue; the globals
// and the clock go to the SIMI. Objects that are gone leave the module.
// Objects the VM created are added with the save type of their GUID in the
// lot's table, or a new objt record built from their definition
// (save_types.hpp), or one from `moreSaveTypes`. What the VM does not keep (a
// frame's primitive state, the token's inventory fields, the fields after
// the person data that are not the queue, the animator, ...) stays as read
// for the objects and frames that were read, and is zero otherwise.
struct CaptureResult {
    LotObjects lot;
    size_t updated = 0, added = 0, removed = 0;
    size_t kept = 0; // objects the VM could not create, kept as read
    size_t newSaveTypes = 0; // objt records added for objects created during play
    std::vector<std::string> warnings;
};
// `moreSaveTypes` (by GUID, from other lots' tables) gives the save types of
// objects the VM created that the lot's table lacks; they are added to it.
CaptureResult captureLot(VM& vm, const LotObjects& saved,
                         const std::map<uint32_t, LotObjects::SaveType>* moreSaveTypes = nullptr);

// Differences between two VMs holding the same lot (objects, positions,
// attributes, object/person data, motives, arrays, tokens, timers, thread
// stacks and temps, queues, globals and clock), one line each, at most
// `limit` lines (and a count of the rest).
std::vector<std::string> compareLotState(VM& a, VM& b, size_t limit = 50);

} // namespace os2::vm
