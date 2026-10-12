// Interaction queues, Push Interaction, Idle for Input and the pie menu on
// synthetic objects (src/vm/interactions.hpp).

#include "vm/interactions.hpp"

#include <cstdio>
#include <initializer_list>

using namespace os2;
using namespace os2::vm;

static int failures = 0;

static void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

constexpr uint32_t kSimGroup = 0x7F000010;
constexpr uint32_t kObjGroup = 0x7F000020;
constexpr uint32_t kBedGroup = 0x7F000030;

using Target = Behavior::Target;
using Kind = Behavior::TargetKind;
static const Target T{Kind::True, 0}, F{Kind::False, 0}, E{Kind::Error, 0};
static Target go(uint16_t i) { return {Kind::Instruction, i}; }

static Behavior::Instruction instr(uint16_t opcode, Target onTrue, Target onFalse,
                                   std::initializer_list<uint8_t> operands = {}) {
    Behavior::Instruction in;
    in.opcode = opcode;
    in.onTrue = onTrue;
    in.onFalse = onFalse;
    size_t i = 0;
    for (auto b : operands)
        in.operands[i++] = b;
    return in;
}

enum Owner : uint8_t { Attr = 0x00, SAttr = 0x01, Data = 0x03, Lit = 0x07, Temp = 0x08, Param = 0x09,
                       StackId = 0x0A, PData = 0x12, Local = 0x19 };
enum Op : uint8_t { Gt = 0, Eq = 2, Add = 3, Assign = 5 };

static Behavior::Instruction expr(uint8_t lo, int16_t ld, uint8_t op, uint8_t ro, int16_t rd, Target t, Target f) {
    return instr(0x02, t, f,
                 {uint8_t(ld), uint8_t(uint16_t(ld) >> 8), uint8_t(rd), uint8_t(uint16_t(rd) >> 8), 0, op, lo, ro});
}

// Idle for Input: ticks in parameter `param`, allow push.
static Behavior::Instruction idle(uint8_t param, bool push, Target t, Target f = E) {
    return instr(0x11, t, f, {param, 0, uint8_t(push), 0});
}

// Push Interaction: interaction, object in parameter/local `loc`, priority, flags.
static Behavior::Instruction push(uint8_t interaction, uint8_t loc, uint8_t prio, uint8_t flags, Target t, Target f) {
    return instr(0x0D, t, f, {interaction, loc, prio, flags, 0, 0x0A, 0, 0, 0x0A, 0, 0, 0x0A, 0, 0, 0, 1});
}

static Behavior tree(std::vector<Behavior::Instruction> code, uint8_t args = 0, uint8_t locals = 0) {
    Behavior b;
    b.fileName = "test";
    b.signature = 0x8009;
    b.argCount = args;
    b.localCount = locals;
    b.instructions = std::move(code);
    return b;
}

static ObjectDefinition definition(uint16_t type, uint16_t treeTable, uint32_t guid, int16_t interactionGroup = 0,
                                   uint16_t master = 0, uint16_t sub = 0) {
    ObjectDefinition d;
    d.fileName = "test object";
    d.fields.resize(120);
    auto at = [&](size_t offset) -> uint16_t& { return d.fields[(offset - 0x44) / 2]; };
    at(0x4E) = treeTable;
    at(0x50) = uint16_t(interactionGroup);
    at(0x52) = type;
    at(0x54) = master;
    at(0x56) = sub;
    at(0x5C) = uint16_t(guid);
    at(0x5E) = uint16_t(guid >> 16);
    return d;
}

static StringTable ttas(std::initializer_list<const char*> english, std::initializer_list<const char*> portuguese) {
    StringTable t;
    t.format = 0xFFFD;
    for (const char* s : english)
        t.entries.push_back({lang::EnglishUS, s, "", 0});
    for (const char* s : portuguese)
        t.entries.push_back({lang::Portuguese, s, "", 0});
    return t;
}

static InteractionTable::Interaction entry(uint16_t action, uint16_t guard, uint32_t stringIndex) {
    InteractionTable::Interaction i;
    i.action = action;
    i.guard = guard;
    i.stringIndex = stringIndex;
    return i;
}

