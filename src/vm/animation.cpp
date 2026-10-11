// Animation primitives: Animate Sim/Object/Overlay/Stop, Assign Locomotion
// Animations and Effect Stop/Start. Operand layouts are inferred from how
// the game's trees use them (`s2pkg prim-usage`) and from the strings of the
// SimAntics editor's disassembler left in Sims2.exe; see TODO.md (M5) for
// what is inferred.
#include "animation.hpp"

#include "vm.hpp"

#include "formats/binreader.hpp"
#include "formats/resindex.hpp"
#include "formats/sim_actor.hpp"

#include <cmath>

namespace os2::vm {

std::optional<float> AnimationLibrary::seconds(const std::string& name) {
    if (auto it = cache_.find(name); it != cache_.end())
        return it->second;
    std::optional<float> out;
    if (index_) {
        try {
            const Animation a = loadAnimation(*index_, name);
            out = float(a.duration) / Animation::kTicksPerSecond;
        } catch (const std::exception&) {
        }
    }
    cache_[name] = out;
    return out;
}

float AnimationLibrary::tilesPerSecond(const std::string& name) {
    if (auto it = speeds_.find(name); it != speeds_.end())
        return it->second;
    float v = 0;
    if (index_) {
        try {
            v = loadAnimation(*index_, name).velocity * Animation::kTicksPerSecond;
        } catch (const std::exception&) {
        }
    }
    speeds_[name] = v;
    return v;
}

std::optional<int> AnimationLibrary::eventTicks(const std::string& name, const std::string& line) {
    const std::string key = name + '\n' + line;
    if (auto it = events_.find(key); it != events_.end())
        return it->second;
    std::optional<int> out;
    if (index_) {
        try {
            for (const auto& e : loadAnimation(*index_, name).events) {
                const std::string first = e.data.substr(0, e.data.find_first_of("\r\n"));
                if (first == line) {
                    out = e.ticks;
                    break;
                }
            }
        } catch (const std::exception&) {
        }
    }
    events_[key] = out;
    return out;
}

std::string agePrefix(const Object& sim) {
    if (!sim.person)
        return "a";
    switch (sim.person->data[persondata::PersonAge]) {
    case lifestage::Baby: return "b";
    case lifestage::Toddler: return "p";
    case lifestage::Child: return "c";
    case lifestage::Teen: return "t";
    case lifestage::Elder: return "e";
    default: return "a";
    }
}

std::string genderName(const Object& sim) {
    constexpr int16_t kGender = 65;
    return sim.person && sim.person->data[kGender] == 1 ? "female" : "male";
}

namespace {

constexpr uint8_t kOwnerParam = 0x09;

AnimationLibrary& library(VM& vm) {
    if (!vm.animations.index())
        vm.animations.setIndex(vm.code().index());
    return vm.animations;
}

std::string trim(std::string s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
        s.erase(s.begin());
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))
        s.pop_back();
    return s;
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t start = 0;
    for (;;) {
        const size_t end = s.find(sep, start);
        out.push_back(s.substr(start, end == std::string::npos ? std::string::npos : end - start));
        if (end == std::string::npos)
            return out;
        start = end + 1;
    }
}

// "walk-alt0-normal(walk)", "turnInPlace-alt@1.5(turnInPlace@1.5)": the
// name and the role, without the speeds.
struct LocoItem {
    std::string name, role;
};
LocoItem parseItem(const std::string& item) {
    LocoItem out;
    std::string s = trim(item);
    if (const size_t open = s.find('('); open != std::string::npos) {
        const size_t close = s.find(')', open);
        out.role = trim(s.substr(open + 1, close == std::string::npos ? std::string::npos : close - open - 1));
        s = trim(s.substr(0, open));
    }
    if (const size_t at = s.find('@'); at != std::string::npos)
        s.resize(at);
    if (const size_t at = out.role.find('@'); at != std::string::npos)
        out.role.resize(at);
    out.name = trim(s);
    return out;
}

// The global group's list for a Sim's age (teens and elders use the adult
// list, whose names have their own versions; inferred).
uint16_t globalListFor(const Object& sim) {
    const std::string p = agePrefix(sim);
    if (p == "c")
        return animlist::Child;
    if (p == "p")
        return animlist::Toddler;
    if (p == "b")
        return animlist::Baby;
    return animlist::Adult;
}

