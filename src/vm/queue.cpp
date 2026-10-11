// Interaction queues, Push Interaction, Idle for Input and the pie menu
// (interactions.hpp). The queue logic follows FreeSO's
// tso.simantics/Engine/VMThread.cs (MPL-2.0), rewritten for this VM.

#include "interactions.hpp"

#include "formats/binreader.hpp"
#include "lot.hpp"

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

// FreeSO's EvaluateQueuePriorities: drops waiting entries whose object is
// gone, and tells the running interaction to stop when something more
// important waits behind it, or when its priority was dropped to 0 (how
// The Sims 1 cancels an interaction from its own tree).
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
    int current = currentPriority(sim);
    if (current == kPriorityAutonomous)
        current -= 1; // other autonomous interactions may interrupt it
    auto& running = q[size_t(active)];
    if (running.priority != kPriorityIdle && current == kPriorityIdle)
        running.cancelled = true;
    for (size_t i = size_t(active + 1); i < q.size(); ++i)
        if (q[i].priority > current)
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

// Push Interaction (0x0D), The Sims 1's layout (FreeSO's VMPushInteraction),
// which the game's trees agree with: interaction (TTA index) u8 at 0, object
// location u8 at 1 (a local with flag 0x02, else a parameter: 1,299 of the
// 1,305 pushes in the game name one the tree has), priority u8 at 2 (STR#
// 0xE0: inherited, maximum, autonomous, user driven), flags u8 at 3 (0x01
// custom icon, 0x02 object in a local, 0x04 push head continuation; 0x08,
// 0x10, 0x20 and 0x40 are new and unknown), icon location u8 at 4. Bytes
// 5-13 are three (owner u8, data u16) variables and byte 15 is 0/1; the
// editor's disassembly strings (Sims2.exe) suggest "getting interaction #
// from", "Returing ID in", "Linking to interaction with ID in" and "Passing
// first 4 params in", but which is which is not known, so they are kept
// in the queue entry and not used. The entry goes onto the stack object's
// queue ("... onto the stack object's queue"); an object that does not
// exist or has no such interaction fails the push (false, as in FreeSO).
Exit pushInteraction(Context& c) {
    const auto& o = c.instruction.operands;
    const uint8_t flags = o[3];
    const ObjectId source = c.get(flags & 0x02 ? kOwnerLocal : kOwnerParam, o[1]);
    if (!c.vm.object(source))
        return Exit::False;
    Object& target = c.stackObject();
    auto q = makeInteraction(c.vm, source, o[0]);
    if (!q)
        return Exit::False;
    switch (o[2]) {
    case 0: { // inherited: the running interaction's priority (at least 1)
        int16_t old = 1;
        if (inInteraction(c.thread))
            old = currentPriority(c.me());
        q->priority = std::max<int16_t>(1, old);
        break;
    }
    case 1: q->priority = kPriorityMaximum; break;
    case 2: q->priority = kPriorityAutonomous; break;
    case 3: q->priority = kPriorityUserDriven; break;
    default: throw VMError("Push Interaction: priority " + std::to_string(o[2]) + " not implemented");
    }
    q->flags = flags;
    q->pushedBy = c.thread.me;
    q->operands = o;
    enqueue(c.vm, target, std::move(*q), flags & 0x04);
    return Exit::True;
}

