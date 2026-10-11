// Get Distance To (0x0B) and Get Direction To (0x0C): The Sims 1's layouts
// (FreeSO's VMGetDistanceTo/VMGetDirectionTo), which the game's trees fit.
#include "vm.hpp"

#include "formats/binreader.hpp"

#include <cmath>
#include <cstdlib>

namespace os2::vm {

namespace {

// The object an operand names at `at` (flag 0x01 at `flagAt`: owner u8 at
// `at`, data u16 after it), else me.
const Object& operandObject(Context& c, size_t flagAt, size_t at) {
    const auto& o = c.instruction.operands;
    if (!(o[flagAt] & 0x01))
        return c.me();
    const Object* x = c.vm.object(c.get(o[at], int16_t(le16(&o[at + 1]))));
    if (!x)
        throw VMError("Invalid object id.");
    return *x;
}

// Get Distance To (0x0B): temp u16 at 0 := the distance from an object
// (flags u8 at 2, 0x01: owner u8 at 3, data u16 at 4; else me) to the stack
// object, in whole tiles, plus 20 per level between them (FreeSO's). With
// flag 0x02 in byte 6 it is in hundredths of a tile (inferred: the editor
// writes " in 1/100ths of a tile" after the rest, and byte 6 is the only
// other byte the trees set, to 0 or 2).
Exit getDistanceTo(Context& c) {
    const auto& o = c.instruction.operands;
    const Object& from = operandObject(c, 2, 3);
    const Object& to = c.stackObject();
    const double d = std::hypot(double(to.position.x - from.position.x), double(to.position.y - from.position.y));
    const int levels = std::abs(to.position.level - from.position.level);
    const double value = (o[6] & 0x02) ? std::floor(d * 100) : std::floor(d);
    const uint16_t temp = le16(&o[0]);
    if (temp >= kTempCount)
        throw VMError("Get Distance To: temp " + std::to_string(temp) + " out of range");
    c.thread.temps[temp] = int16_t(std::min(32767.0, value + 20.0 * levels));
    return Exit::True;
}

// Get Direction To (0x0C): (data u16 at 0, owner u16 at 2) := the direction
// (0-7, as object data "direction") from an object (flags u8 at 4, 0x01:
// owner u8 at 5, data u16 at 6; else me) to the stack object; 0 when they
// are on the same point (FreeSO's).
Exit getDirectionTo(Context& c) {
    const auto& o = c.instruction.operands;
    const Object& from = operandObject(c, 4, 5);
    const Object& to = c.stackObject();
    const int d = directionOf(to.position.x - from.position.x, to.position.y - from.position.y);
    c.set(uint8_t(le16(&o[2])), int16_t(le16(&o[0])), int16_t(d < 0 ? 0 : d));
    return Exit::True;
}

} // namespace

void registerMeasurePrimitives(VM& vm) {
    vm.setPrimitive(0x0B, getDistanceTo);
    vm.setPrimitive(0x0C, getDirectionTo);
}

} // namespace os2::vm