// Entry `index` of animation list `list` for code running in `frame`: the
// global group's list for the Sim's age (0x80), else STR# `list` of the
// group the running tree is in, then of the code owner's private and
// semi-global groups, then the global group (inferred: semi-global trees
// carry their own lists, "Eat - Anim - Bite" uses 0x81 of its semi-global).
std::string listEntry(Context& c, const Object& sim, uint16_t list, int index) {
    CodeLibrary& code = c.vm.code();
    const StringTable* table = nullptr;
    if (list == animlist::Global) {
        table = code.strings(kGlobalGroup, globalListFor(sim));
    } else {
        std::vector<uint32_t> groups{c.frame.group};
        if (const Object* owner = c.vm.object(c.frame.codeOwner))
            groups.push_back(owner->group), groups.push_back(owner->semiGroup);
        groups.push_back(kGlobalGroup);
        // A list whose entry is blank is a placeholder (the chair semi-global's
        // 0x81 is all blank; the chairs' own lists hold the names): go on to
        // the next group, keeping the first that has the entry at all.
        const StringTable* first = nullptr;
        for (uint32_t g : groups) {
            const StringTable* t = g ? code.strings(g, list) : nullptr;
            if (!t || index < 0 || size_t(index) >= t->count(lang::EnglishUS))
                continue;
            if (!first)
                first = t;
            if (!trim(t->get(size_t(index), lang::EnglishUS)).empty()) {
                table = t;
                break;
            }
        }
        if (!table)
            table = first;
    }
    if (!table)
        throw VMError("no animation list " + hexString(list));
    if (index < 0 || size_t(index) >= table->count(lang::EnglishUS))
        throw VMError("animation " + std::to_string(index) + " out of range of list " + hexString(list) + " (" +
                      std::to_string(table->count(lang::EnglishUS)) + ")");
    return table->get(size_t(index), lang::EnglishUS);
}

// The animation index of an operand: u16 at `at`, or the parameter of that
// number when the flag says so ("anim id in s.v.").
int animationIndex(Context& c, size_t at, bool fromParam) {
    const auto v = int16_t(le16(&c.instruction.operands[at]));
    return fromParam ? c.get(kOwnerParam, v) : v;
}

Object& objectAt(Context& c, size_t ownerAt) {
    const auto& o = c.instruction.operands;
    Object* x = c.vm.object(c.get(o[ownerAt], int16_t(le16(&o[ownerAt + 1]))));
    if (!x)
        throw VMError("Invalid object id.");
    return *x;
}

int ticksFor(VM& vm, float seconds) {
    return int(std::ceil(double(seconds) * vm.clock.ticksPerMinute - 1e-6));
}

// Animate Sim (0x6A): plays a full-body animation on me and waits for it to
// end. Operands (inferred from the trees and the editor's strings): animation
// index u16 at 0, flags u8 at 2 (0x04: the index is in the parameter of that
// number, "anim id in s.v."; 0x01, 0x02, 0x08, 0x40, 0x80 not understood:
// the editor lists Interruptible, Flipped, Trans to Idle, No Blend in/out,
// Start at tag in Temp 0, Anim Speed in Temp 2, Flip Flag in Temp 3,
// Synching Anim to last Anim Run, Using Controlling Object as Anim Source,
// Not Hurryable), flags u8 at 3 (0x20 in 96% of the uses), event tree u16 at
// 4 (a call opcode, 0 none; byte 7 says where it is: 0 private, 1
// semi-global, 2 global), the list u8 at 6 (0x80 the global list for the
// Sim's age, else that STR#), u8 at 8 more flags, an object (owner u8 at 9,
// data u16: the "IK Object"/controlling object?) and u8 at 12 (0-4: the
// priority?). The animation's events would run the event tree; they are
// not read, so it never runs. True when the animation ends; the game's
// error for an empty name; an animation that does not exist ends at once
// (counted in AnimationLibrary::missing).
Exit animateSim(Context& c) {
    Object& me = c.me();
    AnimationState& a = me.animation;
    const size_t depth = c.thread.stack.size();
    if (a.playing() && a.depth == depth && a.tree == c.frame.behavior && a.node == c.frame.pc) {
        if (--a.ticksLeft > 0)
            return Exit::Yield;
        a.name.clear();
        a.tree = nullptr;
        return Exit::True;
    }
    const auto& o = c.instruction.operands;
    const std::string entry = listEntry(c, me, o[6], animationIndex(c, 0, o[2] & 0x04));
    const std::string name = resolveListAnimation(c.vm, me, entry);
    const auto seconds = library(c.vm).seconds(name);
    if (!seconds)
        ++c.vm.animations.missing[name];
    a.name = name;
    ++a.serial;
    a.ticksLeft = seconds ? ticksFor(c.vm, *seconds) : 0;
    a.depth = depth, a.tree = c.frame.behavior, a.node = c.frame.pc;
    if (a.ticksLeft > 0)
        return Exit::Yield;
    a.name.clear();
    a.tree = nullptr;
    return Exit::True;
}