// Idle for Input (0x11): u16 index of the parameter holding the ticks left,
// u16 "allow push" at 2 (as in The Sims 1; the editor's strings "allow
// push" / "do not allow push"); byte 4 = 1 in 106 of the game's 111 uses
// ("_Handle SubQueue" trees; the strings name a "Handle Sub Queue
// Interactions" mode), not implemented: those idle like the rest. As in
// FreeSO: the parameter counts down once per tick (the instruction returns
// true when it goes below 0); outside an interaction, "allow push" runs the
// next queued interaction (its action tree is pushed on top and, when it
// returns, this instruction returns true on the next tick); a queue with
// something waiting returns true at once and sets the object flag "notified
// by idle for input"; inside an interaction that was cancelled it returns
// true at once.
Exit idleForInput(Context& c) {
    Thread& t = c.thread;
    if (t.interactionReturned == 1) {
        t.interactionReturned = 2;
        return Exit::Yield;
    }
    if (t.interactionReturned == 2) {
        t.interactionReturned = 0;
        return Exit::True;
    }
    const auto& o = c.instruction.operands;
    const auto param = int16_t(le16(&o[0]));
    const bool allowPush = le16(&o[2]) == 1;
    const auto left = int16_t(c.get(kOwnerParam, param) - 1);
    c.set(kOwnerParam, param, left);
    const bool inAction = inInteraction(t);
    if (allowPush && !inAction && attemptPush(c.vm, t))
        return Exit::Call;
    Object& me = c.me();
    if (me.idleInterrupt) {
        me.idleInterrupt = false;
        return Exit::True;
    }
    if (inAction ? interactionCancelled(me) : size_t(me.activeInteraction + 1) < me.queue.size()) {
        me.data[objdata::Flags] = int16_t(me.data[objdata::Flags] | kFlagNotifiedByIdleForInput);
        return Exit::True;
    }
    return left < 0 ? Exit::True : Exit::Yield;
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

// Add/Change the Action String (0x32): STR# u16 at 0 (0 = 0x12E, the
// "MakeActionString prim string set" every group with action strings has;
// 0x12E is also given explicitly), flags u16 at 2 (0x01: the table is the
// semi-global one, as in the person semi-global's own trees; 0x02 global,
// inferred; 0x04-0x80 unknown, maybe the icon), string number u8 at 4
// (1-based); bytes 5-12 (a GUID-like value and a variable) are not used. In
// a check tree run for the pie menu it adds an entry with the stack object
// (FreeSO's VMChangeActionString); in an interaction it renames it.
Exit changeActionString(Context& c) {
    const auto& o = c.instruction.operands;
    uint16_t table = le16(&o[0]);
    if (table == 0)
        table = 0x12E;
    const uint16_t flags = le16(&o[2]);
    const Object* owner = c.vm.object(c.frame.codeOwner);
    if (!owner)
        return Exit::True;
    const uint32_t group = flags & 0x01 ? owner->semiGroup : flags & 0x02 ? kGlobalGroup : owner->group;
    const StringTable* strings = group ? c.vm.code().strings(group, table) : nullptr;
    if (!strings || o[4] == 0)
        throw VMError("Add/Change the Action String: no STR# " + hexString(table) + " string " + std::to_string(o[4]));
    const std::string name = expandActionString(c, strings->get(size_t(o[4] - 1)));
    if (c.thread.actionStrings) {
        c.thread.actionStrings->push_back({name, c.frame.stackObject});
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
    const uint16_t tt = o.definition->field(kObjdTreeTableId);
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
    auto& queue = sim.queue;
    if (queue.size() >= kMaxQueuedInteractions)
        throw VMError("interaction queue of object " + std::to_string(sim.id) + " is full");
    if (sim.nextInteractionUid == 0)
        ++sim.nextInteractionUid;
    q.uid = sim.nextInteractionUid++;
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
    if (it == q.end())
        return false;
    const auto index = int(it - q.begin());
    if (index > sim.activeInteraction) {
        q.erase(it);
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
        if (!o.queue.empty())
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
        syncQueueData(sim);
    }
}

bool interactionCancelled(const Object& sim) {
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
    if (sim->activeInteraction >= 0) {
        if (size_t(sim->activeInteraction) < q.size())
            q.erase(q.begin() + sim->activeInteraction);
        --sim->activeInteraction;
    }
    if (sim->person)
        sim->person->data[persondata::NonInterruptible] = 0; // FreeSO: "verified in ts1"
    setPriority(*sim, sim->activeInteraction >= 0 ? q[size_t(sim->activeInteraction)].priority : kPriorityIdle);
    evaluatePriorities(vm, *sim);
    syncQueueData(*sim);
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
    const size_t next = size_t(sim.activeInteraction + 1);
    d[persondata::NextInteractionIndex] = next < q.size() ? int16_t(q[next].interaction) : 0;
    d[persondata::NextInteractionObject] = next < q.size() ? q[next].object : 0;
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
        if (!p.debug) {
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
            d.args[0] = s.stackObject;
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
