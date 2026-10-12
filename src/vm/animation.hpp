#pragma once

// Animation state of the VM's objects: what the animation primitives (Animate
// Sim/Object/Overlay/Stop, Assign Locomotion Animations, Effect Stop/Start)
// leave for the renderer to play, and the animation names they resolve.

#include "formats/bhav.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace os2 {
class ResourceIndex;
struct Animation;
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
// One effect an Effect Stop/Start started (docs/effects-mac.md): its name
// from STR# 0x8F (entry index + 1 in the binary's lookup; the global table
// with flag 0x01, the code's semi-global with 0x02, else the running tree's
// group), where it attaches (slot type 0/1/2 and slot index: Sims' type 2
// slot 4 is the head's balloon slot) and the operand flags (icons: 0x04 an
// object's thumbnail, 0x20 a cell of a STR# 0x95 icon atlas, ...). `serial` grows
// with every start, so the renderer restarts an effect started again;
// `oneShot` (mode 6, fire and forget) entries play once.
struct ActiveEffect {
    int16_t index = 0;
    std::string name;
    uint8_t slot = 0;
    int8_t slotType = 0;
    uint16_t flags = 0;
    std::string icon;       // icon atlas texture (flag 0x20: STR# 0x95 entry operand byte 15), else empty
    int16_t iconFrame = 0;  // its cell (the variable owner u8 12, data u16 13; float param 0xD)
    int16_t iconObject = 0; // object whose thumbnail is the icon (flag 0x04), else 0
    uint32_t iconGuid = 0;  // GUID whose thumbnail is the icon (flag 0x04 with 0x400)
    int16_t iconSim = 0;    // Sim whose portrait is the icon (flag 0x10: the variable of bytes 12-14)
    int16_t iconId = 0;     // flag 0x1000: the thumbnail by ID in temp 6 (a want's, "CT - Handle Feedback - Score")
    int16_t wantSim = 0;    // flag 0x2000: the icon of that Sim's want (temp 4 = 0) or fear, slot temp 5
    bool wantFear = false;
    int16_t wantSlot = 0;
    std::string node;       // a saved effect's bone or slot node ("slot_plumbob_0_fx"), else empty
    uint32_t serial = 0;
    bool oneShot = false;
    bool hard = false;      // hard start/stop: no fade
    int8_t state = 0;       // modes 10-13: the effect's state 0-3 (float parameter 0xF)
};

// The kind of an animation (the binary's tOEType), from the role of its
// animation-list entry: "a-idle-neutral-...(idle)", "(gesture)", "(facial)",
// ...; an entry without one is a normal animation. Animate Stop's modes 4-11
// stop the overlays of one kind (try_animate_stop: mode 4 carry, 5 idle, 6
// gesture, 7 reaction, 8 normal, 9 facial, 10 idle facial, 11 receptivity).
namespace oetype {
constexpr int8_t Normal = 2, Carry = 3, Idle = 4, Gesture = 5, Reaction = 6, Facial = 7, FacialIdle = 8,
                 Receptivity = 9;
}
// The kind named by an entry's role ("idle", "normal, reclined", ...).
int8_t oeTypeOfRole(const std::string& role);

// The standard events an animation's event tree gets besides its own
// "event=N" (global BCON 0x124 "G - Anim Events - Std"): 9997 when it starts
// to blend out ("Event - Animation Blending Out" of the idle system starts
// the next idle on it), 9999 at its end ("Event - Animation - End of
// Anim"), -1 when it is stopped before (both idle trees clear their
// "playing" flag on it). The binary's animation event handler
// (nTSSG::cCustomEventHandler2, Mac BV 0x780ad4) maps the engine's event key
// types onto them: 7 -> 9997, 6 -> 9998, 3 -> 9999, -1 -> -1, 1005 -> the
// key's "event=N". 9998 ("Marker - Snap Into" drops a carried baby into its
// marker on it, "Stance - Event - Stop", "Death By Satellite") is taken here
// as the overlay's action ending, sent just before 9999 (inferred: what
// emits key type 6 was not found; the animations carry no such key).
namespace animevent {
constexpr int16_t BlendOut = 9997, ActionEnd = 9998, End = 9999, Stopped = -1;
}

