// s2pkg route: walks a lot's Sim to an object with Go To Routing Slot (or Go
// To Relative Position) in the VM, and shows the route the path planner
// (src/vm/path_planner.hpp) gives it, where it ends up and the route result.

#include "route_cmds.hpp"

#include "formats/dbpf.hpp"
#include "formats/lot.hpp"
#include "formats/lot_objects.hpp"
#include "formats/resindex.hpp"
#include "vm/hood_bridge.hpp"
#include "vm/lot_neighborhood.hpp"
#include "vm/interactions.hpp"
#include "vm/lot.hpp"
#include "vm/lot_resume.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>
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

std::string nameOf(const vm::Object& o) { return o.definition ? o.definition->fileName : "?"; }

// The planner's cells of a level around a point, one character per 1/4
// tile: '#' wall, '/' diagonal wall, 'D' door opening, ' ' nothing to stand
// on, 'o' object, '.' free; 'S' the Sim, 'T' the target.
void printMap(vm::VM& vm, int level, float cx, float cy, int radius, const vm::Object& sim, const vm::Object& target) {
    vm::PathPlanner planner(*vm.world);
    for (const auto& [id, o] : vm.objects())
        if (o.position.inWorld() && !o.person && o.data[vm::objdata::ContainerId] == 0 && vm::VM::blocksRouting(o) &&
            id != target.id) {
            const auto t = vm::VM::tileOf(o);
            planner.addObstacle(t.level, float(t.x), float(t.y), float(t.x + 1), float(t.y + 1), vm::PathPlanner::kObject);
        }
    const int x0 = int(cx) - radius, x1 = int(cx) + radius, y0 = int(cy) - radius, y1 = int(cy) + radius;
    std::printf("level %d, x %d-%d (right), y %d-%d (down), 1/4 tile per character:\n", level, x0, x1, y1, y0);
    for (int qy = (y1 + 1) * 4 - 1; qy >= y0 * 4; --qy) {
        std::printf("%5.2f ", float(qy) / 4);
        for (int qx = x0 * 4; qx < (x1 + 1) * 4; ++qx) {
            const vm::Point2 p{(float(qx) + 0.5f) / 4, (float(qy) + 0.5f) / 4};
            char c = '.';
            const uint8_t b = planner.at(level, p);
            if (b & vm::PathPlanner::kWall) c = '#';
            else if (b & vm::PathPlanner::kDiagonal) c = '/';
            else if (b & vm::PathPlanner::kDoor) c = 'D';
            else if (b & vm::PathPlanner::kNoFloor) c = ' ';
            else if (b & vm::PathPlanner::kObject) c = 'o';
            auto here = [&](const vm::Object& o) {
                return o.position.level == level && int(std::floor(o.position.x * 4)) == qx &&
                       int(std::floor(o.position.y * 4)) == qy;
            };
            if (here(target)) c = 'T';
            if (here(sim)) c = 'S';
            std::putchar(c);
        }
        std::putchar('\n');
    }
}

