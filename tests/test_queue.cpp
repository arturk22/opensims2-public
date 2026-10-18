// Interaction queues, Push Interaction, Idle for Input and the pie menu on
// synthetic objects (src/vm/interactions.hpp).

#include "vm/engine.hpp"
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
    // As cTSPerson::AddAction: a higher priority skips the lower ones waiting
    // (the autonomous 1 when the user's 3 came, then 0 and 3 when the
    // maximum 2 came); push head goes first of all.
    std::vector<uint16_t> order;
    for (const auto& x : s.queue)
        order.push_back(x.interaction);
    check((order == std::vector<uint16_t>{0, 2}), "queue order: a higher priority skips the lower waiting ones; push head first");
    enqueue(fx.vm, s, q(kPriorityMaximum, 3));
    enqueue(fx.vm, s, q(kPriorityMaximum, 1));
    order.clear();
    for (const auto& x : s.queue)
        order.push_back(x.interaction);
    // (the pushed head is autonomous: from it on everything is skipped)
    check((order == std::vector<uint16_t>{3, 1}), "queue order: the same priority goes last");
    check(s.person->data[persondata::NextInteractionObject] == fx.fridge &&
              s.person->data[persondata::NextInteractionIndex] == 3,
          "person data 79/45: the next queued interaction");
    check(cancelInteraction(fx.vm, s, s.queue[1].uid) && s.queue.size() == 1, "cancelling a waiting entry removes it");
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
              s.person->data[persondata::InteractionAutonomous] == 0,
          "a long interaction is running; person data 33/80 say so, 166 is its autonomous bit (not its priority)");
    s.queue[0].autonomous = true;
    syncQueueData(s);
    check(s.person->data[persondata::InteractionAutonomous] == 1, "person data 166: the running interaction's autonomous bit");
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
                      push(0, 0, 1, 0x00, go(3), E),      // Have Snack, maximum, object in param 0
                      push(1, 0, 2, 0x02, T, E)},         // Clean, autonomous, object in local 0 (after: a
                                                          // higher one would skip it, AddAction)
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

// A queue with more than 6 entries refuses a push (false, primitive result
// 3) unless it is "continue as current" (byte 3 bit 0x04): TryPushAction
// tests the sizes only for the others ("Pickup - Do Carry Sim").
static void testPushOntoFullQueue() {
    Fixture fx;
    fx.code.add(kObjGroup, 0x1013,
                tree({expr(StackId, 0, Assign, Lit, int16_t(fx.sim), go(1), E), push(1, 0, 2, 0x00, T, F)}, 2, 1));
    fx.code.add(kObjGroup, 0x1014,
                tree({expr(StackId, 0, Assign, Lit, int16_t(fx.sim), go(1), E), push(1, 0, 2, 0x04, T, F)}, 2, 1));
    for (int i = 0; i < 7; ++i)
        enqueue(fx.vm, fx.s(), fx.make(0));
    const int16_t args[] = {int16_t(fx.fridge), 0};
    Thread a = fx.vm.start(fx.fridge, 0x1013, args);
    fx.vm.run(a);
    check(a.status == Thread::Status::Finished && !a.result && fx.s().queue.size() == 7 &&
              fx.o().data[kObjdataPrimitiveResult] == 3,
          "full queue: a push is refused (result 3)");
    Thread b = fx.vm.start(fx.fridge, 0x1014, args);
    fx.vm.run(b);
    check(b.status == Thread::Status::Finished && b.result && fx.s().queue.size() == 8,
          "full queue: a continue-as-current push goes in");
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
    // and every part whose tree table ID is -1 uses it
    // (cTSObjectSelector::GetEffectiveTreeTableID).
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
    check(interactionTable(vm, c00) && interactionTable(vm, c00)->instance == 0x80 && interactionTable(vm, c10) &&
              interactionTable(vm, c10)->instance == 0x80,
          "a crib's parts (tree table -1) have their master's interactions");
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
    clearSubActions(fx.vm, s);
    check(s.subQueue.empty() && !s.subAction && s.person->data[persondata::SubQueueCount] == 0 &&
              s.person->data[persondata::SubQueueMasterObject] == tv &&
              s.person->data[persondata::CurrentRunningObject] == tv,
          "clearing the sub-actions resets person data 26; 75 and 103 stay the running action's "
          "(cTSPerson::SetSubInteractionData)");
}

