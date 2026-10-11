#include "formats/dbpf.hpp"
#include "formats/lot_objects.hpp"
#include "vm/vm.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
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

constexpr uint32_t kObjGroup = 0x7F000001;
constexpr uint32_t kSemiGroup = 0x7F000002;

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

// Expression: lhs (owner, data) op rhs (owner, data).
static Behavior::Instruction expr(uint8_t lo, int16_t ld, uint8_t op, uint8_t ro, int16_t rd, Target t, Target f) {
    return instr(0x02, t, f,
                 {uint8_t(ld), uint8_t(uint16_t(ld) >> 8), uint8_t(rd), uint8_t(uint16_t(rd) >> 8), 0, op, lo, ro});
}

enum Owner : uint8_t { Attr = 0x00, SAttr = 0x01, Semi = 0x02, Data = 0x03, SSemi = 0x05, Global = 0x06,
                       Lit = 0x07, Temp = 0x08, Param = 0x09, StackId = 0x0A, Motive = 0x0E, SMotiveTemp = 0x11,
                       PData = 0x12, SPerson = 0x13, SDef = 0x15, Local = 0x19, Const = 0x1A, PersonTemp = 0x1E };
enum Op : uint8_t { Gt = 0, Lt = 1, Eq = 2, Add = 3, Assign = 5, Mul = 6, Div = 7, FlagSet = 8, SetFlag = 9,
                    IncLt = 11, Or = 18, Xor = 19, Abs = 20 };

static Behavior tree(std::vector<Behavior::Instruction> code, uint8_t args = 0, uint8_t locals = 0) {
    Behavior b;
    b.fileName = "test";
    b.signature = 0x8009;
    b.argCount = args;
    b.localCount = locals;
    b.instructions = std::move(code);
    return b;
}

struct Fixture {
    CodeLibrary code;
    VM vm{code};
    Fixture() { code.setSemiGlobal(kObjGroup, kSemiGroup); }
    Object& obj() { return vm.object(1) ? *vm.object(1) : vm.createObject(kObjGroup, 8); }
    Thread run(uint16_t tree, std::initializer_list<int16_t> args = {}) {
        obj();
        Thread t = vm.start(1, tree, std::vector<int16_t>(args));
        vm.run(t);
        if (t.status == Thread::Status::Failed)
            std::fprintf(stderr, "  thread failed: %s\n", t.error.c_str());
        return t;
    }
};

static void testLoopWithLocalsAndParams() {
    Fixture fx;
    // local0 := 0; do local0++ while local0 < param0; temp0 := local0 * 10.
    fx.code.add(kObjGroup, 0x1000,
                tree({expr(Local, 0, Assign, Lit, 0, go(1), E),
                      expr(Local, 0, IncLt, Param, 0, go(1), go(2)),
                      expr(Temp, 0, Assign, Local, 0, go(3), E),
                      expr(Temp, 0, Mul, Lit, 10, T, E)},
                     1, 1));
    Thread t = fx.run(0x1000, {5});
    check(t.status == Thread::Status::Finished && t.result, "loop finishes true");
    check(t.temps[0] == 50, "loop count in temp 0");
    check(t.instructions == 1 + 5 + 2, "instruction count");
}

static void testCalls() {
    Fixture fx;
    // Callee: temp0 := param0 + param1; return (temp0 > 5).
    fx.code.add(kObjGroup, 0x1001,
                tree({expr(Temp, 0, Assign, Param, 0, go(1), E), expr(Temp, 0, Add, Param, 1, go(2), E),
                      expr(Temp, 0, Gt, Lit, 5, T, F)},
                     2));
    // Literal arguments 3 and 4: callee returns true, so temp1 := 1.
    fx.code.add(kObjGroup, 0x1000,
                tree({instr(0x1001, go(1), go(2), {3, 0, 4, 0}), expr(Temp, 1, Assign, Lit, 1, T, E),
                      expr(Temp, 1, Assign, Lit, 2, F, E)}));
    Thread t = fx.run(0x1000);
    check(t.status == Thread::Status::Finished && t.result && t.temps[0] == 7 && t.temps[1] == 1,
          "call with literal arguments, branch on true");

    // 0xFFFF in all four arguments passes temps 0-3.
    fx.code.add(kObjGroup, 0x1002,
                tree({expr(Temp, 0, Assign, Lit, 1, go(1), E), expr(Temp, 1, Assign, Lit, 2, go(2), E),
                      instr(0x1001, go(3), go(4), {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}),
                      expr(Temp, 2, Assign, Lit, 1, T, E), expr(Temp, 2, Assign, Lit, 2, F, E)}));
    t = fx.run(0x1002);
    check(t.status == Thread::Status::Finished && !t.result && t.temps[0] == 3 && t.temps[2] == 2,
          "call with temps as arguments, branch on false");

    // Byte 12 = 1: (owner, data) argument pairs, here local0 and literal 10.
    fx.code.add(kObjGroup, 0x1003,
                tree({expr(Local, 0, Assign, Lit, 6, go(1), E),
                      instr(0x1001, T, F, {Local, 0, 0, Lit, 10, 0, 0x0A, 0, 0, 0x0A, 0, 0, 1})},
                     0, 1));
    t = fx.run(0x1003);
    check(t.status == Thread::Status::Finished && t.result && t.temps[0] == 16, "call with owner/data arguments");
}

static void testScopes() {
    Fixture fx;
    // Global, private and semi-global trees and constants.
    fx.code.add(kGlobalGroup, 0x100, tree({expr(Temp, 0, Add, Const, (128 + 2) << 7 | 1, T, E)}));
    fx.code.add(kSemiGroup, 0x2005, tree({instr(0x100, T, E)}));
    fx.code.add(kObjGroup, 0x1000,
                tree({expr(Temp, 0, Assign, Const, 0 << 7 | 0, go(1), E), expr(Temp, 0, Add, Const, (64 + 3) << 7 | 2, go(2), E),
                      instr(0x2005, T, E)}));
    fx.code.add(kObjGroup, 0x1000, Constants{"", 0, {100}});
    fx.code.add(kSemiGroup, 0x2003, Constants{"", 0, {0, 0, 20}});
    fx.code.add(kGlobalGroup, 0x102, Constants{"", 0, {0, 3}});
    Thread t = fx.run(0x1000);
    check(t.status == Thread::Status::Finished && t.temps[0] == 123, "private/semi-global/global constants and calls");
    check(semiGlobalGroup("PersonGlobals") == 0x7F01EC29, "semi-global group hash");
}

static void testStackObjectAndAttributes() {
    Fixture fx;
    fx.obj();
    Object& other = fx.vm.createObject(kObjGroup, 4);
    other.attributes[3] = 42;
    // The callee starts with the caller's stack object; its own change is undone on return.
    fx.code.add(kObjGroup, 0x1001,
                tree({expr(Attr, 2, Assign, SAttr, 3, go(1), E), expr(StackId, 0, Assign, Lit, 1, T, E)}));
    fx.code.add(kObjGroup, 0x1000,
                tree({expr(StackId, 0, Assign, Lit, other.id, go(1), E), instr(0x1001, go(2), E),
                      expr(Global, 7, Assign, StackId, 0, T, E)}));
    Thread t = fx.run(0x1000);
    check(t.status == Thread::Status::Finished && fx.obj().attributes[2] == 42 && fx.vm.globals[7] == other.id,
          "stack object is inherited by subroutines and restored after them");

    // A thread can start with a stack object.
    fx.code.add(kObjGroup, 0x1002, tree({expr(Attr, 1, Assign, SAttr, 3, T, E)}));
    Thread s = fx.vm.start(1, 0x1002, {}, other.id);
    fx.vm.run(s);
    check(s.status == Thread::Status::Finished && fx.obj().attributes[1] == 42, "thread starts with a stack object");

    // An interaction: another object's private tree in my thread.
    Object& tv = fx.vm.createObject(0x7F000003, 4);
    tv.attributes[0] = 7;
    fx.code.add(0x7F000003, 0x1000, tree({expr(Attr, 3, Assign, SAttr, 0, T, E)}));
    Thread i = fx.vm.start(1, 0x1000, {}, tv.id, tv.id);
    fx.vm.run(i);
    check(i.status == Thread::Status::Finished && fx.obj().attributes[3] == 7,
          "a thread can run the stack object's code");
}

