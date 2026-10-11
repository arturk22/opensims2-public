// s2pkg pie: the pie menu a lot's Sim gets for an object, from the TTABs and
// check trees run in the VM (src/vm/interactions.hpp), and what happens when
// an entry is picked.

#include "pie_cmds.hpp"

#include "formats/dbpf.hpp"
#include "formats/lot.hpp"
#include "formats/lot_objects.hpp"
#include "formats/resindex.hpp"
#include "vm/interactions.hpp"
#include "vm/lot.hpp"

#include <cstdio>
#include <filesystem>
#include <map>
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

const char* checkName(vm::CheckResult r) {
    switch (r) {
    case vm::CheckResult::NoGuard: return "no guard";
    case vm::CheckResult::True: return "true";
    case vm::CheckResult::False: return "false";
    case vm::CheckResult::Failed: return "failed";
    case vm::CheckResult::Running: return "running";
    }
    return "?";
}

std::string nameOf(const vm::Object& o) { return o.definition ? o.definition->fileName : "?"; }

int cmdPie(const std::filesystem::path& lotFile, const std::string& simQuery, const std::string& objectQuery,
           int select, int ticks, bool all) {
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
    vm::startLotObjects(machine, load, 0);

    const vm::ObjectId sim = vm::findSim(machine, simQuery);
    if (!sim)
        throw std::runtime_error("no Sim \"" + simQuery + "\" in the lot's world");
    std::printf("Sim %d: %s\n", sim, nameOf(*machine.object(sim)).c_str());

    if (objectQuery.empty()) {
        // Every object in the world with interactions, by interaction leader.
        std::map<vm::ObjectId, std::vector<vm::ObjectId>> leaders;
        for (const auto& [id, o] : machine.objects())
            if (o.position.inWorld() && !o.person)
                if (const vm::Object* l = machine.object(vm::interactionLeader(machine, id)); l && vm::interactionTable(machine, *l))
                    leaders[l->id].push_back(id);
        std::printf("%zu objects with interactions:\n", leaders.size());
        for (const auto& [id, parts] : leaders) {
            const vm::Object& o = *machine.object(id);
            const auto ref = vm::interactionTable(machine, o);
            std::printf("  %5d %-50s (%.1f, %.1f, %d) TTAB %08X/%X, %zu entries\n", id, nameOf(o).c_str(),
                        o.position.x, o.position.y, o.position.level, ref->group, ref->instance,
                        ref->table->interactions.size());
        }
        return 0;
    }

    const vm::ObjectId object = vm::findInteractionObject(machine, objectQuery);
    if (!object)
        throw std::runtime_error("no object \"" + objectQuery + "\" with interactions in the lot's world");
    const vm::ObjectId leader = vm::interactionLeader(machine, object);
    std::printf("object %d: %s", object, nameOf(*machine.object(object)).c_str());
    if (leader != object)
        std::printf(" (interactions of %d: %s)", leader, nameOf(*machine.object(leader)).c_str());
    const auto ref = vm::interactionTable(machine, *machine.object(leader));
    std::printf("\nTTAB %08X/%X\n", ref->group, ref->instance);
    const auto pie = vm::pieMenu(machine, sim, object);
    int n = 0;
    std::vector<const vm::PieEntry*> shown;
    for (const auto& e : pie) {
        if (!all && (e.debug || !e.available()))
            continue;
        shown.push_back(&e);
        std::printf("  [%d] TTA %-3u %-9s %s", n++, e.interaction, e.debug ? "debug" : checkName(e.check),
                    e.name.c_str());
        if (!e.error.empty())
            std::printf("  -- %s", e.error.substr(0, e.error.find('\n')).c_str());
        std::printf("\n");
    }
    std::printf("%zu of %zu entries shown%s\n", shown.size(), pie.size(),
                all ? "" : " (available, not debug; --all shows every entry)");
    if (select < 0)
        return 0;
    if (size_t(select) >= shown.size())
        throw std::runtime_error("--select: no entry " + std::to_string(select));
    const vm::PieEntry& e = *shown[size_t(select)];
    auto q = vm::makeInteraction(machine, e.object, e.interaction);
    q->pushedBy = sim;
    vm::Object& s = *machine.object(sim);
    vm::enqueue(machine, s, *q);
    std::printf("queued \"%s\" (TTA %u, action %04X) on %s; %d ticks:\n", q->name.c_str(), q->interaction, q->action,
                nameOf(s).c_str(), ticks);
    for (int i = 0; i < ticks; ++i) {
        const bool before = s.activeInteraction >= 0;
        machine.tick();
        vm::Object* now = machine.object(sim);
        if (!now)
            break;
        if (!before && now->activeInteraction >= 0)
            std::printf("  tick %d: running \"%s\"\n", i + 1, now->queue[size_t(now->activeInteraction)].name.c_str());
    }
    if (auto it = machine.threads.find(sim); it != machine.threads.end() && it->second.status == vm::Thread::Status::Failed)
        std::printf("  the Sim's Main failed: %s\n", it->second.error.substr(0, it->second.error.find('\n')).c_str());
    std::printf("queue of %s:", nameOf(s).c_str());
    for (size_t i = 0; i < s.queue.size(); ++i)
        std::printf(" [%s%s]", s.queue[i].name.c_str(), int(i) == s.activeInteraction ? ", running" : "");
    std::printf("%s\n", s.queue.empty() ? " empty" : "");
    return 0;
}

} // namespace

int pieCommand(const std::string& cmd, int argc, char** argv) {
    if (cmd != "pie" || argc < 3)
        return -1;
    std::vector<std::string> pos;
    int select = -1, ticks = 30;
    bool all = false;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--select" && i + 1 < argc)
            select = std::stoi(argv[++i]);
        else if (a == "--ticks" && i + 1 < argc)
            ticks = std::stoi(argv[++i]);
        else if (a == "--all")
            all = true;
        else
            pos.push_back(a);
    }
    if (pos.empty() || pos.size() > 3)
        return -1;
    return cmdPie(pos[0], pos.size() > 1 ? pos[1] : "", pos.size() > 2 ? pos[2] : "", select, ticks, all);
}

const char* pieUsage() {
    return "       s2pkg pie <lot.package> [sim [object]] [--all] [--select N [--ticks N]]\n";
}

} // namespace os2
