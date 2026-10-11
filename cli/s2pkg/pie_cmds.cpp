// s2pkg pie: the pie menu a lot's Sim gets for an object, from the TTABs and
// check trees run in the VM (src/vm/interactions.hpp), and what happens when
// an entry is picked.

#include "pie_cmds.hpp"

#include "formats/bhav.hpp"
#include "formats/binreader.hpp"
#include "formats/dbpf.hpp"
#include "formats/lot.hpp"
#include "formats/lot_objects.hpp"
#include "formats/resindex.hpp"
#include "vm/hood_bridge.hpp"
#include "vm/lot_neighborhood.hpp"
#include "vm/interactions.hpp"
#include "vm/lot.hpp"
#include "vm/lot_resume.hpp"

#include <cstdio>
#include <cstdlib>
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
           int select, int ticks, bool all, std::optional<int> motives, bool fromInit, size_t trace,
           int dialogAnswer, size_t traceChecks, bool anims) {
    ResourceIndex index(resRoot(lotFile));
    vm::CodeLibrary code(&index);
    DbpfPackage pkg(lotFile);
    const Lot lot = Lot::load(pkg);
    const LotObjects saved = LotObjects::load(pkg);
    vm::VM machine(code);
    machine.initCreatedObjects = true;
    machine.instructionsPerTurn = 20000;
    machine.setClockGlobals();
    machine.autoDialogAnswer = dialogAnswer; // headless: every dialog gets this button
    const bool hood = vm::loadLotNeighborhood(machine, lotFile);
    const vm::LotLoad load = vm::loadLot(machine, lot, saved, vm::charactersDirOf(lotFile));
    if (hood)
        vm::bindLotSims(machine);
    // The saved state (motives, tree stacks, clock), or every Init with --init.
    vm::startSavedLot(machine, load, saved, fromInit);
    if (motives)
        vm::setAllMotives(machine, int16_t(*motives));

    const vm::ObjectId sim = vm::findSim(machine, simQuery);
    if (!sim)
        throw std::runtime_error("no Sim \"" + simQuery + "\" in the lot's world");
    std::printf("Sim %d: %s\n", sim, nameOf(*machine.object(sim)).c_str());
    // The queue as the saved lot left it (vm/lot_resume.hpp).
    if (const vm::Object* so = machine.object(sim); so && !so->queue.empty()) {
        std::printf("saved queue:");
        for (size_t i = 0; i < so->queue.size(); ++i)
            std::printf(" [%s on %d%s]", so->queue[i].name.c_str(), so->queue[i].object,
                        int(i) == so->activeInteraction ? ", running" : "");
        std::printf("\n");
    }

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
    // --trace-checks N: the first N instructions of the entries' check trees.
    size_t checkTraced = 0;
    if (traceChecks)
        machine.trace = [&](const vm::Thread& t, const vm::Frame& f, const Behavior::Instruction& in) {
            if (t.me != sim || checkTraced++ >= traceChecks)
                return;
            const std::string op = in.opcode < 0x100 ? primitiveName(in.opcode) : "call " + hexString(in.opcode);
            std::printf("    %*s%08X %04X #%-3u %-28s %s\n", int(t.stack.size() - 1) * 2, "", f.group, f.instance, f.pc,
                        op.c_str(), f.behavior->fileName.c_str());
        };
    const auto pie = vm::pieMenu(machine, sim, object);
    machine.trace = nullptr;
    int n = 0;
    std::vector<const vm::PieEntry*> shown;
    for (const auto& e : pie) {
        if (!all && (e.debug || !e.available()))
            continue;
        shown.push_back(&e);
        std::printf("  [%d] TTA %-3u %-9s %s", n++, e.interaction, e.debug ? "debug" : e.wrongAge ? "age" : e.hidden ? "hidden" : checkName(e.check),
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
    auto q = vm::interactionFromPie(machine, e); // keeps a dynamic entry's name and parameter 0
    q->pushedBy = sim;
    vm::Object& s = *machine.object(sim);
    vm::enqueue(machine, s, *q);
    std::printf("queued \"%s\" (TTA %u, action %04X) on %s; %d ticks:\n", q->name.c_str(), q->interaction, q->action,
                nameOf(s).c_str(), ticks);
    // --trace N: the first N instructions the Sim's thread runs from here on.
    size_t traced = 0;
    if (trace)
        machine.trace = [&](const vm::Thread& t, const vm::Frame& f, const Behavior::Instruction& in) {
            // S2PKG_TRACE_ALL=1: every thread, each line tagged with its object.
            static const bool everyThread = std::getenv("S2PKG_TRACE_ALL") != nullptr;
            if ((!everyThread && t.me != sim) || traced++ >= trace)
                return;
            const std::string op = in.opcode < 0x100 ? primitiveName(in.opcode) : "call " + hexString(in.opcode);
            if (everyThread)
                std::printf("[%d]", t.me);
            std::printf("    %*s%08X %04X #%-3u %-28s %s\n", int(t.stack.size() - 1) * 2, "", f.group, f.instance, f.pc,
                        op.c_str(), f.behavior->fileName.c_str());
        };
    uint32_t lastDialog = 0;
    auto showDialog = [&](const vm::Dialog& d, int tick) {
        if (d.id <= lastDialog)
            return;
        lastDialog = d.id;
        std::printf("  tick %d: dialog %d \"%s\" from %d (%s): \"%s\"\n      \"%s\"\n", tick, d.type,
                    d.typeName.c_str(), d.me, nameOf(*machine.object(d.me)).c_str(), d.title.c_str(), d.message.c_str());
        for (size_t b = 0; b < d.buttons.size(); ++b)
            std::printf("      [%zu] %s%s\n", b, d.buttons[b].c_str(), int(b) == d.answer ? "  <- answered" : "");
    };
    std::map<vm::ObjectId, std::string> lastAnim;
    for (int i = 0; i < ticks; ++i) {
        const bool before = s.activeInteraction >= 0;
        machine.tick();
        for (const auto& [who, d] : machine.dialogs)
            showDialog(d, i + 1);
        for (const auto& d : machine.notifications)
            showDialog(d, i + 1);
        vm::Object* now = machine.object(sim);
        if (!now)
            break;
        // --anims: every Sim's full-body animation as it changes (both sides of a social).
        if (anims)
            for (const auto& [oid, ob] : machine.objects())
                if (ob.person && ob.animation.name != lastAnim[oid]) {
                    lastAnim[oid] = ob.animation.name;
                    std::printf("  tick %d: %d %s plays \"%s\" at (%.2f, %.2f) dir %d\n", i + 1, oid, nameOf(ob).c_str(),
                                ob.animation.name.c_str(), ob.position.x, ob.position.y, ob.data[vm::objdata::Direction]);
                }
        if (!before && now->activeInteraction >= 0)
            std::printf("  tick %d: running \"%s\"\n", i + 1, now->queue[size_t(now->activeInteraction)].name.c_str());
        if ((i + 1) % 60 == 0)
            std::printf("  tick %d: at (%.2f, %.2f, %d)%s, hunger %d\n", i + 1, now->position.x, now->position.y,
                        now->position.level, now->route ? ", walking" : "", now->person ? now->person->motives[7] : 0);
    }
    if (auto it = machine.threads.find(sim); it != machine.threads.end() && it->second.status == vm::Thread::Status::Failed)
        std::printf("  the Sim's Main failed: %s\n", it->second.error.substr(0, it->second.error.find('\n')).c_str());
    for (int16_t hand = 0; hand < 2; ++hand)
        if (const vm::ObjectId held = machine.containedIn(sim, hand)) {
            const vm::Object& h = *machine.object(held);
            std::printf("  in hand %d: %d %s; semi-attributes", hand, held, nameOf(h).c_str());
            for (int16_t v : h.semiAttributes)
                std::printf(" %d", v);
            std::printf("\n");
        }
    // Lights Change Light set (vm/dialog.hpp), of the object and its parts.
    for (const auto& [id, o] : machine.objects())
        for (const auto& [light, st] : o.lights)
            std::printf("  light of %d %s%s%s: %d%%\n", id, nameOf(o).c_str(), light.empty() ? "" : " / ",
                        light.c_str(), st.level(machine.ticks));
    if (!machine.animations.missing.empty()) {
        std::printf("  missing animations:");
        for (const auto& [name, n] : machine.animations.missing)
            std::printf(" \"%s\" %zu,", name.c_str(), n);
        std::printf("\n");
    }
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
    size_t trace = 0, traceChecks = 0;
    int dialogAnswer = 0;
    std::optional<int> motives;
    bool fromInit = false;
    bool all = false, anims = false;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--select" && i + 1 < argc)
            select = std::stoi(argv[++i]);
        else if (a == "--ticks" && i + 1 < argc)
            ticks = std::stoi(argv[++i]);
        else if (a == "--init")
            fromInit = true;
        else if (a == "--motives" && i + 1 < argc)
            motives = std::stoi(argv[++i]);
        else if (a == "--trace" && i + 1 < argc)
            trace = std::stoul(argv[++i]);
        else if (a == "--trace-checks" && i + 1 < argc)
            traceChecks = std::stoul(argv[++i]);
        else if (a == "--dialog-answer" && i + 1 < argc)
            dialogAnswer = std::stoi(argv[++i]);
        else if (a == "--all")
            all = true;
        else if (a == "--anims")
            anims = true;
        else
            pos.push_back(a);
    }
    if (pos.empty() || pos.size() > 3)
        return -1;
    return cmdPie(pos[0], pos.size() > 1 ? pos[1] : "", pos.size() > 2 ? pos[2] : "", select, ticks, all, motives, fromInit,
                  trace, dialogAnswer, traceChecks, anims);
}

const char* pieUsage() {
    return "       s2pkg pie <lot.package> [sim [object]] [--all] [--select N [--ticks N] [--trace N] [--dialog-answer N] [--anims]] [--trace-checks N] [--motives N] [--init]\n";
}

} // namespace os2
