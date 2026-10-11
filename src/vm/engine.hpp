#pragma once

// Primitives added for the Sims' trees on real lots (round 5): Message
// (message.cpp) and the engine/UI ones (engine_prims.cpp), with what the
// host can look at.

#include "vm.hpp"

#include <array>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>
#include <span>

namespace os2::vm {

// Message (0x77).
void registerMessagePrimitives(VM& vm);
// Refresh, Debug, UI Effect, Camera Control, Cinematic, Test Sim Interacting With.
void registerEnginePrimitives(VM& vm);
// Age, RayTrace (person_prims.cpp).
void registerPersonPrimitives(VM& vm);

// Runs `tree` of `codeOwner`'s code to its end at once, as the binary's
// RunOneTickTree (Message, Run Tree by Name "in stack object's stack"): a
// thread for `me` with that stack object, the temps given (and written
// back), up to 8 parameters. A tree that would wait is the game's error
// 0x451; one that fails fails the caller. Returns what the tree returned.
bool runOneTickTree(VM& vm, std::array<int16_t, kTempCount>& temps, ObjectId me, ObjectId codeOwner,
                    ObjectId stackObject, uint16_t tree, std::span<const int16_t> params);

// Object data 62 "use count", which the engine computes when it is read
// (as FreeSO's VMEntity::GetUsers/IsInUse; not checked in the binary): the
// Sims other than the object whose queue, up to the running entry, has an
// interaction on it or on another part of its multi-tile object, or whose
// thread runs its code (a reaction object stays while the reacting Sim runs
// "CT - Do Anim" of it: its Main removes it once the use count is 0).
int16_t useCount(VM& vm, const Object& o);

// A UI Effect / Camera Control / Cinematic a tree ran (headless they do
// nothing): the latest 256, for the host to act on.
struct UiEvent {
    uint8_t opcode = 0;
    ObjectId me = 0;
    ObjectId stackObject = 0;
    std::array<uint8_t, 16> operands{};
};
std::deque<UiEvent>& uiEvents();

// A Cinematic (0x7B) a tree ran, resolved when it ran (docs/cinematics-mac.md
// §4): the scene name from STR# 0x97 "Cinematics" (index in operands 0-1, or
// from the variable in 6/7-8 with flag 0x10; the table of the code's group,
// the global group with flag 0x20, the semi-global one with 0x40 — the two
// flags inferred) and the actors, array 0 of the object whose ID is in the
// variable of operands 9/10-11. The CINE is "<scene>_cine". The latest 32.
struct CinematicRequest {
    std::string scene;
    std::vector<ObjectId> actors;
    uint8_t flags = 0;
    int16_t temp0 = 0; // flag 0x02 mirrors the scene when it is not 0
    ObjectId me = 0;
    uint64_t tick = 0;
    // Flag 0x01, or 0x02 with temp 0 set: the scene's animations flipped
    // horizontally ("Flip Cinematic Anims Horizontally"); 0x04 "Start
    // Animations Now" (TryCinematic passes flags >> 2 & 1 as PrepCinematicScene's
    // start-now argument, Mac binary).
    bool mirrored() const { return (flags & 0x01) || ((flags & 0x02) && temp0 != 0); }
    bool startNow() const { return flags & 0x04; }
};
std::deque<CinematicRequest>& cinematicRequests();

} // namespace os2::vm
