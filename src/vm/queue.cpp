// Interaction queues, Push Interaction, Idle for Input and the pie menu
// (interactions.hpp). The queue logic follows FreeSO's
// tso.simantics/Engine/VMThread.cs (MPL-2.0), rewritten for this VM.

#include "autonomy.hpp"
#include "interactions.hpp"

#include "errors.hpp"
#include "formats/binreader.hpp"
#include "lot.hpp"
#include "ownership.hpp"

#include <algorithm>
#include <cctype>
#include <functional>

namespace os2::vm {

namespace {

constexpr uint8_t kOwnerParam = 0x09;
constexpr uint8_t kOwnerLocal = 0x19;

int16_t interactionGroupOf(const Object& o) {
    return o.definition ? int16_t(o.definition->field(kObjdInteractionGroup)) : 0;
}

int16_t currentPriority(const Object& sim) {
    if (sim.person)
        return sim.person->data[persondata::Priority];
    return sim.activeInteraction >= 0 && size_t(sim.activeInteraction) < sim.queue.size()
               ? sim.queue[size_t(sim.activeInteraction)].priority
               : kPriorityIdle;
}

void setPriority(Object& sim, int16_t p) {
    if (sim.person)
        sim.person->data[persondata::Priority] = p;
}

// Drops waiting entries whose object is gone (FreeSO's
// EvaluateQueuePriorities), and tells the running interaction to stop when
// its priority was dropped to 0 (how The Sims 1 cancels an interaction from
// its own tree; kept from FreeSO). Something more important waiting behind
// it does not cancel it: the binary asks cTSPerson::ShouldInterrupt each
// time instead (shouldInterrupt below), and only a priority strictly above
// the person's interrupts. FreeSO lets another autonomous entry interrupt
// an autonomous one ("allow other auto actions to interrupt us"); the TS2
// binary does not, and with that rule every "*Push Look At" or social
// reaction another Sim pushed cancelled cooking half-way (the route to the
// stove failed "interrupted", "Food Chain - Failure - Cook" dropped the tray).
void evaluatePriorities(VM& vm, Object& sim) {
    auto& q = sim.queue;
    const int active = sim.activeInteraction;
    for (size_t i = size_t(active + 1); i < q.size();)
        if (!vm.object(q[i].object))
            q.erase(q.begin() + std::ptrdiff_t(i));
        else
            ++i;
    if (active < 0 || size_t(active) >= q.size())
        return;
    auto& running = q[size_t(active)];
    if (running.priority != kPriorityIdle && currentPriority(sim) == kPriorityIdle)
        running.cancelled = true;
}

QueuedInteraction entryFor(ObjectId object, const InteractionTableRef& ref, const InteractionTable::Interaction& e,
                           uint8_t language) {
    QueuedInteraction q;
    q.object = object;
    q.interaction = uint16_t(e.stringIndex);
    q.priority = kPriorityUserDriven;
    q.action = e.action;
    q.guard = e.guard;
    q.ttabFlags = uint32_t(e.flags) | uint32_t(e.flags2) << 16;
    if (ref.strings)
        q.name = ref.strings->get(e.stringIndex, language);
    return q;
}

// A nested interaction goes to the end of the sub-queue.
uint16_t enqueueSub(VM& vm, Object& sim, QueuedInteraction q) {
    q.uid = newInteractionId(vm, sim);
    const uint16_t uid = q.uid;
    sim.subQueue.push_back(std::move(q));
    syncQueueData(sim);
    return uid;
}

// Push Interaction (0x0D), as the Mac binary has it (TryPushAction,
// PushActionParam; docs/primitives-mac.md §4): interaction (TTA index) u8
// at 0, where the object is u8 at 1 (a parameter, or a local with flag
// 0x02), priority u8 at 2 (STR# 0xE0: 0 inherited = my person data 33, 1 for
// an object that is not a Sim; 1 maximum = 100; 2 autonomous = 20; 3 user
// driven = 50; others: error 0x2D), flags u8 at 3 (0x01 icon of the object
// in the local of byte 4, 0x02 object in a local, 0x04 "continue as
// current": at the head of the queue, 0x08 "use name", 0x10 the interaction
// number is in (owner u8 at 5, data u16), 0x20 "force run check tree",
// 0x40 link to the interaction whose ID is in (owner u8 at 8, data u16),
// 0x80 the new interaction's ID goes to (owner u8 at 11, data u16)), flags
// u8 at 14 (0x01 pass the first 4 parameters, 0x02 the interaction is the
// last Find Best Interaction's: the object in the target's person data 53
// and the entry in that object's data 20, 0x04/0x08 icon from the temps),
// icon index u8 at 15. The entry goes onto the stack object's queue, which
// must exist (error 0x15) and be a Sim (0x1C). The object must exist
// (0x17), have a TTAB (0x1F) and the entry (0x16); a parameter or local out
// of range is 0x08/0x33. My object data 52 "primitive result" is set to 0,
// or to why the push failed (false): 1 inherited priority but I have none,
// 2 the forced check tree said no, 3 the target's queue is full (more than 6
// entries). Not done: icons and names (no UI here), the link of 0x40 (kept
// in the entry's operand), nested pushes (sub-queue 4, error 0x6F) and the
// part of my running interaction a Sim's push inherits (inferred: its
// owner/group).
Exit pushInteraction(Context& c) {
    const auto& o = c.instruction.operands;
    const uint8_t flags = o[3], flags2 = o[14];
    Object& me = c.me();
    me.data[kObjdataPrimitiveResult] = 0;
    auto failed = [&](int16_t why) {
        if (Object* m = c.vm.object(c.thread.me))
            m->data[kObjdataPrimitiveResult] = why;
        return Exit::False;
    };
    if (!c.vm.object(c.frame.stackObject))
        throw gameError(err::NoStackObject, "Push Interaction");
    const ObjectId targetId = c.frame.stackObject;
    if (!c.vm.object(targetId)->person)
        throw gameError(err::NotAPerson, "Push Interaction onto object " + std::to_string(targetId));

    ObjectId source = 0;
    uint16_t number = flags & 0x10 ? uint16_t(c.get(o[5], int16_t(le16(&o[6])))) : o[0];
    if (flags2 & 0x02) {
        source = c.vm.object(targetId)->person->data[kPersondataFindBestObject];
        const Object* x = c.vm.object(source);
        if (!x)
            throw gameError(err::NoObject, "object " + std::to_string(source));
        // The entry is the TARGET's object data 20, where its Find Best
        // Interaction left it (TryPushAction BV 0xe33530 reads it from the
        // stack object, not from the found object): reading the object's
        // made every "Desespero" push fail (error 0x16) when the found object
        // was another Sim, and its Main died (N001_Lot36 Beau).
        number = uint16_t(c.vm.object(targetId)->data[kObjdataTreeTableEntry]);
    } else if (flags & 0x02) {
        if (o[1] >= c.frame.locals.size())
            throw gameError(err::LocalOutOfRange, "local " + std::to_string(o[1]));
        source = c.get(kOwnerLocal, o[1]);
    } else {
        if (o[1] >= c.frame.params.size())
            throw gameError(err::StackNumberOutOfRange, "parameter " + std::to_string(o[1]));
        source = c.get(kOwnerParam, o[1]);
    }
    const Object* x = c.vm.object(source);
    if (!x)
        throw gameError(err::NoObject, "object " + std::to_string(source));
    if (!x->definition)
        return Exit::False;
    if (!interactionTable(c.vm, *x))
        throw gameError(err::NoTreeTable, "object " + std::to_string(source));
    auto q = makeInteraction(c.vm, source, number);
    if (!q)
        throw gameError(err::NoTreeTableEntry, "interaction " + std::to_string(number) + " of object " +
                                                   std::to_string(source));
    switch (o[2]) {
    case 0: { // inherited: my priority
        const int16_t mine = me.person ? me.person->data[persondata::Priority] : int16_t(1);
        if (mine == 0)
            return failed(1);
        q->priority = mine;
        break;
    }
    case 1: q->priority = kPriorityMaximum; break;
    case 2: q->priority = kPriorityAutonomous; break;
    case 3: q->priority = kPriorityUserDriven; break;
    default: throw gameError(err::BadPriority, "Push Interaction priority " + std::to_string(o[2]));
    }
    if (flags2 & 0x01)
        for (size_t i = 0; i < 4 && i < c.frame.params.size(); ++i)
            q->args[i] = c.frame.params[i];
    if (flags & 0x20) {
        const CheckResult r = checkInteraction(c.vm, targetId, *q);
        if (r != CheckResult::NoGuard && r != CheckResult::True)
            return failed(2);
    }
    Object* target = c.vm.object(targetId);
    if (!target)
        throw gameError(err::NoStackObject, "Push Interaction");
    // A nested interaction (TTAB flag 0x8000) needs the target's current
    // interaction to allow nesting (TTAB flag 0x4000), else error 0x6F, and
    // goes to the sub-queue, which takes 4 (more: result 4)
    // (cTSObject::TryPushAction, docs/primitives-mac.md §4b).
    const bool nested = q->ttabFlags & 0x8000;
    if (nested) {
        const bool running = target->activeInteraction >= 0 && size_t(target->activeInteraction) < target->queue.size();
        if (!running || !(target->queue[size_t(target->activeInteraction)].ttabFlags & 0x4000))
            throw gameError(err::NestedPush, "Push Interaction");
        if (target->subQueue.size() > kMaxSubQueue)
            return failed(4);
    }
    constexpr size_t kMaxQueue = 6;
    if (!nested && target->queue.size() > kMaxQueue)
        return failed(3);
    // Flag 0x01: the object in the local of byte 4 is the interaction's
    // target (and icon; TryPushAction sets it with cTSInteraction's
    // SetTargetObject); a local out of range is error 0x33.
    if (flags & 0x01) {
        if (o[4] >= c.frame.locals.size())
            throw gameError(err::LocalOutOfRange, "local " + std::to_string(o[4]));
        q->target = c.get(kOwnerLocal, o[4]);
    }
    q->flags = flags;
    q->pushedBy = c.thread.me;
    q->operands = o;
    const uint16_t uid = nested ? enqueueSub(c.vm, *target, std::move(*q)) : enqueue(c.vm, *target, std::move(*q), flags & 0x04);
    if (flags & 0x40) // link to the interaction whose ID is in (owner u8 at 8, data u16)
        if (const auto linked = uint16_t(c.get(o[8], int16_t(le16(&o[9])))))
            if (Object* t = c.vm.object(targetId)) {
                auto find = [&](std::vector<QueuedInteraction>& v) -> QueuedInteraction* {
                    for (auto& x : v)
                        if (x.uid == uid)
                            return &x;
                    return nullptr;
                };
                if (QueuedInteraction* made = nested ? find(t->subQueue) : find(t->queue))
                    linkInteractions(c.vm, *made, linked);
            }
    if (flags & 0x80)
        c.set(o[11], int16_t(le16(&o[12])), int16_t(uid));
    return Exit::True;
}

// Idle for Input in "Handle Sub Queue Interactions" mode (byte 4 bit 0;
// cTSPerson::TryIdleForInput, Mac BV): with nothing in the sub-queue, true.
// Else the first entry leaves it and its check tree runs; when it passes
// and the entry has an action tree, that becomes the current sub-action and
// its tree is pushed on top of this one (the primitive's 3); unlike the
// queue's, this tree's result is what this instruction branches on (the
// binary does not ask for the re-run here). A failed check clears the whole
// sub-queue and the result is true.
Exit handleSubQueue(Context& c) {
    Object& me = c.me();
    if (me.subQueue.empty())
        return Exit::True;
    const QueuedInteraction item = me.subQueue.front();
    me.subQueue.erase(me.subQueue.begin());
    bool ok = c.vm.object(item.object) != nullptr && item.action != 0;
    if (ok) {
        const CheckResult r = checkInteraction(c.vm, c.thread.me, item);
        ok = r == CheckResult::NoGuard || r == CheckResult::True;
    }
    Object* sim = c.vm.object(c.thread.me);
    if (!sim)
        return Exit::True;
    if (ok) {
        try {
            c.vm.call(c.thread, item.object, item.action, item.args);
        } catch (const VMError&) {
            ok = false;
        }
    }
    if (!ok) {
        sim->subQueue.clear();
        syncQueueData(*sim);
        return Exit::True;
    }
    Frame& f = c.thread.stack.back();
    f.stackObject = item.object;
    f.subInteraction = true;
    sim->subAction = item;
    updateObjectOwnership(c.vm, c.thread.me, item.object);
    syncQueueData(*sim);
    return Exit::Call;
}

// Idle for Input (0x11), as the Mac binary has it (cTSPerson::
// TryIdleForInput; docs/primitives-mac.md §4): the parameter (u16 at 0)
// holding the ticks left (out of range: error 0x08), "allow push" u16 at 2
// (non-zero), byte 4 bit 0 "Handle Sub Queue Interactions". With nothing to
// do (ShouldInterrupt: an interaction waiting in the queue, the running one
// cancelled, or a Notify the Stack Object out of Idle) it counts the
// parameter down and yields while it is above 0 and, at 0, clears the
// object flag "notified by idle for input" and returns true: it waits
// exactly N ticks. With something to do it sets that flag; with "allow push"
// outside an interaction the next queued interaction that may run (its
// priority beats the running one and its check tree passes: attemptPush)
// is started: its action tree is pushed on top of this tree and the counter
// goes to 0; when it returns, this instruction runs again at once (see
// below). Otherwise true. In the sub-queue mode the binary runs the queued
// sub-actions; nested interactions are not pushed here, so there are none
// and the instruction returns true at once. Not done: the binary's
// "ActionSkipped" (an entry whose check tree fails makes it yield; here the
// next entry is tried at once).
Exit idleForInput(Context& c) {
    Thread& t = c.thread;
    // The interaction this instruction ran has returned: the binary runs the
    // instruction again at once (the tree stack element's "re-run" flag, set
    // when it started the action, keeps DoNodeAction from branching) and, in
    // that re-run, only starts the next entry when it was pushed "continue as
    // current" (Push Interaction byte 3 bit 0x04, cTSInteraction +0x84) and
    // the finished one was not cancelled (inferred from the check on the
    // current action); else it returns true and Main runs its "Post
    // Interaction" (cTSPerson::TryIdleForInput). "Sub - Push Eat" pushes Eat
    // that way, so Main's "Kill - Objects in Hands" does not take the snack.
    if (t.interactionReturned) {
        const bool cancelled = t.interactionReturned == 2;
        t.interactionReturned = 0;
        const Object& me = c.me();
        const size_t next = size_t(me.activeInteraction + 1);
        if (cancelled || next >= me.queue.size() || !(me.queue[next].flags & 0x04))
            return Exit::True;
    }
    const auto& o = c.instruction.operands;
    if (o[4] & 0x01)
        return handleSubQueue(c);
    const auto param = int16_t(le16(&o[0]));
    if (param < 0 || size_t(param) >= c.frame.params.size())
        throw gameError(err::StackNumberOutOfRange, "Idle for Input parameter " + std::to_string(param));
    const bool allowPush = le16(&o[2]) != 0;
    Object& me = c.me();
    const bool inAction = inInteraction(t);
    const size_t next = size_t(me.activeInteraction + 1);
    const bool waiting = next < me.queue.size();
    // cTSPerson::ShouldInterrupt (BV 0xad6388): person data 71
    // "non-interruptible" says no; the current action or sub-action
    // cancelled says yes; else only a first waiting entry whose priority is
    // above the person's (data 33). A waiting entry of no higher priority
    // left Mains spinning here without yielding (the social bunny's
    // "Function - Main - Check Queue" ran out of its instruction budget).
    const bool pending = me.idleInterrupt || (me.person ? shouldInterrupt(me) : inAction ? interactionCancelled(me) : waiting);
    const int16_t ticks = c.get(kOwnerParam, param);
    if (!pending) {
        if (ticks > 0) {
            c.set(kOwnerParam, param, int16_t(ticks - 1));
            return Exit::Yield;
        }
        me.data[objdata::Flags] = int16_t(me.data[objdata::Flags] & ~kFlagNotifiedByIdleForInput);
        return Exit::True;
    }
    me.data[objdata::Flags] = int16_t(me.data[objdata::Flags] | kFlagNotifiedByIdleForInput);
    me.idleInterrupt = false;
    if (allowPush && !inAction && waiting) {
        c.set(kOwnerParam, param, 0); // before the push, which may move this frame
        const size_t depth = t.stack.size();
        if (attemptPush(c.vm, t))
            return Exit::Call;
        if (t.stack.size() == depth) // nothing could run: the counter is left as it was
            t.stack.back().params[size_t(param)] = ticks;
    }
    return Exit::True;
}

// Notify the Stack Object out of Idle (0x31): its next Idle for Input
// returns at once (FreeSO's VMNotifyOutOfIdle; the operand is not used).
Exit notifyOutOfIdle(Context& c) {
    if (Object* o = c.vm.object(c.frame.stackObject))
        o->idleInterrupt = true;
    return Exit::True;
}

// A name for the $-tokens of action strings: a Sim's first name (from the
// OBJD file name "N001_User00011 - Don"), else the object's OBJD title.
std::string displayName(const Object* o) {
    if (!o || !o->definition)
        return "?";
    const std::string& n = o->definition->fileName;
    if (o->person)
        if (auto dash = n.find(" - "); dash != std::string::npos)
            return n.substr(dash + 3);
    return o->definition->title.empty() ? n : o->definition->title;
}

// The $-tokens of action strings that can be filled in here: $Object and $Me
// (stack object and me), $NameLocal:N / $FullNameLocal:N (the object in
// local N), $Local:N / $$Local:N (the value of local N; "$$" is money in the
// game's strings). Others stay as they are (inferred from STR# 0x12E).
std::string expandActionString(Context& c, std::string s) {
    auto replaceAll = [&](const std::string& token, const std::function<std::string(int)>& value, bool indexed) {
        for (size_t at = 0; (at = s.find(token, at)) != std::string::npos;) {
            size_t end = at + token.size();
            int n = 0;
            if (indexed) {
                if (end >= s.size() || s[end] != ':' || end + 1 >= s.size() || !std::isdigit(static_cast<unsigned char>(s[end + 1]))) {
                    at = end;
                    continue;
                }
                ++end;
                while (end < s.size() && std::isdigit(static_cast<unsigned char>(s[end])))
                    n = n * 10 + (s[end++] - '0');
            }
            std::string v;
            try {
                v = value(n);
            } catch (const VMError&) {
                at = end;
                continue;
            }
            s.replace(at, end - at, v);
            at += v.size();
        }
    };
    auto local = [&](int n) { return c.get(kOwnerLocal, int16_t(n)); };
    replaceAll("$FullNameLocal", [&](int n) { return displayName(c.vm.object(local(n))); }, true);
    replaceAll("$NameLocal", [&](int n) { return displayName(c.vm.object(local(n))); }, true);
    replaceAll("$$Local", [&](int n) { return "§" + std::to_string(local(n)); }, true);
    replaceAll("$Local", [&](int n) { return std::to_string(local(n)); }, true);
    replaceAll("$Object", [&](int) { return displayName(c.vm.object(c.frame.stackObject)); }, false);
    replaceAll("$Me", [&](int) { return displayName(c.vm.object(c.thread.me)); }, false);
    return s;
}

// Add/Change the Action String (0x32), as the Mac base-game binary has it
// (cTSObject::TryMakeActionString, base 1.0): STR# u16 at 0 (0 = 0x12E, the
// "MakeActionString prim string set" every group with action strings has),
// flags u16 at 2: 0x04 the global table, 0x08 the semi-global one, neither:
// the private one, then the semi-global, then the global one (the first
// that has the string); 0x10 the string number is temp 0; string number u8
// at 4 (1-based). In a check tree run for the pie menu or autonomy it adds
// an entry with parameters {stack object, temp 0} (sMakeNewMenuItem); in an
// interaction it renames it.
//
// Mode s8 at 9 (docs/primitives-mac.md §5): not 0 sets the entry's icon
// (an object's thumbnail: GUID u32 at 5, index byte 10) instead of adding
// or renaming one: "Interaction - Watch Clouds/Stars - TEST" of the go-here
// destination sets the cloud icon and then adds "Ver Nuvens" (string 2);
// read as text, the icon operand added a second entry "Nadar Aqui" (string
// 4). The pie menu shows no icons: nothing is kept.
Exit changeActionString(Context& c) {
    const auto& o = c.instruction.operands;
    if (int8_t(o[9]) != 0)
        return Exit::True;
    uint16_t table = le16(&o[0]);
    if (table == 0)
        table = 0x12E;
    const uint16_t flags = le16(&o[2]);
    const Object* owner = c.vm.object(c.frame.codeOwner);
    if (!owner)
        return Exit::True;
    const int index = flags & 0x10 ? c.thread.temps[0] : o[4];
    const StringTable* strings = nullptr;
    auto tryGroup = [&](uint32_t group) {
        if (strings || !group)
            return;
        const StringTable* t = c.vm.code().strings(group, table);
        if (t && index >= 1 && !t->get(size_t(index - 1)).empty())
            strings = t;
    };
    if (flags & 0x04)
        tryGroup(kGlobalGroup);
    else if (flags & 0x08)
        tryGroup(owner->semiGroup);
    else {
        tryGroup(owner->group);
        tryGroup(owner->semiGroup);
        tryGroup(kGlobalGroup);
    }
    if (!strings || index < 1)
        throw VMError("Add/Change the Action String: no STR# " + hexString(table) + " string " + std::to_string(index));
    const std::string name = expandActionString(c, strings->get(size_t(index - 1)));
    if (c.thread.actionStrings) {
        ActionString a{name, c.frame.stackObject};
        a.params = {c.frame.stackObject, c.thread.temps[0], 0, 0};
        a.adMin = c.thread.adMin;
        a.adRange = c.thread.adRange;
        a.adPersonality = c.thread.adPersonality;
        c.thread.actionStrings->push_back(std::move(a));
    } else if (inInteraction(c.thread)) {
        Object& me = c.me();
        if (me.activeInteraction >= 0 && size_t(me.activeInteraction) < me.queue.size())
            me.queue[size_t(me.activeInteraction)].name = name;
    }
    return Exit::True;
}

} // namespace

std::optional<InteractionTableRef> interactionTable(VM& vm, const Object& o) {
    if (!o.definition)
        return std::nullopt;
    uint16_t tt = o.definition->field(kObjdTreeTableId);
    // A multi-tile part that leads its interaction group (OBJD interaction
    // group < 0) without a table of its own uses its master's (the vehicles'
    // 0,5 part; the master "Car - Career - Sedan" has the rides the riders
    // push; inferred).
    if (tt == 0xFFFF && int16_t(o.definition->interactionGroup()) < 0)
        if (const uint16_t m = o.definition->masterId())
            if (auto mi = vm.code().masterDefinition(o.group, m))
                if (const ObjectDefinition* md = vm.code().definition(o.group, *mi))
                    tt = md->field(kObjdTreeTableId);
    if (tt == 0 || tt == 0xFFFF)
        return std::nullopt;
    for (uint32_t g : {o.group, o.semiGroup})
        if (g)
            if (const InteractionTable* t = vm.code().interactions(g, tt))
                return InteractionTableRef{g, tt, t, vm.code().interactionStrings(g, tt)};
    return std::nullopt;
}

ObjectId interactionLeader(VM& vm, ObjectId id) {
    const Object* o = vm.object(id);
    if (!o)
        return id;
    const int16_t g = interactionGroupOf(*o);
    if (g < 1)
        return id;
    const ObjectId lead = multiTileGroup(*o);
    for (const auto& [oid, x] : vm.objects())
        if (multiTileGroup(x) == lead && interactionGroupOf(x) == -g)
            return oid;
    // The lots' saved multi-tile parts do not say which object they belong
    // to (each leads itself), so fall back to the nearest leader of the same
    // object (OBJD group) within two tiles on the same level (inferred).
    ObjectId best = id;
    float bestD = 2.0f * 2.0f + 0.01f;
    for (const auto& [oid, x] : vm.objects()) {
        if (x.group != o->group || interactionGroupOf(x) != -g || x.position.level != o->position.level)
            continue;
        const float dx = x.position.x - o->position.x, dy = x.position.y - o->position.y;
        if (dx * dx + dy * dy < bestD)
            bestD = dx * dx + dy * dy, best = oid;
    }
    return best;
}

std::optional<QueuedInteraction> makeInteraction(VM& vm, ObjectId object, uint16_t interaction, uint8_t language) {
    const Object* o = vm.object(object);
    if (!o)
        return std::nullopt;
    const auto ref = interactionTable(vm, *o);
    if (!ref)
        return std::nullopt;
    for (const auto& e : ref->table->interactions)
        if (e.stringIndex == interaction)
            return entryFor(object, *ref, e, language);
    return std::nullopt;
}

uint16_t enqueue(VM& vm, Object& sim, QueuedInteraction q, bool pushHead) {
    // A nested interaction picked while the running one allows nesting goes
    // to the sub-queue (as Push Interaction puts it; inferred for the menu).
    if ((q.ttabFlags & 0x8000) && sim.activeInteraction >= 0 && size_t(sim.activeInteraction) < sim.queue.size() &&
        (sim.queue[size_t(sim.activeInteraction)].ttabFlags & 0x4000) && sim.subQueue.size() <= kMaxSubQueue)
        return enqueueSub(vm, sim, std::move(q));
    auto& queue = sim.queue;
    if (queue.size() >= kMaxQueuedInteractions)
        throw VMError("interaction queue of object " + std::to_string(sim.id) + " is full");
    q.uid = newInteractionId(vm, sim);
    const uint16_t uid = q.uid;
    const size_t first = size_t(sim.activeInteraction + 1);
    size_t at = first;
    if (queue.empty()) {
        at = 0;
    } else if (!pushHead) {
        // After the last waiting entry of the same or higher priority; else
        // right after the running one.
        for (size_t i = queue.size(); i-- > first;)
            if (q.priority <= queue[i].priority) {
                at = i + 1;
                break;
            }
    }
    queue.insert(queue.begin() + std::ptrdiff_t(std::min(at, queue.size())), std::move(q));
    evaluatePriorities(vm, sim);
    syncQueueData(sim);
    return uid;
}

bool cancelInteraction(VM& vm, Object& sim, uint16_t uid) {
    auto& q = sim.queue;
    auto it = std::find_if(q.begin(), q.end(), [&](const QueuedInteraction& x) { return x.uid == uid; });
    if (it == q.end()) {
        // A sub-action: a waiting one leaves the sub-queue, the running one
        // is told to stop as the running interaction is (inferred, as for
        // the queue; the binary's cTSPerson::DeleteTopSubAction drops the
        // first waiting one when a Sim is cleaned up).
        auto& sq = sim.subQueue;
        auto st = std::find_if(sq.begin(), sq.end(), [&](const QueuedInteraction& x) { return x.uid == uid; });
        if (st != sq.end())
            sq.erase(st);
        else if (sim.subAction && sim.subAction->uid == uid)
            sim.subAction->cancelled = true;
        else
            return false;
        syncQueueData(sim);
        return true;
    }
    const auto index = int(it - q.begin());
    if (index > sim.activeInteraction) {
        const QueuedInteraction gone = *it;
        q.erase(it);
        unlinkInteraction(vm, sim.id, gone);
    } else {
        it->cancelled = true;
        setPriority(sim, kPriorityIdle);
    }
    evaluatePriorities(vm, sim);
    syncQueueData(sim);
    return true;
}

void cancelInteractionsWith(VM& vm, ObjectId object) {
    std::vector<ObjectId> ids;
    for (const auto& [id, o] : vm.objects())
        if (!o.queue.empty() || !o.subQueue.empty())
            ids.push_back(id);
    for (ObjectId id : ids) {
        Object& sim = *vm.object(id);
        auto& q = sim.queue;
        for (size_t i = q.size(); i-- > 0;) {
            if (q[i].object != object)
                continue;
            if (int(i) > sim.activeInteraction)
                q.erase(q.begin() + std::ptrdiff_t(i));
            else
                q[i].cancelled = true;
        }
        std::erase_if(sim.subQueue, [&](const QueuedInteraction& x) { return x.object == object; });
        syncQueueData(sim);
    }
}

// cTSPerson::ShouldInterrupt (BV 0xad6388): person data 71
// "non-interruptible" says no; the current action or sub-action cancelled
// says yes; else only a first waiting entry whose priority is above the
// person's (data 33). A waiting entry of no higher priority left Mains
// spinning in Idle for Input without yielding (the social bunny's "Function
// - Main - Check Queue" ran out of its instruction budget).
bool shouldInterrupt(const Object& sim) {
    if (!sim.person)
        return interactionCancelled(sim);
    if (sim.person->data[persondata::NonInterruptible] != 0)
        return false;
    const size_t next = size_t(sim.activeInteraction + 1);
    return interactionCancelled(sim) ||
           (next < sim.queue.size() && sim.queue[next].priority > sim.person->data[persondata::Priority]);
}

bool interactionCancelled(const Object& sim) {
    if (sim.subAction && sim.subAction->cancelled)
        return true;
    return sim.activeInteraction >= 0 && size_t(sim.activeInteraction) < sim.queue.size() &&
           sim.queue[size_t(sim.activeInteraction)].cancelled;
}

CheckResult checkInteraction(VM& vm, ObjectId sim, const QueuedInteraction& q, std::string* error, bool autonomous,
                             size_t maxTurns, std::vector<ActionString>* strings) {
    if (!q.guard)
        return CheckResult::NoGuard;
    const int16_t args[1] = {int16_t(autonomous ? 1 : 0)};
    Thread t = vm.start(sim, q.guard, args, q.object, q.object);
    t.actionStrings = strings;
    if (auto it = vm.threads.find(sim); it != vm.threads.end())
        t.temps = it->second.temps;
    for (size_t turn = 0; t.status == Thread::Status::Running && turn < maxTurns; ++turn)
        vm.run(t, vm.instructionsPerTurn);
    switch (t.status) {
    case Thread::Status::Finished: return t.result ? CheckResult::True : CheckResult::False;
    case Thread::Status::Running:
        if (error)
            *error = "still running after " + std::to_string(maxTurns) + " ticks";
        return CheckResult::Running;
    default:
        if (error)
            *error = t.status == Thread::Status::Killed ? "killed" : t.error;
        return CheckResult::Failed;
    }
}

bool attemptPush(VM& vm, Thread& t) {
    Object* sim = vm.object(t.me);
    if (!sim)
        return false;
    int compare = currentPriority(*sim);
    if (compare <= kPriorityAutonomous)
        compare -= 1; // autonomous interactions fall through to other ones
    for (;;) {
        const size_t next = size_t(sim->activeInteraction + 1);
        if (sim->queue.size() <= next)
            return false;
        const QueuedInteraction item = sim->queue[next];
        if (item.priority <= compare)
            return false;
        // Taking the next entry clears the sub-actions, whether it passes its
        // check or not (cTSPerson::TryIdleForInput calls ClearSubActions before
        // the test; SetCurrentAction does it again).
        clearSubActions(*sim);
        bool ok = vm.object(item.object) != nullptr;
        if (ok) {
            const CheckResult r = checkInteraction(vm, t.me, item);
            ok = r == CheckResult::NoGuard || r == CheckResult::True;
            sim = vm.object(t.me); // the check tree may have removed the Sim
            if (!sim)
                return false;
        }
        // The check tree may have changed the queue: find the entry again.
        auto it = std::find_if(sim->queue.begin(), sim->queue.end(),
                               [&](const QueuedInteraction& x) { return x.uid == item.uid; });
        if (it == sim->queue.end())
            continue;
        if (ok) {
            try {
                vm.call(t, item.object, item.action, item.args);
            } catch (const VMError&) {
                ok = false; // the action tree does not exist
            }
        }
        if (!ok) {
            sim->queue.erase(it);
            syncQueueData(*sim);
            continue;
        }
        Frame& f = t.stack.back();
        f.stackObject = item.object;
        f.interaction = true;
        // Run it from where it is now in the queue (normally `next`).
        const auto at = it - sim->queue.begin();
        if (size_t(at) != next) {
            QueuedInteraction moved = std::move(*it);
            sim->queue.erase(it);
            sim->queue.insert(sim->queue.begin() + std::ptrdiff_t(next), std::move(moved));
        }
        sim->activeInteraction = int(next);
        updateObjectOwnership(vm, t.me, item.object);
        setPriority(*sim, item.priority);
        evaluatePriorities(vm, *sim);
        syncQueueData(*sim);
        return true;
    }
}

void endInteraction(VM& vm, Thread& t) {
    t.interactionReturned = 1;
    Object* sim = vm.object(t.me);
    if (!sim)
        return;
    auto& q = sim->queue;
    if (interactionCancelled(*sim))
        t.interactionReturned = 2;
    if (sim->activeInteraction >= 0) {
        if (size_t(sim->activeInteraction) < q.size()) {
            const QueuedInteraction gone = q[size_t(sim->activeInteraction)];
            q.erase(q.begin() + sim->activeInteraction);
            unlinkInteraction(vm, sim->id, gone);
            sim = vm.object(t.me);
            if (!sim)
                return;
        }
        --sim->activeInteraction;
    }
    if (sim->person)
        sim->person->data[persondata::NonInterruptible] = 0; // FreeSO: "verified in ts1"
    setPriority(*sim, sim->activeInteraction >= 0 ? q[size_t(sim->activeInteraction)].priority : kPriorityIdle);
    evaluatePriorities(vm, *sim);
    syncQueueData(*sim);
}

void clearSubActions(Object& sim) {
    if (sim.subQueue.empty() && !sim.subAction)
        return;
    sim.subQueue.clear();
    sim.subAction.reset();
    syncQueueData(sim);
}

void endSubInteraction(VM& vm, Thread& t) {
    if (Object* sim = vm.object(t.me)) {
        if (sim->subAction) {
            const QueuedInteraction gone = *sim->subAction;
            sim->subAction.reset();
            unlinkInteraction(vm, sim->id, gone);
            sim = vm.object(t.me);
            if (!sim)
                return;
        }
        syncQueueData(*sim);
    }
}

bool inInteraction(const Thread& t) {
    return std::any_of(t.stack.begin(), t.stack.end(), [](const Frame& f) { return f.interaction; });
}

void syncQueueData(Object& sim) {
    if (!sim.person)
        return;
    auto& d = sim.person->data;
    const auto& q = sim.queue;
    const bool running = sim.activeInteraction >= 0 && size_t(sim.activeInteraction) < q.size();
    const QueuedInteraction* a = running ? &q[size_t(sim.activeInteraction)] : nullptr;
    d[persondata::CurrentInteractionIndex] = a ? int16_t(a->interaction) : 0;
    d[persondata::CurrentRunningInteractionIndex] = a ? int16_t(a->interaction) : 0;
    d[persondata::CurrentInteractionObject] = a ? a->object : 0;
    d[persondata::InteractionAutonomous] = a && a->priority == kPriorityAutonomous;
    // 109 "ci: Current Interaction GUID": the running interaction's ID, which
    // trees hand to Push Interaction's link (flag 0x40; "Interaction - Read to").
    d[109] = a ? int16_t(a->uid) : 0;
    const size_t next = size_t(sim.activeInteraction + 1);
    d[persondata::NextInteractionIndex] = next < q.size() ? int16_t(q[next].interaction) : 0;
    d[persondata::NextInteractionObject] = next < q.size() ? q[next].object : 0;
    // 72 "cn: Next Queued Interaction Continuation?": the next entry was
    // pushed "continue as current" (Push Interaction byte 3 bit 0x04).
    // "Function - Main - Process Autonomy" reads it after Gosub Found Action
    // and, when set, waits for input (Wait For Notify) instead of running
    // "Post Interaction", so an autonomous snack goes on to "Eat" before
    // Main's "Kill - Objects in Hands" takes it (inferred from the label).
    d[72] = next < q.size() && (q[next].flags & 0x04) ? 1 : 0;
    // The sub-queue: its size, the interaction it nests in (the running
    // one), the next waiting entry and the running sub-action (labels of
    // STR# 0xC8; filled in as the queue's are, inferred).
    const auto& sq = sim.subQueue;
    d[persondata::SubQueueCount] = int16_t(sq.size());
    d[persondata::SubQueueMasterObject] = a && (!sq.empty() || sim.subAction) ? a->object : 0;
    d[persondata::SubQueueMasterIndex] = a && (!sq.empty() || sim.subAction) ? int16_t(a->interaction) : 0;
    d[persondata::NextSubQueueIndex] = sq.empty() ? 0 : int16_t(sq.front().interaction);
    d[persondata::NextSubQueueObject] = sq.empty() ? 0 : sq.front().object;
    d[persondata::SubQueueObject] = sim.subAction ? sim.subAction->object : 0;
}

std::vector<std::string> splitPiePath(const std::string& name) {
    std::vector<std::string> out;
    size_t start = 0;
    for (size_t slash; (slash = name.find('/', start)) != std::string::npos; start = slash + 1)
        out.push_back(name.substr(start, slash - start));
    out.push_back(name.substr(start));
    return out;
}

std::vector<PieEntry> pieMenu(VM& vm, ObjectId sim, ObjectId object, const PieOptions& opt) {
    std::vector<PieEntry> out;
    const ObjectId leader = interactionLeader(vm, object);
    const Object* o = vm.object(leader);
    if (!o)
        return out;
    const auto ref = interactionTable(vm, *o);
    if (!ref)
        return out;
    for (size_t i = 0; i < ref->table->interactions.size(); ++i) {
        const auto& e = ref->table->interactions[i];
        QueuedInteraction q = entryFor(leader, *ref, e, opt.language);
        PieEntry p;
        p.object = leader;
        p.interaction = q.interaction;
        p.tableIndex = i;
        p.name = q.name.empty() ? "#" + std::to_string(q.interaction) : q.name;
        p.path = splitPiePath(p.name);
        // Debug interactions: TTAB flag 0x80 (as in The Sims 1: 212 distinct
        // "DEBUG - ...", "CHEAT - ..." names have it) or a US English name
        // starting with '*' (the translations sometimes drop the star).
        const std::string english = ref->strings ? ref->strings->get(e.stringIndex, lang::EnglishUS) : "";
        p.debug = (e.flags & 0x80) || (!english.empty() && english[0] == '*') || (!p.name.empty() && p.name[0] == '*');
        // Debug interactions are only offered with the testing cheats; their
        // check trees are not run (as FreeSO does for its debug flag).
        std::vector<ActionString> strings;
        if (const Object* s = vm.object(sim))
            p.wrongAge = !availableToAge(*s, *ref->table, e);
        if (!p.debug && !p.wrongAge) {
            if (Object* s = vm.object(sim))
                s->data[objdata::HideInteraction] = 0;
            p.check = checkInteraction(vm, sim, q, &p.error, opt.autonomous, opt.maxTurns, &strings);
            if (Object* s = vm.object(sim)) {
                p.hidden = s->data[objdata::HideInteraction] == 1;
                s->data[objdata::HideInteraction] = 0;
            }
        }
        if (!p.available() || strings.empty()) {
            out.push_back(std::move(p));
            continue;
        }
        for (const auto& s : strings) {
            PieEntry d = p;
            d.dynamic = true;
            d.name = s.name;
            d.path = splitPiePath(s.name);
            d.args = s.params;
            out.push_back(std::move(d));
        }
    }
    return out;
}

std::optional<QueuedInteraction> interactionFromPie(VM& vm, const PieEntry& e) {
    auto q = makeInteraction(vm, e.object, e.interaction);
    if (!q)
        return std::nullopt;
    q->name = e.name;
    q->args = e.args;
    return q;
}

void registerInteractionPrimitives(VM& vm) {
    vm.setPrimitive(0x0D, pushInteraction);
    vm.setPrimitive(0x11, idleForInput);
    vm.setPrimitive(0x31, notifyOutOfIdle);
    vm.setPrimitive(0x32, changeActionString);
    registerTurnPrimitive(vm); // Turn Body Towards (turn.cpp)
}

namespace {

std::string lower(std::string s) {
    for (auto& c : s)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool isNumber(const std::string& s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c); });
}

} // namespace

