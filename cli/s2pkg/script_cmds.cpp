// s2pkg lua / lua-script: the Lua 5.0 scripts the game keeps in
// TSData/Res/ObjectScripts (the `0x7E` Run Script runs them;
// docs/lua-scripts.md). `lua` disassembles a loose `.objLua`; `lua-script`
// finds one by name in the installation (the EPs pack them as resources of
// types 0x9012468B/0x9012468A) and disassembles it.
#include "script_cmds.hpp"

#include "data_index.hpp"
#include "formats/lua50.hpp"
#include "formats/resindex.hpp"
#include "vm/vm.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::vector<uint8_t> readFile(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in)
        throw std::runtime_error("cannot open " + file.string());
    return {(std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()};
}

int cmdLua(const std::filesystem::path& file) {
    const std::vector<uint8_t> bytes = readFile(file);
    const os2::LuaProto chunk = os2::parseLua50(bytes);
    std::printf("%s", os2::describeLua50(chunk, file.filename().string() + ": " + std::to_string(bytes.size()) +
                                                    " bytes, a Lua 5.0 chunk")
                            .c_str());
    return 0;
}

int cmdLuaScript(const std::filesystem::path& res, const std::string& name) {
    os2::ResourceIndex index;
    s2pkgAddInstallation(index, res);
    os2::vm::CodeLibrary code(&index);
    const std::vector<uint8_t>* chunk = code.luaScript(name);
    if (!chunk)
        throw std::runtime_error("no script \"" + name + "\" in " + res.string());
    std::printf("%s", os2::describeLua50(os2::parseLua50(*chunk),
                                         "script \"" + name + "\" (" + std::to_string(chunk->size()) + " bytes)")
                            .c_str());
    return 0;
}

} // namespace

int scriptCommand(const std::string& cmd, int argc, char** argv) {
    if (cmd == "lua") {
        if (argc < 3) {
            std::fprintf(stderr, "usage: s2pkg lua <file.objLua>\n");
            return 2;
        }
        return cmdLua(argv[2]);
    }
    if (cmd == "lua-script") {
        if (argc < 4) {
            std::fprintf(stderr, "usage: s2pkg lua-script <Res-dir> <name>\n");
            return 2;
        }
        return cmdLuaScript(argv[2], argv[3]);
    }
    return -1;
}
