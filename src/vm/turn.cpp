// Turn Body Towards (0x16): The Sims 1's "look towards" (FreeSO's
// VMLookTowards): mode u8 at 0 (STR# of the editor not found; the TS1
// modes): 0 head towards the stack object, 1 body towards the camera, 2
// body towards the stack object (261 of the game's 327 uses), 3 away from
// it, 4/5 towards/away from the middle of its multi-tile object. The body
// turns at once (object data "direction", rounded to 45 degrees; the app
// turns the Sim smoothly); FreeSO turns through the router. Modes 6+ (up to
// 0x14; "Turn toward absolute direction" uses 0x0C-0x0E) are unknown.

#include "interactions.hpp"

namespace os2::vm {

namespace {

Exit turnBodyTowards(Context& c) {
    const uint8_t mode = c.instruction.operands[0];
    Object& me = c.me();
    switch (mode) {
    case 0: // the head (look-at), not done: the body stays as it is
    case 1: // the camera: nothing to turn to in the simulation
        return Exit::True;
    case 2:
    case 3:
    case 4:
    case 5: {
        const Object& target = c.stackObject();
        float tx = target.position.x, ty = target.position.y;
        if (mode >= 4) {
            const ObjectId lead = multiTileGroup(target);
            float sx = 0, sy = 0;
            int n = 0;
            for (const auto& [id, o] : c.vm.objects())
                if (multiTileGroup(o) == lead && o.position.inWorld())
                    sx += o.position.x, sy += o.position.y, ++n;
            if (n)
                tx = sx / float(n), ty = sy / float(n);
        }
        int d = directionOf(tx - me.position.x, ty - me.position.y);
        if (d < 0)
            return Exit::True; // on the same spot
        if (mode == 3 || mode == 5)
            d = (d + 4) % 8;
        me.data[objdata::Direction] = int16_t(d);
        return Exit::True;
    }
    default: throw VMError("Turn Body Towards: mode " + std::to_string(mode) + " not implemented");
    }
}

} // namespace

void registerTurnPrimitive(VM& vm) { vm.setPrimitive(0x16, turnBodyTowards); }

} // namespace os2::vm
