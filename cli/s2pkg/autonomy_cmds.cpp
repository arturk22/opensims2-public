// s2pkg autonomy: runs a lot with its saved state for N ticks and prints
// what each Sim chooses on its own (Find Best Interaction, src/vm/autonomy)
// and how its motives change (src/vm/motives).

#include "autonomy_cmds.hpp"

#include "formats/bhav.hpp"
#include "formats/binreader.hpp"
#include "formats/dbpf.hpp"
#include "formats/lot.hpp"
#include "formats/lot_objects.hpp"
#include "formats/resindex.hpp"
#include "vm/autonomy.hpp"
#include "vm/interactions.hpp"
#include "vm/lot.hpp"
#include "vm/lot_resume.hpp"
#include "vm/motives.hpp"

#include <cstdio>
#include <filesystem>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>

namespace os2 {

namespace {

std::filesystem::path resRoot(const std::filesystem::path& pkg) {
    for (auto p = std::filesystem::absolute(pkg).parent_path(); p.has_relative_path(); p = p.parent_path())
        if (p.filename() == "Res")
            return p;
    throw std::runtime_error("the lot is not under a TSData/Res directory");
}

std::string simName(const vm::Object& o) {
    const std::string n = o.definition ? o.definition->fileName : "?";
    const auto dash = n.find(" - ");
    return (dash == std::string::npos ? n : n.substr(dash + 3)) + "#" + std::to_string(o.id);
}

std::string objectName(vm::VM& machine, vm::ObjectId id) {
    const vm::Object* o = machine.object(id);
    if (!o)
        return "#" + std::to_string(id);
    if (o->person)
        return simName(*o);
    return o->definition ? o->definition->fileName : "?";
}

std::string clockText(const vm::VM& m) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%02d:%02d", m.clock.hour, m.clock.minute);
    return buf;
}

void printMotivesHeader() {
    std::printf("    %-12s %6s %6s %6s %6s %6s %6s %6s %6s %6s\n", "", "mood", "energy", "comfrt", "hunger", "hygien",
                "bladdr", "envir", "social", "fun");
}

void printMotives(const vm::Object& s) {
    const auto& m = s.person->motives;
    std::printf("    %-12s %6d %6d %6d %6d %6d %6d %6d %6d %6d\n", simName(s).c_str(), m[3], m[5], m[6], m[7], m[8], m[9],
                m[13], m[14], m[15]);
}

int cmdAutonomy(const std::filesystem::path& lotFile, long ticks, long every, const std::string& simQuery,
                std::optional<int> motives, bool fromInit, bool verbose, bool noDecay, int traceId, size_t traceDepth) {
    ResourceIndex index(resRoot(lotFile));
    vm::CodeLibrary code(&index);
    DbpfPackage pkg(lotFile);
    const Lot lot = Lot::load(pkg);
    const LotObjects saved = LotObjects::load(pkg);
    vm::VM machine(code);
    machine.initCreatedObjects = true;
    machine.instructionsPerTurn = 20000;
    machine.setClockGlobals();
    const vm::LotLoad load = vm::loadLot(machine, lot, saved, vm::charactersDirOf(lotFile));
    vm::startSavedLot(machine, load, saved, fromInit);
    if (motives)
        vm::setAllMotives(machine, int16_t(*motives));
    vm::gMotiveDecay = !noDecay;

    std::vector<vm::ObjectId> sims;
    for (const auto& [id, o] : machine.objects())
        if (o.person && o.position.inWorld())
            if (simQuery.empty() || simName(o).find(simQuery) != std::string::npos)
                sims.push_back(id);
    std::printf("%s: %zu Sims in the world, %ld ticks from %s (%ld game minutes)\n",
                lotFile.filename().string().c_str(), sims.size(), ticks, clockText(machine).c_str(),
                ticks / machine.clock.ticksPerMinute);
    printMotivesHeader();
    for (vm::ObjectId id : sims)
        printMotives(*machine.object(id));

    std::map<vm::ObjectId, size_t> choices, searches;
    vm::autonomyObserver() = [&](const vm::AutonomyDecision& d) {
        if (std::find(sims.begin(), sims.end(), d.sim) == sims.end())
            return;
        ++searches[d.sim];
        if (!d.found && !verbose)
            return;
        std::printf("  tick %6llu %s %-10s Find Best Interaction (mask %04X): %zu advertise, %zu checked",
                    static_cast<unsigned long long>(machine.ticks), clockText(machine).c_str(),
                    objectName(machine, d.sim).c_str(), d.motiveMask, d.candidates, d.checked);
        if (!d.checkErrors.empty())
            std::printf(", %zu check trees failed", d.checkErrors.size());
        if (verbose) {
            std::map<std::string, int> why;
            for (const auto& e : d.checkErrors)
                ++why[e.substr(e.find(": ") + 2)];
            for (const auto& [w, n] : why)
                std::printf("\n      check tree failure x%d: %s", n, w.c_str());
        }
        if (!d.found) {
            std::printf(", nothing\n");
            return;
        }
        ++choices[d.sim];
        std::printf("\n");
        for (size_t i = 0; i < d.best.size() && i < (verbose ? 5u : 3u); ++i)
            std::printf("      %s %-34s on %-38s score %9.3e, %4.1f tiles\n", i == 0 ? "->" : "  ", d.best[i].name.c_str(),
                        objectName(machine, d.best[i].object).c_str(), d.best[i].score, d.best[i].distance);
    };

    // --trace ID: the Sim's instructions in its first frames, from its first
    // Find Best Interaction on (at most 3000 lines).
    bool tracing = false;
    size_t traced = 0;
    if (traceId)
        machine.trace = [&](const vm::Thread& t, const vm::Frame& f, const Behavior::Instruction& in) {
            if (t.me != traceId || t.stack.empty() || &t.stack.front() != &machine.threads[traceId].stack.front())
                return;
            if (in.opcode == 0x03)
                tracing = true;
            if (!tracing || t.stack.size() > traceDepth || traced++ >= 3000)
                return;
            const std::string op = in.opcode < 0x100 ? primitiveName(in.opcode) : "call " + hexString(in.opcode);
            std::printf("    t%-6llu %*s%04X #%-3u %-30s %s\n", static_cast<unsigned long long>(machine.ticks),
                        int(t.stack.size() - 1) * 2, "", f.instance, f.pc, op.c_str(), f.behavior->fileName.c_str());
        };

    std::map<vm::ObjectId, std::string> running;
    std::map<vm::ObjectId, std::pair<uint64_t, size_t>> lastMemory; // newest remembered tick, count
    for (long i = 0; i < ticks; ++i) {
        machine.tick();
        for (vm::ObjectId id : sims) {
            const vm::Object* s = machine.object(id);
            if (!s)
                continue;
            std::string now;
            if (s->activeInteraction >= 0 && size_t(s->activeInteraction) < s->queue.size())
                now = s->queue[size_t(s->activeInteraction)].name;
            if (const auto& mem = s->person->autonomyMemory; !mem.empty()) {
                const auto key = std::make_pair(mem.back().tick, mem.size());
                if (lastMemory[id] != key) {
                    lastMemory[id] = key;
                    const vm::Object* o = machine.object(mem.back().object);
                    std::string name = "#" + std::to_string(mem.back().interaction);
                    if (o)
                        if (auto q = vm::makeInteraction(machine, o->id, mem.back().interaction))
                            name = q->name;
                    std::printf("  tick %6llu %s %-10s autonomous \"%s\" on %s ended: %s\n",
                                static_cast<unsigned long long>(machine.ticks), clockText(machine).c_str(),
                                simName(*s).c_str(), name.c_str(), objectName(machine, mem.back().object).c_str(),
                                mem.back().failed ? "false (remembered as failed)" : "true");
                }
            }
            if (now != running[id]) {
                if (!now.empty())
                    std::printf("  tick %6llu %s %-10s running \"%s\"\n", static_cast<unsigned long long>(machine.ticks),
                                clockText(machine).c_str(), simName(*s).c_str(), now.c_str());
                else
                    std::printf("  tick %6llu %s %-10s done with \"%s\"\n", static_cast<unsigned long long>(machine.ticks),
                                clockText(machine).c_str(), simName(*s).c_str(), running[id].c_str());
                running[id] = now;
            }
        }
        if (every > 0 && (i + 1) % every == 0) {
            std::printf("  motives at tick %ld (%s):\n", i + 1, clockText(machine).c_str());
            printMotivesHeader();
            for (vm::ObjectId id : sims)
                if (const vm::Object* s = machine.object(id))
                    printMotives(*s);
        }
    }
    vm::autonomyObserver() = nullptr;

    std::printf("after %ld ticks (%s):\n", ticks, clockText(machine).c_str());
    printMotivesHeader();
    for (vm::ObjectId id : sims)
        if (const vm::Object* s = machine.object(id))
            printMotives(*s);
    for (vm::ObjectId id : sims) {
        const vm::Object* s = machine.object(id);
        if (!s)
            continue;
        std::printf("  %-12s %zu searches, %zu picks", simName(*s).c_str(), searches[id], choices[id]);
        if (auto it = machine.threads.find(id); it != machine.threads.end()) {
            const vm::Thread& t = it->second;
            if (t.status == vm::Thread::Status::Failed)
                std::printf("; Main failed: %s", (verbose ? t.error : t.error.substr(0, t.error.find('\n'))).c_str());
            else if (!t.stack.empty())
                std::printf("; Main in %s #%u", t.stack.back().behavior->fileName.c_str(), t.stack.back().pc);
        }
        std::printf("\n");
    }
    if (!machine.missingPrimitives.empty()) {
        std::printf("missing primitives:");
        for (const auto& [op, n] : machine.missingPrimitives)
            std::printf(" %s %zu,", primitiveName(op).c_str(), n);
        std::printf("\n");
    }
    return 0;
}

} // namespace

