// Turn Body Towards (0x16; cTSRoutingObjectAggregate::TryTurnTowards in
// the Mac binary): the type is u16 at 0, labelled by the global STR# 0xD8
// "Turn body types": 1 towards the camera, 2 towards the stack object, 3
// away from it, 4-11 towards north, north east, ... north west (directions
// 0-7), 12-14 left 45/90/135 degrees, 15-17 right 45/90/135, 18 180
// degrees, 19-21 towards the target/routing/containment slot, 22-24 away
// from them, 25 to the absolute direction in temp 0, 26 to the relative
// direction in temp 0. The body turns at once (object data "direction";
// the app turns the Sim smoothly). Inferred: "right" is +1 direction (the
// compass order of the labels), the slots are taken as the stack object
// (no slot positions here), and 0 does nothing.

#include "interactions.hpp"

namespace os2::vm {

namespace {

Exit turnBodyTowards(Context& c) {
    const int mode = c.instruction.operands[0] | (c.instruction.operands[1] << 8);
    Object& me = c.me();
    const int dir = ((me.data[objdata::Direction] % 8) + 8) % 8;
    auto face = [&](int d) {
        me.data[objdata::Direction] = int16_t(((d % 8) + 8) % 8);
        return Exit::True;
    };
    auto towards = [&](bool away) {
        const Object& target = c.stackObject();
        int d = directionOf(target.position.x - me.position.x, target.position.y - me.position.y);
        if (d < 0)
            return Exit::True; // on the same spot
        return face(away ? d + 4 : d);
    };
    switch (mode) {
    case 0:
    case 1: // the camera: nothing to turn to in the simulation
        return Exit::True;
    case 2: return towards(false);
    case 3: return towards(true);
    case 12: case 13: case 14: return face(dir - (mode - 11));
    case 15: case 16: case 17: return face(dir + (mode - 14));
    case 18: return face(dir + 4);
    case 19: case 20: case 21: return towards(false);
    case 22: case 23: case 24: return towards(true);
    case 25: return face(c.thread.temps[0]);
    case 26: return face(dir + c.thread.temps[0]);
    default:
        if (mode >= 4 && mode <= 11)
            return face(mode - 4);
        throw VMError("Turn Body Towards: mode " + std::to_string(mode) + " not implemented");
    }
}

} // namespace

void registerTurnPrimitive(VM& vm) { vm.setPrimitive(0x16, turnBodyTowards); }

} // namespace os2::vm