static void testObjectModel() {
    Fixture fx;
    fx.code.setAttributeLabelCount(kObjGroup, 3);
    fx.code.setAttributeLabelCount(kSemiGroup, 5);
    // OBJD fields from offset 0x44: version 0x8C, type (0x52) person, num attributes (0xB4) 2, price (0x64) 750.
    ObjectDefinition def;
    def.version = 0x8C;
    def.fields.resize(60);
    def.fields[(0x52 - 0x44) / 2] = kObjectTypePerson;
    def.fields[(0xB4 - 0x44) / 2] = 2;
    def.fields[(0x64 - 0x44) / 2] = 750;
    fx.code.add(kObjGroup, 0x80, def);
    Object& sim = fx.vm.instantiate(kObjGroup, 0x80);
    check(sim.attributes.size() == 3 && sim.semiAttributes.size() == 5 && sim.person, "instance sized from OBJD and labels");
    check(sim.data[objdata::ObjectId] == sim.id && sim.data[objdata::SemiAttributeCount] == 5,
          "engine-maintained object data");
    Object& chair = fx.vm.createObject(kObjGroup);
    check(!chair.person, "objects are not persons by default");

    // Semi-attributes, motives, person data (direct and [temp]) and the stack object's definition.
    fx.code.add(kObjGroup, 0x1000,
                tree({expr(StackId, 0, Assign, Lit, sim.id, go(1), E), expr(Semi, 4, Assign, Lit, 9, go(2), E),
                      expr(Temp, 0, Assign, SSemi, 4, go(3), E), expr(Motive, 7, Assign, Lit, -50, go(4), E),
                      expr(Temp, 1, Assign, Lit, 7, go(5), E), expr(Temp, 2, Assign, SMotiveTemp, 1, go(6), E),
                      expr(Temp, 1, Assign, Lit, 169, go(7), E), expr(PersonTemp, 1, Assign, Lit, 3, go(8), E),
                      expr(Temp, 3, Assign, SPerson, 169, go(9), E), expr(Temp, 4, Assign, SDef, 18, go(10), E),
                      expr(Temp, 5, Assign, SDef, 0, go(11), E), expr(Temp, 6, Assign, Data, objdata::ObjectId, go(12), E),
                      expr(Attr, 10, Assign, Lit, 1, T, E)}));
    Thread t = fx.vm.start(sim.id, 0x1000);
    fx.vm.run(t);
    if (t.status == Thread::Status::Failed)
        std::fprintf(stderr, "  thread failed: %s\n", t.error.c_str());
    check(t.status == Thread::Status::Finished, "object model tree runs");
    check(t.temps[0] == 9 && t.temps[2] == -50 && t.temps[3] == 3, "semi-attributes, motives and person data");
    check(t.temps[4] == 750 && t.temps[5] == 0x8C, "definition fields in the editor's numbering");
    check(t.temps[6] == sim.id, "object id in object data");
    check(sim.attributes.size() == 11 && sim.attributes[10] == 1, "attributes grow when written past the end");

    // Person data of an object that is not a Sim fails.
    fx.code.add(kObjGroup, 0x1001, tree({expr(Temp, 0, Assign, PData, 0, T, E)}));
    t = fx.vm.start(chair.id, 0x1001);
    fx.vm.run(t);
    check(t.status == Thread::Status::Failed && t.error.find("not a person") != std::string::npos,
          "person data needs a person");
    fx.code.add(kObjGroup, 0x1002, tree({expr(Global, int16_t(kGlobalCount), Assign, Lit, 1, T, E)}));
    t = fx.vm.start(chair.id, 0x1002);
    fx.vm.run(t);
    check(t.status == Thread::Status::Failed, "globals are bounded");
}

static void testExpressionOperators() {
    Fixture fx;
    fx.code.add(kObjGroup, 0x1000,
                tree({expr(Temp, 0, SetFlag, Lit, 3, go(1), E), expr(Temp, 0, FlagSet, Lit, 3, go(2), E),
                      expr(Temp, 1, Or, Lit, 0x30, go(3), E), expr(Temp, 1, Xor, Lit, 0x10, go(4), E),
                      expr(Temp, 2, Abs, Lit, -9, go(5), E), expr(Temp, 3, Assign, Lit, 32767, go(6), E),
                      expr(Temp, 3, Add, Lit, 1, T, E)}));
    Thread t = fx.run(0x1000);
    check(t.status == Thread::Status::Finished, "operators run");
    check(t.temps[0] == 4, "flags are numbered from 1");
    check(t.temps[1] == 0x20, "|= and ^=");
    check(t.temps[2] == 9, ":= abs");
    check(t.temps[3] == -32768, "16-bit wraparound");
}

static void testYieldAndErrors() {
    Fixture fx;
    // A primitive that yields until temp 5 reaches 3.
    fx.vm.setPrimitive(0x00, [](Context& c) {
        return ++c.thread.temps[5] < 3 ? Exit::Yield : Exit::True;
    });
    fx.code.add(kObjGroup, 0x1000, tree({instr(0x00, go(1), E), expr(Temp, 0, Assign, Lit, 1, T, E)}));
    fx.obj();
    Thread t = fx.vm.start(1, 0x1000);
    fx.vm.run(t);
    check(t.status == Thread::Status::Running && t.temps[5] == 1 && t.temps[0] == 0, "first yield");
    fx.vm.run(t);
    fx.vm.run(t);
    check(t.status == Thread::Status::Finished && t.temps[0] == 1, "resumes after yields");

    fx.code.add(kObjGroup, 0x1001, tree({instr(0x24, T, F)})); // Dialog: not implemented
    t = fx.run(0x1001);
    check(t.status == Thread::Status::Failed && fx.vm.missingPrimitives[0x24] == 1, "missing primitive fails");

    fx.code.add(kObjGroup, 0x1002, tree({expr(Temp, 0, Div, Lit, 0, T, F)}));
    t = fx.run(0x1002);
    check(t.status == Thread::Status::Failed && t.error.find("division by zero") != std::string::npos &&
              t.error.find("0x1002") != std::string::npos,
          "division by zero fails with a stack trace");

    fx.code.add(kObjGroup, 0x1003, tree({expr(Temp, 0, Eq, Lit, 1, T, E)}));
    t = fx.run(0x1003);
    check(t.status == Thread::Status::Failed, "error branch fails");

    fx.code.add(kObjGroup, 0x1004, tree({expr(Temp, 0, Add, Lit, 1, go(0), E)}));
    Thread loop = fx.vm.start(1, 0x1004);
    fx.vm.run(loop, 1000);
    check(loop.status == Thread::Status::Failed && loop.instructions == 1000, "instruction budget");

    fx.code.add(kObjGroup, 0x1005, tree({instr(0x1005, T, F)}));
    t = fx.run(0x1005);
    check(t.status == Thread::Status::Failed && t.error.find("stack overflow") != std::string::npos,
          "unbounded recursion fails");

    t = fx.run(0x1FFF);
    check(t.status == Thread::Status::Failed, "missing tree fails");

    fx.code.add(kObjGroup, 0x1006, tree({expr(0x0D, 0, Assign, Lit, 1, T, E)}));
    t = fx.run(0x1006);
    check(t.status == Thread::Status::Failed && fx.vm.missingOwners[0x0D] == 1, "missing data owner fails");
}