// Inside an interaction, an Idle for Input with "allow push" runs a waiting
// entry of higher priority on top of it (cTSPerson::TryIdleForInput); the
// interrupted one goes on when it returns.
static void testNestedInterruption() {
    Fixture fx;
    // Clean: wait 1000 ticks in an Idle for Input that allows pushes, then attr 1 := 1.
    fx.code.add(kObjGroup, 0x1002, tree({expr(Param, 0, Assign, Lit, 1000, go(1), E), idle(0, true, go(2)),
                                         expr(SAttr, 1, Assign, Lit, 1, T, E)},
                                        4));
    // Snack: attr 2 := attr 1 (0 while Clean has not finished), attr 0 := 7.
    fx.code.add(kObjGroup, 0x1001, tree({expr(SAttr, 2, Assign, SAttr, 1, go(1), E), expr(SAttr, 0, Assign, Lit, 7, T, E)}));
    Object& s = fx.s();
    auto clean = fx.make(1);
    clean.priority = kPriorityAutonomous;
    enqueue(fx.vm, s, clean);
    fx.vm.tick();
    check(s.activeInteraction == 0, "the long interaction runs");
    auto snack = fx.make(0);
    snack.priority = 50;
    enqueue(fx.vm, s, snack);
    fx.vm.tick();
    check(fx.o().attributes.size() > 2 && fx.o().attributes[0] == 7 && fx.o().attributes[2] == 0,
          "the higher priority entry ran on top of the running one, before it finished");
    check(fx.o().attributes[1] == 1 && s.queue.empty(), "then the interrupted one went on to its end");
}

// Push Interaction's link (flag 0x40) as TryPushAction makes it: the new
// interaction takes the linked one's link and the linked one links to it,
// so A with B and then C linked to it is A -> C -> B; when A ends both are
// told (person data 110 for a running one, a waiting one leaves its queue).
// The baby's holder of "Social - Baby - Talk To" (C) was never told.
static void testLinkedInteractions() {
    Fixture fx;
    const ObjectId b = fx.vm.instantiate(kSimGroup, 0x80).id, c = fx.vm.instantiate(kSimGroup, 0x80).id;
    fx.vm.startMain(b);
    const uint16_t ua = enqueue(fx.vm, fx.s(), fx.make(1));
    const uint16_t ub = enqueue(fx.vm, *fx.vm.object(b), fx.make(1));
    const uint16_t uc = enqueue(fx.vm, *fx.vm.object(c), fx.make(1)); // c has no Main: it waits
    fx.vm.tick();
    linkInteractions(fx.vm, fx.vm.object(b)->queue[0], ua);
    linkInteractions(fx.vm, fx.vm.object(c)->queue[0], ua);
    check(fx.s().queue[0].link == uc && fx.vm.object(c)->queue[0].link == ub && fx.vm.object(b)->queue[0].link == 0,
          "links: A -> C -> B");
    const QueuedInteraction a = fx.s().queue[0];
    unlinkInteraction(fx.vm, fx.sim, a);
    check(fx.vm.object(c)->queue.empty(), "A ends: C, waiting, leaves its queue");
    check(fx.vm.object(b)->person->data[110] == 1 && fx.vm.object(b)->queue.size() == 1,
          "and B, running, gets person data 110");
    (void)ub;
}

// A "Run Immediately" entry (TTAB flag 0x04, "*Push Look At" of the look-at
// controller) never waits in the queue: Push Interaction runs its action
// tree at once (cTSPerson::AddAction), however often it is pushed; its
// check tree failing is the push's false (result 6). Four of them piled up
// behind Daniel's dance in N001_Lot68 (live QA 15).
static void testRunImmediately() {
    Fixture fx;
    constexpr uint32_t kLookGroup = 0x7F000040;
    fx.code.add(kLookGroup, 0x80, definition(4, 1, 0x0B1E0002));
    InteractionTable t;
    t.version = 0x4D;
    auto now = entry(0x1001, 0, 0), refused = entry(0x1001, 0x1003, 1);
    now.flags = refused.flags = 0x04;
    t.interactions = {now, refused};
    fx.code.add(kLookGroup, 1, std::move(t));
    fx.code.addInteractionStrings(kLookGroup, 1, ttas({"*Push Look At", "*Refused"}, {"*Push Look At", "*Refused"}));
    // action 0x1001: stack object attribute 0 += 1; guard 0x1003 false
    fx.code.add(kLookGroup, 0x1001, tree({expr(SAttr, 0, Add, Lit, 1, T, E)}));
    fx.code.add(kLookGroup, 0x1003, tree({instr(0x02, T, F, {0, 0, 1, 0, 0, Eq, Lit, Lit})}));
    fx.code.add(kLookGroup, 0x1010,
                tree({expr(Local, 0, Assign, Data, objdata::ObjectId, go(1), E),
                      expr(StackId, 0, Assign, Lit, int16_t(fx.sim), go(2), E),
                      push(0, 0, 2, 0x02, go(3), E), push(0, 0, 2, 0x02, go(4), E), push(0, 0, 2, 0x02, go(5), E),
                      push(0, 0, 2, 0x02, go(6), E), push(1, 0, 2, 0x02, E, T)},
                     2, 1));
    const ObjectId look = fx.vm.instantiate(kLookGroup, 0x80).id;
    const int16_t args[] = {int16_t(look), 0};
    Thread th = fx.vm.start(look, 0x1010, args);
    fx.vm.run(th);
    if (th.status == Thread::Status::Failed)
        std::fprintf(stderr, "  %s\n", th.error.c_str());
    check(th.status == Thread::Status::Finished && th.result, "run immediately: pushes true, the refused one false");
    check(fx.s().queue.empty(), "run immediately: nothing goes into the queue");
    check(fx.vm.object(look)->attributes.size() > 0 && fx.vm.object(look)->attributes[0] == 4,
          "run immediately: the action tree ran once per push");
}

