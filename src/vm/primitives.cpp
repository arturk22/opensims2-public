#include "vm.hpp"

#include "formats/binreader.hpp"

#include <cstdlib>

namespace os2::vm {

namespace {

// Expression (0x02): operands lhs data u16, rhs data u16, flags u8,
// operator u8, lhs owner u8, rhs owner u8. Operators 0-17 are The Sims 1's;
// 18-20 are inferred from how the game's trees use them (|= to accumulate
// flags, ^= 1 to toggle a hand, := abs() on differences).
Exit expression(Context& c) {
    const auto& o = c.instruction.operands;
    const auto lhsData = int16_t(le16(&o[0]));
    const auto rhsData = int16_t(le16(&o[2]));
    const uint8_t op = o[5], lhsOwner = o[6], rhsOwner = o[7];

    const int32_t rhs = c.get(rhsOwner, rhsData);
    auto lhs = [&] { return int32_t(c.get(lhsOwner, lhsData)); };
    auto assign = [&](int32_t v) {
        c.set(lhsOwner, lhsData, int16_t(v));
        return Exit::True;
    };
    auto test = [](bool b) { return b ? Exit::True : Exit::False; };
    auto bit = [&] {
        if (rhs < 1 || rhs > 16)
            throw VMError("Expression: flag " + std::to_string(rhs) + " out of range");
        return int32_t(1) << (rhs - 1); // flags are numbered from 1
    };

    switch (op) {
    case 0: return test(lhs() > rhs);
    case 1: return test(lhs() < rhs);
    case 2: return test(lhs() == rhs);
    case 3: return assign(lhs() + rhs);
    case 4: return assign(lhs() - rhs);
    case 5: return assign(rhs);
    case 6: return assign(lhs() * rhs);
    case 7:
        if (rhs == 0)
            throw VMError("Expression: division by zero");
        return assign(lhs() / rhs);
    case 8: return test(lhs() & bit());
    case 9: return assign(lhs() | bit());
    case 10: return assign(lhs() & ~bit());
    case 11: {
        const int16_t v = int16_t(lhs() + 1);
        assign(v);
        return test(v < rhs);
    }
    case 12:
        if (rhs == 0)
            throw VMError("Expression: modulo by zero");
        return assign(lhs() % rhs);
    case 13: return assign(lhs() & rhs);
    case 14: return test(lhs() >= rhs);
    case 15: return test(lhs() <= rhs);
    case 16: return test(lhs() != rhs);
    case 17: {
        const int16_t v = int16_t(lhs() - 1);
        assign(v);
        return test(v > rhs);
    }
    case 18: return assign(lhs() | rhs);
    case 19: return assign(lhs() ^ rhs);
    case 20: return assign(std::abs(rhs));
    default: throw VMError("Expression: operator " + std::to_string(op) + " not implemented");
    }
}

// Operand layouts below follow The Sims 1's primitives where the game's own
// trees agree with them; enumerations are named after the editor's labels
// (STR# 0x7FE59FD0/0xNN, noted where used).

constexpr uint8_t kOwnerParam = 0x09;
constexpr uint8_t kOwnerStackObjectId = 0x0A;

// Sleep (0x00) and Idle for Input (0x11): u16 index of the parameter holding
// the ticks left, counted down once per tick (one call to VM::run). Idle for
// Input can also be cut short by a pushed interaction (u16 at 4 seems to allow
// that); there are no interaction queues yet, so it only times out.
Exit countDownParam(Context& c) {
    const auto param = int16_t(le16(&c.instruction.operands[0]));
    const int16_t ticks = c.get(kOwnerParam, param);
    if (ticks <= 0)
        return Exit::True;
    c.set(kOwnerParam, param, int16_t(ticks - 1));
    return Exit::Yield;
}

// Random Number (0x08): destination (data u16, owner u16), range (data u16,
// owner u16); stores a value in [0, range). A range <= 0 stores 0 (inferred).
// Bytes 8+ are 1 in most uses and look like editor leftovers elsewhere.
Exit randomNumber(Context& c) {
    const auto& o = c.instruction.operands;
    const int16_t range = c.get(o[6], int16_t(le16(&o[4])));
    int16_t value = 0;
    if (range > 0)
        value = int16_t(std::uniform_int_distribution<int>(0, range - 1)(c.vm.random));
    c.set(o[2], int16_t(le16(&o[0])), value);
    return Exit::True;
}

// Break Point (0x0F): stops in the editor's debugger when (data u16, owner
// u16) is non-zero (all 0xFF in about half the uses); the game goes on through
// the true branch. Some trees point that branch back at the Break Point (472
// of 2,637) to halt the thread for good, which the game only ends by
// resetting the object; here that fails the thread.
Exit breakPoint(Context& c) {
    const auto& t = c.instruction.onTrue;
    if (t.kind == Behavior::TargetKind::Instruction && t.index == c.frame.pc)
        throw VMError("Break Point: halts here (its true branch loops to itself)");
    return Exit::True;
}

// Remove Object Instance (0x12): u16 target, 0 = me, 1 = stack object (STR#
// 0x9C), then flags (u8 at 2: 0-3, probably "clean up all parts" of a
// multi-tile object and "return immediately"; not used yet). A thread whose
// object is removed is killed.
Exit removeObjectInstance(Context& c) {
    const uint16_t target = le16(&c.instruction.operands[0]);
    if (target > 1)
        throw VMError("Remove Object Instance: target " + std::to_string(target) + " not implemented");
    c.vm.removeObject(target == 0 ? c.me().id : c.stackObject().id);
    return Exit::True;
}

// Set to Next (0x1F): GUID u32, flags u8 (low 7 bits: search type, STR# 0xA4;
// 0x80: the target is (owner u8 at 5, data u16 at 7) instead of the stack
// object ID), local u8 at 6 (for the searches relative to an object in a
// local). Iterating searches move the target to the next matching object by
// ID and return false when there is none; the verify ones only test it.
Exit setToNext(Context& c) {
    const auto& o = c.instruction.operands;
    const uint32_t guid = le32(&o[0]);
    const uint8_t type = o[4] & 0x7F;
    const bool explicitTarget = o[4] & 0x80;
    const uint8_t owner = explicitTarget ? o[5] : kOwnerStackObjectId;
    const auto data = explicitTarget ? int16_t(le16(&o[7])) : int16_t(0);
    const ObjectId current = c.get(owner, data);

    std::function<bool(const Object&)> matches;
    switch (type) {
    case 0: matches = [](const Object&) { return true; }; break;               // object
    case 1: matches = [](const Object& x) { return x.person.has_value(); }; break;  // person
    case 2: matches = [](const Object& x) { return !x.person; }; break;            // non-person
    case 4: matches = [guid](const Object& x) { return x.guid() == guid; }; break; // object of type
    case 26: return c.vm.object(current) ? Exit::True : Exit::False;               // verify stack object
    case 30: {                                                                     // verify sim in stack object
        const Object* x = c.vm.object(current);
        return x && x->person ? Exit::True : Exit::False;
    }
    default: throw VMError("Set to Next: search type " + std::to_string(type) + " not implemented");
    }
    const auto& objects = c.vm.objects();
    for (auto it = objects.upper_bound(current); it != objects.end(); ++it)
        if (matches(it->second)) {
            c.set(owner, data, it->first);
            return Exit::True;
        }
    return Exit::False;
}

// Test Object Type (0x20): GUID u32, object ID (data u16, owner u8); true if
// that object has the GUID, error if it does not exist. Byte 7 (0-4) changes
// the test: 1 is used with one GUID (0x2C9BDEB4) on Sims and 4 with memory and
// want GUIDs; neither is understood yet.
Exit testObjectType(Context& c) {
    const auto& o = c.instruction.operands;
    if (o[7] != 0)
        throw VMError("Test Object Type: mode " + std::to_string(o[7]) + " not implemented");
    const ObjectId id = c.get(o[6], int16_t(le16(&o[4])));
    const Object* x = c.vm.object(id);
    if (!x)
        throw VMError("Test Object Type: object " + std::to_string(id) + " does not exist");
    return x->guid() == le32(&o[0]) ? Exit::True : Exit::False;
}

// Create New Object Instance (0x2A): GUID u32, where u8 (STR# 0xA7), flags u8
// (STR# 0xA8 lists 0-2, but the trees also use bits 0x04/0x10/0x80; ignored).
// Only "out of world" (6, 72% of the uses) is implemented: the new object's ID
// goes to the stack object. The object's init tree does not run yet.
Exit createObjectInstance(Context& c) {
    const auto& o = c.instruction.operands;
    const uint32_t guid = le32(&o[0]);
    if (o[4] != 6)
        throw VMError("Create New Object Instance: placement " + std::to_string(o[4]) + " not implemented");
    Object* x = c.vm.instantiateGuid(guid);
    if (!x)
        throw VMError("Create New Object Instance: no OBJD with GUID " + hexString(guid));
    c.frame.stackObject = x->id;
    return Exit::True;
}

} // namespace

void registerPrimitives(VM& vm) {
    vm.setPrimitive(0x00, countDownParam);
    vm.setPrimitive(0x02, expression);
    vm.setPrimitive(0x08, randomNumber);
    vm.setPrimitive(0x0F, breakPoint);
    vm.setPrimitive(0x11, countDownParam);
    vm.setPrimitive(0x12, removeObjectInstance);
    vm.setPrimitive(0x1F, setToNext);
    vm.setPrimitive(0x20, testObjectType);
    vm.setPrimitive(0x2A, createObjectInstance);
}

} // namespace os2::vm