static uint8_t lo(uint32_t v, int byte) { return uint8_t(v >> (8 * byte)); }

// An OBJD with a GUID, optionally a person.
static ObjectDefinition definition(uint32_t guid, bool person = false) {
    ObjectDefinition def;
    def.fields.resize(60);
    def.fields[(0x5C - 0x44) / 2] = uint16_t(guid);
    def.fields[(0x5E - 0x44) / 2] = uint16_t(guid >> 16);
    if (person)
        def.fields[(0x52 - 0x44) / 2] = kObjectTypePerson;
    return def;
}

static void testPrimitives() {
    Fixture fx;
    constexpr uint32_t kChair = 0x11223344, kSim = 0x55667788;
    fx.code.add(kObjGroup, 0x80, definition(kChair));
    fx.code.add(kObjGroup, 0x81, definition(kSim, true));
    Object& me = fx.vm.instantiate(kObjGroup, 0x81);       // id 1
    Object& chair = fx.vm.instantiate(kObjGroup, 0x80);    // id 2
    Object& sim = fx.vm.instantiate(kObjGroup, 0x81);      // id 3
    Object& chair2 = fx.vm.instantiate(kObjGroup, 0x80);   // id 4
    check(me.id == 1 && chair.guid() == kChair && sim.person && chair2.id == 4, "objects from OBJDs");

    // Sleep (param 0 ticks), then Random Number temp0 := [0, 10).
    fx.code.add(kObjGroup, 0x1000, tree({instr(0x00, go(1), E, {0, 0}),
                                          instr(0x08, T, E, {0, 0, Temp, 0, 10, 0, Lit, 0})}, 1));
    Thread t = fx.vm.start(1, 0x1000, std::vector<int16_t>{2});
    fx.vm.run(t);
    fx.vm.run(t);
    check(t.status == Thread::Status::Running, "sleeps two ticks");
    fx.vm.run(t);
    check(t.status == Thread::Status::Finished && t.temps[0] >= 0 && t.temps[0] < 10, "wakes and draws a random number");

    // Set to Next: count persons into temp 1, find the chairs by GUID into temps 2/3.
    fx.code.add(kObjGroup, 0x1001,
                tree({instr(0x1F, go(1), go(2), {0, 0, 0, 0, 0x01}),             // next person -> stack object
                      expr(Temp, 1, Add, Lit, 1, go(0), E),
                      expr(StackId, 0, Assign, Lit, 0, go(3), E),
                      instr(0x1F, go(4), E, {lo(kChair, 0), lo(kChair, 1), lo(kChair, 2), lo(kChair, 3), 0x84, Temp, 0, 2, 0}),
                      expr(Temp, 3, Assign, Temp, 2, go(5), E),
                      instr(0x1F, go(6), E, {lo(kChair, 0), lo(kChair, 1), lo(kChair, 2), lo(kChair, 3), 0x84, Temp, 0, 3, 0}),
                      instr(0x1F, E, T, {lo(kChair, 0), lo(kChair, 1), lo(kChair, 2), lo(kChair, 3), 0x84, Temp, 0, 3, 0})}));
    t = fx.run(0x1001);
    check(t.status == Thread::Status::Finished && t.temps[1] == 2, "Set to Next iterates persons");
    check(t.temps[2] == chair.id && t.temps[3] == chair2.id, "Set to Next finds objects of a GUID in order");

    // Test Object Type on the stack object; verify sim; Break Point passes through.
    fx.code.add(kObjGroup, 0x1002,
                tree({expr(StackId, 0, Assign, Lit, sim.id, go(1), E),
                      instr(0x20, E, go(2), {lo(kChair, 0), lo(kChair, 1), lo(kChair, 2), lo(kChair, 3), 0, 0, StackId}),
                      instr(0x1F, go(3), E, {0, 0, 0, 0, 30}),
                      instr(0x0F, go(4), E, {1, 0, Lit, 0}),
                      expr(StackId, 0, Assign, Lit, chair.id, go(5), E),
                      instr(0x1F, E, T, {0, 0, 0, 0, 30})}));
    t = fx.run(0x1002);
    check(t.status == Thread::Status::Finished && t.result, "Test Object Type, verify sim and Break Point");

    fx.code.add(kObjGroup, 0x1003, tree({instr(0x0F, go(0), E, {1, 0, Lit, 0})}));
    t = fx.run(0x1003);
    check(t.status == Thread::Status::Failed && t.error.find("halts") != std::string::npos,
          "a Break Point that loops to itself halts");

    // Create a chair out of world, then remove it; then remove me.
    fx.code.add(kObjGroup, 0x1004,
                tree({instr(0x2A, go(1), E, {lo(kChair, 0), lo(kChair, 1), lo(kChair, 2), lo(kChair, 3), 6}),
                      expr(Temp, 0, Assign, StackId, 0, go(2), E),
                      instr(0x12, go(3), E, {1, 0}),
                      instr(0x12, go(4), E, {0, 0}),
                      expr(Temp, 1, Assign, Lit, 1, T, E)}));
    t = fx.vm.start(sim.id, 0x1004);
    fx.vm.run(t);
    check(t.temps[0] == 5 && !fx.vm.object(5) && !fx.vm.object(sim.id), "create and remove objects");
    check(t.status == Thread::Status::Killed && t.temps[1] == 0, "removing me kills the thread");
}

static StringTable names(std::initializer_list<const char*> values) {
    StringTable t;
    for (auto v : values)
        t.entries.push_back({lang::EnglishUS, v, "", 0});
    return t;
}

static void testRunTreeByName() {
    Fixture fx;
    constexpr uint32_t kTv = 0x7F000003;
    fx.obj();
    Object& tv = fx.vm.createObject(kTv, 2);
    // My code's table has one name; index 2 falls through to the global table.
    fx.code.add(kObjGroup, kNamedTreeStrings, names({"CT - Turn On"}));
    fx.code.add(kGlobalGroup, kNamedTreeStrings, names({"CT - Unused", "CT - Missing"}));
    // CT - Turn On: stack object's attribute 1 := param 0 + param 1, returns false.
    fx.code.add(kTv, 0x1000, [] {
        Behavior b = tree({expr(SAttr, 1, Assign, Param, 0, go(1), E), expr(SAttr, 1, Add, Param, 1, F, E)}, 2);
        b.fileName = "CT - Turn On";
        return b;
    }());
    fx.code.add(kObjGroup, 0x1000,
                tree({expr(StackId, 0, Assign, Lit, tv.id, go(1), E),
                      // name 1, push onto my stack, args (literal 5), (temp 0)
                      instr(0x1C, E, go(2), {0, 0, 0, 0, 1, 2, Lit, 5, 0, Temp, 0, 0, Lit, 0, 0}),
                      // name 2 is not a tree of the stack object: false
                      instr(0x1C, E, go(3), {0, 0, 0, 0, 2, 2}),
                      expr(Temp, 1, Assign, Lit, 1, T, E)}));
    Thread t = fx.vm.start(1, 0x1000, {});
    t.temps[0] = 3;
    fx.vm.run(t);
    if (t.status == Thread::Status::Failed)
        std::fprintf(stderr, "  thread failed: %s\n", t.error.c_str());
    check(t.status == Thread::Status::Finished && t.temps[1] == 1, "Run Tree by Name branches on the result");
    check(tv.attributes[1] == 8, "Run Tree by Name runs the stack object's tree with arguments");

    // Zeros at 6-7 and 2 at 8: my parameters are passed on.
    fx.code.add(kObjGroup, 0x1001, tree({expr(StackId, 0, Assign, Lit, tv.id, go(1), E),
                                         instr(0x1C, T, T, {0, 0, 0, 0, 1, 2, 0, 0, 2, 0, 9, 0xFD})},
                                        2));
    t = fx.vm.start(1, 0x1001, std::vector<int16_t>{4, 6});
    fx.vm.run(t);
    check(t.status == Thread::Status::Finished && tv.attributes[1] == 10, "Run Tree by Name passes my parameters on");
}

