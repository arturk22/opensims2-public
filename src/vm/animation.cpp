// Animation primitives: Animate Sim/Object/Overlay/Stop, Assign Locomotion
// Animations and Effect Stop/Start. Operand layouts are inferred from how
// the game's trees use them (`s2pkg prim-usage`) and from the strings of the
// SimAntics editor's disassembler left in Sims2.exe; see TODO.md (M5) for
// what is inferred.
#include "engine.hpp"
#include "animation.hpp"

#include "errors.hpp"
#include "interactions.hpp"

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
    if (!index_)
        return out; // not cached: the library may get its index later (a route planned first)
    {
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
    if (!index_)
        return v;
    {
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
    if (!index_)
        return out;
    {
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

const std::vector<std::pair<int, int16_t>>& AnimationLibrary::treeEvents(const std::string& name) {
    if (auto it = treeEvents_.find(name); it != treeEvents_.end())
        return it->second;
    auto& out = treeEvents_[name];
    if (index_) {
        try {
            for (const auto& e : loadAnimation(*index_, name).events) {
                if (e.type != 1005 || e.data.rfind("event=", 0) != 0)
                    continue;
                const auto end = e.data.find_first_of("|\r\n");
                try {
                    out.push_back({e.ticks, int16_t(std::stoi(e.data.substr(6, end == std::string::npos ? end : end - 6)))});
                } catch (const std::exception&) {
                }
            }
        } catch (const std::exception&) {
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
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

// The global group's list for a Sim's age (Mac binary,
// DetermineAppropriateAnimTable, person data 58): >= 51 elder 0x8B, 19-50
// adult 0x81, 16-18 teen 0x8A, 3-15 child 0x82, 2 toddler 0x89, < 2 baby
// 0x91. The elder and teen lists fall back to the adult one (`reserve`).
uint16_t globalListFor(const Object& sim, bool reserve = false) {
    const int16_t age = sim.person ? sim.person->data[persondata::PersonAge] : lifestage::Adult;
    if (age == 0)
        return animlist::Adult;
    if (age >= 51)
        return reserve ? animlist::Adult : animlist::Elder;
    if (age >= 19)
        return animlist::Adult;
    if (age >= 16)
        return reserve ? animlist::Adult : animlist::Teen;
    if (age >= 3)
        return animlist::Child;
    if (age == 2)
        return animlist::Toddler;
    return animlist::Baby;
}

// The age letter of the Sim-to-Sim prefix ("a2a-", list 0x83), by the same
// ranges ("a" when the age is not set).
char ageLetter(const Object& sim) {
    const int16_t age = sim.person ? sim.person->data[persondata::PersonAge] : lifestage::Adult;
    if (age == 0 || (age >= 19 && age < 51))
        return 'a';
    if (age >= 51)
        return 'e';
    if (age >= 16)
        return 't';
    if (age >= 3)
        return 'c';
    return age == 2 ? 'p' : 'b';
}

bool placeholder(const std::string& e) {
    std::string t = e;
    while (!t.empty() && (t.front() == ' ' || t.front() == '\t'))
        t.erase(t.begin());
    while (!t.empty() && (t.back() == ' ' || t.back() == '\t'))
        t.pop_back();
    return t.empty() || t == ".";
}

// STR# `list` of the stack object's object, else of its semi-global, the
// running code's group, the code owner's groups and the global group: the
// first that has a non-placeholder entry `index` (or the first that has the
// entry at all); null if none has it.
const StringTable* objectList(Context& c, uint16_t list, int index) {
    CodeLibrary& code = c.vm.code();
    std::vector<uint32_t> groups;
    if (const Object* so = c.vm.object(c.frame.stackObject))
        groups.push_back(so->group), groups.push_back(so->semiGroup);
    groups.push_back(c.frame.group);
    if (const Object* owner = c.vm.object(c.frame.codeOwner))
        groups.push_back(owner->group), groups.push_back(owner->semiGroup);
    groups.push_back(kGlobalGroup);
    const StringTable* first = nullptr;
    for (uint32_t g : groups) {
        const StringTable* t = g ? code.strings(g, list) : nullptr;
        if (!t || index < 0 || size_t(index) >= t->count(lang::EnglishUS))
            continue;
        if (!first)
            first = t;
        if (!placeholder(t->get(size_t(index), lang::EnglishUS)))
            return t;
    }
    return first;
}

// Entry `index` of animation list `list` for code running in `frame`, as the
// Mac binary's try_animate_sim finds it: 0x80 = the global group's list for
// the Sim's age (FindGlobalName; an elder/teen entry whose animation does
// not exist retries in the adult list); any other list is STR# `list` of
// the stack object's object and, where its entry is blank, of that object's
// semi-global (FindSpecificName). When the stack object has none, the
// running code's group, the code owner's private and semi-global groups and
// the global group are tried (inferred: the reaction semi-global's trees,
// "Interaction - Standard Reaction - SG", read the reaction object's list
// with the Sim as stack object). Blank and "." entries are placeholders
// (the chair semi-global's 0x81 is all blank; the reaction semi-global's
// lists are "." up to 19).
std::string listEntry(Context& c, const Object& sim, uint16_t list, int index) {
    CodeLibrary& code = c.vm.code();
    const StringTable* table = nullptr;
    if (list == animlist::Global) {
        table = code.strings(kGlobalGroup, globalListFor(sim));
        if (table && index >= 0 && size_t(index) < table->count(lang::EnglishUS)) {
            const std::string e = table->get(size_t(index), lang::EnglishUS);
            if (globalListFor(sim) != animlist::Adult &&
                (placeholder(e) || !library(c.vm).exists(parseItem(split(e, ',')[0]).name)))
                table = code.strings(kGlobalGroup, globalListFor(sim, true));
        } else if (globalListFor(sim) != animlist::Adult) {
            table = code.strings(kGlobalGroup, globalListFor(sim, true));
        }
    } else {
        // An object's list 0x81 is the Sim's age list too
        // (DetermineAppropriateAnimTable: a child reads the object's 0x82,
        // "c2o-bed-jump-start"); a teen's or elder's that has no animation
        // there retries in 0x81 (the binary's reserve table).
        const bool byAge = list == animlist::Adult && sim.person;
        table = objectList(c, byAge ? globalListFor(sim) : list, index);
        if (byAge && globalListFor(sim, true) != globalListFor(sim)) {
            const std::string e = table ? table->get(size_t(index), lang::EnglishUS) : "";
            if (!table || placeholder(e) || !library(c.vm).exists(resolveListAnimation(c.vm, sim, e)))
                if (const StringTable* adult = objectList(c, animlist::Adult, index))
                    table = adult;
        }
        if (!table && byAge)
            table = objectList(c, list, index);
    }
    if (!table)
        throw VMError("no animation list " + hexString(list));
    if (index < 0 || size_t(index) >= table->count(lang::EnglishUS))
        throw VMError("animation " + std::to_string(index) + " out of range of list " + hexString(list) + " (" +
                      std::to_string(table->count(lang::EnglishUS)) + ")");
    return table->get(size_t(index), lang::EnglishUS);
}

// List 0x83 holds Sim-to-Sim animations without their prefix: the name is
// "<my age>2<the other Sim's age>-" + the entry ("a2a-soc-entertain-...";
// GetAnimPrefixStart/End in the Mac binary: the other is the Sim of the
// interaction, here the stack object, else the object of my current
// interaction; not a Sim = error 0x5A). When that
// animation does not exist my age becomes "a" (the binary's reserve).
std::string socialName(Context& c, const Object& me, const std::string& entry) {
    // The other Sim is the target of my current sub-action, else of my
    // current action (DetermineAppropriateAnimTable: GetTargetObject of the
    // person's current (sub)action; Push Interaction's flag 0x01 sets it, as
    // the social objects' "Person A" pushes do); without one, the stack
    // object (inferred).
    const Object* other = nullptr;
    auto targetOf = [&](const QueuedInteraction& q) { return c.vm.object(q.target ? q.target : q.object); };
    if (me.subAction)
        other = targetOf(*me.subAction);
    else if (me.activeInteraction >= 0 && size_t(me.activeInteraction) < me.queue.size())
        other = targetOf(me.queue[size_t(me.activeInteraction)]);
    else
        other = c.vm.object(c.frame.stackObject);
    if (!other || !other->person)
        throw gameError(0x5A, "Sim-to-Sim animation without a Sim: stack object " + std::to_string(c.frame.stackObject) +
                                  ", target " + std::to_string(other ? other->id : 0));
    // When "<mine>2<theirs>-" does not exist, the binary retries with my age
    // as "a" (the reserve table) and then swaps the age letters for others
    // (FUN_0068bcb0); here: mine as "a", theirs as "a" (an elder's are the
    // adult ones: "a2e-soc-hugFriendly-hugger-reject" is missing,
    // "a2a-..." exists; inferred), then both.
    const std::string item = parseItem(split(entry, ',')[0]).name;
    const char mine = ageLetter(me), theirs = ageLetter(*other);
    auto named = [&](char a, char b) { return std::string(1, a) + "2" + b + "-" + item; };
    for (const auto& [a, b] : {std::pair{mine, theirs}, std::pair{'a', theirs}, std::pair{mine, 'a'}, std::pair{'a', 'a'}})
        if (library(c.vm).exists(named(a, b)))
            return named(a, b);
    return named(mine, theirs);
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
    return int(std::ceil(double(seconds) * vm.clock.ticksPerSecond - 1e-6));
}

// The speed of an Animate Sim/Object/Overlay operand: byte 3 in 1/32 (0x20
// = normal), or temp 2 with flag 0x02 of byte 2 (Mac binary). Taken as
// signed (0xE0, 0xC0 in ~4% of the uses: backwards, inferred), so only its
// size changes how long it plays. 0 is the game's error 0x7E.
float animationSpeed(Context& c) {
    const auto& o = c.instruction.operands;
    const int v = o[2] & 0x02 ? int(c.thread.temps[2]) : int(int8_t(o[3]));
    if (v == 0)
        throw VMError("Animation speed cannot be zero");
    return std::abs(float(v)) / 32.0f;
}

// Animate Sim (0x6A): plays a full-body animation on me and waits for it to
// end (Mac binary: it returns 2, "wait", until the animation is over, then
// 1). Operands (editor disassembler, docs/primitives-mac.md §5): animation
// index u16 at 0, flags u8 at 2 (0x01 flipped, 0x02 speed in temp 2, 0x04
// the index is in the parameter of that number, "anim id in s.v.", 0x08
// interruptible, 0x10 start at the tag in temp 0, 0x20 "Trans to Idle",
// 0x40 no blend out, 0x80 no blend in), speed u8 at 3 (1/32: 0x20 = 1.0;
// animationSpeed), event tree u16 at 4 (a call opcode, 0 none; s8 7 says
// where it is: 0 private, 1 semi-global, 2 global), the list u8 at 6 (0x80
// the global list for the Sim's age, else that STR#), flags u8 at 8 (0x01
// flip flag in temp 3, 0x02 sync to the last animation, 0x10 the
// controlling object as the source, 0x20 not hurryable), the IK object
// (owner u8 at 9, data u16), priority s8 at 12 (0 low, 1 medium, 2 high)
// and a blend weight byte 13 / 100. Only the speed and the index are used
// here; the animation's events would run the event tree, and they are not
// read. True when the animation ends; the game's error for an empty name;
// an animation that does not exist ends at once (counted in
// AnimationLibrary::missing).
// Runs the event tree for the tree events the animation has reached
// (all of them once it ends): at once, in my thread, with the event number
// as parameter 0 (inferred from "Events - Get - Bag Snack": it branches on
// parameter 0 = 100, 101, 201, the "event=" numbers of
// a2o-snackBag-get's events, which put the bag in the hand and take it
// away). The tree resolves against the running code, as a call would.
void runAnimationEvents(Context& c, AnimationState& a, bool all) {
    while (!a.pendingEvents.empty() && (all || a.pendingEvents.front().first <= a.playedTicks)) {
        const int16_t event = a.pendingEvents.front().second;
        a.pendingEvents.erase(a.pendingEvents.begin());
        if (!a.eventTree)
            continue;
        const int16_t params[5] = {event, 0, 0, 0, 0};
        runOneTickTree(c.vm, c.thread.temps, c.thread.me, c.frame.codeOwner, c.frame.stackObject, a.eventTree, params);
    }
}

Exit animateSim(Context& c) {
    Object& me = c.me();
    AnimationState& a = me.animation;
    const size_t depth = c.thread.stack.size();
    if (a.playing() && a.depth == depth && a.tree == c.frame.behavior && a.node == c.frame.pc) {
        a.playedTicks += a.ticksPerVmTick;
        if (--a.ticksLeft > 0) {
            runAnimationEvents(c, a, false);
            return Exit::Yield;
        }
        runAnimationEvents(c, a, true);
        a.name.clear();
        a.tree = nullptr;
        return Exit::True;
    }
    const auto& o = c.instruction.operands;
    const std::string entry = listEntry(c, me, o[6], animationIndex(c, 0, o[2] & 0x04));
    const std::string name = o[6] == animlist::Social && !placeholder(entry) ? socialName(c, me, entry)
                                                                             : resolveListAnimation(c.vm, me, entry);
    const auto seconds = library(c.vm).seconds(name);
    if (!seconds)
        ++c.vm.animations.missing[name];
    a.name = name;
    ++a.serial;
    a.ticksLeft = seconds ? ticksFor(c.vm, *seconds / animationSpeed(c)) : 0;
    a.depth = depth, a.tree = c.frame.behavior, a.node = c.frame.pc;
    a.pendingEvents = library(c.vm).treeEvents(name);
    a.eventTree = le16(&o[4]);
    a.playedTicks = 0;
    a.ticksPerVmTick = float(1000.0 / c.vm.clock.ticksPerSecond) * animationSpeed(c);
    if (a.ticksLeft > 0) {
        runAnimationEvents(c, a, false);
        return Exit::Yield;
    }
    runAnimationEvents(c, a, true);
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

// The loop count of Animate Object/Overlay with flag 0x20 of byte 2 is in
// temp 1; 0 or less than -1 is the game's error 0x70 (Mac binary).
void checkLoops(Context& c) {
    if ((c.instruction.operands[2] & 0x20) && (c.thread.temps[1] == 0 || c.thread.temps[1] < -1))
        throw gameError(err::BadLoopCount, std::to_string(c.thread.temps[1]));
}

// Animate Overlay (0x6B): animation index u16 at 0, flags u8 at 2 (as
// Animate Sim's: 0x02 speed in temp 2, 0x04 index in a parameter; 0x20 loop
// count in temp 1), speed u8 at 3, event tree u16 at 4 (s8 14 where), the
// object (owner u8 at 6, data u16), the list u8 at 9 (as Animate Sim's byte
// 6: 0x80 global, 0x81 adult, 0x86 object, ...), priority s8 11, byte 12 bit
// 0 not hurryable, flags byte 15 (Mac binary, docs/primitives-mac.md §5).
// It does not block (the binary returns 1 once it has started it).
Exit animateOverlay(Context& c) {
    const auto& o = c.instruction.operands;
    checkLoops(c);
    Object& target = objectAt(c, 6);
    const std::string entry = listEntry(c, target, o[9], animationIndex(c, 0, o[2] & 0x04));
    return startOverlay(c, target, entry);
}

// Animate Object (0x69): animation index u16 at 0, flags u8 at 2 (as
// Animate Overlay's), speed u8 at 3, event tree u16 at 4 (s8 9 where), the
// object (owner u8 at 6, data u16), from the object list (STR# 0x86) of the
// running tree's group or the code owner's; flags byte 10 (0x01 flip flag in
// temp 3, 0x04 sync to the caller's animation, 0x08 align the end, 0x80 not
// hurryable). It does not block (Mac binary).
Exit animateObject(Context& c) {
    const auto& o = c.instruction.operands;
    checkLoops(c);
    Object& target = objectAt(c, 6);
    const std::string entry = listEntry(c, target, animlist::Object, animationIndex(c, 0, o[2] & 0x04));
    if (trim(entry).empty() || trim(entry) == ".")
        return Exit::True; // the lists keep empty slots for objects with nothing to play there (inferred)
    return startOverlay(c, target, entry);
}

// Animate Stop (0x6C), by the Mac binary's disassembler
// (docs/primitives-mac.md §5): the object (owner u8 at 3, data u16), mode s8
// at 7: 0 one animation (u16 at 0 of the list byte 6), 1 the overlays, 2 the
// full-body animations, 3 all, 4 the carry poses, 5 idle, 6 gestures, 7
// reactions, 8 normals, 9 facials, 10 idle facials, 11 receptivity; flags
// byte 2 (0x01 flipped, 0x02 blended, 0x20 short blend, 0x08 flip flag in
// temp 3, 0x40 normal and flipped), priority s8 at 8. Here: 0 takes that
// animation off the object's overlays, 1 and 3 take every overlay off, 4 the
// grasp/carry overlays of what a Sim holds; the full-body animation is left
// to the Animate Sim waiting for it, and the overlay kinds of 5-11 are not
// told apart (nothing stops).
Exit animateStop(Context& c) {
    const auto& o = c.instruction.operands;
    Object& target = objectAt(c, 3);
    auto& a = target.animation;
    const size_t before = a.overlays.size();
    switch (int8_t(o[7])) {
    case 0: {
        const std::string entry = listEntry(c, target, o[6], int16_t(le16(&o[0])));
        const std::string first = trim(split(entry, ',')[0]);
        if (first.empty() || first == ".")
            break;
        const std::string name = target.person ? resolveListAnimation(c.vm, target, entry) : parseItem(first).name;
        if (auto it = std::find(a.overlays.begin(), a.overlays.end(), name); it != a.overlays.end())
            a.overlays.erase(it);
        break;
    }
    case 1:
    case 3: a.overlays.clear(); break;
    case 4:
        for (const auto& name : target.carryOverlays)
            if (auto it = std::find(a.overlays.begin(), a.overlays.end(), name); it != a.overlays.end())
                a.overlays.erase(it);
        break;
    default: break;
    }
    if (a.overlays.size() != before)
        ++a.overlaySerial;
    return Exit::True;
}

// Assign Locomotion Animations (0x72): the Sim's stack of locomotion sets.
// Mode u16 at 0 (Mac binary, docs/primitives-mac.md §5): 0 pops every set
// ("Pop all entries from Stack."), 1 pops the last ("Pop last entry from
// Stack."), n >= 2 pushes group n - 1 ("Push group #%d onto Stack"), taken
// as the block of six strings n - 2 (groups numbered from 1, inferred: "CT
// - Walk Like A Ghost" pushes 2 = the first block of the Sims' semi-global,
// the ghost walk). Where the set comes from, byte 2: 0x02 STR# 0x84 of the
// code owner's semi-global group, 0x04 the global group, else its own group.
Exit assignLocomotion(Context& c) {
    const auto& o = c.instruction.operands;
    Object& me = c.me();
    auto& stack = me.animation.locomotion;
    const uint16_t mode = le16(&o[0]);
    switch (mode) {
    case 0: stack.clear(); return Exit::True;
    case 1:
        if (stack.empty())
            throw VMError("Invalid pop: locomotion stack is empty");
        stack.pop_back();
        return Exit::True;
    default: {
        const Object* owner = c.vm.object(c.frame.codeOwner);
        const uint32_t group = o[2] & 0x04 ? kGlobalGroup : !owner ? 0 : o[2] & 0x02 ? owner->semiGroup : owner->group;
        if (!group || !c.vm.code().strings(group, animlist::Locomotion))
            throw VMError("Assign Locomotion Animations: no locomotion set (STR# 0x84) in group " + hexString(group));
        if (stack.size() >= kMaxLocomotionDepth)
            throw VMError("Locomotion stack is too deep: check for imbalanced push/pops");
        stack.push_back({group, uint8_t(mode - 2)});
        return Exit::True;
    }
    }
}

// Effect Stop/Start (0x70): particle effects and balloons, which only the
// renderer shows. Mode u8 at 0 (Mac binary's disassembler): 0 soft start, 1
// hard start, 2 soft stop, 3 hard stop, 4 soft stop all, 5 hard stop all, 6
// fire and forget, 7 interrogate bone, 8 hard stop all and clear the queue,
// 9 stop ALL, 10-13 state 1-4, 14 stop ALL soft; the object (owner u8 at 1,
// data u16), the effect u16 at 4 (low byte -1: the default effect), slot
// byte 6 of type s8 9, flags u16 at 10 (0x40 the effect ID is in temp 1,
// 0x80 priority queue, others icons). Starts and stops are kept in
// AnimationState::effects; always true.
Exit effectStopStart(Context& c) {
    const auto& o = c.instruction.operands;
    Object& target = objectAt(c, 1);
    auto& fx = target.animation.effects;
    const auto effect = le16(&o[10]) & 0x40 ? c.thread.temps[1] : int16_t(le16(&o[4]));
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
    case 14: fx.clear(); break;
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
            return v / float(vm.clock.ticksPerSecond);
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