// The person's "*Object Menu Subqueue" (TTAB 7F01EC29, entry 183): a '*'
// entry whose check tree runs the bed's "CT - Object Menu Subqueue" by name;
// that tree adds the socials' strings and hides itself. The
// strings are the menu (sMakeNewMenuItem adds them as they are made), and a
// pick of the Run Immediately entry runs it at once (addAction).
static void testStarEntryStringsFromNamedTree() {
    Fixture fx;
    fx.code.add(kBedGroup, 0x80, definition(4, 0, 0x0B1E0003));
    fx.code.add(kBedGroup, 0x12E, ttas({"WooHoo"}, {"Oba-Oba"}));
    Behavior menu = tree({expr(StackId, 0, Assign, Lit, 9, go(1), E), instr(0x32, go(2), E, {0, 0, 0, 0, 1}),
                          expr(Data, objdata::HideInteraction, Assign, Lit, 1, T, E)});
    menu.fileName = "CT - Menu";
    fx.code.add(kBedGroup, 0x1100, std::move(menu));
    const ObjectId bed = fx.vm.instantiate(kBedGroup, 0x80).id;
    StringTable named;
    named.format = 0xFFFD;
    named.entries.push_back({lang::EnglishUS, "CT - Menu", "", 0});
    fx.code.add(kObjGroup, 0x12F, std::move(named));
    fx.code.add(kObjGroup, 0x1007, tree({expr(StackId, 0, Assign, Lit, int16_t(bed), go(1), E),
                                         instr(0x1C, T, F, {0x2F, 0x01, 0, 0, 1, 0})}, 1));
    InteractionTable t;
    t.version = 0x4D;
    auto star = entry(0x1001, 0x1007, 0);
    star.flags = 0x04;
    t.interactions = {star};
    fx.code.add(kObjGroup, 1, std::move(t));
    fx.code.addInteractionStrings(kObjGroup, 1, ttas({"*Object Menu"}, {"*Object Menu"}));
    auto pie = pieMenu(fx.vm, fx.sim, fx.fridge);
    check(pie.size() == 1 && pie[0].dynamic && !pie[0].debug && pie[0].available() && pie[0].name == "Oba-Oba" &&
              pie[0].args[0] == 9,
          "a '*' entry's check tree adds menu entries through a tree run by name, whatever it then hides");
    auto q = interactionFromPie(fx.vm, pie[0]);
    check(q && addAction(fx.vm, fx.s(), *q) == 0 && fx.s().queue.empty() && fx.o().attributes.size() > 0 &&
              fx.o().attributes[0] == 7,
          "a picked Run Immediately entry runs at once, not queued");
}

// Push Interaction with byte 14 flags.
static Behavior::Instruction pushFlags2(uint8_t interaction, uint8_t loc, uint8_t prio, uint8_t flags, uint8_t flags2,
                                       Target t, Target f) {
    return instr(0x0D, t, f, {interaction, loc, prio, flags, 0, 0x0A, 0, 0, 0x0A, 0, 0, 0x0A, 0, 0, flags2, 1});
}