static void testRemoveAndRandom() {
    Fixture fx;
    fx.obj();
    fx.vm.createObject(kObjGroup);                                       // id 2: lead tile
    fx.vm.createObject(kObjGroup).data[objdata::LeadTileObjectId] = 2;   // id 3: its other tile
    fx.vm.createObject(kObjGroup);                                       // id 4
    fx.code.add(kObjGroup, 0x1000,
                tree({expr(StackId, 0, Assign, Lit, 3, go(1), E),
                      instr(0x12, go(2), E, {1, 0, 0x02}),                // remove every part of 3's object
                      instr(0x08, T, E, {0, 0, Temp, 0, 0, 0, Lit, 0})})); // range 0: error
    Thread t = fx.run(0x1000);
    check(t.status == Thread::Status::Failed && t.error.find("range evaluated to zero") != std::string::npos &&
              !fx.vm.object(2) && !fx.vm.object(3) && fx.vm.object(4),
          "Remove Object Instance cleans up all parts; Random Number with range 0 fails");
}

static void testRelationship() {
    Fixture fx;
    constexpr uint32_t kSim = 0x55667788;
    fx.code.add(kObjGroup, 0x81, definition(kSim, true));
    Object& me = fx.vm.instantiate(kObjGroup, 0x81);  // id 1
    Object& her = fx.vm.instantiate(kObjGroup, 0x81); // id 2
    me.person->data[persondata::NeighborId] = 5;
    her.person->data[persondata::NeighborId] = 7;
    const uint8_t myId = 0x0B; // object data 11
    fx.code.add(kObjGroup, 0x1000,
                tree({// var 1 of a relationship that does not exist yet: fail
                      instr(0x1A, E, go(1), {1, 0x01, Data, myId, 0, StackId, 0, 0, Temp, 0, 0}),
                      // set var 1 from me to her (object IDs) := 40
                      instr(0x1A, go(2), E, {1, 0x04, Data, myId, 0, StackId, 0, 0, Lit, 40, 0}),
                      // get it back by neighbor IDs into temp 1
                      instr(0x1A, go(3), E, {1, 0x03, Lit, 5, 0, Lit, 7, 0, Temp, 1, 0}),
                      expr(Temp, 2, Assign, Lit, 1, T, E)}));
    Thread t = fx.vm.start(me.id, 0x1000, {}, her.id);
    fx.vm.run(t);
    if (t.status == Thread::Status::Failed)
        std::fprintf(stderr, "  thread failed: %s\n", t.error.c_str());
    check(t.status == Thread::Status::Finished && t.temps[1] == 40 && t.temps[2] == 1,
          "Relationship sets and gets; Sims are keyed by neighbor ID");
}

static void testArrays() {
    Fixture fx;
    fx.obj();
    // Array op: (op, whose, array, owner1, data1, owner2, data2)
    auto arr = [](uint8_t op, uint8_t array, uint8_t o1, int16_t d1, uint8_t o2, int16_t d2, Target t, Target f) {
        return instr(0x76, t, f, {0, op, 0, array, 0, o1, uint8_t(d1), 0, o2, uint8_t(d2), 0});
    };
    fx.code.add(kObjGroup, 0x1000,
                tree({arr(5, 2, Lit, 10, Temp, 0, go(1), E),     // [10], temp0 = 0
                      arr(5, 2, Lit, 20, Temp, 0, go(2), E),     // [10, 20], temp0 = 1
                      arr(4, 2, Lit, 30, Temp, 0, go(3), E),     // [30, 10, 20], temp0 = 0
                      arr(10, 2, Lit, 20, 0x29, 2, go(4), E),    // iterator := 2
                      expr(Temp, 1, Assign, 0x2B, 2, go(5), E),  // temp1 := 20 (element at iterator)
                      arr(9, 2, Temp, 2, Lit, 0, go(6), E),      // remove [0] -> temp2 = 30
                      arr(1, 2, Temp, 3, Temp, 3, go(7), E),     // temp3 := 2
                      arr(10, 2, Lit, 30, Temp, 4, E, go(8)),    // 30 is gone: false
                      arr(12, 2, Temp, 0, Temp, 0, go(9), E),    // sort: [20, 10]
                      expr(Temp, 6, Assign, 0x2D, 2, go(10), E), // temp6 := [temp0 = 0] = 20
                      arr(8, 2, Temp, 7, Temp, 0, go(11), E),    // pop the last: temp7 = 10, [20]
                      arr(2, 2, Lit, 3, Temp, 0, go(12), E),     // [20, 0, 0]
                      arr(3, 2, Lit, 7, Temp, 0, go(13), E),     // [7, 7, 7]
                      arr(1, 2, Temp, 8, Temp, 8, go(14), E),    // temp8 := 3
                      arr(0, 2, Temp, 0, Temp, 0, go(15), E),    // clear
                      arr(1, 2, Temp, 5, Temp, 5, go(16), E),    // temp5 := 0
                      arr(7, 2, Temp, 0, Temp, 0, T, E)}));      // empty: error
    Thread t = fx.run(0x1000);
    check(t.status == Thread::Status::Failed && t.error.find("empty") != std::string::npos && t.temps[1] == 20 &&
              t.temps[2] == 30 && t.temps[3] == 2 && t.temps[6] == 20 && t.temps[7] == 10 && t.temps[8] == 3 &&
              t.temps[5] == 0,
          "Array Operation inserts, finds, removes, sorts, resizes and clears");
}

static void testChangeMaterial() {
    Fixture fx;
    fx.obj();
    fx.code.add(kObjGroup, kMaterialStrings, names({"lamp_off", "lamp_on"}));
    fx.code.add(kObjGroup, kMeshGroupStrings, names({"shade", "bulb"}));
    fx.code.add(kObjGroup, 0x1000,
                tree({instr(0x6D, go(1), E, {1, 0, 0, 1, 0, Data, 0x0B, 0}),           // bulb := lamp_on on me
                      instr(0x6D, T, E, {0, 0, 0, 0, 0, Data, 0x0B, 0})}));             // shade := lamp_off
    Thread t = fx.run(0x1000);
    const auto& m = fx.obj().materials;
    check(t.status == Thread::Status::Finished && m.size() == 2 && m.at("bulb") == "lamp_on" &&
              m.at("shade") == "lamp_off",
          "Change Material records mesh group materials");
}