// How the engine mixes what plays on an object (Mac BV, docs/primitives-mac.md
// §4 "Mistura dos canais"): every animation request is a set of channels,
// one per bone it keys, and each bone mixes the channels feeding it by
// their request's priority level, 0-8 (cChannelUberOutlet's base channel
// blender): from the highest level down, a level whose channels' weights
// (fade x request weight) add up to 1 takes the bone whole, a lighter one
// takes its share and leaves the rest to the levels under it, channels of
// one level are averaged by weight, and the result is normalized. The
// levels the primitives give (AddChoreoAnimRequest's debug channel names in
// brackets): Animate Sim 2 x priority byte 12 (0 "sil", 2 "sim", 4 "sih",
// 6 "siu"; < 0 is 0, > 3 is 6), Animate Overlay 2 x priority byte 11 + 1
// (1 "ovl", 3 "ovm", 5 "ovh", 7 "ovu"; < 0 is 1, 4 or more is 0 "osl"),
// Animate Object 2 ("obj"), a route's locomotion 2, 3 or 5 ("rsm", "rom",
// "roh"). So a Sim's full-body animation does not take the place of the
// overlays: an overlay of a higher level (a carry pose, priority 2-3) stays
// over it on the bones both key, and one of a lower level (the idles,
// priority 0 or -1: level 1) gives way to it.
namespace animlevel {
// The locomotion's requests are priority 0 (CreateLocoRequest, BV 0x6b2158,
// passes r29 = 0 to CreateChoreoLocoRequest's SetPriority argument); Reach/
// Put's grasp phases are 3 and its carry pose 5 (try_new_reach, BV
// 0x69111c: the phases' levels 3, 2, 3, 3, 5, the last the carry name's,
// with the debug names "rom", "rsm", "roh" the doc once took for a route's).
constexpr int8_t SimLow = 0, Object = 2, Walk = 0, Grasp = 3, Carry = 5;
// Request heading offsets in SimAntics directions (Animate Sim byte 8 bit
// 0x04, Animate Overlay byte 15 bit 0x02: temp 4): -temp 4 x pi/4
// (FUN_008b6860; its constant is the one FUN_008b61fc turns the 8
// directions into radians with, (2 - d) x k, and FUN_008b68a4 back modulo 8).
inline float headingOfDirections(int16_t steps) { return -float(steps) * 0.78539816f; }
// Animate Sim's priority byte (try_animate_sim, BV 0x69dd18).
inline int8_t ofSim(int priority) { return priority < 0 ? 0 : priority > 3 ? 6 : int8_t(2 * priority); }
// Animate Overlay's priority byte (try_animate_overlay, BV 0x69f584).
inline int8_t ofOverlay(int priority) {
    const int p = priority < 0 ? 0 : priority > 4 ? 4 : priority;
    return p == 4 ? 0 : int8_t(2 * p + 1);
}
} // namespace animlevel

// The engine's blend time (BV __bss 0x2c0ddd0, set to 233.333 animation
// ticks (ms) by TSSGAnimatable.cpp's static initializer, BV 0x20b4ef0): how
// long a request blends in and out (CreateTransitionBlend), Animate Stop's
// "blended Out" (0x02; 0.3 of it with "short blended Out", 0x20; without
// either a stop is a cut), and how long before its end an overlay starts
// to blend out (event 9997).
constexpr float kBlendTicks = 233.33333f;

// An animation playing over an object (Animate Overlay, Animate Object, the
// carry poses of what a Sim holds).
struct Overlay {
    std::string name;
    uint32_t id = 0;               // unique per start (the host restarts on a new one)
    int8_t oeType = oetype::Normal;
    int8_t level = animlevel::Carry; // channel priority (animlevel); carry poses: inferred
    // How many times it plays before it ends (Animate Overlay: once, or temp
    // 1 with flag 0x20); 0 = it holds or loops until stopped (carry poses,
    // Animate Object, temp 1 = -1, animations without length).
    int loops = 0;
    float played = 0, perTick = 0, length = 0; // animation ticks (ms at speed 1)
    bool blendOutSent = false;
    bool flipped = false; // played mirrored (Animate Object/Overlay's flip: a chair pulled out to the Sim's side)
    int16_t ikObject = 0; // the object its IK targets of type 2 name bones of (0: none)
    float headingOffset = 0; // the request's heading offset, radians (headingOfDirections)
    // Its event tree (Animate Overlay's u16 at 4) and where it runs: the
    // thread that started it.
    uint16_t eventTree = 0;
    int16_t me = 0, codeOwner = 0, stackObject = 0;
    std::vector<std::pair<int, int16_t>> pending; // tree events not reached yet
    bool operator==(const std::string& n) const { return name == n; }
    bool operator==(const char* n) const { return name == n; }
};

