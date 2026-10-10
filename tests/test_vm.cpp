#include "vm/vm.hpp"

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
    // Callee sets the stack object; it stays set in the caller.
    fx.code.add(kObjGroup, 0x1001, tree({expr(StackId, 0, Assign, Lit, other.id, T, E)}));
    fx.code.add(kObjGroup, 0x1000,
                tree({instr(0x1001, go(1), E), expr(Attr, 2, Assign, SAttr, 3, go(2), E),
                      expr(Global, 7, Assign, StackId, 0, T, E)}));
    Thread t = fx.run(0x1000);
    check(t.status == Thread::Status::Finished && fx.obj().attributes[2] == 42 && fx.vm.globals[7] == other.id,
          "stack object set by a subroutine");
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

    fx.code.add(kObjGroup, 0x1001, tree({instr(0x0B, T, F)}));
    t = fx.run(0x1001);
    check(t.status == Thread::Status::Failed && fx.vm.missingPrimitives[0x0B] == 1, "missing primitive fails");

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

    fx.code.add(kObjGroup, 0x1006, tree({expr(0x2B, 0, Assign, Lit, 1, T, E)}));
    t = fx.run(0x1006);
    check(t.status == Thread::Status::Failed && fx.vm.missingOwners[0x2B] == 1, "missing data owner fails");
}

int main() {
    testLoopWithLocalsAndParams();
    testCalls();
    testScopes();
    testStackObjectAndAttributes();
    testObjectModel();
    testExpressionOperators();
    testYieldAndErrors();
    if (failures)
        std::fprintf(stderr, "%d failure(s)\n", failures);
    else
        std::printf("all vm tests passed\n");
    return failures ? 1 : 0;
}