static void testManageInventory() {
    Fixture fx;
    fx.obj();
    constexpr uint32_t kGuid = 0x4DDF0E12;
    // Neighbor 5's singular inventory: (op, guid, owner1, data1, owner2, data2).
    auto inv = [](uint8_t op, uint32_t guid, uint8_t o1, int16_t d1, uint8_t o2, int16_t d2, Target t, Target f) {
        return instr(0x33, t, f,
                     {3, Lit, 5, 0, op, uint8_t(guid), uint8_t(guid >> 8), uint8_t(guid >> 16), uint8_t(guid >> 24), 0,
                      o1, uint8_t(d1), 0, o2, uint8_t(d2), 0});
    };
    constexpr uint8_t Token = 0x27;
    fx.code.add(kObjGroup, 0x1000,
                tree({inv(0, kGuid, Lit, 0, Lit, 0, go(1), E),         // add
                      inv(0, kGuid, Lit, 0, Lit, 0, go(2), E),         // add another
                      inv(3, kGuid, Temp, 0, Lit, 0, go(3), E),        // find: temp0 = 1
                      inv(4, 0, Temp, 0, Lit, 42, go(4), E),           // token 1 properties: [42]
                      inv(10, 0, Lit, 0, Temp, 1, go(5), E),           // count: temp1 = 2
                      inv(6, 0, Lit, 1, Lit, 0, go(6), E),             // read token 1
                      inv(7, 0, Lit, 0, Temp, 2, go(7), E),            // temp2 = 42
                      inv(8, 0, Lit, 0, Lit, 7, go(8), E),             // property 0 := 7
                      inv(9, 0, Lit, 0, Lit, 0, go(9), E),             // save it back
                      expr(Temp, 3, Assign, Token, 7, go(10), E),      // temp3 = property count = 1
                      inv(6, kGuid, Lit, 2, Lit, 0, go(11), E),        // next of the GUID from 2: token 2
                      inv(7, 0, Lit, 0, Temp, 6, E, go(12)),           // it has no properties: false
                      inv(5, 0, Lit, 1, Temp, 4, go(13), E),           // pop token 1's: temp4 = 7
                      inv(2, kGuid, Lit, 0, Lit, 0, go(14), E),        // remove all of the GUID
                      inv(10, 0, Lit, 0, Temp, 5, go(15), E),          // temp5 = 0
                      inv(6, 0, Lit, 1, Lit, 0, E, T)}));              // nothing to read: false
    Thread t = fx.run(0x1000);
    check(t.status == Thread::Status::Finished && t.temps[0] == 1 && t.temps[1] == 2 && t.temps[2] == 42 &&
              t.temps[3] == 1 && t.temps[4] == 7 && t.temps[5] == 0,
          "Manage Inventory adds, finds, reads, changes and saves tokens");
}

// Set to Next (0x80 | type) with the target in a temp.
static Behavior::Instruction next(uint8_t type, uint8_t temp, Target t, Target f, uint32_t guid = 0) {
    return instr(0x1F, t, f, {lo(guid, 0), lo(guid, 1), lo(guid, 2), lo(guid, 3), uint8_t(0x80 | type), Temp, 0, temp, 0});
}

static void testSetToNextSearches() {
    Fixture fx;
    constexpr uint32_t kChair = 0x11223344, kCar = 0x0BADCAFE;
    fx.code.add(kObjGroup, 0x80, definition(kChair));
    ObjectDefinition car = definition(kCar);
    car.fields[(0x52 - 0x44) / 2] = kObjectTypeVehicle;
    fx.code.add(kObjGroup, 0x81, std::move(car));
    fx.vm.instantiate(kObjGroup, 0x80);                    // id 1: me
    fx.vm.instantiate(kObjGroup, 0x81);                    // id 2: a vehicle
    fx.vm.instantiate(kObjGroup, 0x80);                    // id 3: lead tile of a 2-tile object
    Object& part = fx.vm.instantiate(kObjGroup, 0x80);     // id 4: its other tile
    Object& lone = fx.vm.instantiate(kObjGroup, 0x80);     // id 5
    part.data[objdata::LeadTileObjectId] = 3;
    lone.data[objdata::Category] = 7;
    fx.code.add(kObjGroup, 0x1000,
                tree({expr(Temp, 0, Assign, Lit, 7, go(1), E),
                      next(12, 7, go(2), E),                                // category == temp 0: temp7 = 5
                      next(16, 2, go(3), E),                                // vehicle: temp2 = 2
                      expr(Temp, 0, Assign, Lit, int16_t(kChair & 0xFFFF), go(4), E),
                      expr(Temp, 1, Assign, Lit, int16_t(kChair >> 16), go(5), E),
                      next(18, 3, go(6), E),                                // GUID in temps 0/1: temp3 = 1
                      next(18, 3, go(7), E),                                // and then 3
                      expr(Temp, 4, Assign, Lit, 4, go(8), E),
                      next(3, 4, go(9), E),                                 // part after 4 wraps to 3
                      expr(Temp, 5, Assign, Lit, 5, go(10), E),
                      next(3, 5, E, go(11)),                                // a single tile has no parts
                      next(6, 6, T, E)},                                    // category == param 0: temp6 = 5
                     1));
    Thread t = fx.run(0x1000, {7});
    check(t.status == Thread::Status::Finished && t.temps[7] == 5 && t.temps[2] == 2 && t.temps[3] == 3 &&
              t.temps[4] == 3 && t.temps[6] == 5,
          "Set to Next by category, vehicle, GUID in temps and multi-tile part");
}

static void testNeighbors() {
    Fixture fx;
    constexpr uint32_t kSim = 0x55667788, kOther = 0x01020304;
    fx.code.add(kObjGroup, 0x81, definition(kSim, true));
    ObjectDefinition other = definition(kOther, true);
    other.fields[(0x40 + 2 * 20 - 0x44) / 2] = 123; // editor's definition field 20
    fx.code.add(kObjGroup, 0x82, std::move(other));
    Object& me = fx.vm.instantiate(kObjGroup, 0x81);
    me.person->data[persondata::NeighborId] = 5;
    me.person->data[persondata::PersonAge] = 20;
    fx.vm.neighbors[5] = {kSim, 3, {}};
    Neighbor away{kOther, 3, {}};
    away.personData[persondata::PersonAge] = 40;
    fx.vm.neighbors[9] = away;
    constexpr uint8_t InStack = 0x18, PersonOf = 0x20, DefOf = 0x26;
    fx.code.add(kObjGroup, 0x1000,
                tree({instr(0x1F, go(1), E, {0, 0, 0, 0, 5}),           // next neighbor -> stack object: 5
                      expr(Temp, 0, Assign, InStack, 0, go(2), E),      // instance id: me
                      expr(Temp, 1, Assign, InStack, 2, go(3), E),      // age from the Sim on the lot: 20
                      instr(0x1F, go(4), E, {0, 0, 0, 0, 5}),           // next: 9
                      expr(Temp, 2, Assign, InStack, 0, go(5), E),      // not on the lot: 0
                      expr(Temp, 3, Assign, PersonOf, persondata::PersonAge, go(6), E), // 40
                      expr(Temp, 4, Assign, InStack, 6, go(7), E),      // house number: 3
                      expr(Temp, 5, Assign, DefOf, 20, go(8), E),       // 123
                      instr(0x1F, E, go(9), {0, 0, 0, 0, 5}),           // no more neighbors
                      expr(StackId, 0, Assign, Lit, 0, go(10), E),
                      instr(0x1F, go(11), E, {lo(kOther, 0), lo(kOther, 1), lo(kOther, 2), lo(kOther, 3), 7}), // 9
                      instr(0x1F, go(12), E, {0, 0, 0, 0, 29}),         // 9 exists
                      expr(StackId, 0, Assign, Lit, 4, go(13), E),
                      instr(0x1F, E, go(14), {0, 0, 0, 0, 29}),         // 4 does not
                      expr(Temp, 6, Assign, InStack, 0, T, E)}));       // so its data is missing
    Thread t = fx.run(0x1000);
    check(t.status == Thread::Status::Failed && t.error.find("Missing neighbor") != std::string::npos &&
              t.temps[0] == me.id && t.temps[1] == 20 && t.temps[2] == 0 && t.temps[3] == 40 && t.temps[4] == 3 &&
              t.temps[5] == 123,
          "neighbor searches and neighbor data owners");
}