// A Sim whose Main idles for input with "allow push" every tick, and an
// object (a fridge) with four interactions:
//   TTA 0 "Open/Have Snack" (action 0x1001: stack object attribute 0 := 7,
//     temp 1 := stack object ID), guard 0x1003 (true)
//   TTA 1 "Clean" (action 0x1002: waits 1000 ticks in an Idle for Input,
//     then stack object attribute 1 := 1), no guard
//   TTA 3 "Open/Restock" guard 0x1004 (false); TTA 2 "*Debug" (debug)
struct Fixture {
    CodeLibrary code;
    VM vm{code};
    ObjectId sim = 0, fridge = 0;

    Fixture() {
        code.add(kSimGroup, 0x80, definition(kObjectTypePerson, 0, 0x5150001));
        ObjectFunctions f;
        f.functions.resize(2);
        f.functions[1].action = 0x1000;
        code.add(kSimGroup, 0x80, std::move(f));
        code.add(kSimGroup, 0x1000,
                 tree({expr(Param, 0, Assign, Lit, 1, go(1), E), idle(0, true, go(0))}, 1));

        code.add(kObjGroup, 0x80, definition(4, 1, 0x0B1E0001));
        InteractionTable t;
        t.version = 0x4D;
        t.interactions = {entry(0x1001, 0x1003, 0), entry(0x1002, 0, 1), entry(0x1001, 0, 2), entry(0x1001, 0x1004, 3)};
        code.add(kObjGroup, 1, std::move(t));
        code.addInteractionStrings(kObjGroup, 1,
                                   ttas({"Open.../Have Snack", "Clean", "*Debug", "Open.../Restock"},
                                        {"Abrir.../Fazer um Lanche", "Limpar", "*Depurar", "Abrir.../Reabastecer"}));
        code.add(kObjGroup, 0x1001, tree({expr(SAttr, 0, Assign, Lit, 7, go(1), E),
                                          expr(Temp, 1, Assign, StackId, 0, T, E)}));
        code.add(kObjGroup, 0x1002, tree({expr(Param, 0, Assign, Lit, 1000, go(1), E), idle(0, false, go(2)),
                                          expr(SAttr, 1, Assign, Lit, 1, T, E)},
                                         4));
        code.add(kObjGroup, 0x1003, tree({instr(0x02, T, E, {0, 0, 0, 0, 0, Eq, Lit, Lit})})); // 0 == 0
        code.add(kObjGroup, 0x1004, tree({instr(0x02, T, F, {0, 0, 1, 0, 0, Eq, Lit, Lit})})); // 0 == 1

        sim = vm.instantiate(kSimGroup, 0x80).id;
        fridge = vm.instantiate(kObjGroup, 0x80).id;
        vm.startMain(sim);
    }
    Object& s() { return *vm.object(sim); }
    Object& o() { return *vm.object(fridge); }
    QueuedInteraction make(uint16_t i) { return *makeInteraction(vm, fridge, i); }
};

static void testTableAndNames() {
    Fixture fx;
    auto ref = interactionTable(fx.vm, fx.o());
    check(ref && ref->group == kObjGroup && ref->instance == 1 && ref->table->interactions.size() == 4,
          "the OBJD's tree table id names the object's TTAB");
    auto q = makeInteraction(fx.vm, fx.fridge, 3);
    check(q && q->action == 0x1001 && q->guard == 0x1004 && q->name == "Abrir.../Reabastecer" &&
              q->priority == kPriorityUserDriven,
          "interactions are found by TTA index, named in Portuguese (Brazilian falls back to it)");
    check(!makeInteraction(fx.vm, fx.fridge, 9), "no entry with that TTA index");
    check(!makeInteraction(fx.vm, fx.sim, 0), "an object without a TTAB has no interactions");
    auto path = splitPiePath("Abrir.../Fazer um Lanche");
    check(path.size() == 2 && path[0] == "Abrir..." && path[1] == "Fazer um Lanche", "submenu paths split at '/'");
}

