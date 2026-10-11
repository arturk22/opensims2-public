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
#include "formats/resindex.hpp"
#include "vm/build_edit.hpp"
#include "vm/placement.hpp"

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
    return flag == "--door" || flag == "--wall" || flag == "--room" || flag == "--delete-wall" || flag == "--floor" ||
           flag == "--wallpaper" || flag == "--wallpaper-room" || flag == "--build-undo" || flag == "--build-redo";
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
                throw std::runtime_error("--door: expected <guid|name>@x,y[,dir][,level]");
            const auto g = fields(value.substr(at + 1));
            const vm::ObjectId id = vm::createGhost(vm, e->group, e->instance);
            const vm::Placement p = vm::checkPlacement(vm, id, {num(g, 0, 0), num(g, 1, 0), num(g, 3, 1)}, num(g, 2, 2));
            std::string why = p.reason;
            r.ok = p.ok && vm::buy(vm, id, p, e->price, &why);
            if (!r.ok)
                vm::deleteGroup(vm, id);
            else
                editor.syncWorld();
            r.message = r.ok ? e->name + " bought as object " + std::to_string(id) : e->name + ": " + why;
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
        } else if (flag == "--floor") {
            if (f.size() < 5)
                throw std::runtime_error("--floor: expected x0,y0,x1,y1,pattern[,level]");
            const BuildPattern* p = f[4] == "none" ? nullptr : pattern(f[4], true);
            r = editor.paintFloor(num(f, 5, 1), num(f, 0, 0), num(f, 1, 0), num(f, 2, 0), num(f, 3, 0), p);
        } else if (flag == "--wallpaper") {
            if (f.size() < 6)
                throw std::runtime_error("--wallpaper: expected x0,y0,x1,y1,side,pattern[,level]");
            const int side = f[4] == "r" ? 1 : f[4] == "l" ? -1 : 0;
            r = editor.paintWalls(num(f, 6, 1), num(f, 0, 0), num(f, 1, 0), num(f, 2, 0), num(f, 3, 0), side,
                                  f[5] == "none" ? nullptr : pattern(f[5], false));
        } else if (flag == "--wallpaper-room") {
            if (f.size() < 3)
                throw std::runtime_error("--wallpaper-room: expected x,y,pattern[,level]");
            r = editor.paintRoom(num(f, 3, 1), num(f, 0, 0), num(f, 1, 0), f[2] == "none" ? nullptr : pattern(f[2], false));
        }
        std::printf("  %s %s: %s%s (funds §%d)\n", flag.c_str(), value.c_str(), r.ok ? "" : "FAILED: ",
                    r.message.c_str(), vm::funds(vm));
        ok &= r.ok;
    }
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