static void testGenericSimsCall() {
    Fixture fx;
    fx.obj();
    Object& sim = fx.vm.createObject(kObjGroup); // id 2, a Sim on the lot (neighbor 5)
    sim.person.emplace();
    sim.person->data[persondata::NeighborId] = 5;
    fx.vm.neighbors[5] = {};
    fx.vm.neighbors[6] = {};
    fx.vm.neighbors[6].personData[persondata::FamilyNumber] = 3;
    fx.vm.globals[global::CurrentFamily] = 3;
    auto call = [](uint8_t i, Target t, Target f = E) { return instr(0x01, t, f, {i}); };
    fx.code.add(kObjGroup, 0x1000,
                tree({call(39, go(1)),                                    // recalc wants: recorded
                      expr(StackId, 0, Assign, Lit, 5, go(2), E),
                      expr(Temp, 0, Assign, Lit, 0, go(3), E),
                      call(4, go(4)),                                     // neighbor 5 joins family 3
                      expr(Temp, 0, Assign, Lit, -1, go(5), E),
                      expr(StackId, 0, Assign, Lit, 6, go(6), E),
                      call(4, go(7)),                                     // neighbor 6 gets a new family: 4
                      expr(StackId, 0, Assign, Lit, 9, go(8), E),
                      call(6, E, go(9)),                                  // no neighbor 9
                      call(10, go(10)),
                      expr(Temp, 0, Assign, Lit, 0, go(11), E),
                      call(18, go(12)),                                   // pause
                      expr(Temp, 0, Assign, Lit, 2, go(13), E),
                      call(25, go(14)),                                   // selected Sim := 2
                      call(48, E, go(15)),                                // no remapped IDs
                      call(52, T)}));                                     // wall in front: not implemented
    Thread t = fx.run(0x1000);
    check(t.status == Thread::Status::Failed && t.error.find("52 not implemented") != std::string::npos &&
              fx.vm.engineRequests.size() == 1 && fx.vm.engineRequests[0].call == 39 &&
              sim.person->data[persondata::FamilyNumber] == 3 && fx.vm.neighbors[5].personData[persondata::FamilyNumber] == 0 &&
              fx.vm.neighbors[6].personData[persondata::FamilyNumber] == 4 && !fx.vm.buildBuyEnabled &&
              fx.vm.globals[global::Paused] == 1 && fx.vm.globals[global::SelectedPersonId] == 2,
          "Generic Sims Call");
}

static void testDefinitionOwners() {
    Fixture fx;
    constexpr uint32_t kMaster = 0x0A0B0C0D, kPart = 0x01020304;
    auto field = [](ObjectDefinition& d, size_t label) -> uint16_t& { return d.fields[(0x40 + 2 * label - 0x44) / 2]; };
    ObjectDefinition master = definition(kMaster), part = definition(kPart);
    for (auto* d : {&master, &part}) {
        d->fields.resize(120);
        d->fields[(0x54 - 0x44) / 2] = 7; // master id
    }
    master.fields[(0x56 - 0x44) / 2] = 0xFFFF; // sub index -1
    field(master, 90) = 3;                     // num type attributes
    fx.code.add(kObjGroup, 0x80, std::move(master));
    fx.code.add(kObjGroup, 0x81, std::move(part));
    ObjectFunctions functions;
    functions.functions = {{0, 0x1001}, {0, 0x1002}};
    fx.code.add(kObjGroup, 0x81, std::move(functions));
    fx.code.add(kGlobalGroup, 0x140, Constants{"", 0, {0, 30, 48, 60}});
    fx.vm.instantiate(kObjGroup, 0x81); // id 1: me
    fx.vm.instantiate(kObjGroup, 0x81); // id 2: another part of the same type
    fx.vm.rooms[2].values = {100, 0, 1, 12, 0, 2};
    constexpr uint8_t Room = 0x17, Function = 0x23, MyType = 0x24, SType = 0x25, ConstTemp = 0x2F, Master = 0x33;
    fx.code.add(kObjGroup, 0x1000,
                tree({expr(StackId, 0, Assign, Lit, 2, go(1), E),
                      expr(MyType, 1, Assign, Lit, 5, go(2), E),
                      expr(Temp, 0, Assign, SType, 1, go(3), E),          // shared by the type: 5
                      expr(Temp, 1, Assign, Master, 14, go(4), E),        // master's GUID low word
                      expr(Temp, 2, Assign, Function, 1, go(5), E),       // 0x1002
                      expr(Temp, 3, Assign, Function, 9, go(6), E),       // none: 0
                      expr(Temp, 3, Assign, Lit, 2, go(7), E),
                      expr(Temp, 4, Assign, ConstTemp, 0x4203, go(8), E), // BCON 0x140 [temp 3] = 48
                      expr(Temp, 0, Assign, Lit, 2, go(9), E),
                      expr(Temp, 5, Assign, Room, 2, go(10), E),          // room 2 level: 1
                      expr(Temp, 0, Assign, Lit, 3, go(11), E),
                      expr(Temp, 6, Assign, Room, 2, T, E)}));            // no room 3
    Thread t = fx.run(0x1000);
    check(t.status == Thread::Status::Failed && t.error.find("room 3 does not exist") != std::string::npos &&
              t.temps[1] == int16_t(kMaster & 0xFFFF) && t.temps[2] == 0x1002 && t.temps[3] == 2 && t.temps[4] == 48 &&
              t.temps[5] == 1 && fx.vm.typeAttributes[kMaster].size() == 3 && fx.vm.typeAttributes[kMaster][1] == 5,
          "type attributes, master definition, object functions, constants [temp] and rooms");
}

static void testFamilyTree() {
    Fixture fx;
    fx.obj();
    auto call = [](uint8_t i, Target t) { return instr(0x01, t, E, {i}); };
    auto next = [](uint8_t type, uint8_t temp, Target t, Target f) {
        return instr(0x1F, t, f, {0, 0, 0, 0, uint8_t(type | 0x80), Temp, 0, temp, 0});
    };
    // Children 3 and 4 of parent 1, child 5 of parent 2; 1 and 2 are spouses.
    for (int16_t id : {1, 2, 3, 4, 5})
        fx.vm.neighbors[id] = {};
    fx.vm.neighbors[4].personData[persondata::FamilyNumber] = 6;
    std::vector<Behavior::Instruction> code;
    for (auto [child, parent] : {std::pair{3, 1}, {4, 1}, {5, 2}}) {
        const auto n = uint16_t(code.size());
        code.push_back(expr(Temp, 0, Assign, Lit, int16_t(child), go(n + 1), E));
        code.push_back(expr(Temp, 1, Assign, Lit, int16_t(parent), go(n + 2), E));
        code.push_back(call(21, go(n + 3)));
    }
    const auto n = uint16_t(code.size());
    for (auto in : {expr(Temp, 0, Assign, Lit, 1, go(n + 1), E), expr(Temp, 1, Assign, Lit, 2, go(n + 2), E),
                    call(22, go(n + 3)),
                    expr(Temp, 0, Assign, Lit, 3, go(n + 4), E),
                    next(22, 6, go(n + 5), E),                // sibling of 3: 4
                    next(22, 6, E, go(n + 6)),                // no more
                    next(21, 7, go(n + 7), E),                // parent of 3: 1
                    expr(Temp, 0, Assign, Lit, 1, go(n + 8), E),
                    next(31, 8, go(n + 9), E),                // spouse of 1: 2
                    next(23, 9, go(n + 10), E),               // first child of 1: 3
                    expr(Temp, 1, Assign, Lit, 3, go(n + 11), E),
                    call(23, go(n + 12)),                     // 3 is no longer 1's child
                    next(23, 10, go(n + 13), E),              // first child of 1: 4
                    expr(Temp, 0, Assign, Lit, 6, go(n + 14), E),
                    next(17, 11, go(n + 15), E),              // first neighbor of family 6: 4
                    next(17, 11, E, T)})                      // and no other
        code.push_back(in);
    fx.code.add(kObjGroup, 0x1000, tree(std::move(code)));
    Thread t = fx.run(0x1000);
    check(t.status == Thread::Status::Finished && t.temps[6] == 4 && t.temps[7] == 1 && t.temps[8] == 2 &&
              t.temps[9] == 3 && t.temps[10] == 4 && t.temps[11] == 4,
          "family tree calls and Set to Next family searches");
}