ObjectId findInteractionObject(VM& vm, const std::string& query) {
    if (isNumber(query))
        return vm.object(ObjectId(std::stoi(query))) ? ObjectId(std::stoi(query)) : 0;
    const std::string q = lower(query);
    for (const auto& [id, o] : vm.objects()) {
        if (!o.position.inWorld() || o.person || !o.definition || lower(o.definition->fileName).find(q) == std::string::npos)
            continue;
        const Object* leader = vm.object(interactionLeader(vm, id));
        if (leader && interactionTable(vm, *leader))
            return id;
    }
    return 0;
}

ObjectId findSim(VM& vm, const std::string& query) {
    if (isNumber(query)) {
        const Object* o = vm.object(ObjectId(std::stoi(query)));
        return o && o->person ? o->id : 0;
    }
    if (query.empty()) {
        const Object* sel = vm.object(vm.globals[global::SelectedPersonId]);
        if (sel && sel->person && sel->position.inWorld())
            return sel->id;
    }
    const std::string q = lower(query);
    for (const auto& [id, o] : vm.objects())
        if (o.person && o.position.inWorld() &&
            (q.empty() || (o.definition && lower(o.definition->fileName).find(q) != std::string::npos)))
            return id;
    return 0;
}

void setAllMotives(VM& vm, int16_t value) {
    std::vector<ObjectId> sims;
    for (const auto& [id, o] : vm.objects())
        if (o.person)
            sims.push_back(id);
    for (ObjectId id : sims)
        for (size_t k = 3; k < kMotiveCount; ++k)
            vm.object(id)->person->motives[k] = value;
}

void startLotObjects(VM& vm, const LotLoad& load, int ticks) {
    for (const auto& e : load.objects)
        if (e.created && vm.object(e.id))
            vm.runFunction(e.id, 0);
    for (const auto& e : load.objects)
        if (e.created && vm.object(e.id))
            vm.startMain(e.id);
    for (int i = 0; i < ticks; ++i)
        vm.tick();
}

} // namespace os2::vm