static void testPieMenu() {
    Fixture fx;
    auto pie = pieMenu(fx.vm, fx.sim, fx.fridge);
    check(pie.size() == 4, "every TTAB entry is listed");
    if (pie.size() != 4)
        return;
    check(pie[0].check == CheckResult::True && pie[0].available() && pie[0].path.size() == 2,
          "guard returning true: available");
    check(pie[1].check == CheckResult::NoGuard && pie[1].available() && pie[1].name == "Limpar", "no guard: available");
    check(pie[2].debug, "names starting with '*' are debug interactions");
    check(!pie[0].debug && !pie[1].debug, "the others are not");
    check(pie[3].check == CheckResult::False && !pie[3].available(), "guard returning false: not available");
}

static void testEnqueueOrder() {
    Fixture fx;
    auto q = [&](int16_t prio, uint16_t i) {
        auto x = fx.make(i);
        x.priority = prio;
        return x;
    };
    Object& s = fx.s();
    enqueue(fx.vm, s, q(kPriorityUserDriven, 0));
    enqueue(fx.vm, s, q(kPriorityAutonomous, 1));
    enqueue(fx.vm, s, q(kPriorityUserDriven, 3));
    enqueue(fx.vm, s, q(kPriorityMaximum, 2));
    enqueue(fx.vm, s, q(kPriorityAutonomous, 0), true);
    // max first, then the user driven ones in order, then autonomous; push head goes first of all.
    std::vector<uint16_t> order;
    for (const auto& x : s.queue)
        order.push_back(x.interaction);
    check((order == std::vector<uint16_t>{0, 2, 0, 3, 1}), "queue order: priority, then arrival; push head first");
    check(s.person->data[persondata::NextInteractionObject] == fx.fridge &&
              s.person->data[persondata::NextInteractionIndex] == 0,
          "person data 79/45: the next queued interaction");
    check(cancelInteraction(fx.vm, s, s.queue[1].uid) && s.queue.size() == 4, "cancelling a waiting entry removes it");
    cancelInteractionsWith(fx.vm, fx.fridge);
    check(s.queue.empty() && s.person->data[persondata::NextInteractionObject] == 0,
          "Generic Sims Call 13 removes waiting interactions of the object");
}

static void testMainRunsInteraction() {
    Fixture fx;
    Object& s = fx.s();
    enqueue(fx.vm, s, fx.make(0));
    enqueue(fx.vm, s, fx.make(3)); // guard false: dropped when its turn comes
    fx.vm.tick();
    Thread& t = fx.vm.threads[fx.sim];
    check(t.status == Thread::Status::Running, "the Sim's Main keeps running");
    check(fx.o().attributes.size() > 0 && fx.o().attributes[0] == 7, "the action tree ran with the object as stack object");
    check(t.temps[1] == fx.fridge, "... in the Sim's thread");
    // The Idle for Input that ran it runs again at once (Mac binary) and
    // returns true; Main loops into the next Idle for Input, which tries the
    // next entry in the same tick: its guard fails and it is dropped.
    check(s.activeInteraction == -1 && t.interactionReturned == 0, "the finished interaction left the queue");
    check(s.queue.empty(), "the next entry was tried in the same tick");
    fx.vm.tick(); // the Idle for Input returns true; the loop idles again
    fx.vm.tick(); // allow push: the next entry's guard fails, so it is dropped
    fx.vm.tick();
    check(s.queue.empty() && t.status == Thread::Status::Running, "an entry whose guard fails is dropped");
}