// Round n31 (docs/primitives-mac.md §4g "Rodada n31"): the interaction's
// autonomous bits, cTSInteraction +0x54 0x400 (person data 166) and 0x800
// (the check tree's parameter 0 when Idle for Input takes the entry). The
// Find Best Interaction's entry, pushed with byte 14 bit 0x02, has both: an
// entry whose check tree only takes autonomy (parameter 0 = 1) runs; pushed
// by an object without 0x02 it is refused. A Sim's push passes on its
// current interaction's 0x400, not 0x800. Push's own forced check (byte 3
// 0x20) is autonomous with 0x02 (TryPushAction BV 0xe33530).
static void testAutonomousBits() {
    Fixture fx;
    constexpr uint32_t kTubGroup = 0x7F000050;
    fx.code.add(kTubGroup, 0x80, definition(4, 2, 0x0B1E0004));
    InteractionTable t;
    t.version = 0x4D;
    t.interactions = {entry(0x1002, 0x1005, 0)};
    fx.code.add(kTubGroup, 2, std::move(t));
    fx.code.addInteractionStrings(kTubGroup, 2, ttas({"Only Alone"}, {"So Sozinho"}));
    // action: waits 1000 ticks; guard: parameter 0 == 1 (autonomous only)
    fx.code.add(kTubGroup, 0x1002, tree({expr(Param, 0, Assign, Lit, 1000, go(1), E), idle(0, false, go(2)),
                                         expr(SAttr, 1, Assign, Lit, 1, T, E)},
                                        4));
    fx.code.add(kTubGroup, 0x1005, tree({expr(Param, 0, Eq, Lit, 1, T, F)}, 1));
    const ObjectId tub = fx.vm.instantiate(kTubGroup, 0x80).id;
    // The Sim's own push of its last Find Best Interaction's entry (Main's
    // "Push Interaction" after Find Best Interaction): byte 14 bit 0x02.
    fx.code.add(kSimGroup, 0x1020, tree({expr(StackId, 0, Assign, Lit, int16_t(fx.sim), go(1), E),
                                         pushFlags2(0, 0, 2, 0x00, 0x02, T, F)},
                                        0, 1));
    // The same with "force run check tree" (byte 3 0x20), and without 0x02.
    fx.code.add(kSimGroup, 0x1021, tree({expr(StackId, 0, Assign, Lit, int16_t(fx.sim), go(1), E),
                                         pushFlags2(0, 0, 2, 0x20, 0x02, T, F)},
                                        0, 1));
    fx.code.add(kTubGroup, 0x1022, tree({expr(StackId, 0, Assign, Lit, int16_t(fx.sim), go(1), E),
                                         pushFlags2(0, 0, 2, 0x20, 0x00, T, F)},
                                        1, 1));
    // An object's push of the entry, object in parameter 0, without 0x02.
    fx.code.add(kTubGroup, 0x1023, tree({expr(StackId, 0, Assign, Lit, int16_t(fx.sim), go(1), E),
                                         pushFlags2(0, 0, 2, 0x00, 0x00, T, F)},
                                        1, 1));
    // The Sim pushes the fridge's "Clean" (TTA 1) onto itself, object in parameter 0.
    fx.code.add(kSimGroup, 0x1024, tree({expr(StackId, 0, Assign, Lit, int16_t(fx.sim), go(1), E),
                                         pushFlags2(1, 0, 2, 0x00, 0x00, T, F)},
                                        1, 1));
    Object& s = fx.s();
    s.person->data[kPersondataFindBestObject] = tub;
    s.data[kObjdataTreeTableEntry] = 0;
    auto runOn = [&](ObjectId me, uint16_t tree, int16_t arg = 0) {
        const int16_t args[] = {arg};
        Thread th = fx.vm.start(me, tree, args);
        fx.vm.run(th);
        if (th.status == Thread::Status::Failed)
            std::fprintf(stderr, "  %s\n", th.error.c_str());
        return th.status == Thread::Status::Finished && th.result;
    };
    check(runOn(fx.sim, 0x1020) && s.queue.size() == 1 && s.queue[0].object == tub && s.queue[0].autonomous &&
              s.queue[0].testAutonomous,
          "the Find Best Interaction's entry (byte 14 0x02) is autonomous and tested as autonomous");
    fx.vm.tick();
    check(s.activeInteraction == 0 && s.queue.size() == 1 && s.person->data[persondata::InteractionAutonomous] == 1,
          "Idle for Input runs its check tree with parameter 0 = 1: it runs, person data 166 = 1");
    check(runOn(fx.sim, 0x1024, int16_t(fx.fridge)) && s.queue.size() == 2 && s.queue[1].autonomous &&
              !s.queue[1].testAutonomous,
          "a Sim's push passes on its current interaction's autonomous bit, not the test bit");

    Fixture fy;
    fy.code.add(kTubGroup, 0x80, definition(4, 2, 0x0B1E0004));
    InteractionTable t2;
    t2.version = 0x4D;
    t2.interactions = {entry(0x1002, 0x1005, 0)};
    fy.code.add(kTubGroup, 2, std::move(t2));
    fy.code.addInteractionStrings(kTubGroup, 2, ttas({"Only Alone"}, {"So Sozinho"}));
    fy.code.add(kTubGroup, 0x1002, tree({expr(Param, 0, Assign, Lit, 1000, go(1), E), idle(0, false, go(2)),
                                         expr(SAttr, 1, Assign, Lit, 1, T, E)},
                                        4));
    fy.code.add(kTubGroup, 0x1005, tree({expr(Param, 0, Eq, Lit, 1, T, F)}, 1));
    fy.code.add(kTubGroup, 0x1022, tree({expr(StackId, 0, Assign, Lit, int16_t(fy.sim), go(1), E),
                                         pushFlags2(0, 0, 2, 0x20, 0x00, T, F)},
                                        1, 1));
    fy.code.add(kTubGroup, 0x1023, tree({expr(StackId, 0, Assign, Lit, int16_t(fy.sim), go(1), E),
                                         pushFlags2(0, 0, 2, 0x00, 0x00, T, F)},
                                        1, 1));
    fy.code.add(kSimGroup, 0x1021, tree({expr(StackId, 0, Assign, Lit, int16_t(fy.sim), go(1), E),
                                         pushFlags2(0, 0, 2, 0x20, 0x02, T, F)},
                                        0, 1));
    const ObjectId tub2 = fy.vm.instantiate(kTubGroup, 0x80).id;
    Object& s2 = fy.s();
    s2.person->data[kPersondataFindBestObject] = tub2;
    s2.data[kObjdataTreeTableEntry] = 0;
    auto runOn2 = [&](ObjectId me, uint16_t tree, int16_t arg = 0) {
        const int16_t args[] = {arg};
        Thread th = fy.vm.start(me, tree, args);
        fy.vm.run(th);
        return th.status == Thread::Status::Finished && th.result;
    };
    check(!runOn2(tub2, 0x1022, int16_t(tub2)) && fy.vm.object(tub2)->data[kObjdataPrimitiveResult] == 2 && s2.queue.empty(),
          "Push's forced check without byte 14 0x02 is not autonomous: refused (result 2)");
    check(runOn2(fy.sim, 0x1021) && s2.queue.size() == 1 && s2.queue[0].testAutonomous,
          "with 0x02 it is: the push goes in");
    s2.queue.clear();
    syncQueueData(s2);
    check(runOn2(tub2, 0x1023, int16_t(tub2)) && s2.queue.size() == 1 && !s2.queue[0].autonomous &&
              !s2.queue[0].testAutonomous,
          "an object's push without 0x02: neither bit");
    fy.vm.tick();
    check(s2.queue.empty() && s2.activeInteraction == -1, "Idle for Input runs its check tree with parameter 0 = 0: refused");
}

