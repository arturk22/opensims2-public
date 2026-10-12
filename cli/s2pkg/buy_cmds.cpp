// s2pkg catalog: the Buy-mode catalog (formats/catalog.hpp).

#include "buy_cmds.hpp"
#include "budget.hpp"

#include "formats/catalog.hpp"
#include "formats/image.hpp"
#include "formats/dbpf.hpp"
#include "formats/lot.hpp"
#include "formats/lot_objects.hpp"
#include "formats/resindex.hpp"
#include "vm/hood_bridge.hpp"
#include "vm/lot.hpp"
#include "vm/lot_neighborhood.hpp"
#include "vm/lot_resume.hpp"
#include "vm/placement.hpp"

#include <cstdio>
#include <memory>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace os2 {

namespace {

namespace fs = std::filesystem;

std::string bits(uint16_t v, const char* (*name)(int), int count) {
    std::string s;
    for (int i = 0; i < count; ++i)
        if (v & (1u << i))
            s += (s.empty() ? "" : ",") + std::string(name(i));
    return s.empty() ? "-" : s;
}

void printEntry(const CatalogEntry& e, Catalog& c, bool verbose) {
    std::printf("  %08X  §%-6d %-40s  sub %04X  rooms %s%s%s\n", e.guid, e.price, e.name.c_str(), e.functionSubSort,
                bits(e.roomSort, roomSortName, kRoomSortCount).c_str(), e.multiTile ? "  [multi-tile]" : "",
                c.thumbnail(e) ? "" : "  [no thumbnail]");
    if (!verbose)
        return;
    std::printf("      %s (group %08X OBJD %04X, CTSS %04X)\n", e.fileName.c_str(), e.group, e.instance,
                e.catalogStrings);
    std::printf("      %s\n", e.description.c_str());
    std::printf("      ratings hunger %d comfort %d hygiene %d bladder %d energy %d fun %d environment %d; skills "
                "%04X\n",
                e.ratings[0], e.ratings[1], e.ratings[2], e.ratings[3], e.ratings[4], e.ratings[5], e.ratings[6],
                e.skillFlags);
    std::printf("      depreciation initial %d daily %d limit %d%s; placement %04X wall %04X adjacent %04X height "
                "%04X\n",
                e.initialDepreciation, e.dailyDepreciation, e.depreciationLimit, e.selfDepreciating ? " self" : "",
                e.placementFlags, e.wallPlacementFlags, e.wallAdjacentFlags, e.allowedHeightFlags);
}

int cmdCatalog(const std::vector<std::string>& args) {
    fs::path res;
    int function = -1, room = -1, sub = -1;
    bool verbose = false;
    std::string thumbKey, thumbOut;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--function" && i + 1 < args.size())
            function = std::stoi(args[++i]);
        else if (args[i] == "--room" && i + 1 < args.size())
            room = std::stoi(args[++i]);
        else if (args[i] == "--sub" && i + 1 < args.size())
            sub = std::stoi(args[++i]);
        else if (args[i] == "-v" || args[i] == "--verbose")
            verbose = true;
        else if (args[i] == "--thumb" && i + 2 < args.size())
            thumbKey = args[i + 1], thumbOut = args[i + 2], i += 2;
        else
            res = args[i];
    }
    if (res.empty())
        throw std::runtime_error("usage: " + std::string(buyUsage()));
    ResourceIndex index(res);
    Catalog c = Catalog::load(index);
    c.setThumbnailPackage(res / "UserData/Thumbnails/ObjectThumbnails.package");
    if (!thumbKey.empty()) {
        const CatalogEntry* e = c.find(thumbKey);
        if (!e)
            throw std::runtime_error("no catalog entry " + thumbKey);
        const RgbaImage* img = c.thumbnail(*e);
        if (!img)
            throw std::runtime_error("no thumbnail for " + e->name);
        writePng(thumbOut, *img);
        std::printf("%s: %ux%u -> %s\n", e->name.c_str(), img->width, img->height, thumbOut.c_str());
        return 0;
    }
    std::printf("catalog: %zu buyable objects, %zu with a thumbnail\n", c.entries().size(), c.thumbnailCount());
    if (function < 0 && room < 0) {
        for (int f = 0; f < kFunctionSortCount; ++f) {
            const auto list = c.byFunction(f);
            std::printf("\n%s (function bit %d): %zu\n", functionSortName(f), f, list.size());
            for (int s = 0; s < 16; ++s)
                if (const auto l = c.byFunction(f, s); !l.empty())
                    std::printf("  subsort bit %d: %zu (e.g. %s)\n", s, l.size(), l.front()->name.c_str());
            if (verbose)
                for (const auto* e : list)
                    printEntry(*e, c, false);
        }
        std::printf("\nBy room:\n");
        for (int r = 0; r < kRoomSortCount; ++r)
            std::printf("  %-14s %zu\n", roomSortName(r), c.byRoom(r).size());
        size_t none = 0;
        for (const auto& e : c.entries())
            none += (e.functionSort & ((1u << kFunctionSortCount) - 1)) == 0;
        std::printf("  with no function sort: %zu\n", none);
        return 0;
    }
    const auto list = function >= 0 ? c.byFunction(function, sub) : c.byRoom(room, sub);
    std::printf("%s %s%s: %zu\n", function >= 0 ? "function" : "room",
                function >= 0 ? functionSortName(function) : roomSortName(room),
                sub >= 0 ? (" / " + std::to_string(sub)).c_str() : "", list.size());
    for (const auto* e : list)
        printEntry(*e, c, verbose);
    return 0;
}