static void testCancelRunning() {
    Fixture fx;
    Object& s = fx.s();
    auto clean = fx.make(1);
    clean.priority = kPriorityAutonomous;
    const uint16_t uid = enqueue(fx.vm, s, clean);
    fx.vm.tick();
    Thread& t = fx.vm.threads[fx.sim];
    check(s.activeInteraction == 0 && inInteraction(t) && s.person->data[persondata::Priority] == kPriorityAutonomous &&
              s.person->data[persondata::CurrentInteractionObject] == fx.fridge &&
              s.person->data[persondata::InteractionAutonomous] == 1,
          "a long interaction is running; person data 33/80/166 say so");
    for (int i = 0; i < 5; ++i)
        fx.vm.tick();
    check(s.activeInteraction == 0 && !interactionCancelled(s), "it keeps running");
    // Another autonomous entry does not interrupt it (cTSPerson::ShouldInterrupt:
    // only a priority above the person's; FreeSO lets one autonomous entry
    // interrupt another, the TS2 binary does not).
    auto other = fx.make(0);
    other.priority = kPriorityAutonomous;
    const uint16_t otherUid = enqueue(fx.vm, s, other);
    check(!shouldInterrupt(s) && !interactionCancelled(s), "an autonomous entry waits behind an autonomous one");
    for (int i = 0; i < 3; ++i)
        fx.vm.tick();
    check(s.activeInteraction == 0 && s.queue.size() == 2, "and the running one goes on");
    check(cancelInteraction(fx.vm, s, otherUid) && s.queue.size() == 1, "(the waiting one is dropped)");
    // Something more important interrupts it (the user picks another interaction).
    enqueue(fx.vm, s, fx.make(0));
    check(shouldInterrupt(s) && !interactionCancelled(s), "a user driven interaction interrupts an autonomous one");
    fx.vm.tick(); // Idle for Input in the interaction returns: it ends
    check(fx.o().attributes.size() > 1 && fx.o().attributes[1] == 1, "the cancelled interaction finished its tree");
    check(s.queue.empty() && s.activeInteraction == -1, "and left the queue");
    check(fx.o().attributes[0] == 7, "then the user's interaction ran (the next Idle for Input of Main, same tick)");
    (void)uid;

    Fixture fy;
    const uint16_t u2 = enqueue(fy.vm, fy.s(), fy.make(1));
    fy.vm.tick();
    check(fy.s().activeInteraction == 0, "running");
    check(cancelInteraction(fy.vm, fy.s(), u2) && interactionCancelled(fy.s()) &&
              fy.s().person->data[persondata::Priority] == 0,
          "cancelling the running interaction tells it to stop and drops the priority");
    fy.vm.tick();
    check(fy.s().queue.empty() && fy.o().attributes.size() > 1 && fy.o().attributes[1] == 1,
          "the user cancel ends it at its next Idle for Input");
}

static void testPushInteraction() {
    Fixture fx;
    // A tree of the fridge pushing its own interactions onto the Sim (the
    // stack object): the object in local 0 (flag 0x02) or parameter 0.
    fx.code.add(kObjGroup, 0x1010,
                tree({expr(Local, 0, Assign, Data, objdata::ObjectId, go(1), E),
                      expr(StackId, 0, Assign, Lit, int16_t(fx.sim), go(2), E),
                      push(1, 0, 2, 0x02, go(3), E),      // Clean, autonomous, object in local 0
                      push(0, 0, 1, 0x00, T, E)},         // Have Snack, maximum, object in param 0
                     2, 1));
    // The game's errors (Mac binary): no such interaction (0x16), no such object (0x17).
    fx.code.add(kObjGroup, 0x1011,
                tree({expr(Local, 0, Assign, Data, objdata::ObjectId, go(1), E),
                      expr(StackId, 0, Assign, Lit, int16_t(fx.sim), go(2), E), push(9, 0, 3, 0x02, T, T)}, 2, 1));
    fx.code.add(kObjGroup, 0x1012,
                tree({expr(StackId, 0, Assign, Lit, int16_t(fx.sim), go(1), E), push(3, 1, 3, 0x00, T, T)}, 2, 1));
    const int16_t args[] = {int16_t(fx.fridge), 0};
    Thread t = fx.vm.start(fx.fridge, 0x1010, args);
    fx.vm.run(t);
    if (t.status == Thread::Status::Failed)
        std::fprintf(stderr, "  %s\n", t.error.c_str());
    Object& s = fx.s();
    check(t.status == Thread::Status::Finished && t.result, "Push Interaction: true/false as expected");
    check(s.queue.size() == 2 && s.queue[0].interaction == 0 && s.queue[0].priority == kPriorityMaximum &&
              s.queue[1].interaction == 1 && s.queue[1].priority == kPriorityAutonomous &&
              s.queue[1].pushedBy == fx.fridge && s.queue[1].flags == 0x02,
          "Push Interaction queues onto the stack object by priority");
    Thread e1 = fx.vm.start(fx.fridge, 0x1011, args);
    fx.vm.run(e1);
    check(e1.status == Thread::Status::Failed && e1.error.find("tree table entry") != std::string::npos,
          "no such interaction: error 0x16");
    Thread e2 = fx.vm.start(fx.fridge, 0x1012, args);
    fx.vm.run(e2);
    check(e2.status == Thread::Status::Failed && e2.error.find("Could not find object") != std::string::npos,
          "no such object: error 0x17");
}