// Round n31: an entry Idle for Input takes and refuses (its check tree
// fails) leaves the queue with its links undone (cTSPerson::ActionSkipped,
// BV 0xaca0cc: the Sim its link reaches stops waiting, person data 110), and
// the Idle for Input waits a tick before the next entry (TryIdleForInput
// returns 2). ClearSubActions (BV 0xadc4f4) undoes the waiting sub-actions'
// links too.
static void testSkippedEntryUnlinks() {
    Fixture fx;
    const ObjectId b = fx.vm.instantiate(kSimGroup, 0x80).id;
    fx.vm.startMain(b);
    const uint16_t ua = enqueue(fx.vm, fx.s(), fx.make(1)); // A runs "Clean" (long)
    fx.vm.tick();
    check(fx.s().activeInteraction == 0 && fx.s().person->data[110] == 0, "A runs its interaction");
    Object& sb = *fx.vm.object(b);
    auto refused = fx.make(3); // its check tree says no
    enqueue(fx.vm, sb, refused);
    auto snack = fx.make(0);
    enqueue(fx.vm, sb, snack);
    sb.queue[0].link = ua; // linked to A's interaction (a social's chain)
    fx.vm.tick();
    check(fx.s().person->data[110] == 1, "B's refused entry undoes its link: A gets person data 110");
    check(sb.queue.size() == 1 && sb.activeInteraction == -1 && (fx.o().attributes.empty() || fx.o().attributes[0] != 7),
          "B's Idle for Input waits a tick after the refused entry");
    fx.vm.tick();
    check(fx.o().attributes.size() > 0 && fx.o().attributes[0] == 7, "the next entry runs the next tick");

    Fixture fy;
    const ObjectId c = fy.vm.instantiate(kSimGroup, 0x80).id;
    const uint16_t ua2 = enqueue(fy.vm, fy.s(), fy.make(1));
    fy.vm.tick();
    Object& sc = *fy.vm.object(c);
    auto sub = fy.make(0);
    sub.uid = newInteractionId(fy.vm, sc);
    sub.link = ua2;
    sc.subQueue.push_back(sub);
    clearSubActions(fy.vm, sc);
    check(sc.subQueue.empty() && fy.s().person->data[110] == 1,
          "ClearSubActions undoes the waiting sub-actions' links");
}