// Multi-tile objects: instantiating the master makes every part, all leading
// to the part with sub index 0; Set to Next "part of a multi-tile object"
// goes around them. Saved objects keep their IDs.
static void testMultiTileAndIds() {
    Fixture fx;
    constexpr uint32_t kMaster = 0x0A0B0C0D;
    auto mt = [](uint32_t guid, uint16_t sub) {
        ObjectDefinition d = definition(guid);
        d.fields[(0x54 - 0x44) / 2] = 5; // master ID
        d.fields[(0x56 - 0x44) / 2] = sub;
        return d;
    };
    fx.code.add(kObjGroup, 0x80, mt(kMaster, 0xFFFF));
    fx.code.add(kObjGroup, 0x81, mt(0x1001, 0x0100));
    fx.code.add(kObjGroup, 0x82, mt(0x1002, 0x0000));
    fx.code.add(kObjGroup, 0x83, mt(0x1003, 0x0001));
    check(fx.code.parts(kObjGroup, 5) == std::vector<uint16_t>{0x81, 0x82, 0x83}, "parts of a master");
    Object* lead = fx.vm.instantiateGuid(kMaster);
    check(lead && lead->definitionInstance == 0x82 && fx.vm.objects().size() == 3, "master makes all its parts");
    size_t led = 0;
    for (const auto& [id, o] : fx.vm.objects())
        led += o.data[objdata::LeadTileObjectId] == lead->id;
    check(led == 3, "every part leads to sub index 0");
    Object& single = fx.vm.instantiatePart(kObjGroup, 0x83);
    check(single.data[objdata::LeadTileObjectId] == single.id && single.data[objdata::SlotNumber] == -1,
          "a single part leads itself, in no slot");

    // Set to Next 3 from the lead visits the other two parts and comes back.
    fx.code.add(kObjGroup, 0x1000,
                tree({expr(StackId, 0, Assign, Lit, lead->id, go(1), E),
                      instr(0x1F, go(2), E, {0, 0, 0, 0, 3}), expr(Temp, 0, Assign, StackId, 0, go(3), E),
                      instr(0x1F, go(4), E, {0, 0, 0, 0, 3}), expr(Temp, 1, Assign, StackId, 0, go(5), E),
                      instr(0x1F, go(6), E, {0, 0, 0, 0, 3}), expr(Temp, 2, Assign, StackId, 0, T, E)}));
    Thread t = fx.vm.start(lead->id, 0x1000);
    fx.vm.run(t);
    check(t.status == Thread::Status::Finished && t.temps[0] != lead->id && t.temps[1] != lead->id &&
              t.temps[0] != t.temps[1] && t.temps[2] == lead->id,
          "Set to Next goes around the parts");

    Object& saved = fx.vm.createObject(kObjGroup, 0, 700);
    check(saved.id == 700 && saved.data[objdata::ObjectId] == 700, "objects can keep a saved ID");
    bool threw = false;
    try {
        fx.vm.createObject(kObjGroup, 0, 700);
    } catch (const VMError&) {
        threw = true;
    }
    check(threw, "an ID cannot be used twice");
}

