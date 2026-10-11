// s2pkg lot-state: the SimAntics state saved in lot packages.

#include "state_cmds.hpp"

#include "formats/dbpf.hpp"
#include "formats/lot_objects.hpp"

#include <cstdio>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace os2 {

namespace {

void printShorts(const char* label, const std::vector<int16_t>& v) {
    std::printf("    %s (%zu):", label, v.size());
    for (int16_t x : v)
        std::printf(" %d", x);
    std::printf("\n");
}

void printState(const LotObjects::State& s, bool hex) {
    std::printf("%s %d at (%.1f, %.1f, %d)%s%s\n", s.person ? "PERS" : "XOBJ", s.id, s.x, s.y, s.level,
                s.tailError.empty() ? "" : "  tail error: ", s.tailError.c_str());
    if (!s.token.guid && s.token.properties.empty() && s.arrays.empty() && s.stack.empty() && s.timers.empty() &&
        s.rest.empty())
        return;
    if (s.token.guid)
        std::printf("    token %08X flags %X, %zu properties\n", s.token.guid, s.token.flags, s.token.properties.size());
    for (size_t i = 0; i < s.arrays.size(); ++i)
        if (!s.arrays[i].empty())
            printShorts(("array " + std::to_string(i)).c_str(), s.arrays[i]);
    for (const auto& f : s.stack) {
        std::printf("    frame: object %u tree %04X node %u running %u on %u state %d savetype %u params", f.objectId,
                    f.treeId, f.node, f.runningObject, f.runningOn, f.primitiveState, f.behaviorSaveType);
        for (int16_t p : f.params)
            std::printf(" %d", p);
        std::printf(" locals");
        for (int16_t l : f.locals)
            std::printf(" %d", l);
        std::printf("\n");
    }
    for (const auto& t : s.timers) {
        std::printf("    timer: %08X %08X %04X %08X %d %d params", t.a, t.b, t.c, t.d, t.e, t.f);
        for (int16_t p : t.params)
            std::printf(" %d", p);
        std::printf("\n");
    }
    if (!s.rest.empty()) {
        std::printf("    rest: %zu bytes\n", s.rest.size());
        if (hex) {
            for (size_t i = 0; i < s.rest.size(); i += 2) {
                if (i % 32 == 0)
                    std::printf("      %04zX:", i);
                const int16_t v = int16_t(s.rest[i] | (i + 1 < s.rest.size() ? s.rest[i + 1] << 8 : 0));
                std::printf(" %6d", v);
                if (i % 32 == 30)
                    std::printf("\n");
            }
            std::printf("\n");
        }
    }
}

int cmdLotState(const std::vector<std::string>& args) {
    std::vector<std::filesystem::path> lots;
    int only = 0;
    bool hex = false;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--id" && i + 1 < args.size())
            only = std::stoi(args[++i]);
        else if (args[i] == "--rest")
            hex = true;
        else
            lots.emplace_back(args[i]);
    }
    std::map<std::string, size_t> errors;
    size_t states = 0, frames = 0, timers = 0, withTail = 0;
    for (const auto& path : lots) {
        DbpfPackage pkg(path);
        const LotObjects saved = LotObjects::load(pkg);
        if (lots.size() == 1 || only)
            std::printf("%s: object module version %X, %zu states\n", path.filename().string().c_str(), saved.version,
                        saved.objects.size());
        for (const auto& w : saved.warnings)
            std::printf("  warning: %s\n", w.c_str());
        for (const auto& s : saved.objects) {
            ++states;
            frames += s.stack.size();
            timers += s.timers.size();
            if (!s.tailError.empty())
                ++errors[s.tailError.substr(0, s.tailError.find(" at offset"))];
            else if (!s.person && !s.rest.empty())
                ++withTail;
            if ((lots.size() == 1 && !only) || s.id == only)
                printState(s, hex);
        }
    }
    std::printf("%zu states, %zu stack frames, %zu timers; %zu XOBJs with bytes after the timers\n", states, frames,
                timers, withTail);
    for (const auto& [e, n] : errors)
        std::printf("  %6zu %s\n", n, e.c_str());
    return 0;
}

} // namespace

int stateCommand(const std::string& cmd, int argc, char** argv) {
    if (cmd == "lot-state" && argc >= 3)
        return cmdLotState({argv + 2, argv + argc});
    return -1;
}

const char* stateUsage() { return "       s2pkg lot-state <lot.package>... [--id object-id] [--rest]\n"; }

} // namespace os2