// Round n31: TryPushAction always runs cEdithObjectTestSim::TestInteraction
// for the target (BV 0x4c6214), and Idle for Input before an entry runs: a
// debug entry (TTAB 0x80) is refused, and so is one without "allow
// visitors" (TTAB 0x0001) for a visitor (person data 32 not 0); the push
// fails with result 2, the queued entry is skipped.
static void testPushTestsTheEntry() {
    Fixture fx;
    constexpr uint32_t kBoxGroup = 0x7F000060;
    fx.code.add(kBoxGroup, 0x80, definition(4, 3, 0x0B1E0005));
    InteractionTable t;
    t.version = 0x54;
    auto debug = entry(0x1001, 0, 0), resident = entry(0x1001, 0, 1), visitors = entry(0x1001, 0, 2);
    debug.flags = 0x80;
    visitors.flags = 0x01;
    t.interactions = {debug, resident, visitors};
    fx.code.add(kBoxGroup, 3, std::move(t));
    fx.code.addInteractionStrings(kBoxGroup, 3, ttas({"*Debug", "Residents", "Everyone"}, {"*Debug", "Moradores", "Todos"}));
    fx.code.add(kBoxGroup, 0x1001, tree({expr(SAttr, 0, Add, Lit, 1, T, E)}));
    for (uint8_t i : {0, 1, 2})
        fx.code.add(kBoxGroup, uint16_t(0x1030 + i), tree({expr(StackId, 0, Assign, Lit, int16_t(fx.sim), go(1), E),
                                                            push(i, 0, 3, 0x00, T, F)},
                                                           1, 1));
    const ObjectId box = fx.vm.instantiate(kBoxGroup, 0x80).id;
    auto pushed = [&](uint16_t tree) {
        const int16_t args[] = {int16_t(box)};
        Thread th = fx.vm.start(box, tree, args);
        fx.vm.run(th);
        return th.status == Thread::Status::Finished && th.result;
    };
    Object& s = fx.s();
    check(!pushed(0x1030) && fx.vm.object(box)->data[kObjdataPrimitiveResult] == 2 && s.queue.empty(),
          "a debug entry is never pushed (result 2)");
    check(pushed(0x1031) && s.queue.size() == 1, "a resident takes an entry without \"allow visitors\"");
    s.queue.clear();
    syncQueueData(s);
    s.person->data[kPersondataPersonType] = 1; // a visitor
    check(!pushed(0x1032 - 1) && fx.vm.object(box)->data[kObjdataPrimitiveResult] == 2 && s.queue.empty(),
          "a visitor does not (result 2)");
    check(pushed(0x1032) && s.queue.size() == 1, "an entry with \"allow visitors\" goes onto a visitor's queue");
    s.queue.clear();
    syncQueueData(s);
    enqueue(fx.vm, s, *makeInteraction(fx.vm, box, 1)); // put there by other means
    fx.vm.tick();
    check(s.queue.empty() && s.activeInteraction == -1 &&
              (fx.vm.object(box)->attributes.empty() || fx.vm.object(box)->attributes[0] == 0),
          "Idle for Input skips a queued entry the visitor may not run");
}

