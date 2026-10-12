#pragma once

// How a Sim's walk looks and how fast it goes: the locomotion set's clips
// (STR# 0x84 of the top of the locomotion stack, docs/routing-mac.md §6.1)
// as nTSSG::cLocomotable and the scene graph's cLocoSet pick them.
//
// Each set is six strings: walk turns, walks, walk blends, run turns, runs,
// run blends. The turn string names the start, stop, step and turn clips by
// role ("turnAndGo-alt(turnAndGo)", "turnInPlace-alt@1.5(turnInPlace@1.5)":
// the run set plays them 1.5 times as fast), each made at 0, 45, 90, 135 and
// 180 degrees ("a-turnAndGo-alt-90", "a-TurnInPlace-90", "a-step-45").

#include <optional>
#include <string>

namespace os2::vm {

class VM;
struct Object;

// The walk cycle and its speed (BeginFollow, BV 0x6b5770; the set's speed
// FUN_00d0c444): person data 112 and 113 ("LocoWeight 0/1") weigh the walk
// cycles alt0 and alt1, 114 ("LocoPersonalityIndex", 2-19) names another
// with the weight 115/100 ("LocoPersonalityWeight"), all of them times
// 1 - 117/100, and 116 ("LocoMoodIndex", 2-19) adds 117/100
// ("LocoMoodWeight") to another ("Sneak Here": index 8 weight 80, the
// alt8 sneak; "Skip Here": 7 and 75, the alt7 skip). The speed is the
// cycles' speeds (ANIM velocity) weighed by those weights over the cycles
// that weigh more than 0.01 (alt0 always counts), or alt0's when none does.
// Object data 17 ("walk style", 1 = run: "Go Here Frantically" and the
// trees that hurry) takes the run string instead of the walk one.
// The game blends the weighed cycles; here the one that weighs the most
// plays (inferred: a visual simplification).
struct Gait {
    std::string animation; // resolved resource name, empty if none
    float speed = 0;       // metres per second
    bool run = false;
};
Gait gaitOf(VM& vm, const Object& sim);

// A start, stop, step or turn clip of the walker's set for `role`
// ("turnAndGo", "turnAndGoR", "turnAndGoShort", "turnAndStop",
// "turnAndStopShortR", "step", "stepShort", "turnInPlace") turning or
// stepping `steps` 45-degree steps (counter-clockwise positive). The clips
// are made clockwise (header turn -45 x steps); the set finds the one whose
// turn is the wanted one, else the one whose turn is its opposite, played
// mirrored (cLocoSet vtable 0x30, BV 0xd0c7ec). An "R" role (the other
// foot) falls back to the plain one, as the set takes a start or stop of
// either foot when there is no other (cycle portion 0 or 0.5).
struct LocoPick {
    std::string animation;
    bool mirror = false;
    float speed = 1; // the set's playback multiplier ("@1.5")
};
std::optional<LocoPick> locoClip(VM& vm, const Object& sim, const std::string& role, int steps);

} // namespace os2::vm