// A package's own group (a Sim's character file) is added under a group of its own.
static std::vector<uint8_t> named(std::vector<uint8_t> body) {
    std::vector<uint8_t> out(64, 0);
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

static void put16(std::vector<uint8_t>& v, uint16_t x) { v.push_back(uint8_t(x)), v.push_back(uint8_t(x >> 8)); }
static void put32(std::vector<uint8_t>& v, uint32_t x) { put16(v, uint16_t(x)), put16(v, uint16_t(x >> 16)); }

static void testAddPackage() {
    constexpr uint32_t kLocal = 0xFFFFFFFF, kAs = 0xFE123456, kSimGuid = 0x51515151;
    std::vector<uint8_t> objd;
    put32(objd, 0x8C);
    const ObjectDefinition d = definition(kSimGuid, true);
    for (uint16_t f : d.fields)
        put16(objd, f);
    put32(objd, 3);
    objd.insert(objd.end(), {'S', 'i', 'm'});
    std::vector<uint8_t> objf;
    put32(objf, 0), put32(objf, 0);
    objf.insert(objf.end(), {'f', 'J', 'B', 'O'});
    put32(objf, 2);
    put16(objf, 0), put16(objf, 0x1001); // init
    put16(objf, 0), put16(objf, 0x1002); // main
    std::vector<uint8_t> glob{8, 'P', 'e', 'r', 's', 'o', 'n', 'G', 'l'};
    DbpfWriter w;
    w.add({kTypeObjd, kLocal, 0x80, 0}, named(objd));
    w.add({kTypeObjf, kLocal, 0x80, 0}, named(objf));
    w.add({kTypeGlob, kLocal, 0x80, 0}, named(glob));
    const auto path = std::filesystem::temp_directory_path() / "os2_test_character.package";
    w.save(path);
    DbpfPackage pkg(path);
    CodeLibrary code;
    const auto added = code.addPackage(pkg, kAs);
    check(added == std::vector<uint16_t>{0x80}, "the package's OBJD is added");
    check(code.definitionByGuid(kSimGuid) == std::pair{kAs, uint16_t(0x80)}, "under the group it was given");
    check(code.semiGlobalFor(kAs) == semiGlobalGroup("PersonGl"), "with its semi-global library");
    VM vm(code);
    Object& sim = vm.instantiatePart(kAs, 0x80, 42);
    const ObjectFunctions* f = code.functions(kAs, 0x80);
    check(sim.person && f && f->functions.size() == 2 && f->functions[1].action == 0x1002, "Sim with its functions");
    check(code.addPackage(pkg, kAs) == added && code.definition(kAs, 0x80) == sim.definition,
          "a package is added once");
    std::filesystem::remove(path);
}

// The saved state of an object (XOBJ), as the base game's lots store it.
static void testSavedObjectState() {
    std::vector<uint8_t> b(64, 0);
    for (int i = 0; i < 4; ++i)
        put32(b, 0);
    auto f32 = [&](float f) {
        uint32_t u;
        std::memcpy(&u, &f, 4);
        put32(b, u);
    };
    f32(13.5f), f32(9.5f); // y, x
    put32(b, 2);           // level
    put16(b, 1);
    f32(0);
    put32(b, 0xFFFFFFFF); // multi-tile group
    put16(b, 0);
    put16(b, 2), put16(b, 7), put16(b, 8); // attributes
    put16(b, 1), put16(b, 5);              // semi-attributes
    for (int i = 0; i < 8; ++i)
        put16(b, uint16_t(i));
    for (int i = 0; i < 88; ++i)
        put16(b, uint16_t(i == 1 ? 4 : i == 2 ? 615 : i == 3 ? 0 : i == 11 ? 1 : 0));
    const auto s = LotObjects::parseState(b, 0xAD);
    check(s.x == 9.5f && s.y == 13.5f && s.level == 2 && s.multiTileGroup == -1, "saved position");
    check(s.attributes == std::vector<int16_t>{7, 8} && s.semiAttributes == std::vector<int16_t>{5} &&
              s.temps[7] == 7,
          "saved attributes, semi-attributes and temps");
    check(s.data.size() == 88 && s.data[1] == 4 && s.data[2] == 615 && s.data[11] == 1, "saved object data");
    bool threw = false;
    try {
        LotObjects::parseState(b, 0xC0);
    } catch (const std::exception&) {
        threw = true;
    }
    check(threw, "other versions are not guessed at");
}

// Slots: data owners 0x14/0x10 (my / stack object's slot), 0x30/0x31 (slot in
// a temp) give the object in that slot of the container, 0 if empty; the slot
// count comes from the SLOT the OBJD names.
static void testSlots() {
    Fixture fx;
    ObjectDefinition counter = definition(0x0C0C0C0C);
    counter.fields[(0x68 - 0x44) / 2] = 0x80; // slots id
    fx.code.add(kObjGroup, 0x80, std::move(counter));
    Slots sl;
    sl.slots.resize(3);
    sl.slots[1].type = 3; // a target slot; the others are containers
    fx.code.add(kObjGroup, 0x80, std::move(sl));
    Object& c = fx.vm.instantiate(kObjGroup, 0x80);   // id 1
    Object& plate = fx.vm.createObject(kObjGroup);     // id 2, in slot 1 of 1
    Object& other = fx.vm.createObject(kObjGroup);     // id 3, holds 1 in its slot 0
    check(c.data[objdata::SlotCount] == 2, "slot count from the SLOT's container slots");
    plate.data[objdata::ContainerId] = c.id, plate.data[objdata::SlotNumber] = 1;
    c.data[objdata::ContainerId] = other.id, c.data[objdata::SlotNumber] = 0;
    check(fx.vm.containedIn(c.id, 1) == plate.id && fx.vm.containedIn(c.id, 0) == 0, "containment");
    enum : uint8_t { SSlot = 0x10, MySlot = 0x14, MySlotTemp = 0x30, SSlotTemp = 0x31 };
    fx.code.add(kObjGroup, 0x1000,
                tree({expr(StackId, 0, Assign, Lit, other.id, go(1), E), expr(Temp, 0, Assign, MySlot, 1, go(2), E),
                      expr(Temp, 1, Assign, MySlot, 0, go(3), E), expr(Temp, 5, Assign, Lit, 1, go(4), E),
                      expr(Temp, 2, Assign, MySlotTemp, 5, go(5), E), expr(Temp, 3, Assign, SSlot, 0, go(6), E),
                      expr(Temp, 6, Assign, Lit, 0, go(7), E), expr(Temp, 4, Assign, SSlotTemp, 6, T, E)}));
    Thread t = fx.run(0x1000);
    check(t.status == Thread::Status::Finished && t.temps[0] == plate.id && t.temps[1] == 0 &&
              t.temps[2] == plate.id && t.temps[3] == c.id && t.temps[4] == c.id,
          "slot data owners");
    fx.code.add(kObjGroup, 0x1001, tree({expr(MySlot, 0, Assign, Lit, 2, T, E)}));
    t = fx.run(0x1001);
    check(t.status == Thread::Status::Failed && t.error.find("read-only") != std::string::npos, "slots are read-only");
    fx.code.add(kObjGroup, 0x1002, tree({expr(Temp, 0, Assign, MySlot, -1, T, E)}));
    t = fx.run(0x1002);
    check(t.status == Thread::Status::Failed, "negative slot");

    // A lot's rooms know only some of their values.
    fx.vm.rooms[4].values = {0, 0, 2, 12, 0, 0};
    fx.vm.rooms[4].known = 0x0C;
    fx.code.add(kObjGroup, 0x1003, tree({expr(Temp, 0, Assign, Lit, 4, go(1), E), expr(Temp, 1, Assign, 0x17, 3, go(2), E),
                                         expr(Temp, 2, Assign, 0x17, 1, T, E)}));
    t = fx.run(0x1003);
    check(t.status == Thread::Status::Failed && t.temps[1] == 12 && t.error.find("outside not known") != std::string::npos,
          "unknown room values fail");
}

// Scheduler: Init runs to completion (its sleeps do not wait for ticks), Main
// gets a turn per tick and starts again when it returns; objects created while
// running are initialized; the clock keeps the time globals.
static void testScheduler() {
    Fixture fx;
    constexpr uint32_t kLamp = 0x1A2B3C4D;
    fx.code.add(kObjGroup, 0x80, definition(0x0D0D0D0D));
    fx.code.add(kObjGroup, 0x81, definition(kLamp));
    ObjectFunctions f;
    f.functions = {{0, 0x1000}, {0, 0x1001}};
    fx.code.add(kObjGroup, 0x80, f);
    f.functions = {{0, 0x1003}};
    fx.code.add(kObjGroup, 0x81, f);
    auto sleepOn = [](uint16_t param, Target t) { return instr(0x00, t, E, {uint8_t(param), uint8_t(param >> 8)}); };
    // Init: attr 0 := 5, sleep 3 ticks, attr 2 := 1.
    fx.code.add(kObjGroup, 0x1000,
                tree({expr(Attr, 0, Assign, Lit, 5, go(1), E), expr(Param, 0, Assign, Lit, 3, go(2), E),
                      sleepOn(0, go(3)), expr(Attr, 2, Assign, Lit, 1, T, E)},
                     1));
    // Main: attr 1 += 1, sleep 1 tick, return.
    fx.code.add(kObjGroup, 0x1001,
                tree({expr(Attr, 1, Add, Lit, 1, go(1), E), expr(Param, 0, Assign, Lit, 1, go(2), E), sleepOn(0, T)},
                     1));
    // A Main that makes a lamp on its first tick, then idles.
    fx.code.add(kObjGroup, 0x1002,
                tree({instr(0x2A, go(1), E, {lo(kLamp, 0), lo(kLamp, 1), lo(kLamp, 2), lo(kLamp, 3), 6}),
                      expr(Param, 0, Assign, Lit, 1000, go(2), E), sleepOn(0, T)},
                     1));
    fx.code.add(kObjGroup, 0x1003, tree({expr(Attr, 0, Assign, Lit, 9, T, E)})); // the lamp's Init

    Object& o = fx.vm.instantiate(kObjGroup, 0x80);
    o.attributes.resize(3);
    auto init = fx.vm.runFunction(o.id, 0);
    check(init && init->status == Thread::Status::Finished && o.attributes[0] == 5 && o.attributes[2] == 1,
          "Init runs to completion");
    check(!fx.vm.runFunction(o.id, 7), "no such function");
    check(fx.vm.startMain(o.id), "Main starts");
    for (int i = 0; i < 6; ++i)
        fx.vm.tick();
    check(o.attributes[1] == 3 && fx.vm.threads.at(o.id).status != Thread::Status::Failed,
          "Main runs a turn per tick and starts again when it returns");

    fx.vm.initCreatedObjects = true;
    Object& maker = fx.vm.createObject(kObjGroup);
    fx.vm.threads.insert_or_assign(maker.id, fx.vm.start(maker.id, 0x1002));
    fx.vm.tick();
    const Object* lamp = nullptr;
    for (const auto& [id, x] : fx.vm.objects())
        if (x.guid() == kLamp)
            lamp = &x;
    check(lamp && lamp->attributes.size() > 0 && lamp->attributes[0] == 9 && fx.vm.createdInits.size() == 1,
          "a created object runs its Init");

    fx.vm.clock = {};
    fx.vm.clock.ticksPerMinute = 2;
    fx.vm.clock.hour = 23, fx.vm.clock.minute = 59, fx.vm.clock.day = 30, fx.vm.clock.month = 12;
    fx.vm.tick(), fx.vm.tick();
    check(fx.vm.globals[global::Hour] == 0 && fx.vm.globals[global::Minute] == 0 &&
              fx.vm.globals[global::DayOfMonth] == 1 && fx.vm.globals[global::Month] == 1 &&
              fx.vm.globals[global::Year] == 2,
          "the clock rolls over into the globals");
}

int main() {
    testLoopWithLocalsAndParams();
    testCalls();
    testScopes();
    testStackObjectAndAttributes();
    testObjectModel();
    testExpressionOperators();
    testYieldAndErrors();
    testPrimitives();
    testRunTreeByName();
    testRelationship();
    testArrays();
    testChangeMaterial();
    testManageInventory();
    testSetToNextSearches();
    testNeighbors();
    testGenericSimsCall();
    testDefinitionOwners();
    testRemoveAndRandom();
    testFamilyTree();
    testMultiTileAndIds();
    testAddPackage();
    testSavedObjectState();
    testSlots();
    testScheduler();
    if (failures)
        std::fprintf(stderr, "%d failure(s)\n", failures);
    else
        std::printf("all vm tests passed\n");
    return failures ? 1 : 0;
}
