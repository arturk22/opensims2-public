// prim-usage: how the game's trees use one primitive (operand bytes), to work
// out operand layouts.
#include "prim_cmds.hpp"

#include "formats/bhav.hpp"
#include "formats/dbpf.hpp"
#include "formats/resindex.hpp"
#include "formats/strings.hpp"

#include <algorithm>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace os2 {

namespace {

constexpr uint32_t kTypeBhav = 0x42484156;

// Every use of `opcode` in the BHAVs under `res`: a histogram of each
// operand byte, then up to `examples` uses (group, instance, tree name,
// node, operands).
int cmdPrimUsage(const std::string& res, uint16_t opcode, size_t examples) {
    ResourceIndex index(res);
    std::array<std::map<uint8_t, size_t>, 16> bytes;
    size_t uses = 0, shown = 0;
    std::vector<std::string> lines;
    for (const auto& loc : index.ofType(kTypeBhav)) {
        Behavior b;
        try {
            b = Behavior::parse(index.read(loc));
        } catch (const std::exception&) {
            continue;
        }
        for (size_t i = 0; i < b.instructions.size(); ++i) {
            const auto& in = b.instructions[i];
            if (in.opcode != opcode)
                continue;
            ++uses;
            for (size_t k = 0; k < 16; ++k)
                ++bytes[k][in.operands[k]];
            if (shown++ < examples) {
                char buf[512];
                int n = std::snprintf(buf, sizeof buf, "%08X %04X #%-3zu v%u ", loc.entry->tgi.group,
                                      unsigned(loc.entry->tgi.instance), i, in.nodeVersion);
                for (uint8_t x : in.operands)
                    n += std::snprintf(buf + n, sizeof buf - size_t(n), "%02X ", x);
                std::snprintf(buf + n, sizeof buf - size_t(n), " %s", b.fileName.c_str());
                lines.emplace_back(buf);
            }
        }
    }
    std::printf("%s (0x%02X): %zu uses\n", primitiveName(opcode).c_str(), opcode, uses);
    for (size_t k = 0; k < 16; ++k) {
        std::vector<std::pair<size_t, uint8_t>> v;
        for (auto [x, n] : bytes[k])
            v.emplace_back(n, x);
        std::sort(v.rbegin(), v.rend());
        std::printf("  byte %2zu (%3zu values):", k, v.size());
        for (size_t j = 0; j < v.size() && j < 10; ++j)
            std::printf(" %02X x%zu", v[j].second, v[j].first);
        std::printf("\n");
    }
    for (const auto& l : lines)
        std::printf("  %s\n", l.c_str());
    return 0;
}

} // namespace

int primCommand(const std::string& cmd, int argc, char** argv) {
    if (cmd == "prim-usage" && (argc == 4 || argc == 5))
        return cmdPrimUsage(argv[2], uint16_t(std::stoul(argv[3], nullptr, 16)),
                            argc == 5 ? std::stoul(argv[4]) : 40);
    return -1;
}

const char* primUsage() { return "       s2pkg prim-usage <Res-dir> <opcode-hex> [examples]\n"; }

} // namespace os2