// Round n31: when an interaction returns, the Idle for Input that ran it
// runs again and starts the next entry only if it is "continue as current"
// and the finished one does not say "no continuation" (cTSInteraction +0x54
// bit 0x4000, Push Interaction byte 14 bit 0x10; TryIdleForInput reads it
// through vtable 0x8c), else returns true and Main goes on (here: counts in
// temp 2) before the next Idle for Input takes it.
static void testContinuationAfterReturn() {
    for (const bool noContinuation : {false, true}) {
        Fixture fx;
        constexpr uint32_t kMainGroup = 0x7F000070;
        fx.code.add(kMainGroup, 0x80, definition(kObjectTypePerson, 0, 0x5150002));
        ObjectFunctions f;
        f.functions.resize(2);
        f.functions[1].action = 0x1000;
        fx.code.add(kMainGroup, 0x80, std::move(f));
        fx.code.add(kMainGroup, 0x1000, tree({expr(Param, 0, Assign, Lit, 1, go(1), E), idle(0, true, go(2)),
                                              expr(Temp, 2, Add, Lit, 1, go(0), E)},
                                             1));
        const ObjectId sim = fx.vm.instantiate(kMainGroup, 0x80).id;
        fx.vm.startMain(sim);
        Object& s = *fx.vm.object(sim);
        auto first = fx.make(0), then = fx.make(0);
        first.noContinuation = noContinuation;
        then.flags = 0x04; // continue as current
        enqueue(fx.vm, s, first);
        enqueue(fx.vm, s, then);
        fx.vm.tick();
        const Thread& t = fx.vm.threads[sim];
        check(s.queue.empty() && t.temps[2] == (noContinuation ? 2 : 1),
              noContinuation ? "\"no continuation\": Main goes on before the continuation runs"
                             : "a continuation runs from the re-run, before Main goes on");
    }
}

// Round n46: the person's object records (cTSPerson +0x60c,
// docs/primitives-mac.md §4j). Idle for Input's gosub of the action tree
// makes one (the object, the stack's depth with the frame); the object's
// data 62 "use count" is the number of records on it; the record goes with
// its frame. Test Sim Interacting With reads them. A person's reset resets
// the objects of its records (their cleanup tree runs) unless it is dying.
// An entry that leaves the queue unrun (ActionSkipped) runs its object's
// OBJf entry 4 "queue skipped" with the person as stack object.
static void testObjectRecords() {
    Fixture fx;
    ObjectFunctions f;
    f.functions.resize(5);
    f.functions[1].action = 0x1010; // Main: idle for ever
    f.functions[3].action = 0x1011; // cleanup: attribute 2 := 9
    f.functions[4].action = 0x1012; // queue skipped: attribute 3 := the stack object
    fx.code.add(kObjGroup, 0x80, std::move(f));
    fx.code.add(kObjGroup, 0x1010, tree({expr(Param, 0, Assign, Lit, 30000, go(1), E), idle(0, false, go(0))}, 1));
    fx.code.add(kObjGroup, 0x1011, tree({expr(Attr, 2, Assign, Lit, 9, T, E)}));
    fx.code.add(kObjGroup, 0x1012, tree({expr(Attr, 3, Assign, StackId, 0, T, E)}));
    fx.code.add(kObjGroup, 0x1013, tree({instr(0x25, T, F)})); // Test Sim Interacting With (the stack object)
    fx.vm.startMain(fx.fridge);
    if (fx.o().attributes.size() < 4)
        fx.o().attributes.resize(4);

    enqueue(fx.vm, fx.s(), fx.make(1)); // "Clean": waits 1000 ticks
    fx.vm.tick();
    auto& rec = fx.s().person->objectRecords;
    check(fx.s().activeInteraction == 0 && rec.size() == 1 && rec[0].object == fx.fridge && rec[0].depth == 2 &&
              !rec[0].flag,
          "the action tree's gosub made an object record (the object, the depth with its frame)");
    check(fx.o().data[62] == 1, "the object's data 62 'use count' counts the record");
    check(fx.s().person->recordsAtAction == 1, "+0x646: the records with the action's own");
    check(hasObjectReference(fx.s(), fx.fridge) && !hasObjectReference(fx.s(), fx.sim), "HasObjectReference");
    {
        std::array<int16_t, kTempCount> temps{};
        check(runOneTickTree(fx.vm, temps, fx.sim, fx.fridge, fx.fridge, 0x1013, {}),
              "Test Sim Interacting With: the stack object is in my records");
        check(fx.s().person->objectRecords.size() == 1, "a one-tick tree leaves the records as they were");
    }
    cancelInteraction(fx.vm, fx.s(), fx.s().queue[0].uid);
    for (int i = 0; i < 3; ++i)
        fx.vm.tick();
    check(fx.s().activeInteraction == -1 && rec.empty() && fx.o().data[62] == 0,
          "the record goes with its frame; data 62 back to 0");

    // The reset cascade.
    enqueue(fx.vm, fx.s(), fx.make(1));
    fx.vm.tick();
    check(fx.o().data[62] == 1 && fx.o().attributes[2] == 0, "using it again");
    fx.vm.resetObject(fx.sim, fx.vm.threads[fx.sim]);
    check(fx.o().attributes[2] == 9 && fx.o().data[62] == 0 && fx.s().person->objectRecords.empty(),
          "a person's reset resets the objects of its records (their cleanup tree ran)");
    fx.o().attributes[2] = 0;
    enqueue(fx.vm, fx.s(), fx.make(1));
    fx.vm.tick();
    fx.vm.dyingObjects.insert(fx.sim);
    fx.vm.resetObject(fx.sim, fx.vm.threads[fx.sim]);
    fx.vm.dyingObjects.clear();
    check(fx.o().attributes[2] == 0 && fx.o().data[62] == 0, "not when the person is in the kill queue");

    // ActionSkipped: OBJf entry 4 of the skipped entry's object.
    fx.o().attributes[3] = 0;
    fx.vm.tick();
    enqueue(fx.vm, fx.s(), fx.make(3)); // its guard says no
    for (int i = 0; i < 3; ++i)
        fx.vm.tick();
    check(fx.s().queue.empty() && fx.o().attributes[3] == fx.sim,
          "a skipped entry runs its object's 'queue skipped' tree with the person as stack object");
}