// Adds an overlay animation to an object (Animate Overlay, Animate Object):
// they do not block the thread (inferred: "Anim - Sim & Object by Index"
// starts the object's animation, plays the Sim's and then stops the
// object's).
Exit startOverlay(Context& c, Object& target, const std::string& entry) {
    std::string name = target.person ? resolveListAnimation(c.vm, target, entry) : parseItem(split(entry, ',')[0]).name;
    if (name.empty() || name == ".")
        throw VMError("Animation name is empty string.");
    if (!library(c.vm).exists(name))
        ++c.vm.animations.missing[name];
    auto& ov = target.animation.overlays;
    ov.push_back(name);
    if (ov.size() > 8)
        ov.erase(ov.begin());
    ++target.animation.overlaySerial;
    return Exit::True;
}

// Animate Overlay (0x6B): animation index u16 at 0 (byte 2 flag 0x04: in a
// parameter), u8 at 3 (0x20), event tree u16 at 4, the object (owner u8 at 6,
// data u16), the list u8 at 9 (as Animate Sim's byte 6: 0x80 global, 0x81
// adult, 0x86 object, ...). Byte 11 (0-4, 0xFF), 12, 14 and 15 are not
// understood (priority? "Priority", "Loop Count in Temp 1", "Anim Speed in
// Temp2", "Sync to calling objects Anim" in the editor's strings).
Exit animateOverlay(Context& c) {
    const auto& o = c.instruction.operands;
    Object& target = objectAt(c, 6);
    const std::string entry = listEntry(c, target, o[9], animationIndex(c, 0, o[2] & 0x04));
    return startOverlay(c, target, entry);
}

// Animate Object (0x69): animation index u16 at 0 (byte 2 flag 0x04: in a
// parameter), u8 at 3 (0x20), event tree u16 at 4, the object (owner u8 at
// 6, data u16), from the object list (STR# 0x86) of the running tree's group
// or the code owner's. Bytes 9 (0-2) and 10 (flags, 0x20 in half the uses)
// are not understood.
Exit animateObject(Context& c) {
    const auto& o = c.instruction.operands;
    Object& target = objectAt(c, 6);
    const std::string entry = listEntry(c, target, animlist::Object, animationIndex(c, 0, o[2] & 0x04));
    if (trim(entry).empty() || trim(entry) == ".")
        return Exit::True; // the lists keep empty slots for objects with nothing to play there (inferred)
    return startOverlay(c, target, entry);
}

// Animate Stop (0x6C): stops the overlays of an object (owner u8 at 3, data
// u16). Byte 2 is the mode (flags: 0x01, 0x02, 0x20 in "CT - Route Waiting
// Animation"; the editor names "All Overlay animations running", "All Full
// Body Animations running", "All Animations running", "Carry Pose", "blended
// Out", "short blended Out", "Stopping normal AND flipped anims"), byte 6 the
// list and u16 at 0 / byte 7 which animation; not understood, so every
// overlay stops.
Exit animateStop(Context& c) {
    Object& target = objectAt(c, 3);
    if (!target.animation.overlays.empty()) {
        target.animation.overlays.clear();
        ++target.animation.overlaySerial;
    }
    return Exit::True;
}

