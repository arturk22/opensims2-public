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

int cmdLua(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in)
        throw std::runtime_error("cannot open " + file.string());
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const os2::LuaProto chunk = os2::parseLua50(bytes);
    std::printf("%s", os2::describeLua50(chunk, file.filename().string() + ": " + std::to_string(bytes.size()) +
                                                    " bytes, a Lua 5.0 chunk")
                            .c_str());
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
