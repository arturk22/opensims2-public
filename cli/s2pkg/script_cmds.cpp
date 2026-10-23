// s2pkg lua: the Lua 5.0 scripts the game keeps in TSData/Res/ObjectScripts
// (the `0x7E` Run Script runs them; docs/lua-scripts.md). Prints the chunk's
// functions, their constants and the disassembly of every instruction, with
// the source line, so the scripts' logic can be read without a Lua VM.
#include "script_cmds.hpp"

#include "formats/lua50.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

std::string text(const os2::LuaConstant& c) {
    switch (c.kind) {
    case os2::LuaConstant::Nil: return "nil";
    case os2::LuaConstant::Boolean: return c.boolean ? "true" : "false";
    case os2::LuaConstant::Number: {
        char buf[40];
        std::snprintf(buf, sizeof buf, "%.14g", c.number);
        return buf;
    }
    case os2::LuaConstant::String: return "\"" + c.string + "\"";
    }
    return "?";
}

void printProto(const os2::LuaProto& f, int depth) {
    const std::string pad(size_t(depth) * 2, ' ');
    std::printf("%sfunction %s (defined at line %d): %d params, %d upvalues, %d stack, %zu instructions\n",
                pad.c_str(), f.source.empty() ? "?" : f.source.c_str(), f.lineDefined, f.numparams, f.nups, f.maxStack,
                f.code.size());
    if (!f.constants.empty()) {
        std::printf("%s  constants:", pad.c_str());
        for (const auto& c : f.constants)
            std::printf(" %s", text(c).c_str());
        std::printf("\n");
    }
    for (size_t i = 0; i < f.code.size(); ++i) {
        const int line = i < f.lines.size() ? f.lines[i] : 0;
        std::printf("%s  %3zu  %-30s ; line %d\n", pad.c_str(), i, os2::disassembleLua50(f.code[i], f.constants).c_str(), line);
    }
    for (const auto& p : f.protos)
        printProto(p, depth + 1);
}

int cmdLua(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in)
        throw std::runtime_error("cannot open " + file.string());
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const os2::LuaProto main = os2::parseLua50(bytes);
    std::printf("%s: %zu bytes, a Lua 5.0 chunk\n", file.filename().string().c_str(), bytes.size());
    printProto(main, 0);
    return 0;
}

} // namespace

int scriptCommand(const std::string& cmd, int argc, char** argv) {
    if (cmd != "lua")
        return -1;
    if (argc < 3) {
        std::fprintf(stderr, "usage: s2pkg lua <file.objLua>\n");
        return 2;
    }
    return cmdLua(argv[2]);
}
