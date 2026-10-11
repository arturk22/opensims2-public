// Age (0x75) and RayTrace (0x78), from the Mac binary (BV 1.10, Ghidra
// headless, read only):
//
// - 0x75 Age (nTSSG::cPerson::TryAge): the new life stage is operand byte 0
//   (STR# labels: 0 adult, 1 child, 2 toddler, 3 teen, 4 elder, 0x0D baby,
//   0x0E young adult, Bon Voyage only); the binary writes person data 58
//   "Person Age" (0x13, 3, 2, 0x10, 0x33, 0 for a baby, 0x13 for a young
//   adult), 175 (1 only for a young adult), 119 and 120 (0), then rebuilds
//   the Sim's model, outfits and face for the new age (the renderer's job
//   here) and returns true. Another value does nothing (true). The binary
//   reads the stage from the operand only (no temp; inferred from the code).
// - 0x78 RayTrace (nTSSG::cObject::TryRayTrace): a ray through the scene
//   graph from the object (owner byte 1, data u16 2; its point: byte 4 =
//   1/2/3 a slot of that kind, byte 5 its index, out of range = error 0x1B;
//   else the middle of its bounding box) to, in mode byte 0 = 0, the object
//   (owner byte 8, data u16 9; point bytes 11/12); missing objects are
//   error 0x17. It writes temp 0 and returns: the target reached (temp 0 =
//   its ID) or nothing hit: true; another object first (temp 0 = it) or a
//   wall/floor (temp 0 = -1): false; byte 15 bit 0 lets it pass through
//   objects of type 9. Here there is no scene graph to trace: the ray is the
//   tile line between the two objects (World::lineOfSight, as Set to Next
//   24 uses), same level only; clear → temp 0 = target, true; blocked →
//   temp 0 = -1, false (inferred approximation). Mode 1 traces along the
//   object's way and is taken as clear (temp 0 = 0, true; inferred); other
//   modes are false, as in the binary.

#include "engine.hpp"
#include "errors.hpp"
#include "vm.hpp"

namespace os2::vm {

namespace {

Exit age(Context& c) {
    Object& me = c.me();
    if (!me.person)
        return Exit::True;
    int16_t stage = 0;
    bool youngAdult = false;
    switch (c.instruction.operands[0]) {
    case 0x00: stage = 0x13; break;
    case 0x01: stage = 0x03; break;
    case 0x02: stage = 0x02; break;
    case 0x03: stage = 0x10; break;
    case 0x04: stage = 0x33; break;
    case 0x0D: stage = 0x00; break;
    case 0x0E: stage = 0x13, youngAdult = true; break;
    default: return Exit::True;
    }
    auto& d = me.person->data;
    d[175] = youngAdult ? 1 : 0;
    d[119] = 0;
    d[120] = 0;
    d[persondata::PersonAge] = stage;
    return Exit::True;
}

Exit rayTrace(Context& c) {
    const auto& o = c.instruction.operands;
    const Object* from = c.vm.object(c.get(o[1], int16_t(o[2] | o[3] << 8)));
    if (!from)
        throw gameError(err::NoObject, "RayTrace: source object");
    if (o[0] == 1) {
        c.thread.temps[0] = 0;
        return Exit::True;
    }
    if (o[0] != 0)
        return Exit::False;
    const Object* to = c.vm.object(c.get(o[8], int16_t(o[9] | o[10] << 8)));
    if (!to)
        throw gameError(err::NoObject, "RayTrace: target object");
    bool clear = from->position.level == to->position.level;
    if (clear && c.vm.world && from->position.inWorld() && to->position.inWorld())
        clear = c.vm.world->lineOfSight(VM::tileOf(*from), VM::tileOf(*to));
    c.thread.temps[0] = clear ? to->id : int16_t(-1);
    return clear ? Exit::True : Exit::False;
}

} // namespace

void registerPersonPrimitives(VM& vm) {
    vm.setPrimitive(0x75, age);
    vm.setPrimitive(0x78, rayTrace);
}

} // namespace os2::vm