int autonomyCommand(const std::string& cmd, int argc, char** argv) {
    if (cmd != "autonomy" || argc < 3)
        return -1;
    std::string lotPath, sim;
    long ticks = 1800, every = 0;
    std::optional<int> motives;
    bool fromInit = false, verbose = false, noDecay = false;
    int traceId = 0;
    size_t traceDepth = 4;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--ticks" && i + 1 < argc)
            ticks = std::stol(argv[++i]);
        else if (a == "--every" && i + 1 < argc)
            every = std::stol(argv[++i]);
        else if (a == "--sim" && i + 1 < argc)
            sim = argv[++i];
        else if (a == "--motives" && i + 1 < argc)
            motives = std::stoi(argv[++i]);
        else if (a == "--init")
            fromInit = true;
        else if (a == "--verbose")
            verbose = true;
        else if (a == "--trace" && i + 1 < argc)
            traceId = std::stoi(argv[++i]);
        else if (a == "--trace-depth" && i + 1 < argc)
            traceDepth = std::stoul(argv[++i]);
        else if (a == "--no-decay")
            noDecay = true;
        else if (lotPath.empty())
            lotPath = a;
        else
            return -1;
    }
    if (lotPath.empty())
        return -1;
    return cmdAutonomy(lotPath, ticks, every, sim, motives, fromInit, verbose, noDecay, traceId, traceDepth);
}

const char* autonomyUsage() {
    return "       s2pkg autonomy <lot.package> [--ticks N] [--every N] [--sim name] [--motives N] [--verbose] [--no-decay] [--trace sim-id [--trace-depth N]] [--init]\n";
}

} // namespace os2