// Assign Locomotion Animations (0x72): the Sim's stack of locomotion sets.
// Mode u8 at 0: 0 pops every set ("Pop all entries from Stack."), 1 pops the
// last ("Pop last entry from Stack."), 2 pushes ("Push group #%d onto
// Stack"), 3 (once, in "Fix Loco" right after a pop all) is taken as a push
// too. Where the set comes from, u8 at 2: 0 STR# 0x84 of the code owner's
// own group, 2 of its semi-global group, 1 (unused) the global group; u8 at
// 1 the block of six strings in it (always 0). Inferred from the trees:
// "Get In"/"Get Out" of the pools push and pop, "CT - Walk Like A Ghost" pops
// all and pushes the ghost walk of the Sims' semi-global, "Fix Loco" (reset)
// pops all.
Exit assignLocomotion(Context& c) {
    const auto& o = c.instruction.operands;
    Object& me = c.me();
    auto& stack = me.animation.locomotion;
    switch (o[0]) {
    case 0: stack.clear(); return Exit::True;
    case 1:
        if (stack.empty())
            throw VMError("Invalid pop: locomotion stack is empty");
        stack.pop_back();
        return Exit::True;
    case 2:
    case 3: {
        const Object* owner = c.vm.object(c.frame.codeOwner);
        uint32_t group = 0;
        switch (o[2]) {
        case 0: group = owner ? owner->group : 0; break;
        case 1: group = kGlobalGroup; break;
        case 2: group = owner ? owner->semiGroup : 0; break;
        default: throw VMError("Assign Locomotion Animations: source " + std::to_string(o[2]) + " not implemented");
        }
        if (!group || !c.vm.code().strings(group, animlist::Locomotion))
            throw VMError("Assign Locomotion Animations: no locomotion set (STR# 0x84) in group " + hexString(group));
        if (stack.size() >= kMaxLocomotionDepth)
            throw VMError("Locomotion stack is too deep: check for imbalanced push/pops");
        stack.push_back({group, o[1]});
        return Exit::True;
    }
    default:
        throw VMError("Assign Locomotion Animations: mode " + std::to_string(o[0]) + " not implemented");
    }
}

// Effect Stop/Start (0x70): particle effects and balloons, which only the
// renderer shows. Mode u8 at 0, in the order of the editor's strings: 0 soft
// start, 1 hard start, 2 soft stop, 3 hard stop, 4 soft stop all, 5 hard
// stop all, 6 fire and forget, 7 interrogate bone, 8 hard stop all and clear
// the queue, 9-11 stop ALL / set state (inferred); the object (owner u8 at 1,
// data u16), the effect u16 at 4 (0xFFFF: "Passing in effect ID in temp
// 1"?). The rest (slot, icons, priority queue) is not understood. Starts and
// stops are kept in AnimationState::effects; always true.
Exit effectStopStart(Context& c) {
    const auto& o = c.instruction.operands;
    Object& target = objectAt(c, 1);
    auto& fx = target.animation.effects;
    const auto effect = int16_t(le16(&o[4]));
    switch (o[0]) {
    case 0:
    case 1:
        if (std::find(fx.begin(), fx.end(), effect) == fx.end() && fx.size() < 64)
            fx.push_back(effect);
        break;
    case 2:
    case 3: std::erase(fx, effect); break;
    case 4:
    case 5:
    case 8:
    case 9:
    case 10: fx.clear(); break;
    default: break;
    }
    return Exit::True;
}

} // namespace

std::string resolveListAnimation(VM& vm, const Object& sim, std::string entry) {
    std::string name = parseItem(split(entry, ',')[0]).name;
    if (name.empty() || name == ".")
        throw VMError("Animation name is empty string.");
    const std::string p = agePrefix(sim);
    if ((p == "t" || p == "e") && name.size() > 1 && name[0] == 'a' && (name[1] == '-' || name[1] == '2')) {
        std::string aged = p + name.substr(1);
        if (library(vm).exists(aged))
            return aged;
    }
    return name;
}

