#pragma once

// Animation state of the VM's objects: what the animation primitives (Animate
// Sim/Object/Overlay/Stop, Assign Locomotion Animations, Effect Stop/Start)
// leave for the renderer to play, and the animation names they resolve.

#include "formats/bhav.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace os2 {
class ResourceIndex;
}

namespace os2::vm {

// STR# of animation names, in an object, semi-global or the global group:
// 0x81 adult, 0x82 child, 0x83 social, 0x89 toddler, 0x8A teen, 0x8B elder,
// 0x91 baby ("Anims - Adult", ...), 0x86 object animations ("Anims -
// Object"), 0x84 locomotion sets ("Loco Anim Names", "Ghost Walker", ...).
namespace animlist {
constexpr uint16_t Global = 0x80; // the global group's list for the Sim's age (operand value, not a STR#)
constexpr uint16_t Adult = 0x81;
constexpr uint16_t Child = 0x82;
constexpr uint16_t Social = 0x83;
constexpr uint16_t Locomotion = 0x84;
constexpr uint16_t Object = 0x86;
constexpr uint16_t Toddler = 0x89;
constexpr uint16_t Teen = 0x8A;
constexpr uint16_t Elder = 0x8B;
constexpr uint16_t Baby = 0x91;
} // namespace animlist

// Person data "Person Age" (58) values, as the neighbourhood's SDSC holds them.
namespace lifestage {
constexpr int16_t Baby = 0x01, Toddler = 0x02, Child = 0x03, Teen = 0x10, Adult = 0x13, Elder = 0x33;
}

// A locomotion set pushed by Assign Locomotion Animations: STR# 0x84 of a
// group, whose strings come in blocks of six (walk turns, walks, walk
// blends, run turns, runs, run blends); `set` picks the block.
struct LocomotionSet {
    uint32_t group = 0;
    uint8_t set = 0;
};
// Pushes past this fail ("Locomotion stack is too deep: check for
// imbalanced push/pops", the game's error; the depth is not known).
constexpr size_t kMaxLocomotionDepth = 8;

// Per object (vm::Object::animation).
struct AnimationState {
    // The full-body animation Animate Sim plays (resource name without
    // "_anim", resolved for the Sim's age and gender), while it plays.
    std::string name;
    uint32_t serial = 0;    // bumped each time one starts, so the host restarts it
    int ticksLeft = 0;      // VM ticks until it ends
    bool playing() const { return !name.empty(); }
    // The instruction waiting for it: its thread's stack depth, tree and node.
    size_t depth = 0;
    const Behavior* tree = nullptr;
    uint16_t node = 0;

    // Animations that play over the object without blocking (Animate
    // Overlay on a Sim or an object, Animate Object), oldest first; Animate
    // Stop clears them.
    std::vector<std::string> overlays;
    uint32_t overlaySerial = 0;

    // Locomotion sets, the last on top; empty = the global group's.
    std::vector<LocomotionSet> locomotion;

    // Effects started on the object (Effect Stop/Start): the effect's index
    // in its table, as the operand gives it.
    std::vector<int16_t> effects;
};

// ANIM resources by name, for their lengths. Reads them on demand from a
// ResourceIndex and/or takes lengths added directly (tests).
class AnimationLibrary {
public:
    void setIndex(ResourceIndex* index) { index_ = index; }
    ResourceIndex* index() const { return index_; }
    // Seconds the animation named `name` ("a-blockedPath-long", without
    // "_anim") lasts, or nothing if there is no such resource.
    std::optional<float> seconds(const std::string& name);
    bool exists(const std::string& name) { return seconds(name).has_value(); }
    // Locomotion speed of the animation (ANIM "velocity", distance per
    // animation tick, a tick being 1 ms) in tiles per second (a tile is a
    // metre); 0 if it does not move or does not exist.
    float tilesPerSecond(const std::string& name);
    void add(const std::string& name, float seconds, float tilesPerSecond = 0) {
        cache_[name] = seconds;
        speeds_[name] = tilesPerSecond;
    }
    // Names the trees asked for that do not exist, with how often.
    std::map<std::string, size_t> missing;

private:
    ResourceIndex* index_ = nullptr;
    std::unordered_map<std::string, std::optional<float>> cache_;
    std::unordered_map<std::string, float> speeds_;
};

class VM;
struct Object;

// The prefix of a Sim's animations for its age (person data 58): "a"
// adult (also when unknown), "t" teen, "e" elder, "c" child, "p" toddler,
// "b" baby.
std::string agePrefix(const Object& sim);
// "male"/"female", from person data 65 ("Gender(0=male/1=female)").
std::string genderName(const Object& sim);

// The resource for an animation-list entry played by `sim`: entries name
// adult animations ("a-blockedPath-long", "a2o-..."), so teens and elders
// get the "t"/"e" version when there is one; an entry that is a list
// ("a2o-plateDessert-grasp,a2o-plateDessert-carry(carry)") plays its first.
std::string resolveListAnimation(VM& vm, const Object& sim, std::string entry);

// Name of the locomotion animation of `role` ("walk", "walkShort", "run",
// "walkPose", "stepInPlace", "turnInPlace", ...) for `sim`, from the top of
// its locomotion stack (the global set when empty), resolved to a resource:
// "<age>-<gender>-<name>", "<age>-<name>", then the adult ones. Empty if
// there is none. Each string of a set lists names with their role in
// parentheses ("walk-alt0-normal(walk)") and optional speeds ("@1.5"); a
// name without a role is an alternate (other walk styles), except in sets
// that give no roles at all ("swimSlowShort,swimSlow,swimPose"), where the
// order is short, normal, pose (inferred).
std::string locomotionAnimation(VM& vm, const Object& sim, std::string_view role);
// How far `sim` walks in a VM tick: the speed of its locomotion walk
// animation (2 m/s for the normal adult walk, 1.37 for elders' and the
// Grim Reaper's), or VM::walkTilesPerTick when it has none.
float walkTilesPerTick(VM& vm, const Object& sim);

// Registers Animate Object (0x69), Animate Sim (0x6A), Animate Overlay
// (0x6B), Animate Stop (0x6C), Effect Stop/Start (0x70) and Assign
// Locomotion Animations (0x72).
void registerAnimationPrimitives(VM& vm);

// Starts a scripted walk in `sim`'s Main thread, replacing what it runs: a
// tree (added to the code library under a group of its own) that goes to
// `target` with Go To Relative Position (anywhere near, facing it) and then,
// if `animation` is set, plays entry `animation` of the global animation
// list with Animate Sim. For hosts and tests that want a Sim to walk
// without an interaction queue.
void startScriptedWalk(VM& vm, int16_t sim, int16_t target, std::optional<uint16_t> animation = {});

} // namespace os2::vm