struct AnimationState {
    // The full-body animation Animate Sim plays (resource name without
    // "_anim", resolved for the Sim's age and gender), while it plays.
    std::string name;
    uint32_t serial = 0;    // bumped each time one starts, so the host restarts it
    // Flipped horizontally: byte 2 flag 0x01, or with byte 8 flag 0x01 temp 3 != 0.
    bool flipped = false;
    int8_t level = animlevel::SimLow; // its channels' priority (animlevel::ofSim)
    float weight = 1;                 // the request weight: byte 13 / 100 (0 or outside 0-1: 1)
    int ticksLeft = 0;      // VM ticks until it ends
    bool playing() const { return !name.empty(); }
    // The instruction waiting for it: its thread's stack depth, tree and node.
    size_t depth = 0;
    const Behavior* tree = nullptr;
    uint16_t node = 0;
    // Its tree events (ANIM events of type 1005 "event=N", in animation
    // ticks) not run yet, the event tree that gets them, and how long it
    // has played (animation ticks: ms at speed 1).
    std::vector<std::pair<int, int16_t>> pendingEvents;
    uint16_t eventTree = 0;
    int16_t eventObject = 0; // the event tree's parameter 1 (animation.cpp)
    float headingOffset = 0;  // the request's heading offset, radians (byte 8 bit 0x04: temp 4)
    float playedTicks = 0, ticksPerVmTick = 0;

    // Animations that play over the object without blocking (Animate
    // Overlay on a Sim or an object, Animate Object), oldest first; Animate
    // Stop clears them, and Animate Overlay's end when they have played.
    std::vector<Overlay> overlays;
    uint32_t overlaySerial = 0;
    // Overlays Animate Stop took off and how long they blend out (seconds:
    // kBlendTicks with "blended Out", 0.3 of it "short", 0 a cut), for the
    // renderer; the last few.
    std::vector<std::pair<uint32_t, float>> stoppedFades;
    uint32_t nextOverlayId = 1;
    // Adds an overlay that holds until stopped and returns it.
    Overlay& addOverlay(const std::string& name) {
        Overlay o;
        o.name = name;
        o.id = nextOverlayId++;
        overlays.push_back(std::move(o));
        return overlays.back();
    }
    std::vector<std::string> overlayNames() const {
        std::vector<std::string> out;
        for (const auto& o : overlays)
            out.push_back(o.name);
        return out;
    }
    // The tree events of the Animate Object animations playing on the object
    // with an event tree (tickOverlayEvents runs them as the animation plays).
    struct OverlayEvents {
        std::string name;
        int16_t me = 0, codeOwner = 0, stackObject = 0;
        uint16_t eventTree = 0;
        std::vector<std::pair<int, int16_t>> pending;
        float played = 0, perTick = 0, length = 0;
    };
    std::vector<OverlayEvents> overlayEvents;

    // Locomotion sets, the last on top; empty = the global group's.
    std::vector<LocomotionSet> locomotion;

    // Effects started on the object (Effect Stop/Start), for the renderer.
    std::vector<ActiveEffect> effects;
};

