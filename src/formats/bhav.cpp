#include "bhav.hpp"

#include "binreader.hpp"

#include <cstdio>

namespace os2 {

namespace {

Behavior::Target target8(uint8_t v) {
    switch (v) {
    case 0xFD: return {Behavior::TargetKind::Error, 0};
    case 0xFE: return {Behavior::TargetKind::True, 0};
    case 0xFF: return {Behavior::TargetKind::False, 0};
    default: return {Behavior::TargetKind::Instruction, v};
    }
}

Behavior::Target target16(uint16_t v) {
    switch (v) {
    case 0xFFFC: return {Behavior::TargetKind::Error, 0};
    case 0xFFFD: return {Behavior::TargetKind::True, 0};
    case 0xFFFE: return {Behavior::TargetKind::False, 0};
    default: return {Behavior::TargetKind::Instruction, v};
    }
}

std::string targetString(const Behavior::Target& t) {
    switch (t.kind) {
    case Behavior::TargetKind::True: return "true";
    case Behavior::TargetKind::False: return "false";
    case Behavior::TargetKind::Error: return "error";
    case Behavior::TargetKind::Instruction: return std::to_string(t.index);
    }
    return "?";
}

} // namespace

std::string primitiveName(uint16_t op) {
    // The primitives The Sims 2 inherited from The Sims 1.
    static const char* const names[] = {
        "Sleep",                    "Generic Sims Call",       "Expression",           "Find Best Interaction",
        "Grab",                     "Drop",                    "Change Suit/Accessory", "Update",
        "Random Number",            "Burn",                    "Tutorial",             "Get Distance To",
        "Get Direction To",         "Push Interaction",        "Find Best Object For Function", "Break Point",
        "Find Location For",        "Idle For Input",          "Remove Object Instance", "Make New Character",
        "Run Functional Tree",      "Show String",             "Look Towards",         "Play Sound Event",
        "Old Relationship",         "Alter Budget",            "Relationship",         "Go To Relative Position",
        "Run Tree By Name",         "Set Motive Change",       "Gosub Found Action",   "Set To Next",
        "Test Object Type",         "Find 5 Worst Motives",    "UI Effect",            "Special Effect",
        "Dialog",                   "Test Sim Interacting With",
    };
    if (op < std::size(names))
        return names[op];
    char buf[16];
    std::snprintf(buf, sizeof buf, "prim_0x%02X", op);
    return buf;
}

Behavior Behavior::parse(std::span<const uint8_t> data) {
    BinReader r(data);
    Behavior b;
    b.fileName = r.fixed(64);
    b.signature = r.u16();
    if (b.signature < 0x8000 || b.signature > 0x8009)
        throw FormatError("BHAV: unknown signature " + hexString(b.signature));
    const uint16_t count = r.u16();
    b.treeType = r.u8();
    b.argCount = r.u8();
    b.localCount = r.u8();
    b.flags = r.u8();
    b.treeVersion = r.u32();

    const bool wideTargets = b.signature >= 0x8007;
    const bool hasNodeVersion = b.signature >= 0x8005;
    const size_t operandSize = b.signature >= 0x8003 ? 16 : 8;
    b.instructions.resize(count);
    for (auto& in : b.instructions) {
        in.opcode = r.u16();
        if (wideTargets) {
            in.onTrue = target16(r.u16());
            in.onFalse = target16(r.u16());
        } else {
            in.onTrue = target8(r.u8());
            in.onFalse = target8(r.u8());
        }
        if (hasNodeVersion)
            in.nodeVersion = r.u8();
        auto ops = r.bytes(operandSize);
        std::copy(ops.begin(), ops.end(), in.operands.begin());
    }
    r.expectEndOrFill("BHAV");

    for (const auto& in : b.instructions)
        for (const auto& t : {in.onTrue, in.onFalse})
            if (t.kind == TargetKind::Instruction && t.index >= count)
                throw FormatError("BHAV: branch to instruction " + std::to_string(t.index) + " of " +
                                  std::to_string(count));
    return b;
}

std::string Behavior::disassemble() const {
    std::string out;
    char buf[256];
    for (size_t i = 0; i < instructions.size(); ++i) {
        const auto& in = instructions[i];
        std::string op;
        if (in.opcode < 0x100)
            op = primitiveName(in.opcode);
        else {
            const char* scope = in.opcode < 0x1000 ? "global" : in.opcode < 0x2000 ? "private" : "semiglobal";
            std::snprintf(buf, sizeof buf, "call %s 0x%04X", scope, in.opcode);
            op = buf;
        }
        std::string ops;
        for (auto o : in.operands) {
            std::snprintf(buf, sizeof buf, "%02X", o);
            ops += buf;
        }
        std::snprintf(buf, sizeof buf, "%3zu: %-34s [%s] T->%s F->%s\n", i, op.c_str(), ops.c_str(),
                      targetString(in.onTrue).c_str(), targetString(in.onFalse).c_str());
        out += buf;
    }
    return out;
}

} // namespace os2