struct Where {
    int x = 0, y = 0, dir = 0, level = 1;
};

Where parseWhere(const std::string& s) {
    Where w;
    if (std::sscanf(s.c_str(), "%d,%d,%d,%d", &w.x, &w.y, &w.dir, &w.level) < 2)
        throw std::runtime_error("expected x,y[,dir[,level]], got \"" + s + "\"");
    return w;
}

std::pair<std::string, std::string> splitAt(const std::string& s) {
    const auto at = s.rfind('@');
    if (at == std::string::npos)
        throw std::runtime_error("expected <what>@x,y[,dir[,level]], got \"" + s + "\"");
    return {s.substr(0, at), s.substr(at + 1)};
}

fs::path resRootOf(const fs::path& lot) {
    for (auto p = fs::absolute(lot).parent_path(); p.has_relative_path(); p = p.parent_path())
        if (p.filename() == "Res")
            return p;
    throw std::runtime_error("the lot is not under a TSData/Res directory");
}

int cmdBuy(const std::vector<std::string>& args) {
    fs::path lotFile;
    std::vector<std::pair<std::string, std::string>> edits;
    for (size_t i = 0; i < args.size(); ++i) {
        if ((args[i] == "--buy" || args[i] == "--sell" || args[i] == "--move" || args[i] == "--check") &&
            i + 1 < args.size())
            edits.emplace_back(args[i], args[i + 1]), ++i;
        else
            lotFile = args[i];
    }
    if (lotFile.empty() || edits.empty())
        throw std::runtime_error("usage: " + std::string(buyUsage()));
    ResourceIndex index(resRootOf(lotFile));
    vm::CodeLibrary code(&index);
    DbpfPackage pkg(lotFile);
    const Lot lot = Lot::load(pkg);
    const LotObjects saved = LotObjects::load(pkg);
    vm::VM machine(code);
    machine.initCreatedObjects = true;
    machine.instructionsPerTurn = s2pkgBudget();
    machine.setClockGlobals();
    const bool hood = vm::loadLotNeighborhood(machine, lotFile);
    const vm::LotLoad load = vm::loadLot(machine, lot, saved, vm::charactersDirOf(lotFile));
    if (hood)
        vm::bindLotSims(machine);
    vm::startSavedLot(machine, load, saved);
    std::printf("%s: funds §%d\n", lotFile.filename().string().c_str(), vm::funds(machine));
    return applyBuyEdits(machine, index, edits) ? 0 : 1;
}

} // namespace