static void testNotifyFlag() {
    Fixture fx;
    // Wait For Notify, as the person Main uses it: Idle for Input without
    // push, then the "notified by idle for input" flag.
    // Expression: object data 8 (flags) has flag 7 (bit 6)? (op 8 = flag test)
    fx.code.add(kSimGroup, 0x1001,
                tree({idle(0, false, go(1)), instr(0x02, T, F, {8, 0, 7, 0, 0, 8, Data, Lit})}, 1));
    {
        const int16_t args[] = {5};
        Thread t = fx.vm.start(fx.sim, 0x1001, args);
        fx.vm.run(t);
        check(t.status == Thread::Status::Running, "nothing queued: Idle for Input waits");
    }
    enqueue(fx.vm, fx.s(), fx.make(1));
    const int16_t args[] = {5};
    Thread t = fx.vm.start(fx.sim, 0x1001, args);
    fx.vm.run(t);
    check(t.status == Thread::Status::Finished && t.result &&
              (fx.s().data[objdata::Flags] & kFlagNotifiedByIdleForInput),
          "something queued: Idle for Input returns at once and sets \"notified by idle for input\"");
}

static void testActionStrings() {
    Fixture fx;
    // A check tree that names two entries itself, for two stack objects.
    fx.code.add(kObjGroup, 0x12E, ttas({"Call.../$Object", "Restock $$Local:0"}, {"Ligar.../$Object", "Reabastecer $$Local:0"}));
    fx.code.add(kObjGroup, 0x1005,
                tree({expr(StackId, 0, Assign, Lit, int16_t(fx.sim), go(1), E),
                      instr(0x32, go(2), E, {0, 0, 0, 0, 1}),
                      expr(StackId, 0, Assign, Lit, int16_t(fx.fridge), go(3), E),
                      expr(Local, 0, Assign, Lit, 25, go(4), E),
                      instr(0x32, T, E, {0x2E, 0x01, 0, 0, 2})},
                     0, 1));
    InteractionTable t;
    t.version = 0x4D;
    t.interactions = {entry(0x1001, 0x1005, 0)};
    fx.code.add(kObjGroup, 1, std::move(t));
    auto pie = pieMenu(fx.vm, fx.sim, fx.fridge);
    check(pie.size() == 2 && pie[0].dynamic && pie[0].name == "Ligar.../test object" && pie[0].args[0] == fx.sim &&
              pie[1].name == "Reabastecer §25" && pie[1].args[0] == fx.fridge,
          "Add/Change the Action String: a check tree adds entries with their stack objects");
    auto q = interactionFromPie(fx.vm, pie[1]);
    check(q && q->name == "Reabastecer §25" && q->args[0] == fx.fridge, "a picked dynamic entry keeps its name and parameter");

    // Notify the Stack Object out of Idle.
    fx.code.add(kObjGroup, 0x1006, tree({expr(StackId, 0, Assign, Lit, int16_t(fx.sim), go(1), E), instr(0x31, T, E)}));
    fx.code.add(kSimGroup, 0x1002, tree({idle(0, false, T)}, 1));
    const int16_t args[] = {100};
    Thread waiting = fx.vm.start(fx.sim, 0x1002, args);
    fx.vm.run(waiting);
    Thread notify = fx.vm.start(fx.fridge, 0x1006);
    fx.vm.run(notify);
    fx.vm.run(waiting);
    check(waiting.status == Thread::Status::Finished, "Notify the Stack Object out of Idle ends its Idle for Input");
}

