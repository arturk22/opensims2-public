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

} // namespace

void registerPrimitives(VM& vm) { vm.setPrimitive(0x02, expression); }

} // namespace os2::vm