// Round n46: person data 109 "ci: Current Interaction GUID" is the current
// action's GUID (SetInteractionData) and, while a sub-action runs, the
// sub-action's (SetCurrentSubAction), which stays after it ends until the
// next SetInteractionData; once the action ends, the empty interaction's
// new GUID. 164 "Current Interaction Joinable" is the action's TTAB flag
// 0x02.
static void testInteractionGuidAndJoinable() {
    Fixture fx;
    constexpr uint32_t kTvGroup = 0x7F000030;
    fx.code.add(kTvGroup, 0x80, definition(4, 1, 0x0B1E0002));
    InteractionTable t;
    t.version = 0x4D;
    auto watch = entry(0x1001, 0, 0), react = entry(0x1002, 0, 1);
    watch.flags = 0x4000 | 0x02; // nests, joinable
    react.flags = 0x8000;
    t.interactions = {watch, react};
    fx.code.add(kTvGroup, 1, std::move(t));
    fx.code.addInteractionStrings(kTvGroup, 1, ttas({"Watch", "React"}, {"Assistir", "Reagir"}));
    // Watch: handles the sub-queue, then waits for the sub-action to have run
    // (stack object attribute 2 = 5) and ends.
    fx.code.add(kTvGroup, 0x1001, tree({instr(0x11, go(1), go(1), {0, 0, 0, 0, 1}),
                                        expr(Param, 0, Assign, Lit, 1, go(2), E), idle(0, false, go(3)),
                                        expr(SAttr, 2, Eq, Lit, 5, T, go(0))},
                                       4));
    fx.code.add(kTvGroup, 0x1002, tree({expr(SAttr, 2, Assign, Lit, 5, T, E)}));
    const ObjectId tv = fx.vm.instantiate(kTvGroup, 0x80).id;
    Object& s = fx.s();
    const uint16_t action = enqueue(fx.vm, s, *makeInteraction(fx.vm, tv, 0));
    fx.vm.tick();
    check(s.activeInteraction == 0 && s.person->data[109] == int16_t(action), "109: the action's GUID");
    check(s.person->data[164] == 1, "164: the action's TTAB entry is joinable (flag 0x02)");
    const uint16_t sub = enqueue(fx.vm, s, *makeInteraction(fx.vm, tv, 1));
    check(s.person->data[109] == int16_t(action), "an added entry: SetInteractionData, the action's");
    fx.vm.tick();
    check(!s.subAction && s.person->data[109] == int16_t(sub),
          "109: the sub-action's GUID, which stays after it ended");
    for (int i = 0; i < 3; ++i)
        fx.vm.tick();
    check(s.activeInteraction == -1 && s.person->data[164] == 0, "164: 0 without an action");
    check(s.person->data[109] != 0 && s.person->data[109] != int16_t(action) && s.person->data[109] != int16_t(sub),
          "109: the empty interaction's new GUID once the action ended");
}

int main() {
    testTableAndNames();
    testPieMenu();
    testEnqueueOrder();
    testMainRunsInteraction();
    testCancelRunning();
    testPushInteraction();
    testPushOntoFullQueue();
    testNotifyFlag();
    testActionStrings();
    testTurnBodyTowards();
    testInteractionGroupLeader();
    testSubQueue();
    testNestedInterruption();
    testLinkedInteractions();
    testRunImmediately();
    testStarEntryStringsFromNamedTree();
    testAutonomousBits();
    testSkippedEntryUnlinks();
    testPushTestsTheEntry();
    testContinuationAfterReturn();
    testObjectRecords();
    testInteractionGuidAndJoinable();
    if (failures)
        std::fprintf(stderr, "%d failure(s)\n", failures);
    else
        std::printf("queue: all tests passed\n");
    return failures ? 1 : 0;
}