static void testTurnBodyTowards() {
    Fixture fx;
    fx.s().position = {2.5f, 2.5f, 1};
    fx.o().position = {5.5f, 2.5f, 1}; // +X of the Sim
    fx.code.add(kSimGroup, 0x1003, tree({expr(StackId, 0, Assign, Lit, int16_t(fx.fridge), go(1), E),
                                         instr(0x16, go(2), E, {2}), expr(Temp, 0, Assign, Data, objdata::Direction, go(3), E),
                                         instr(0x16, go(4), E, {3}), expr(Temp, 1, Assign, Data, objdata::Direction, T, E)}));
    Thread t = fx.vm.start(fx.sim, 0x1003);
    fx.vm.run(t);
    check(t.status == Thread::Status::Finished && t.temps[0] == 4 && t.temps[1] == 0,
          "Turn Body Towards: towards (+X = direction 4) and away from the stack object");
}

static void testInteractionGroupLeader() {
    CodeLibrary code;
    VM vm{code};
    // A two-part bed: part A (interaction group 1) and the head (-1), which has the TTAB.
    code.add(kBedGroup, 0x10, definition(4, 0xFFFF, 0xBED00000, 0, 1, 0xFFFF));
    code.add(kBedGroup, 0x11, definition(4, 0xFFFF, 0xBED00001, 1, 1, 0));
    code.add(kBedGroup, 0x12, definition(4, 0x80, 0xBED00002, -1, 1, 1));
    InteractionTable t;
    t.version = 0x4D;
    t.interactions = {entry(0x1001, 0, 0)};
    code.add(kBedGroup, 0x80, std::move(t));
    code.addInteractionStrings(kBedGroup, 0x80, ttas({"Sleep"}, {"Dormir"}));
    Object& lead = vm.instantiate(kBedGroup, 0x10);
    ObjectId head = 0;
    for (const auto& [id, o] : vm.objects())
        if (o.definitionInstance == 0x12)
            head = id;
    check(head && interactionLeader(vm, lead.id) == head, "a part shows its interaction group leader's interactions");
    check(interactionLeader(vm, head) == head, "the leader leads itself");
    // Saved lots: the parts lead themselves; the nearest leader of the same object is used.
    Object& a = vm.instantiatePart(kBedGroup, 0x11);
    Object& h = vm.instantiatePart(kBedGroup, 0x12);
    a.position = {3.5f, 3.5f, 1};
    h.position = {3.5f, 4.5f, 1};
    vm.object(head)->position = {10.5f, 10.5f, 1};
    check(interactionLeader(vm, a.id) == h.id, "parts of saved lots find the nearest leader");
    Object* sim = &vm.createObject(kSimGroup);
    sim->person.emplace();
    auto pie = pieMenu(vm, sim->id, a.id);
    check(pie.size() == 1 && pie[0].name == "Dormir" && pie[0].object == h.id, "the part's pie menu is the leader's");

    // A two-part crib without interaction groups: the master has the TTAB,
    // the 0,0 part (sub index 0) stands for it, the 1,0 part has none.
    constexpr uint32_t kCribGroup = 0x7F000041;
    code.add(kCribGroup, 0x10, definition(4, 0x80, 0xC0B00000, 0, 1, 0xFFFF));
    code.add(kCribGroup, 0x11, definition(4, 0xFFFF, 0xC0B00001, 0, 1, 0));
    code.add(kCribGroup, 0x12, definition(4, 0xFFFF, 0xC0B00002, 0, 1, 0x100));
    InteractionTable ct;
    ct.version = 0x4D;
    ct.interactions = {entry(0x1001, 0, 0)};
    code.add(kCribGroup, 0x80, std::move(ct));
    code.addInteractionStrings(kCribGroup, 0x80, ttas({"Put In Crib"}, {"Colocar no Berço"}));
    Object& c00 = vm.instantiatePart(kCribGroup, 0x11);
    Object& c10 = vm.instantiatePart(kCribGroup, 0x12);
    check(interactionTable(vm, c00) && interactionTable(vm, c00)->instance == 0x80 && !interactionTable(vm, c10),
          "a crib's 0,0 part has its master's interactions, the 1,0 part none");
}

