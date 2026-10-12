#pragma once

#include "lot.hpp"

#include <map>
#include <string>
#include <vector>

namespace os2::vm {

// Starting a lot's objects the way the game loads a saved lot, instead of
// running every Init: the editor's description of the OBJf entry points
// (STR# 0xF5) says Init runs when an object is placed or reset, and "load"
// whenever an instance is loaded from a save file, only if its "object
// version" (object data 58) differs from its definition's (OBJD label 49).

// The simulator's saved globals (SIMI) and the clock they hold (hour,
// minute, second, day, month, year).
void applySavedSimulator(VM& vm, const LotObjects& saved);

// Only the Sims' saved motives (the first set of floats of the PERS,
// clamped to -100..100), for when the objects start from their Inits.
void applySavedMotives(VM& vm, const LotLoad& load, const LotObjects& saved);

// Restores each created object's saved state (attributes, semi-attributes,
// object data, person data and motives, object arrays, temporary token,
// timers), rebuilds its Main thread from the saved tree stack (frame by
// frame: tree, node, parameters, locals, stack object, code owner = the
// object it runs on, and the 8 saved temps) and runs the load entry point
// of objects whose version changed. Objects without a usable stack start
// their Main afresh. The instruction a frame was at runs again; the
// primitives' saved state is not restored. The Sims' saved interaction
// queues are (what of them LotObjects finds), with the frame of the
// running one marked as its action tree's root.
struct ResumeResult {
    size_t restored = 0, stacks = 0, freshMains = 0, loads = 0;
    size_t queued = 0; // interactions put back in the Sims' queues
    size_t animations = 0; // Animate Sims that go on from their saved requests
    size_t overlays = 0;   // overlays back from the objects' animation queues
    size_t routes = 0;     // routing instructions resumed mid-walk (resumeRoutes)
    size_t threadStates = 0; // threads given the VM's own saved state (thread_state.hpp)
    size_t globalsMade = 0; // global sim objects updateSimObjects made
    std::map<std::string, size_t> problems; // why a stack was not resumed
    std::vector<Thread> failedLoads;
};
ResumeResult resumeLotObjects(VM& vm, const LotLoad& load, const LotObjects& saved);

// cEdithObjectModule::UpdateSimObjects(1) (Mac BV 0x52f040), the step 6 of
// every lot load's SplitLoad (docs/game-flow-mac.md §7.1): one instance of
// each "global sim object" (OBJD type 7, field 42): the first existing one
// stays and the other instances are killed; with none, one is made out of
// the world (Init, then Main). In a CAS lot those with field 26 are killed
// instead. This is what brings the "Controller - Lot Load - Curtain
// Pull-Down" that "CT - Lot Loaded" looks for (the move-in scene). Returns
// how many were made.
size_t updateSimObjects(VM& vm, bool casLot = false);

// The whole start: the saved globals and clock, then either the saved state
// (resumeLotObjects, the default) or, with `fromInit`, every Init and Main
// afresh (startLotObjects) with the saved motives; then updateSimObjects.
ResumeResult startSavedLot(VM& vm, const LotLoad& load, const LotObjects& saved, bool fromInit = false);

} // namespace os2::vm