int cmdRoute(const std::filesystem::path& lotFile, const std::string& simQuery, const std::string& objectQuery,
             int slot, std::optional<int> relative, int ticks, bool map, bool fromInit, int pie) {
    ResourceIndex index(resRoot(lotFile));
    vm::CodeLibrary code(&index);
    DbpfPackage pkg(lotFile);
    const Lot lot = Lot::load(pkg);
    const LotObjects saved = LotObjects::load(pkg);
    vm::VM machine(code);
    machine.initCreatedObjects = true;
    machine.instructionsPerTurn = 20000;
    machine.setClockGlobals();
    const bool hood = vm::loadLotNeighborhood(machine, lotFile); // as lot-run (socials read neighbors)
    const vm::LotLoad load = vm::loadLot(machine, lot, saved, vm::charactersDirOf(lotFile));
    if (hood)
        vm::bindLotSims(machine);
    vm::startSavedLot(machine, load, saved, fromInit);
    vm::ObjectId sim = 0;
    try { // a Sim by object ID too (the lots have two Dons)
        if (const vm::Object* x = machine.object(vm::ObjectId(std::stoi(simQuery))); x && x->person)
            sim = x->id;
    } catch (...) {
    }
    if (!sim)
        sim = vm::findSim(machine, simQuery);
    if (!sim)
        throw std::runtime_error("no Sim \"" + simQuery + "\" in the lot's world");
    vm::ObjectId object = 0;
    try {
        object = vm::ObjectId(std::stoi(objectQuery));
    } catch (...) {
    }
    if (!machine.object(object))
        object = vm::findInteractionObject(machine, objectQuery);
    if (!object)
        throw std::runtime_error("no object \"" + objectQuery + "\"");
    vm::Object& s = *machine.object(sim);
    const vm::Object& o = *machine.object(object);
    std::printf("Sim %d %s at (%.2f, %.2f, %d); object %d %s at (%.2f, %.2f, %d), direction %d\n", sim, nameOf(s).c_str(),
                s.position.x, s.position.y, s.position.level, object, nameOf(o).c_str(), o.position.x, o.position.y,
                o.position.level, o.data[vm::objdata::Direction]);
    const auto& w = *machine.world;
    std::printf("world %ux%u, levels %d-%d, %zu doors, %zu stairs, %zu diagonal walls\n", w.width(), w.height(),
                w.firstLevel(), w.firstLevel() + w.levelCount() - 1, w.doors().size(), w.stairs().size(),
                w.diagonalWalls().size());
    for (const auto& st : w.stairs())
        std::printf("  stair (%d, %d, %d) -> (%d, %d, %d), %d steps\n", st.bottom.x, st.bottom.y, st.bottom.level, st.top.x,
                    st.top.y, st.top.level, st.steps);
    // The object's routing slots and the goals each gives.
    if (const Slots* table = o.definition ? code.slots(o.group, o.definition->slotId()) : nullptr) {
        int n = 0;
        for (const auto& sl : table->slots) {
            if (sl.type != 3)
                continue;
            const auto g = machine.routingGoals(sl, o);
            std::printf("  routing slot %d: flags %X, distance %.2f-%.2f (optimal %.2f), offset (%.2f, %.2f), facing %d: %zu goals",
                        n++, sl.flags, sl.minDistance, sl.maxDistance, sl.optimalDistance, sl.offset[0], sl.offset[1],
                        sl.facing, g.goals.size());
            for (size_t i = 0; i < g.points.size() && i < 8; ++i)
                std::printf(" (%.2f,%.2f)", g.points[i][0], g.points[i][1]);
            std::printf("%s\n", g.onTarget ? " on target" : "");
            if (const auto bp = machine.slotBone(sl, o))
                std::printf("      bone at (%.2f, %.2f, %.2f) in model space\n", (*bp)[0], (*bp)[1], (*bp)[2]);
            std::printf("      raw: stand %d sit %d ground %d snap %d prox %d/%d/%d maxSize %d u10 %d grad %.2f height %d facing %d res %d extra %08X %08X\n",
                        sl.standing, sl.sitting, sl.ground, sl.snapTargetSlot, sl.minProximity, sl.maxProximity,
                        sl.optimalProximity, sl.maxSize, sl.unknown10, sl.gradient, sl.height, sl.facing, sl.resolution,
                        sl.ts2Extra[0], sl.ts2Extra[1]);
        }
    }
    for (const auto id : machine.objectsOnTile(vm::VM::tileOf(o)))
        if (const vm::Object* x = machine.object(id))
            std::printf("  on its tile: %d %s%s\n", id, nameOf(*x).c_str(), vm::VM::blocksRouting(*x) ? " (blocks)" : "");
    if (map) {
        printMap(machine, s.position.level, s.position.x, s.position.y, 6, s, o);
        if (o.position.level != s.position.level || std::hypot(o.position.x - s.position.x, o.position.y - s.position.y) > 6)
            printMap(machine, o.position.level, o.position.x, o.position.y, 6, s, o);
    }

    s.queue.clear();
    s.activeInteraction = -1;
    if (pie >= 0) {
        // The pie menu's entry instead, with the Sim's queue emptied first;
        // every Go To Routing Slot / Relative Position it runs is shown.
        const auto menu = vm::pieMenu(machine, sim, object);
        std::vector<const vm::PieEntry*> shown;
        for (const auto& e : menu)
            if (!e.debug && e.available())
                shown.push_back(&e);
        if (size_t(pie) >= shown.size())
            throw std::runtime_error("--pie: no entry " + std::to_string(pie));
        auto q = vm::makeInteraction(machine, shown[size_t(pie)]->object, shown[size_t(pie)]->interaction);
        q->pushedBy = sim;
        vm::enqueue(machine, s, *q);
        std::printf("queued \"%s\"\n", q->name.c_str());
        machine.trace = [&](const vm::Thread& t, const vm::Frame& f, const Behavior::Instruction& in) {
            if (t.me != sim || (in.opcode != 0x2D && in.opcode != 0x1B && in.opcode != 0x71))
                return;
            const vm::Object& me = *machine.object(sim);
            if (me.route && me.route->tree == f.behavior && me.route->node == f.pc)
                return; // still walking
            std::printf("  tick %llu: %s in \"%s\" (stack object %d) at (%.2f, %.2f, %d)\n",
                        (unsigned long long)machine.ticks, in.opcode == 0x2D ? "Go To Routing Slot"
                                                           : in.opcode == 0x1B ? "Go To Relative Position" : "Snap Into",
                        f.behavior->fileName.c_str(), f.stackObject, me.position.x, me.position.y, me.position.level);
        };
        for (int i = 0; i < ticks; ++i) {
            machine.tick();
            const vm::Object* now = machine.object(sim);
            if ((i + 1) % 120 == 0)
                std::printf("  tick %d: at (%.2f, %.2f, %d)%s, route result %d, container %d\n", i + 1, now->position.x,
                            now->position.y, now->position.level, now->route ? ", walking" : "",
                            now->person ? now->person->data[62] : 0, now->data[vm::objdata::ContainerId]);
        }
        if (auto it = machine.threads.find(sim); it != machine.threads.end() && it->second.status == vm::Thread::Status::Failed)
            std::printf("  the Sim's Main failed: %s\n", it->second.error.substr(0, it->second.error.find('\n')).c_str());
        std::printf("queue:");
        for (size_t i = 0; i < s.queue.size(); ++i)
            std::printf(" [%s%s]", s.queue[i].name.c_str(), int(i) == s.activeInteraction ? ", running" : "");
        std::printf("\n");
        return 0;
    }
    // A tree of the Sim's own group, as vm::startScriptedWalk makes one.
    Behavior b;
    b.fileName = "opensims2 - s2pkg route";
    b.signature = 0x8007;
    Behavior::Instruction go;
    if (relative) {
        go.opcode = 0x1B;
        go.operands[2] = uint8_t(int8_t(*relative));
        go.operands[3] = uint8_t(int8_t(-2));
    } else {
        go.opcode = 0x2D;
        go.operands[0] = uint8_t(slot);
        go.operands[2] = 1; // a literal slot number
    }
    go.onTrue = {Behavior::TargetKind::True, 0};
    go.onFalse = {Behavior::TargetKind::False, 0};
    b.instructions = {go};
    code.add(s.group, 0x1FF1, std::move(b));
    machine.threads.insert_or_assign(sim, machine.start(sim, 0x1FF1, {}, object));
    bool shown = false;
    for (int i = 0; i < ticks; ++i) {
        machine.tick();
        vm::Object* now = machine.object(sim);
        if (!now)
            break;
        if (!shown && now->route) {
            shown = true;
            std::printf("route (%zu points):", now->route->points.size());
            for (const auto& p : now->route->points) {
                std::printf(" (%.2f,%.2f,%d)", p.x, p.y, p.level);
                if (p.ticks)
                    std::printf("[%s %d%s]", p.animation.empty() ? "wait" : p.animation.c_str(), p.ticks,
                                p.door ? (" door " + std::to_string(p.door)).c_str() : "");
            }
            std::printf("\n");
        }
        const auto it = machine.threads.find(sim);
        const bool done = it == machine.threads.end() || it->second.status != vm::Thread::Status::Running;
        if ((i + 1) % 60 == 0 || done)
            std::printf("  tick %d: at (%.2f, %.2f, %d)%s%s\n", i + 1, now->position.x, now->position.y,
                        now->position.level, now->route ? (now->route->waiting ? ", waiting" : ", walking") : "",
                        now->route && now->route->blocker ? (", blocked by " + std::to_string(now->route->blocker)).c_str() : "");
        if (done) {
            if (it != machine.threads.end())
                std::printf("  %s%s%s\n",
                            it->second.status == vm::Thread::Status::Finished ? (it->second.result ? "true" : "false")
                                                                               : "failed: ",
                            it->second.status == vm::Thread::Status::Failed ? it->second.error.substr(0, it->second.error.find('\n')).c_str() : "",
                            "");
            break;
        }
    }
    if (s.person)
        std::printf("route result (person data 62): %d, facing %d\n", s.person->data[62], s.data[vm::objdata::Direction]);
    return 0;
}

} // namespace

int routeCommand(const std::string& cmd, int argc, char** argv) {
    if (cmd != "route" || argc < 5)
        return -1;
    std::vector<std::string> pos;
    int slot = 0, ticks = 1800;
    std::optional<int> relative;
    bool map = false, fromInit = false;
    int pie = -1;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--slot" && i + 1 < argc)
            slot = std::stoi(argv[++i]);
        else if (a == "--relative" && i + 1 < argc)
            relative = std::stoi(argv[++i]);
        else if (a == "--ticks" && i + 1 < argc)
            ticks = std::stoi(argv[++i]);
        else if (a == "--pie" && i + 1 < argc)
            pie = std::stoi(argv[++i]);
        else if (a == "--map")
            map = true;
        else if (a == "--init")
            fromInit = true;
        else
            pos.push_back(a);
    }
    if (pos.size() != 3)
        return -1;
    return cmdRoute(pos[0], pos[1], pos[2], slot, relative, ticks, map, fromInit, pie);
}

const char* routeUsage() {
    return "       s2pkg route <lot.package> <sim> <object|id> [--slot N | --relative LOC | --pie N] [--ticks N] [--map] [--init]\n";
}

} // namespace os2