bool applyBuyEdits(vm::VM& vm, ResourceIndex& index, const std::vector<std::pair<std::string, std::string>>& edits) {
    static std::unique_ptr<Catalog> catalog;
    if (!catalog)
        catalog = std::make_unique<Catalog>(Catalog::load(index));
    bool ok = true;
    for (const auto& [kind, arg] : edits) {
        if (kind == "--buy" || kind == "--check") {
            const auto [what, where] = splitAt(arg);
            const Where w = parseWhere(where);
            const CatalogEntry* e = catalog->find(what);
            if (!e) {
                std::printf("  buy %s: not in the catalog\n", what.c_str());
                ok = false;
                continue;
            }
            const vm::ObjectId ghost = vm::createGhost(vm, e->group, e->instance);
            const vm::Placement p = vm::checkPlacement(vm, ghost, {w.x, w.y, w.level}, w.dir);
            std::string why;
            const bool bought = kind == "--buy" && vm::buy(vm, ghost, p, e->price, &why);
            if (!bought)
                vm::deleteGroup(vm, ghost);
            std::printf("  %s %s (%s, §%d) at (%d, %d) level %d facing %d: %s%s%s", kind.c_str() + 2, e->name.c_str(),
                        e->fileName.c_str(), e->price, w.x, w.y, w.level, w.dir,
                        p.ok ? "fits" : ("does not fit: " + p.reason).c_str(),
                        bought ? ", bought as object " : (kind == "--buy" && p.ok ? (", not bought: " + why).c_str() : ""),
                        bought ? std::to_string(ghost).c_str() : "");
            if (p.container)
                std::printf(" (in slot %d of object %d)", p.slot, p.container);
            std::printf("; funds §%d\n", vm::funds(vm));
            ok = ok && (kind == "--check" || bought);
        } else if (kind == "--sell") {
            const vm::ObjectId id = vm::ObjectId(std::stoi(arg));
            const vm::Object* o = vm.object(id);
            if (!o || !o->definition) {
                std::printf("  sell %d: no such object\n", id);
                ok = false;
                continue;
            }
            const std::string name = o->definition->fileName;
            const int value = vm::sell(vm, id);
            vm::deleteGroup(vm, id); // the session ends with the command
            std::printf("  sell %d (%s) for §%d; funds §%d\n", id, name.c_str(), value, vm::funds(vm));
        } else if (kind == "--move") {
            const auto [what, where] = splitAt(arg);
            const Where w = parseWhere(where);
            const vm::ObjectId id = vm::ObjectId(std::stoi(what));
            if (!vm.object(id)) {
                std::printf("  move %d: no such object\n", id);
                ok = false;
                continue;
            }
            const vm::Placement p = vm::checkPlacement(vm, id, {w.x, w.y, w.level}, w.dir);
            if (p.ok)
                vm::applyPlacement(vm, id, p);
            std::printf("  move %d to (%d, %d) level %d facing %d: %s\n", id, w.x, w.y, w.level, w.dir,
                        p.ok ? "moved" : ("does not fit: " + p.reason).c_str());
            ok = ok && p.ok;
        }
    }
    return ok;
}

int buyCommand(const std::string& cmd, int argc, char** argv) {
    if (cmd == "catalog")
        return cmdCatalog({argv + 2, argv + argc});
    if (cmd == "buy")
        return cmdBuy({argv + 2, argv + argc});
    return -1;
}

const char* buyUsage() {
    return "       s2pkg catalog <Res-dir> [--function bit [--sub bit] | --room bit [--sub function-bit]] [-v]\n"
           "       s2pkg catalog <Res-dir> --thumb <guid|name> <out.png>\n"
           "       s2pkg buy <lot.package> [--buy|--check <guid|name>@x,y[,dir[,level]]] [--sell id] [--move id@x,y[,dir[,level]]]...\n";
}

} // namespace os2
