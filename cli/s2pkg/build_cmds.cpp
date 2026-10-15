// s2pkg build-roundtrip: Build-mode geometry written back (formats/lot_write.hpp)
// and the rooms worked out again (formats/lot_edit.hpp), checked against
// the saved lots.

#include "build_cmds.hpp"

#include "formats/dbpf.hpp"
#include "formats/lot.hpp"
#include "formats/lot_edit.hpp"
#include "formats/lot_write.hpp"
#include "formats/build_catalog.hpp"
#include "formats/catalog.hpp"
#include "formats/design_catalog.hpp"
#include "formats/model.hpp"
#include "formats/resindex.hpp"
#include "vm/build_edit.hpp"
#include "vm/placement.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <map>
#include <optional>
#include <tuple>
#include <sstream>
#include <stdexcept>

namespace os2 {

namespace fs = std::filesystem;

namespace {

std::string key(uint32_t type, uint32_t instance) {
    char b[32];
    std::snprintf(b, sizeof b, "%08X-%02X", type, instance);
    return b;
}

// Writes the geometry of each lot as read (and with --rooms after working
// the rooms out again) and compares it, record by record, with the lot's.
int cmdRoundtrip(const std::vector<std::string>& args) {
    bool rooms = false;
    std::vector<fs::path> lots;
    for (const auto& a : args)
        if (a == "--rooms")
            rooms = true;
        else
            lots.emplace_back(a);
    size_t same = 0, differ = 0;
    std::map<std::string, size_t> wrong;
    for (const auto& path : lots) {
        DbpfPackage pkg(path);
        Lot lot;
        try {
            lot = Lot::load(pkg);
        } catch (const std::exception& ex) {
            std::printf("%s: skipped (%s)\n", path.filename().string().c_str(), ex.what());
            continue;
        }
        const Lot before = lot;
        if (rooms) {
            updateRooms(lot);
            // What changed, quarter by quarter: (level, old, new) -> count.
            std::map<std::tuple<int, int, int>, size_t> changes;
            for (size_t i = 0; i < lot.rooms.size(); ++i)
                for (int q = 0; q < 4; ++q)
                    if (lot.rooms[i][q] != before.rooms[i][q])
                        ++changes[{int(i / (size_t(lot.width) * lot.height)) + lot.minLevel, before.rooms[i][q],
                                   lot.rooms[i][q]}];
            size_t shown = 0;
            for (const auto& [k, n] : changes)
                if (shown++ < 8)
                    std::printf("  %s: level %d room %d -> %d: %zu quarters\n", path.filename().string().c_str(),
                                std::get<0>(k), std::get<1>(k), std::get<2>(k), n);
        }
        const auto records = writeLotGeometry(lot, pkg);
        std::vector<std::string> bad;
        for (const auto& r : records) {
            const DbpfEntry* e = nullptr;
            for (const auto& x : pkg.entries())
                if (x.tgi.type == r.type && x.tgi.instance == r.instance && x.tgi.group == r.group)
                    e = &x;
            if (!e) {
                if (r.bytes.size() > 0)
                    bad.push_back(key(r.type, r.instance) + " new");
                continue;
            }
            const auto original = pkg.read(*e);
            if (original != r.bytes) {
                size_t at = 0;
                while (at < original.size() && at < r.bytes.size() && original[at] == r.bytes[at])
                    ++at;
                bad.push_back(key(r.type, r.instance) + " at " + std::to_string(at) + " (" +
                              std::to_string(original.size()) + " vs " + std::to_string(r.bytes.size()) + ")");
                ++wrong[key(r.type, r.instance)];
            }
        }
        if (bad.empty()) {
            ++same;
        } else {
            ++differ;
            std::printf("%s:", path.filename().string().c_str());
            for (const auto& b : bad)
                std::printf(" %s;", b.c_str());
            std::printf("\n");
        }
    }
    std::printf("%zu lots written back the same, %zu differ\n", same, differ);
    for (const auto& [k, n] : wrong)
        std::printf("  %s differs in %zu\n", k.c_str(), n);
    return differ ? 1 : 0;
}

// "a,b,c,..." -> fields.
std::vector<std::string> fields(const std::string& v) {
    std::vector<std::string> out;
    std::stringstream ss(v);
    for (std::string f; std::getline(ss, f, ',');)
        out.push_back(f);
    return out;
}

int num(const std::vector<std::string>& f, size_t i, int fallback) {
    return i < f.size() && !f[i].empty() ? std::stoi(f[i]) : fallback;
}

} // namespace

bool isBuildEdit(const std::string& flag) {
    return flag == "--door" || flag == "--wall" || flag == "--fence" || flag == "--delete-fence" ||
           flag == "--floor-click" || flag == "--foundation" || flag == "--delete-foundation" ||
           flag == "--terrain" || flag == "--terrain-paint" || flag == "--roof" || flag == "--delete-roof" ||
           flag == "--roof-pattern" || flag == "--pool" || flag == "--delete-pool" || flag == "--erase-pool" || flag == "--room" || flag == "--delete-wall" || flag == "--floor" ||
           flag == "--wallpaper" || flag == "--wallpaper-room" || flag == "--wallpaper-flood" || flag == "--design" || flag == "--auto-roof" || flag == "--stairs" || flag == "--dormer" || flag == "--build-funds" || flag == "--fence-arch" || flag == "--delete-fence-arch" || flag == "--delete-stairs" || flag == "--railing" || flag == "--build-undo" || flag == "--build-redo";
}

bool applyBuildEdits(vm::VM& vm, Lot& lot, ResourceIndex& index,
                     const std::vector<std::pair<std::string, std::string>>& edits) {
    if (edits.empty())
        return true;
    const BuildCatalog catalog = BuildCatalog::load(index);
    std::optional<Catalog> objects;
    vm::BuildEditor editor(vm, lot, &catalog);
    bool ok = true;
    for (const auto& [flag, value] : edits) {
        const auto f = fields(value);
        auto pattern = [&](const std::string& key, bool floor) -> const BuildPattern* {
            const BuildPattern* p = catalog.find(key, floor);
            if (!p)
                throw std::runtime_error(flag + ": no " + (floor ? "floor" : "wall") + " pattern \"" + key + "\"");
            return p;
        };
        vm::BuildEditor::Result r;
        if (flag == "--door") {
            // A door or window from the build catalog, by the object tool's
            // rules (vm/placement.hpp), bought; the world opens it.
            if (!objects)
                objects = Catalog::load(index, true);
            const auto at = value.find('@');
            const CatalogEntry* e = objects->find(value.substr(0, at));
            if (!e || at == std::string::npos)
                throw std::runtime_error("--door: expected <guid|name>@x,y[,dir|auto][,level]");
            const auto g = fields(value.substr(at + 1));
            const vm::ObjectId id = vm::createGhost(vm, e->group, e->instance);
            const vm::TilePos tile{num(g, 0, 0), num(g, 1, 0), num(g, 3, 1)};
            // No direction (or "auto"): turned to its wall as the object
            // tool's TryPlace does (vm::autoRotate, AutoRotateObjectToMatchWalls).
            const bool turn = g.size() < 3 || g[2].empty() || g[2] == "auto";
            int dir = turn ? 2 : num(g, 2, 2);
            if (turn) {
                dir = vm::shouldAutoRotate(vm, id) ? vm::autoRotate(vm, id, tile, 2) : 2;
                std::printf("--door: turned to direction %d\n", dir);
            }
            const vm::Placement p = vm::checkPlacement(vm, id, tile, dir);
            std::string why = p.reason;
            r.ok = p.ok && vm::buy(vm, id, p, e->price, &why);
            if (!r.ok)
                vm::deleteGroup(vm, id);
            else if (e->fileName.rfind("Stairs - ", 0) == 0)
                editor.openStairwell(id); // the floor above the steps goes
            else
                editor.syncWorld();
            r.message = r.ok ? e->name + " bought as object " + std::to_string(id) : e->name + ": " + why;
            if (r.ok)
                for (const auto& part : p.parts)
                    if (const vm::Object* o = vm.object(part.id))
                        r.message += "; " + std::to_string(part.id) + " " +
                                     (o->definition ? o->definition->fileName : std::string("?")) + " at (" +
                                     std::to_string(part.tile.x) + "," + std::to_string(part.tile.y) + "," +
                                     std::to_string(part.tile.level) + ")";
            if (r.ok && vm.world)
                r.message += "; the world has " + std::to_string(vm.world->stairs().size()) + " stairs";
        } else if (flag == "--stairs") { // product,x,y,level,direction[,endx,endy[,whole]]
            if (f.size() < 5)
                throw std::runtime_error("--stairs: expected product,x,y,level,direction[,endx,endy[,whole]]");
            const BuildStairs* product = catalog.stairsById(num(f, 0, 0));
            if (!product)
                throw std::runtime_error("--stairs: no modular stairs " + f[0]);
            std::optional<std::array<float, 2>> end;
            if (f.size() >= 7)
                end = std::array<float, 2>{std::stof(f[5]), std::stof(f[6])};
            r = editor.placeStairs(*product, {num(f, 1, 0), num(f, 2, 0), num(f, 3, 2)}, num(f, 4, 0), end,
                                   f.size() > 7 && f[7] == "whole");
            if (r.ok && vm.world)
                r.message += "; the world has " + std::to_string(vm.world->stairs().size()) + " stairs";
        } else if (flag == "--fence-arch" || flag == "--delete-fence-arch") { // x0,y0,x1,y1[,level]
            if (f.size() < 4)
                throw std::runtime_error(flag + ": expected x0,y0,x1,y1[,level]");
            const BuildFenceArch* arch = catalog.fenceArches().empty() ? nullptr : &catalog.fenceArches().front();
            if (flag == "--fence-arch") {
                if (!arch)
                    throw std::runtime_error("--fence-arch: no fence arch in the catalog");
                r = editor.placeFenceArch(num(f, 4, 1), num(f, 0, 0), num(f, 1, 0), num(f, 2, 0), num(f, 3, 0), *arch);
            } else {
                r = editor.deleteFenceArch(num(f, 4, 1), num(f, 0, 0), num(f, 1, 0), num(f, 2, 0), num(f, 3, 0), arch);
            }
        } else if (flag == "--build-funds") { // the family's funds set to n (headless checks)
            vm::addFunds(vm, num(f, 0, 0) - vm::funds(vm));
            r.ok = true;
            r.message = "funds set";
        } else if (flag == "--dormer") { // product 6-11,x,y,direction[,level]
            if (f.size() < 4)
                throw std::runtime_error("--dormer: expected product,x,y,direction[,level]");
            r = editor.placeDormer(num(f, 0, 6), num(f, 4, 2), num(f, 1, 0), num(f, 2, 0), num(f, 3, 0));
        } else if (flag == "--delete-stairs") { // object id of any part
            r = editor.deleteStairs(vm::ObjectId(num(f, 0, 0)));
            if (r.ok && vm.world)
                r.message += "; the world has " + std::to_string(vm.world->stairs().size()) + " stairs";
        } else if (flag == "--railing") { // part id,side 0|1 (1: left of the steps' facing)[,delete]
            r = editor.changeRailing(vm::ObjectId(num(f, 0, 0)), num(f, 1, 0) != 0, f.size() > 2 && f[2] == "delete");
        } else if (flag == "--auto-roof") { // tool 1-5 (0: remove the roofs)[,keep]
            const int tool = num(f, 0, 1);
            r = tool == 0 ? editor.removeRoofs() : editor.autoRoof(tool, f.size() > 1 && f[1] == "keep");
        } else if (flag == "--design") {
            // The Design tool (app/design_tool.hpp): id,subset=material,...
            // for the §15 of the tool; each material must be a choice of the
            // object's model for that subset.
            if (f.size() < 2)
                throw std::runtime_error("--design: expected id,subset=material[,subset=material]");
            static std::optional<DesignCatalog> designs;
            if (!designs)
                designs = DesignCatalog::load(index);
            const vm::ObjectId id = vm::ObjectId(num(f, 0, 0));
            vm::Object* o = vm.object(id);
            const std::string model =
                o ? objectCresName(index, Tgi{vm::kTypeObjd, o->group, o->definitionInstance, 0}) : std::string();
            std::map<std::string, std::string> design;
            r.ok = o && !model.empty();
            r.message = r.ok ? model : "no object " + std::to_string(id) + " with a model";
            for (size_t k = 1; r.ok && k < f.size(); ++k) {
                const auto eq = f[k].find('=');
                const std::string subset = f[k].substr(0, eq), material = eq == std::string::npos ? "" : f[k].substr(eq + 1);
                bool known = false;
                for (const auto* m : designs->options(model, subset))
                    known |= m->material == material;
                if (!known) {
                    r.ok = false;
                    r.message = material + " is not a design choice of " + model + "'s " + subset;
                }
                design[subset] = material;
            }
            if (r.ok && !vm::canAfford(vm, 15))
                r.ok = false, r.message = "Insufficient Funds!";
            if (r.ok) {
                vm::chargeFunds(vm, -15);
                o->design = design;
                r.cost = 15;
                r.message = model + ": design of object " + std::to_string(id) + " changed for §15";
            }
        } else if (flag == "--build-undo" || flag == "--build-redo") {
            r.ok = flag == "--build-undo" ? editor.undo() : editor.redo();
            r.message = r.ok ? "done" : "nothing to do";
        } else if (flag == "--wall" || flag == "--room" || flag == "--delete-wall") {
            if (f.size() < 4)
                throw std::runtime_error(flag + ": expected x0,y0,x1,y1[,level]");
            const int a = num(f, 0, 0), b = num(f, 1, 0), c = num(f, 2, 0), d = num(f, 3, 0), level = num(f, 4, 1);
            r = flag == "--wall"   ? editor.drawWalls(level, a, b, c, d)
                : flag == "--room" ? editor.drawRoom(level, a, b, c, d)
                                   : editor.deleteWalls(level, a, b, c, d);
        } else if (flag == "--fence") {
            if (f.size() < 5)
                throw std::runtime_error("--fence: expected x0,y0,x1,y1,fence[,level]");
            const BuildFence* fence = catalog.findFence(f[4]);
            if (!fence)
                throw std::runtime_error("--fence: no fence \"" + f[4] + "\"");
            r = editor.drawFences(num(f, 5, 1), num(f, 0, 0), num(f, 1, 0), num(f, 2, 0), num(f, 3, 0), *fence);
        } else if (flag == "--pool") {
            if (f.size() < 4)
                throw std::runtime_error("--pool: expected x0,y0,x1,y1");
            r = editor.drawPool(num(f, 0, 0), num(f, 1, 0), num(f, 2, 0), num(f, 3, 0));
        } else if (flag == "--erase-pool") {
            if (f.size() < 4)
                throw std::runtime_error("--erase-pool: expected x0,y0,x1,y1");
            r = editor.erasePool(num(f, 0, 0), num(f, 1, 0), num(f, 2, 0), num(f, 3, 0));
        } else if (flag == "--delete-pool") {
            if (f.size() < 2)
                throw std::runtime_error("--delete-pool: expected x,y");
            r = editor.deletePool(num(f, 0, 0), num(f, 1, 0));
        } else if (flag == "--roof") {
            if (f.size() < 4)
                throw std::runtime_error("--roof: expected x0,y0,x1,y1[,type[,level]]");
            r = editor.drawRoof(num(f, 5, 1), num(f, 0, 0), num(f, 1, 0), num(f, 2, 0), num(f, 3, 0), uint32_t(num(f, 4, 5)));
        } else if (flag == "--delete-roof") {
            if (f.size() < 2)
                throw std::runtime_error("--delete-roof: expected x,y[,level]");
            r = editor.deleteRoof(num(f, 2, 1), std::stof(f[0]), std::stof(f[1]));
        } else if (flag == "--roof-pattern") {
            const BuildRoofPattern* p = catalog.findRoofPattern(value);
            if (!p)
                throw std::runtime_error("--roof-pattern: no pattern \"" + value + "\"");
            r = editor.setRoofPattern(p->guid);
        } else if (flag == "--terrain" || flag == "--terrain-paint") {
            // op|paint,x,y[,ticks[,size]]: a stroke of `ticks` brush applications.
            if (f.size() < 3)
                throw std::runtime_error(flag + ": expected op,x,y[,ticks[,size]]");
            using Op = vm::BuildEditor::TerrainOp;
            Op op = Op::Paint;
            std::string paint;
            if (flag == "--terrain") {
                op = f[0] == "lower" ? Op::Lower : f[0] == "flatten" ? Op::Flatten : f[0] == "smooth" ? Op::Smooth
                     : f[0] == "water" ? Op::Water : Op::Raise;
            } else if (f[0] == "erase") {
                op = Op::Erase;
            } else {
                const BuildTerrainPaint* t = catalog.findTerrainPaint(f[0]);
                if (!t)
                    throw std::runtime_error("--terrain-paint: no paint \"" + f[0] + "\"");
                paint = t->texture;
            }
            r = editor.terrainStroke(op, std::stof(f[1]), std::stof(f[2]), num(f, 3, 10), num(f, 4, 1),
                                     op == Op::Lower   ? catalog.lowerProfile()
                                     : op == Op::Water ? catalog.waterProfile()
                                                       : catalog.raiseProfile(),
                                     paint);
        } else if (flag == "--foundation" || flag == "--delete-foundation") {
            if (f.size() < 4)
                throw std::runtime_error(flag + ": expected x0,y0,x1,y1");
            r = flag == "--foundation" ? editor.drawFoundation(num(f, 0, 0), num(f, 1, 0), num(f, 2, 0), num(f, 3, 0), num(f, 4, 0))
                                       : editor.deleteFoundation(num(f, 0, 0), num(f, 1, 0), num(f, 2, 0), num(f, 3, 0));
        } else if (flag == "--delete-fence") {
            if (f.size() < 4)
                throw std::runtime_error("--delete-fence: expected x0,y0,x1,y1[,level]");
            r = editor.deleteFences(num(f, 4, 1), num(f, 0, 0), num(f, 1, 0), num(f, 2, 0), num(f, 3, 0));
        } else if (flag == "--floor") {
            if (f.size() < 5)
                throw std::runtime_error("--floor: expected x0,y0,x1,y1,pattern[,level[,rotation]]");
            const BuildPattern* p = f[4] == "none" ? nullptr : pattern(f[4], true);
            r = editor.paintFloor(num(f, 5, 1), num(f, 0, 0), num(f, 1, 0), num(f, 2, 0), num(f, 3, 0), p, 0xF,
                                  num(f, 6, 1));
        } else if (flag == "--floor-click") {
            if (f.size() < 3)
                throw std::runtime_error("--floor-click: expected px,py,pattern[,level[,rotation]]");
            const float px = std::stof(f[0]), py = std::stof(f[1]);
            const int level = num(f, 3, 1), x = int(std::floor(px)), y = int(std::floor(py));
            r = editor.paintFloor(level, x, y, x, y, f[2] == "none" ? nullptr : pattern(f[2], true),
                                  editor.clickQuarters(level, px, py), num(f, 4, 1));
        } else if (flag == "--wallpaper") {
            if (f.size() < 6)
                throw std::runtime_error("--wallpaper: expected x0,y0,x1,y1,side,pattern[,level]");
            const int side = f[4] == "r" ? 1 : f[4] == "l" ? -1 : 0;
            r = editor.paintWalls(num(f, 6, 1), num(f, 0, 0), num(f, 1, 0), num(f, 2, 0), num(f, 3, 0), side,
                                  f[5] == "none" ? nullptr : pattern(f[5], false));
        } else if (flag == "--wallpaper-flood") {
            if (f.size() < 6)
                throw std::runtime_error("--wallpaper-flood: expected x0,y0,x1,y1,r|l,pattern[,level]");
            r = editor.paintFlood(num(f, 6, 1), num(f, 0, 0), num(f, 1, 0), num(f, 2, 0), num(f, 3, 0),
                                  f[4] == "l" ? -1 : 1, f[5] == "none" ? nullptr : pattern(f[5], false));
        } else if (flag == "--wallpaper-room") {
            if (f.size() < 3)
                throw std::runtime_error("--wallpaper-room: expected x,y,pattern[,level]");
            r = editor.paintRoom(num(f, 3, 1), num(f, 0, 0), num(f, 1, 0), f[2] == "none" ? nullptr : pattern(f[2], false));
        }
        std::printf("  %s %s: %s%s (funds §%d)\n", flag.c_str(), value.c_str(), r.ok ? "" : "FAILED: ",
                    r.message.c_str(), vm::funds(vm));
        ok &= r.ok;
    }
    // As leaving Build mode: what was sold with the walls dies now.
    editor.resetHistory();
    return ok;
}

std::vector<std::string> compareLotGeometry(const Lot& a, const Lot& b, size_t limit) {
    std::vector<std::string> out;
    auto say = [&](const std::string& s) {
        if (out.size() < limit)
            out.push_back(s);
    };
    std::map<uint32_t, const Lot::Wall*> wa, wb;
    for (const auto& w : a.walls)
        wa[w.id] = &w;
    for (const auto& w : b.walls)
        wb[w.id] = &w;
    for (const auto& [id, w] : wa) {
        auto it = wb.find(id);
        if (it == wb.end()) {
            say("wall " + std::to_string(id) + " missing");
            continue;
        }
        const Lot::Wall& v = *it->second;
        if (w->a.x != v.a.x || w->a.y != v.a.y || w->a.level != v.a.level || w->b.x != v.b.x || w->b.y != v.b.y ||
            w->type != v.type || w->patterns != v.patterns)
            say("wall " + std::to_string(id) + " differs");
    }
    for (const auto& [id, w] : wb)
        if (!wa.count(id))
            say("wall " + std::to_string(id) + " added");
    if (a.floors != b.floors)
        say("floors differ");
    if (a.elevations != b.elevations)
        say("elevations differ");
    if (a.roofs.size() != b.roofs.size())
        say("roofs differ");
    if (a.pools.size() != b.pools.size() || a.minLevel != b.minLevel || a.levelCount != b.levelCount)
        say("pools or levels differ");
    for (size_t i = 0; i < a.roofs.size() && i < b.roofs.size(); ++i)
        if (a.roofs[i].id != b.roofs[i].id || a.roofs[i].type != b.roofs[i].type || a.roofs[i].x0 != b.roofs[i].x0 ||
            a.roofs[i].y1 != b.roofs[i].y1 || a.roofs[i].level != b.roofs[i].level || a.roofs[i].pattern != b.roofs[i].pattern)
            say("roof " + std::to_string(a.roofs[i].id) + " differs");
    if (a.water != b.water)
        say("water levels differ");
    if (a.paints.size() != b.paints.size() || a.terrainBase != b.terrainBase)
        say("terrain paints differ");
    for (size_t i = 0; i < a.paints.size() && i < b.paints.size(); ++i)
        if (a.paints[i].texture != b.paints[i].texture || a.paints[i].mask != b.paints[i].mask)
            say("terrain paint " + a.paints[i].texture + " differs");
    if (a.floorRotations != b.floorRotations)
        say("floor rotations differ");
    if (a.fencePosts.size() != b.fencePosts.size())
        say("fence posts differ");
    for (size_t i = 0; i < a.fencePosts.size() && i < b.fencePosts.size(); ++i)
        if (a.fencePosts[i].x != b.fencePosts[i].x || a.fencePosts[i].y != b.fencePosts[i].y ||
            a.fencePosts[i].level != b.fencePosts[i].level || a.fencePosts[i].fence != b.fencePosts[i].fence) {
            say("fence post " + std::to_string(i) + " differs");
            break;
        }
    if (a.rooms != b.rooms)
        say("rooms differ");
    if (a.roomLayers != b.roomLayers)
        say("room layers differ");
    // Pattern names of what is used (unused references are not written).
    auto used = [](const Lot& l, bool floor) {
        std::map<uint16_t, std::string> m;
        if (floor) {
            for (const auto& q : l.floors)
                for (uint16_t v : q)
                    if (v && l.floorPatterns.count(v))
                        m[v] = l.floorPatterns.at(v);
        } else {
            for (const auto& w : l.walls)
                for (uint16_t v : w.patterns)
                    if (v != 0xFFFF && l.wallPatterns.count(v))
                        m[v] = l.wallPatterns.at(v);
        }
        return m;
    };
    if (used(a, true) != used(b, true))
        say("floor pattern names differ");
    if (used(a, false) != used(b, false))
        say("wall pattern names differ");
    if (a.wallGraphs.size() != b.wallGraphs.size())
        say("wall graph count differs");
    for (size_t i = 0; i < a.wallGraphs.size() && i < b.wallGraphs.size(); ++i) {
        const auto& g = a.wallGraphs[i];
        const auto& h = b.wallGraphs[i];
        if (g.header != h.header || g.lines.size() != h.lines.size() || g.vertices.size() != h.vertices.size() ||
            g.rooms != h.rooms)
            say("wall graph " + std::to_string(g.instance) + " differs");
    }
    return out;
}

namespace {

// The Build-mode catalog: patterns by subsort, build objects by type and subsort.
int cmdBuildCatalog(const std::string& res, bool verbose) {
    ResourceIndex index(res);
    const BuildCatalog patterns = BuildCatalog::load(index);
    for (bool floor : {true, false}) {
        std::map<std::string, std::vector<const BuildPattern*>> by;
        for (const auto& p : floor ? patterns.floors() : patterns.walls())
            by[p.subsort].push_back(&p);
        std::printf("%s patterns: %zu\n", floor ? "floor" : "wall", (floor ? patterns.floors() : patterns.walls()).size());
        for (const auto& [sub, v] : by) {
            std::printf("  %-10s %3zu (e.g. %s, §%d, %s)\n", sub.c_str(), v.size(), v.front()->name.c_str(),
                        v.front()->price, v.front()->material.c_str());
            if (verbose)
                for (const auto* p : v)
                    std::printf("      %08x §%-3d %s (%s)\n", p->guid, p->price, p->name.c_str(), p->material.c_str());
        }
    }
    for (const auto& b : patterns.stairs())
        std::printf("modular stairs %d: %s, §%d a step (landing %08x, placeholder %08x, steps %08x %08x %08x)\n", b.id,
                    b.name.c_str(), b.price, b.objects[0], b.objects[1], b.objects[2], b.objects[3], b.objects[4]);
    for (const auto& a : patterns.fenceArches())
        std::printf("fence arch %d: %s, §%d (%s)\n", a.id, a.name.c_str(), a.price, a.center.c_str());
    for (const auto& r : patterns.roofProducts())
        std::printf("roof %d: %s, §%d (%s)\n", r.id, r.name.c_str(), r.price, r.defaultPattern.c_str());
    const Catalog objects = Catalog::load(index, true);
    std::map<std::pair<int, int>, std::vector<const CatalogEntry*>> by;
    for (const auto& e : objects.entries())
        by[{e.buildModeType, e.buildModeSubsort}].push_back(&e);
    std::printf("build objects: %zu\n", objects.entries().size());
    for (const auto& [k, v] : by) {
        std::printf("  type %d subsort 0x%x: %zu (e.g. %s, §%d)\n", k.first, k.second, v.size(), v.front()->name.c_str(),
                    v.front()->price);
        if (verbose)
            for (const auto* e : v)
                std::printf("      %08x §%-5d %s (%s)\n", e->guid, e->price, e->name.c_str(), e->fileName.c_str());
    }
    return 0;
}

} // namespace

int buildCommand(const std::string& cmd, int argc, char** argv) {
    if (cmd == "build-catalog" && argc >= 3)
        return cmdBuildCatalog(argv[2], argc > 3 && std::string(argv[3]) == "-v");
    if (cmd == "build-roundtrip" && argc >= 3)
        return cmdRoundtrip({argv + 2, argv + argc});
    return -1;
}

const char* buildUsage() {
    return "       s2pkg build-catalog <Res-dir> [-v]\n"
           "       s2pkg build-roundtrip <lot.package>... [--rooms]\n";
}

} // namespace os2
