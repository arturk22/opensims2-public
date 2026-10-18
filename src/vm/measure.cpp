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
// object. cTSObject::TryDistanceTo (BV 0xe25a28) takes it from
// CalcShortDistance (vtable 0x1f8, BV 0xaf581c) with 10.0 (BV 0x277a900):
// sqrt(the distance in the plane^2 + (10 x the levels between them)^2),
// cut to whole tiles, or with flag 0x02 in byte 6 to hundredths of a tile
// (round n49, docs/routing-mac.md §9.5; was FreeSO's whole tiles + 20 a
// level, which put a Sim a floor up 0.2 tile further in hundredths: the
// social approach's "Goal Check - Continue?" took one 1.6 tiles off on the
// floor above as near enough and the route ended with 13 tick after tick).
Exit getDistanceTo(Context& c) {
    const auto& o = c.instruction.operands;
    const Object& from = operandObject(c, 2, 3);
    const Object& to = c.stackObject();
    const float dx = to.position.x - from.position.x, dy = to.position.y - from.position.y;
    const float dz = float(to.position.level - from.position.level) * 10.0f;
    const double d = double(std::sqrt(dz * dz + (dx * dx + dy * dy)));
    const double value = (o[6] & 0x02) ? std::trunc(d * 100) : std::trunc(d);
    const uint16_t temp = le16(&o[0]);
    if (temp >= kTempCount)
        throw VMError("Get Distance To: temp " + std::to_string(temp) + " out of range");
    c.thread.temps[temp] = int16_t(std::min(32767.0, value));
    return Exit::True;
}

// Get Direction To (0x0C): (data u16 at 0, owner u16 at 2) := the direction
// (0-7, as object data "direction") from an object (flags u8 at 4, 0x01:
// owner u8 at 5, data u16 at 6; else me) to the stack object; 0 when they
// are on the same point (FreeSO's). Flag 0x02 of byte 8: in degrees instead
// (Mac binary's disassembler; no tree uses it), 45 per direction step.
Exit getDirectionTo(Context& c) {
    const auto& o = c.instruction.operands;
    const Object& from = operandObject(c, 4, 5);
    const Object& to = c.stackObject();
    const float dx = to.position.x - from.position.x, dy = to.position.y - from.position.y;
    int16_t value = 0;
    if (o[8] & 0x02) {
        // Direction 0 faces -X, 2 +Y, 4 +X, 6 -Y (directionOf).
        if (dx != 0 || dy != 0) {
            const double deg = std::atan2(double(dy), double(-dx)) * 180.0 / 3.14159265358979323846;
            value = int16_t(std::lround(deg < 0 ? deg + 360 : deg) % 360);
        }
    } else {
        const int d = directionOf(dx, dy);
        value = int16_t(d < 0 ? 0 : d);
    }
    c.set(uint8_t(le16(&o[2])), int16_t(le16(&o[0])), value);
    return Exit::True;
}

} // namespace

void registerMeasurePrimitives(VM& vm) {
    vm.setPrimitive(0x0B, getDistanceTo);
    vm.setPrimitive(0x0C, getDirectionTo);
}

} // namespace os2::vm