// What the locomotion needs of an animation (ANIM header, docs/routing-mac.md
// §6.1): its length, how far its frame goes over the floor and how fast, how
// far it turns the body (negative: clockwise; the turn clips are all made
// clockwise and played mirrored the other way), the walk cycle portion it
// starts or ends at (the "R" stops and starts: 0, the others 0.5), its
// locomotion type (6 turn and go, 7 turn and stop, 8 turn in place, 9 step,
// 10 step in place, 11-19 the walk cycles alt0-alt8) and the root's offset
// from the moving frame at each VM tick (x to the side, y forward, the yaw of
// the body about +Z): a start lags behind the frame and turns the body from
// where it faced to the way, a stop turns it to the final facing.
struct LocoClip {
    int ticks = 0;      // animation ticks (1 ms)
    float distance = 0; // metres the frame covers
    float velocity = 0; // metres per second
    float turn = 0;     // radians
    float cycle = 0;
    int type = 0;
    std::vector<std::array<float, 3>> root; // per VM tick: x, y, yaw
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
    // Whether the animation moves the whole body (ANIM flags bit 0: the
    // chairs' sitting idles, the get-ins; not the carry poses nor the faces).
    bool fullBody(const std::string& name) {
        seconds(name);
        auto it = fullBody_.find(name);
        return it != fullBody_.end() && it->second;
    }
    // How far the animation's root ("root_trans") goes forward (+Y of the
    // model, tiles) from its first to its last frame: a vehicle's drive loop
    // moves it that much each time it plays (0 if none).
    float rootTravel(const std::string& name);
    void addRootTravel(const std::string& name, float tiles) { travel_[name] = tiles; }
    // The animation itself (null if there is no such resource or no index).
    std::shared_ptr<const Animation> animation(const std::string& name);
    // Locomotion speed of the animation (ANIM "velocity", distance per
    // animation tick, a tick being 1 ms) in tiles per second (a tile is a
    // metre); 0 if it does not move or does not exist.
    float tilesPerSecond(const std::string& name);
    void add(const std::string& name, float seconds, float tilesPerSecond = 0) {
        cache_[name] = seconds;
        speeds_[name] = tilesPerSecond;
    }
    // Animation ticks (1 ms) of the first event of the animation whose data
    // starts with the line `line` ("event=0": the reaches' "transfer object
    // to hand"), or nothing if it has none.
    std::optional<int> eventTicks(const std::string& name, const std::string& line);
    void addEvent(const std::string& name, const std::string& line, int ticks) { events_[name + '\n' + line] = ticks; }
    // The tree events of an animation: its type-1005 events whose data
    // starts with "event=N" (N = the event number its event tree gets), as
    // (animation ticks, N), in order.
    const std::vector<std::pair<int, int16_t>>& treeEvents(const std::string& name);
    void addTreeEvent(const std::string& name, int ticks, int16_t event) { treeEvents_[name].push_back({ticks, event}); }
    // The locomotion header and root track of an animation, sampled every
    // 1/`ticksPerSecond` s; null if there is no such resource.
    const LocoClip* loco(const std::string& name, int ticksPerSecond = 30);
    void addLoco(const std::string& name, LocoClip clip) {
        loco_[name] = std::move(clip);
        speeds_[name] = loco_[name]->velocity;
    }
    // Names the trees asked for that do not exist, with how often.
    std::map<std::string, size_t> missing;

private:
    ResourceIndex* index_ = nullptr;
    std::unordered_map<std::string, std::optional<float>> cache_;
    std::unordered_map<std::string, std::shared_ptr<const Animation>> anims_;
    std::unordered_map<std::string, float> speeds_;
    std::unordered_map<std::string, bool> fullBody_;
    std::unordered_map<std::string, float> travel_;
    std::unordered_map<std::string, std::optional<int>> events_;
    std::unordered_map<std::string, std::vector<std::pair<int, int16_t>>> treeEvents_;
    std::unordered_map<std::string, std::optional<LocoClip>> loco_;
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
// How far `sim` walks in a VM tick: the speed of its gait (locomotion.hpp:
// the walk cycles weighed by its person data, 2 m/s for the normal adult
// walk, 1.37 for elders' and the Grim Reaper's, 8.3 for a run), or
// VM::walkTilesPerTick when it has none.
float walkTilesPerTick(VM& vm, const Object& sim);

// Registers Animate Object (0x69), Animate Sim (0x6A), Animate Overlay
// (0x6B), Animate Stop (0x6C), Effect Stop/Start (0x70) and Assign
// Locomotion Animations (0x72).
void registerAnimationPrimitives(VM& vm);
// Once per VM tick: advances the Animate Object animations that have an
// event tree and runs it for the events they reach (all left at the end).
void tickOverlayEvents(VM& vm);

// Starts a scripted walk in `sim`'s Main thread, replacing what it runs: a
// tree (added to the code library under a group of its own) that goes to
// `target` with Go To Relative Position (anywhere near, facing it) and then,
// if `animation` is set, plays entry `animation` of the global animation
// list with Animate Sim. For hosts and tests that want a Sim to walk
// without an interaction queue.
void startScriptedWalk(VM& vm, int16_t sim, int16_t target, std::optional<uint16_t> animation = {});

} // namespace os2::vm