// Nested interactions: a "TV" whose interaction allows nesting (TTAB flag
// 0x4000) loops on an Idle for Input in "Handle Sub Queue Interactions"
// mode; a nested entry (0x8000) waits in the sub-queue and runs there as the
// current sub-action, inside the running interaction.
static void testSubQueue() {
    Fixture fx;
    constexpr uint32_t kTvGroup = 0x7F000030;
    fx.code.add(kTvGroup, 0x80, definition(4, 1, 0x0B1E0002));
    InteractionTable t;
    t.version = 0x4D;
    auto watch = entry(0x1001, 0, 0), react = entry(0x1002, 0, 1);
    watch.flags = 0x4000;
    react.flags = 0x8000;
    t.interactions = {watch, react};
    fx.code.add(kTvGroup, 1, std::move(t));
    fx.code.addInteractionStrings(kTvGroup, 1, ttas({"Watch", "React"}, {"Assistir", "Reagir"}));
    fx.code.add(kTvGroup, 0x1001, tree({instr(0x11, go(1), go(1), {0, 0, 0, 0, 1}), // handle the sub-queue
                                        expr(Param, 0, Assign, Lit, 1, go(2), E), idle(0, false, go(0))},
                                       4));
    fx.code.add(kTvGroup, 0x1002, tree({expr(SAttr, 2, Assign, Lit, 5, T, E)}));
    const ObjectId tv = fx.vm.instantiate(kTvGroup, 0x80).id;
    Object& s = fx.s();
    enqueue(fx.vm, s, *makeInteraction(fx.vm, tv, 0));
    fx.vm.tick();
    check(s.activeInteraction == 0 && s.queue[0].object == tv, "watching");
    enqueue(fx.vm, s, *makeInteraction(fx.vm, tv, 1));
    check(s.queue.size() == 1 && s.subQueue.size() == 1 && s.person->data[persondata::SubQueueCount] == 1 &&
              s.person->data[persondata::NextSubQueueObject] == tv &&
              s.person->data[persondata::SubQueueMasterObject] == tv,
          "a nested interaction waits in the sub-queue (person data 26, 78, 75)");
    fx.vm.tick();
    fx.vm.tick();
    Object& o = *fx.vm.object(tv);
    check(o.attributes.size() > 2 && o.attributes[2] == 5 && s.subQueue.empty() && !s.subAction &&
              s.activeInteraction == 0,
          "it ran as a sub-action inside the running interaction");

    // A waiting sub-action can be cancelled by its uid; taking the next
    // queue entry clears the sub-actions (cTSPerson::ClearSubActions).
    const uint16_t a = enqueue(fx.vm, s, *makeInteraction(fx.vm, tv, 1));
    const uint16_t b = enqueue(fx.vm, s, *makeInteraction(fx.vm, tv, 1));
    check(s.subQueue.size() == 2 && cancelInteraction(fx.vm, s, b) && s.subQueue.size() == 1 &&
              s.subQueue[0].uid == a && s.person->data[persondata::SubQueueCount] == 1,
          "cancelling a waiting sub-action takes it out of the sub-queue");
    clearSubActions(s);
    check(s.subQueue.empty() && !s.subAction && s.person->data[persondata::SubQueueCount] == 0 &&
              s.person->data[persondata::SubQueueMasterObject] == tv &&
              s.person->data[persondata::CurrentRunningObject] == tv,
          "clearing the sub-actions resets person data 26; 75 and 103 stay the running action's "
          "(cTSPerson::SetSubInteractionData)");
}

int main() {
    testTableAndNames();
    testPieMenu();
    testEnqueueOrder();
    testMainRunsInteraction();
    testCancelRunning();
    testPushInteraction();
    testNotifyFlag();
    testActionStrings();
    testTurnBodyTowards();
    testInteractionGroupLeader();
    testSubQueue();
    if (failures)
        std::fprintf(stderr, "%d failure(s)\n", failures);
    else
        std::printf("queue: all tests passed\n");
    return failures ? 1 : 0;
}
