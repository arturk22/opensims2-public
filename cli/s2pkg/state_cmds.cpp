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
    std::printf("%s %d at (%.2f, %.2f, %d) elevation %g dir %d, room %d, data 80/81 %d %d%s%s\n",
                s.kind == LotObjects::Kind::Person      ? "PERS"
                : s.kind == LotObjects::Kind::MultiTile ? "XMTO"
                                                        : "XOBJ",
                s.id, s.x, s.y, s.level, double(s.elevation), s.data.size() > 1 ? s.data[1] : 0, s.data.size() > 29 ? s.data[29] : 0,
                s.data.size() > 81 ? s.data[80] : 0,
                s.data.size() > 81 ? s.data[81] : 0, s.tailError.empty() ? "" : "  tail error: ", s.tailError.c_str());
    if (s.kind == LotObjects::Kind::MultiTile)
        std::printf("    multi-tile: lead %d, %u %u %u / %u %u %u; object data 19 = %d\n", s.leadObject, s.multiTile[0],
                    s.multiTile[1], s.multiTile[2], s.multiTile[3], s.multiTile[4], s.multiTile[5],
                    s.data.size() > 19 ? s.data[19] : 0);
    if (!s.token.guid && s.token.properties.empty() && s.arrays.empty() && s.stack.empty() && s.timers.empty() &&
        s.rest.empty())
        return;
    if (!s.slots.empty()) {
        std::printf("    slots:");
        for (int16_t x : s.slots)
            std::printf(" %d", x);
        std::printf("\n");
    }
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
    for (const auto& e : s.effects) {
        std::printf("    effect: %s on \"%s\" data", e.name.c_str(), e.node.c_str());
        for (uint8_t b : e.data)
            std::printf(" %02X", b);
        std::printf("\n");
    }
    for (const auto& t : s.timers) {
        std::printf("    timer: %08X %08X %04X %08X %d %d params", t.a, t.b, t.c, t.d, t.e, t.f);
        for (int16_t p : t.params)
            std::printf(" %d", p);
        std::printf("\n");
    }
    if (s.personRead) {
        std::printf("    person v%u motives", s.personVersion);
        for (size_t i = 3; i < s.motives.size(); ++i)
            std::printf(" %.1f", s.motives[i]);
        std::printf(" / second set");
        for (size_t i = 3; i < s.motives2.size(); ++i)
            std::printf(" %.1f", s.motives2[i]);
        std::printf("\n");
        printShorts("person data", s.personData);
        auto show = [](const char* what, const LotObjects::SavedInteraction& q) {
            std::printf("    %s: \"%s\" object %d target %d by %d, entry %u, priority %u, params %d %d %d %d (uid %u; %u %d %d %g %u %d %d %d %u %u %u %u)\n",
                        what, q.name.c_str(), q.stackObject, q.target, q.initiator, q.ttabEntry, q.priority, q.params[0],
                        q.params[1], q.params[2], q.params[3], q.uid, q.u08, q.u0C, q.u24, q.u28, q.u54, q.u30, q.u2C,
                        q.u2E, q.u40, q.u42, q.u38, q.u3C);
        };
        if (!s.slotA.empty())
            show("slot A", s.slotA);
        if (!s.slotB.empty())
            show("slot B", s.slotB);
        for (const auto& q : s.queue)
            show("queued", q);
        if (s.pending)
            show("pending", *s.pending);
        for (const auto& q : s.list5AC)
            show("list 5AC", q);
        std::printf("    %zu object records, %zu motive incs, name \"%s\", u644 %u u646 %u\n", s.objectRecords.size(),
                    s.motiveIncs.size(), s.personName.c_str(), s.personU644, s.personU646);
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
    std::map<uint32_t, size_t> orphans;
    std::map<LotObjects::Kind, size_t> kinds;
    size_t states = 0, frames = 0, timers = 0, withTail = 0, pairs = 0, unread = 0, persons = 0, queued = 0;
    for (const auto& path : lots) {
        DbpfPackage pkg(path);
        const LotObjects saved = LotObjects::load(pkg);
        if (lots.size() == 1 || only)
            std::printf("%s: object module version %X, %zu states\n", path.filename().string().c_str(), saved.version,
                        saved.objects.size());
        if ((lots.size() == 1 || only) && !saved.globals.empty()) {
            const auto& g = saved.globals;
            std::printf("  saved at %02d:%02d, day %d month %d year %d; selected %d, family %d, house %d\n", g[0], g[5],
                        g[1], g[7], g[8], g[3], g[9], g[10]);
        }
        for (const auto& w : saved.warnings)
            std::printf("  warning: %s\n", w.c_str());
        pairs += saved.pairs.size();
        for (const auto& [t, n] : saved.orphans)
            orphans[t] += n;
        for (const auto& s : saved.objects) {
            ++states;
            ++kinds[s.kind];
            unread += !s.tailError.empty();
            persons += s.personRead;
            queued += s.queue.size();
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
    std::printf("%zu objects in the object modules, %zu states read (%zu XOBJ, %zu XMTO, %zu PERS), %zu read through "
                "(%.2f%%); %zu persons, %zu queued interactions; %zu stack frames, %zu timers\n",
                pairs, states, kinds[LotObjects::Kind::Object], kinds[LotObjects::Kind::MultiTile],
                kinds[LotObjects::Kind::Person], states - unread, pairs ? 100.0 * double(states - unread) / double(pairs) : 0.0,
                persons, queued, frames, timers);
    std::printf("orphan states (no ObjM pair, not read):");
    for (const auto& [t, n] : orphans)
        std::printf(" %c%c%c%c %zu", char(t >> 24), char(t >> 16), char(t >> 8), char(t), n);
    std::printf("\n");
    (void)withTail;
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