std::string locomotionAnimation(VM& vm, const Object& sim, std::string_view role) {
    LocomotionSet set{kGlobalGroup, 0};
    if (!sim.animation.locomotion.empty())
        set = sim.animation.locomotion.back();
    const StringTable* table = vm.code().strings(set.group, animlist::Locomotion);
    if (!table)
        return {};
    // Walks and poses are in the second string of the walk block, turns in the
    // first; runs in the fifth and fourth.
    const bool run = role.starts_with("run");
    const bool turn = role.find("urn") != std::string_view::npos;
    const size_t index = size_t(set.set) * 6 + (run ? 3 : 0) + (turn ? 0 : 1);
    const std::vector<std::string> items = split(table->get(index, lang::EnglishUS), ',');
    std::string name;
    bool anyRole = false;
    for (const auto& s : items) {
        const LocoItem it = parseItem(s);
        anyRole = anyRole || !it.role.empty();
        if (it.role == role && name.empty())
            name = it.name;
    }
    if (name.empty() && !anyRole && !turn) {
        // short, normal, pose (swimming)
        std::vector<std::string> names;
        for (const auto& s : items)
            if (auto n = parseItem(s).name; !n.empty())
                names.push_back(n);
        const bool isShort = role.find("Short") != std::string_view::npos;
        const bool pose = role.find("Pose") != std::string_view::npos;
        for (const auto& n : names) {
            const bool nShort = n.find("Short") != std::string::npos, nPose = n.find("Pose") != std::string::npos;
            if (nShort == isShort && nPose == pose && n.find("InPlace") == std::string::npos) {
                name = n;
                break;
            }
        }
    }
    if (name.empty())
        return {};
    AnimationLibrary& lib = library(vm);
    const std::string p = agePrefix(sim), g = genderName(sim);
    for (const std::string& candidate : {p + "-" + g + "-" + name, p + "-" + name, "a-" + g + "-" + name, "a-" + name})
        if (lib.exists(candidate))
            return candidate;
    return {};
}

float walkTilesPerTick(VM& vm, const Object& sim) {
    if (sim.person) {
        const std::string walk = locomotionAnimation(vm, sim, "walk");
        if (const float v = walk.empty() ? 0 : library(vm).tilesPerSecond(walk); v > 0)
            return v / float(vm.clock.ticksPerMinute);
    }
    return vm.walkTilesPerTick;
}

void registerAnimationPrimitives(VM& vm) {
    vm.setPrimitive(0x69, animateObject);
    vm.setPrimitive(0x6A, animateSim);
    vm.setPrimitive(0x6B, animateOverlay);
    vm.setPrimitive(0x6C, animateStop);
    vm.setPrimitive(0x70, effectStopStart);
    vm.setPrimitive(0x72, assignLocomotion);
}

void startScriptedWalk(VM& vm, int16_t simId, int16_t target, std::optional<uint16_t> animation) {
    // A tree of the Sim's own group, at an instance the game's private trees
    // do not reach (they stop well below 0x1F00).
    constexpr uint16_t kTree = 0x1FF0;
    Object* sim = vm.object(simId);
    if (!sim)
        throw VMError("startScriptedWalk: no object " + std::to_string(simId));
    Behavior b;
    b.fileName = "opensims2 - Scripted Walk";
    b.signature = 0x8007;
    Behavior::Instruction go;
    go.opcode = 0x1B; // Go To Relative Position: anywhere near (-1), facing it (-2)
    go.operands[2] = uint8_t(int8_t(-1));
    go.operands[3] = uint8_t(int8_t(-2));
    go.onFalse = {Behavior::TargetKind::False, 0};
    if (animation) {
        go.onTrue = {Behavior::TargetKind::Instruction, 1};
        Behavior::Instruction anim;
        anim.opcode = 0x6A;
        anim.operands[0] = uint8_t(*animation & 0xFF);
        anim.operands[1] = uint8_t(*animation >> 8);
        anim.operands[3] = 0x20;
        anim.operands[6] = uint8_t(animlist::Global);
        anim.onTrue = {Behavior::TargetKind::True, 0};
        anim.onFalse = {Behavior::TargetKind::True, 0};
        b.instructions = {go, anim};
    } else {
        go.onTrue = {Behavior::TargetKind::True, 0};
        b.instructions = {go};
    }
    vm.code().add(sim->group, kTree, std::move(b));
    vm.threads.insert_or_assign(simId, vm.start(simId, kTree, {}, target));
}

} // namespace os2::vm
