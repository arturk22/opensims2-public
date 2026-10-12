// Animation primitives: Animate Sim/Object/Overlay/Stop, Assign Locomotion
// Animations and Effect Stop/Start. Operand layouts are inferred from how
// the game's trees use them (`s2pkg prim-usage`) and from the strings of the
// SimAntics editor's disassembler left in Sims2.exe; see TODO.md (M5) for
// what is inferred.
#include "engine.hpp"
#include "animation.hpp"

#include "errors.hpp"
#include "interactions.hpp"
#include "placement.hpp"
#include "world.hpp"

#include "vm.hpp"

#include "formats/binreader.hpp"
#include "formats/resindex.hpp"
#include "formats/sim_actor.hpp"

#include <algorithm>
#include <array>
#include <cctype>
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
            fullBody_[name] = a.flags & 1;
        } catch (const std::exception&) {
        }
    }
    cache_[name] = out;
    return out;
}

float AnimationLibrary::rootTravel(const std::string& name) {
    if (auto it = travel_.find(name); it != travel_.end())
        return it->second;
    float v = 0;
    if (!index_)
        return v;
    try {
        const Animation a = loadAnimation(*index_, name);
        for (const auto& t : a.targets)
            for (const auto& ch : t.channels)
                if (ch.name == "root_trans" && ch.components.size() >= 2 && !ch.components[1].keys.empty()) {
                    const auto& c = ch.components[1];
                    v = c.sample(float(ch.duration()), ch.duration()) - c.sample(0, ch.duration());
                }
    } catch (const std::exception&) {
    }
    travel_[name] = v;
    return v;
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
            // Past the end of the age's list: its reserve, and the adult list
            // for a baby, toddler or child (inferred: "Anims - Baby" stops at
            // 140, inside the blinks "Facial - Background - Start Anim"
            // picks from 135-165, and its own blinks are the adult ones).
            table = code.strings(kGlobalGroup, globalListFor(sim, true));
            if (!table || size_t(index) >= table->count(lang::EnglishUS))
                table = code.strings(kGlobalGroup, animlist::Adult);
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
// away). Parameter 1 is the Animate Sim's IK object (owner u8 at 9, data
// u16 at 10; the binary's animation request keeps it as its target), read
// when the animation starts (inferred: "Events - Bake" snaps the dish into
// parameter 1, the stove that is the IK object; "Events - Mix - Load" stops
// the spoiling of parameter 1; with 0 both fail on object 0). The tree
// resolves against the running code, as a call would.
void runAnimationEvents(Context& c, AnimationState& a, bool all) {
    while (!a.pendingEvents.empty() && (all || a.pendingEvents.front().first <= a.playedTicks)) {
        const int16_t event = a.pendingEvents.front().second;
        a.pendingEvents.erase(a.pendingEvents.begin());
        if (!a.eventTree)
            continue;
        const int16_t params[5] = {event, a.eventObject, 0, 0, 0};
        runOneTickTree(c.vm, c.thread.temps, c.thread.me, c.frame.codeOwner, c.frame.stackObject, a.eventTree, params);
    }
}

// Runs an overlay's event tree for `event`, in the context of the thread
// that started it, as a one-tick tree with the event as parameter 0
// (inferred: the binary calls it from the animation's event callback). A
// tree that fails does not stop the caller (inferred).
void runOverlayEvent(VM& vm, const Overlay& o, int16_t event) {
    if (!o.eventTree || !vm.object(o.me) || !vm.object(o.codeOwner))
        return;
    std::array<int16_t, kTempCount> temps{};
    const int16_t params[5] = {event, 0, 0, 0, 0};
    try {
        runOneTickTree(vm, temps, o.me, o.codeOwner, o.stackObject, o.eventTree, params);
    } catch (const VMError&) {
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
    // A full-body animation takes the place of the full-body overlays (the
    // chair's sitting idle, an Animate Overlay of the same priority 2):
    // the chair's get-out plays standing the Sim up, and the Sim does not
    // keep the sitting pose when it walks away (inferred: the trees never
    // stop the sitting idle; the game's layers replace one of the same
    // priority).
    if (library(c.vm).fullBody(name)) {
        const size_t before = a.overlays.size();
        std::erase_if(a.overlays, [&](const Overlay& ov) {
            return library(c.vm).fullBody(ov.name) &&
                   std::find(me.carryOverlays.begin(), me.carryOverlays.end(), ov.name) == me.carryOverlays.end();
        });
        if (a.overlays.size() != before)
            ++a.overlaySerial;
    }
    a.name = name;
    ++a.serial;
    a.flipped = (o[8] & 0x01) ? c.thread.temps[3] != 0 : (o[2] & 0x01) != 0; // the flag in temp 3 replaces the literal one (inferred)
    a.ticksLeft = seconds ? ticksFor(c.vm, *seconds / animationSpeed(c)) : 0;
    a.depth = depth, a.tree = c.frame.behavior, a.node = c.frame.pc;
    a.pendingEvents = library(c.vm).treeEvents(name);
    a.eventTree = le16(&o[4]);
    a.eventObject = 0;
    try {
        a.eventObject = c.get(o[9], int16_t(le16(&o[10])));
    } catch (const VMError&) {
    }
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
// object's). Its kind comes from the entry's role (oeTypeOfRole). Null when
// two of that name already play: try_animate_overlay looks the name up
// twice (AnimRequestByNameAndID) and starts nothing more then, so an idle
// started again while the last one blends out does not pile up.
Overlay* startOverlay(Context& c, Object& target, const std::string& entry, bool flipped = false) {
    std::string name = target.person ? resolveListAnimation(c.vm, target, entry) : parseItem(split(entry, ',')[0]).name;
    if (name.empty() || name == ".")
        throw VMError("Animation name is empty string.");
    if (!library(c.vm).exists(name))
        ++c.vm.animations.missing[name];
    auto& ov = target.animation.overlays;
    if (std::count(ov.begin(), ov.end(), name) >= 2)
        return nullptr;
    Overlay& o = target.animation.addOverlay(name);
    o.oeType = oeTypeOfRole(parseItem(split(entry, ',')[0]).role);
    o.flipped = flipped;
    if (ov.size() > 8)
        ov.erase(ov.begin());
    ++target.animation.overlaySerial;
    return &ov.back();
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
    // Flipped as Animate Sim's: flag 0x01 of byte 15 takes temp 3 (inferred as there).
    Overlay* ov = startOverlay(c, target, entry, (o[15] & 0x01) ? c.thread.temps[3] != 0 : (o[2] & 0x01) != 0);
    if (!ov)
        return Exit::True;
    // It plays once, or temp 1 times with flag 0x20 (-1: until stopped); an
    // animation without length is a pose that holds (the carry poses).
    const float length = library(c.vm).seconds(ov->name).value_or(0) * 1000.0f;
    const int loops = o[2] & 0x20 ? c.thread.temps[1] : 1;
    ov->loops = length > 0 && loops > 0 ? loops : 0;
    ov->length = length;
    ov->perTick = float(1000.0 / c.vm.clock.ticksPerSecond) * animationSpeed(c);
    ov->pending = library(c.vm).treeEvents(ov->name);
    ov->eventTree = le16(&o[4]);
    ov->me = c.thread.me, ov->codeOwner = c.frame.codeOwner, ov->stackObject = c.frame.stackObject;
    return Exit::True;
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
    // A vehicle drives by playing its drive loop again from its event tree
    // at each loop's end: the same animation started again replaces the one
    // playing (Animate Object's play until stopped, loops 0). With Animate
    // Overlay's "two of a name at most" the third loop did not start, its
    // event tree never got 9999 and "Wait for Vehicle Stop" waited forever:
    // the maid's van (and the school bus) stayed at the curb all day.
    {
        const std::string name = target.person ? std::string() : parseItem(split(entry, ',')[0]).name;
        auto& ov = target.animation.overlays;
        if (!name.empty())
            std::erase_if(ov, [&](const Overlay& x) { return x.loops == 0 && x.name == name; });
    }
    // Mirrored with the Sim's get-in from the other side (flag 0x01 of
    // byte 10: the flip in temp 3, as Animate Sim's).
    if (!startOverlay(c, target, entry, (o[10] & 0x01) ? c.thread.temps[3] != 0 : (o[2] & 0x01) != 0))
        return Exit::True;
    // Its event tree gets the animation's tree events as it plays, in the
    // starting thread's context, as Animate Sim's (the vehicles drive on
    // "Event Tree - Drive": loop again or stop, which ends "Wait for Vehicle
    // Stop"). A new one from the same code replaces the last (inferred).
    if (const uint16_t tree = le16(&o[4])) {
        const std::string name = target.animation.overlays.back().name;
        auto& list = target.animation.overlayEvents;
        std::erase_if(list, [&](const auto& x) { return x.codeOwner == c.frame.codeOwner; });
        AnimationState::OverlayEvents ev;
        ev.name = name;
        ev.me = c.thread.me, ev.codeOwner = c.frame.codeOwner, ev.stackObject = c.frame.stackObject;
        ev.eventTree = tree;
        ev.pending = library(c.vm).treeEvents(name);
        ev.length = library(c.vm).seconds(name).value_or(0) * 1000.0f;
        ev.perTick = float(1000.0 / c.vm.clock.ticksPerSecond) * animationSpeed(c);
        list.push_back(std::move(ev));
    }
    return Exit::True;
}


// Animate Stop (0x6C), by the Mac binary's disassembler
// (docs/primitives-mac.md §5) and try_animate_stop: the object (owner u8 at
// 3, data u16), mode s8 at 7: 0 one animation (u16 at 0 of the list byte
// 6), 1 the overlays, 2 the full-body animations, 3 all, 4-11 the overlays
// of one kind (Overlay::oeType: 4 carry, 5 idle, 6 gesture, 7 reaction, 8
// normal, 9 facial, 10 idle facial, 11 receptivity); flags byte 2 (0x01
// flipped, 0x02 blended, 0x20 short blend, 0x08 flip flag in temp 3, 0x40
// normal and flipped), priority s8 at 8. Mode 4 also takes off the carry
// poses of what a Sim holds (Object::carryOverlays). The full-body
// animation is left to the Animate Sim waiting for it. The event trees of
// the overlays stopped get animevent::Stopped (inferred from the idle
// trees, which clear their "playing" flag on it).
Exit animateStop(Context& c) {
    const auto& o = c.instruction.operands;
    Object& target = objectAt(c, 3);
    auto& a = target.animation;
    const int mode = int8_t(o[7]);
    std::string byName;
    if (mode == 0) {
        const std::string entry = listEntry(c, target, o[6], int16_t(le16(&o[0])));
        const std::string first = trim(split(entry, ',')[0]);
        if (first.empty() || first == ".")
            return Exit::True;
        byName = target.person ? resolveListAnimation(c.vm, target, entry) : parseItem(first).name;
    }
    auto isCarry = [&](const Overlay& x) {
        return std::find(target.carryOverlays.begin(), target.carryOverlays.end(), x.name) != target.carryOverlays.end();
    };
    auto stops = [&](const Overlay& x) {
        switch (mode) {
        case 0: return x.name == byName;
        case 1:
        case 3: return true;
        case 4: return x.oeType == oetype::Carry || isCarry(x);
        case 5: return x.oeType == oetype::Idle;
        case 6: return x.oeType == oetype::Gesture;
        case 7: return x.oeType == oetype::Reaction;
        case 8: return x.oeType == oetype::Normal && !isCarry(x);
        case 9: return x.oeType == oetype::Facial;
        case 10: return x.oeType == oetype::FacialIdle;
        case 11: return x.oeType == oetype::Receptivity;
        default: return false;
        }
    };
    std::vector<Overlay> stopped;
    for (auto it = a.overlays.begin(); it != a.overlays.end();) {
        if (stops(*it)) {
            stopped.push_back(std::move(*it));
            it = a.overlays.erase(it);
            if (mode == 0)
                break;
        } else {
            ++it;
        }
    }
    if (stopped.empty())
        return Exit::True;
    ++a.overlaySerial;
    for (const Overlay& x : stopped)
        runOverlayEvent(c.vm, x, animevent::Stopped);
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
// renderer shows (cTSObject::TryChangeEffect, docs/effects-mac.md). Mode u8
// at 0: 0 soft start, 1 hard start, 2 soft stop, 3 hard stop, 4 soft stop
// all, 5 hard stop all, 6 fire and forget, 7 interrogate bone, 8 hard stop
// all and clear the queue, 9 stop ALL, 10-13 state 1-4, 14 stop ALL soft;
// the object (owner u8 at 1, data u16), the effect u16 at 4 (-1: the
// default effect, an error in TS2 except for the stop-all modes), slot byte
// 6 of type s8 9, flags u16 at 10: 0x01 the global STR# 0x8F, 0x02 the
// semi-global's, else the running tree's group; 0x08 the effect index in
// temp 0; 0x80 priority queue; icons 0x04 (object owner u8 12, data u16 13;
// 0x400 GUID in temps 4/5), 0x20 (atlas texture from STR# 0x95 entry byte
// 15, cell from that variable), 0x10, 0x1000, 0x2000. Always true.
Exit effectStopStart(Context& c) {
    const auto& o = c.instruction.operands;
    Object& target = objectAt(c, 1);
    auto& fx = target.animation.effects;
    const uint16_t flags = le16(&o[10]);
    const int16_t effect = flags & 0x08 ? c.thread.temps[0] : int16_t(le16(&o[4]));
    const uint8_t mode = o[0];
    static uint32_t serial = 0;
    // The name: entry `effect` of STR# 0x8F (the binary adds 1: its string
    // lookups count from 1).
    auto tableName = [&](uint16_t list, int index) -> std::string {
        CodeLibrary& code = c.vm.code();
        std::vector<uint32_t> groups;
        if (flags & 0x01)
            groups = {kGlobalGroup};
        else if (flags & 0x02)
            groups = {code.semiGlobalFor(c.frame.group)};
        else
            groups = {c.frame.group};
        if (list == 0x95) // icon names fall back like the binary: tree, semi-global, global
            groups = {c.frame.group, code.semiGlobalFor(c.frame.group), kGlobalGroup};
        for (uint32_t g : groups) {
            const StringTable* t = g ? code.strings(g, list) : nullptr;
            if (t && index >= 0 && size_t(index) < t->count(lang::EnglishUS)) {
                auto n = t->get(size_t(index), lang::EnglishUS);
                if (!n.empty())
                    return n;
            }
        }
        return {};
    };
    switch (mode) {
    case 0:
    case 1:
    case 6: {
        if (effect == -1 || (effect & 0xFF) == 0xFF)
            break; // "Trying to run default effect - not supported anymore."
        ActiveEffect e;
        e.index = effect;
        e.name = tableName(0x8F, effect);
        e.slot = o[6];
        e.slotType = int8_t(o[9]);
        e.flags = flags;
        e.serial = ++serial;
        e.oneShot = mode == 6;
        e.hard = mode == 1;
        // The icon, in the binary's order: 0x1000, 0x04, 0x20, 0x10, 0x2000.
        if (flags & 0x1000) {
            e.iconId = c.thread.temps[6];
        } else if (flags & 0x04) {
            if (flags & 0x400)
                e.iconGuid = uint32_t(uint16_t(c.thread.temps[4])) | (uint32_t(uint16_t(c.thread.temps[5])) << 16);
            else if (flags & 0x100)
                e.iconObject = c.thread.me; // (the binary: another object of the caller's; taken as me)
            else
                e.iconObject = c.get(o[12], int16_t(le16(&o[13])));
        } else if (flags & 0x20) {
            // The atlas named by STR# 0x95 entry byte 15 ("Set FX - Topic -
            // Speech Balloon" passes 4-9: conversationshared01-06), the
            // cell from the variable (the topic).
            e.icon = tableName(0x95, o[15]);
            e.iconFrame = c.get(o[12], int16_t(le16(&o[13])));
        } else if (flags & 0x10) {
            e.iconSim = c.get(o[12], int16_t(le16(&o[13])));
        } else if (flags & 0x2000) {
            e.wantSim = c.get(o[12], int16_t(le16(&o[13])));
            e.wantFear = c.thread.temps[4] != 0;
            e.wantSlot = c.thread.temps[5];
        }
        if (!e.oneShot)
            std::erase_if(fx, [&](const ActiveEffect& x) { return !x.oneShot && x.index == effect && x.flags == flags; });
        else if (std::count_if(fx.begin(), fx.end(), [](const ActiveEffect& x) { return x.oneShot; }) >= 8)
            fx.erase(std::find_if(fx.begin(), fx.end(), [](const ActiveEffect& x) { return x.oneShot; }));
        if (fx.size() < 64)
            fx.push_back(std::move(e));
        break;
    }
    case 2:
    case 3:
        std::erase_if(fx, [&](const ActiveEffect& x) { return !x.oneShot && x.index == effect; });
        break;
    case 4:
    case 5:
    case 8:
    case 9:
    case 14: std::erase_if(fx, [](const ActiveEffect& x) { return !x.oneShot; }); break;
    case 10:
    case 11:
    case 12:
    case 13:
        // States 1-4: the running effect's float parameter 0xF = 0..3
        // ("Interaction - Feed Fish" mode 11, "Kill Fish" mode 12).
        for (auto& x : fx)
            if (x.index == effect)
                x.state = int8_t(mode - 10);
        break;
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

int8_t oeTypeOfRole(const std::string& role) {
    std::string r = trim(role.substr(0, role.find(',')));
    std::transform(r.begin(), r.end(), r.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
    if (r == "carry")
        return oetype::Carry;
    if (r == "idle")
        return oetype::Idle;
    if (r == "gesture")
        return oetype::Gesture;
    if (r == "reaction")
        return oetype::Reaction;
    if (r == "facial")
        return oetype::Facial;
    if (r == "facialidle" || r == "facial idle")
        return oetype::FacialIdle;
    if (r == "receptivity")
        return oetype::Receptivity;
    return oetype::Normal;
}

// How long before its end an overlay starts to blend out (and its event
// tree gets animevent::BlendOut), animation ticks (inferred: the binary's
// blend time is a global set at run time; the host fades overlays in 0.2 s).
constexpr float kOverlayBlendOutTicks = 200;

void tickOverlayEvents(VM& vm) {
    struct Fire {
        int16_t me, codeOwner, stackObject;
        uint16_t tree;
        int16_t event;
    };
    std::vector<Fire> fires;
    for (Object* po : vm.objectTable()) {
        if (!po)
            continue;
        // Animate Overlay's: tree events as they are reached, 9997 as the
        // last play starts blending out, 9999 and off the object at its end.
        auto& ov = po->animation.overlays;
        for (auto it = ov.begin(); it != ov.end();) {
            if (it->loops <= 0 || it->length <= 0) {
                ++it;
                continue;
            }
            it->played += it->perTick;
            const float total = it->length * float(it->loops);
            const bool ended = it->played >= total;
            const float inPlay = std::fmod(it->played, it->length);
            while (!it->pending.empty() && (ended || it->pending.front().first <= inPlay)) {
                fires.push_back({it->me, it->codeOwner, it->stackObject, it->eventTree, it->pending.front().second});
                it->pending.erase(it->pending.begin());
            }
            // A loop that wraps gets its events again.
            if (!ended && it->pending.empty() && it->loops > 1 && inPlay < it->perTick)
                it->pending = library(vm).treeEvents(it->name);
            if (!it->blendOutSent && it->played >= total - kOverlayBlendOutTicks) {
                it->blendOutSent = true;
                fires.push_back({it->me, it->codeOwner, it->stackObject, it->eventTree, animevent::BlendOut});
            }
            if (ended) {
                fires.push_back({it->me, it->codeOwner, it->stackObject, it->eventTree, animevent::ActionEnd});
                fires.push_back({it->me, it->codeOwner, it->stackObject, it->eventTree, animevent::End});
                it = ov.erase(it);
                ++po->animation.overlaySerial;
            } else {
                ++it;
            }
        }
        auto& list = po->animation.overlayEvents;
        for (auto it = list.begin(); it != list.end();) {
            it->played += it->perTick;
            const bool ended = it->played >= it->length;
            while (!it->pending.empty() && (ended || it->pending.front().first <= it->played)) {
                fires.push_back({it->me, it->codeOwner, it->stackObject, it->eventTree, it->pending.front().second});
                it->pending.erase(it->pending.begin());
            }
            // At its end the event tree also gets event 9999 ("Event Tree -
            // Drive" handles the end of each drive loop on 9999: loop
            // again, or stop and set the speed to 0; inferred from it).
            // The engine moves an animated object by its animation's root
            // (cAnimatable): a vehicle goes forward by the drive loop's root
            // travel each time it plays, and "Function - Should Stop?" ends
            // the drive by the distance left to its stop portal (inferred:
            // the move is applied at the loop's end, along the object's
            // facing, to every part of its multi-tile group).
            if (ended && !po->person && po->position.inWorld())
                if (const float d = library(vm).rootTravel(it->name); d != 0) {
                    const auto step = directionStep(po->data[objdata::Direction]);
                    const float len = std::hypot(float(step[0]), float(step[1]));
                    const float dx = d * float(step[0]) / len, dy = d * float(step[1]) / len;
                    for (ObjectId part : groupOf(vm, po->id))
                        if (Object* x = vm.object(part); x && x->position.inWorld())
                        {
                            x->position.x += dx, x->position.y += dy;
                            if (vm.world)
                                x->data[objdata::Room] = vm.world->room(VM::tileOf(*x));
                        }
                }
            if (ended)
                fires.push_back({it->me, it->codeOwner, it->stackObject, it->eventTree, 9999});
            it = ended ? list.erase(it) : it + 1;
        }
    }
    for (const Fire& f : fires) {
        if (!f.tree || !vm.object(f.me) || !vm.object(f.codeOwner))
            continue;
        std::array<int16_t, kTempCount> temps{};
        const int16_t params[5] = {f.event, 0, 0, 0, 0};
        try {
            runOneTickTree(vm, temps, f.me, f.codeOwner, f.stackObject, f.tree, params);
        } catch (const VMError&) {
            // an event tree that fails does not stop the others (inferred)
        }
    }
}

} // namespace os2::vm
