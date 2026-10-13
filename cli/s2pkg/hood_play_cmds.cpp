// s2pkg commands for playing a neighborhood (formats/hood_moves.hpp):
// lot prices, the family bin, moving in and out, bulldozing.
#include "hood_play_cmds.hpp"

#include "formats/dbpf.hpp"
#include "formats/hood_lots.hpp"
#include "formats/hood_moves.hpp"
#include "formats/resindex.hpp"
#include "formats/lot_objects.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>

namespace os2 {

namespace {

using namespace hood;

// A lot file's price as the neighborhood reads it (GetLotFileInfo).
int cmdLotValue(int argc, char** argv) {
    for (int k = 2; k < argc; ++k) {
        Lot l;
        l.package = argv[k];
        const LotFileInfo i = lotFileInfo(l);
        std::printf("%s: price %lld = lot %lld + lot objects %lld + family objects %lld + architecture %lld x 0.8%s\n",
                    argv[k], (long long)i.price, (long long)i.lotValue, (long long)i.lotObjects, (long long)i.familyObjects,
                    (long long)i.architecture, i.fromFile ? "" : " (no SIMI)");
    }
    return 0;
}

int cmdLots(const char* path) {
    const Neighborhood h = Neighborhood::load(path, false);
    std::printf("%s: %zu lots, %zu families\n", h.prefix.c_str(), h.lots.size(), h.families.size());
    std::printf("  lot  type size  price    (lot+lotobj+famobj+arch*0.8) house movein  family\n");
    for (const auto& l : h.lots) {
        const LotFileInfo i = lotFileInfo(l);
        const uint32_t f = familyOnLot(h, l.id);
        std::printf("  %3u  %u    %ux%u  %7lld  (%lld+%lld+%lld+%lld)%s %s %s  %s\n", l.id, l.info.type,
                    l.info.width, l.info.depth, (long long)i.price, (long long)i.lotValue, (long long)i.lotObjects,
                    (long long)i.familyObjects, (long long)i.architecture, i.fromFile ? "" : " (no file)",
                    i.hasHouse ? "house" : "empty", i.canMoveIn ? "yes" : "no",
                    f ? h.family(f)->name.c_str() : "-");
    }
    std::printf("Family bin:\n");
    for (uint32_t id : homelessFamilies(h)) {
        const Family* f = h.family(id);
        std::printf("  %u %s funds=%d members=%zu, can move into:", id, f->name.c_str(), f->record.funds,
                    f->members.size());
        for (const auto& l : h.lots)
            if (canMoveIn(h, id, l.id) == MoveInCheck::Ok)
                std::printf(" %u", l.id);
        std::printf("\n");
    }
    return 0;
}

int report(const MoveResult& r) {
    for (const auto& l : r.lines)
        std::printf("%s\n", l.c_str());
    if (!r.ok) {
        std::fprintf(stderr, "error: %s\n", r.error.c_str());
        return 1;
    }
    return 0;
}

// The neighborhood a lot placement is checked against (formats/hood_lots.hpp):
// the folder's package, its terrain and occupants, the object catalog of
// $OS2_DATA.
struct World {
    Neighborhood hood;
    Terrain terrain;
    Occupants occupants;
    PlacementWorld place;
    std::filesystem::path gameRes;
};

std::unique_ptr<World> loadWorld(const char* path) {
    auto w = std::make_unique<World>();
    w->hood = Neighborhood::load(path, false);
    DbpfPackage pkg(w->hood.package);
    for (const auto& e : pkg.entries()) {
        if (e.tgi.type == hoodtype::Terrain)
            w->terrain = Terrain::parse(pkg.read(e));
        else if (e.tgi.type == hoodtype::Occupants)
            w->occupants = Occupants::parse(pkg.read(e));
    }
    if (const char* data = std::getenv("OS2_DATA")) {
        w->gameRes = std::filesystem::path(data) / "TSData/Res";
        ResourceIndex index(w->gameRes);
        w->place.objects = loadOccupantFlags(index);
        applyTunables(w->place, TerrainSettings::load(std::filesystem::path(data) / "TSData/Sys"));
    }
    w->place.hood = &w->hood;
    w->place.terrain = &w->terrain;
    w->place.occupants = &w->occupants;
    return w;
}

std::filesystem::path binOf(const World& w) {
    return playerLotCatalog(w.hood.package.parent_path(),
                            w.gameRes.empty() ? std::filesystem::path{} : w.gameRes / "UserData/LotCatalog");
}

int reportLot(const LotResult& r) {
    for (const auto& l : r.lines)
        std::printf("%s\n", l.c_str());
    if (!r.ok) {
        std::fprintf(stderr, "error: %s\n", r.error.c_str());
        return 1;
    }
    return 0;
}

// "empty:<size>", "bin:<n>" or "lot:<id>" -> the base lot info placed.
std::optional<LotInfo> what(const World& w, const std::string& spec, std::optional<BinLot>* bin, uint32_t* self,
                            int* emptySize) {
    const auto colon = spec.find(':');
    if (colon == std::string::npos)
        return std::nullopt;
    const std::string kind = spec.substr(0, colon);
    const int n = std::stoi(spec.substr(colon + 1));
    if (kind == "empty") {
        *emptySize = n;
        return LotInfo{};
    }
    if (kind == "bin") {
        const auto lots = loadLotBin(binOf(w));
        if (n < 0 || size_t(n) >= lots.size())
            return std::nullopt;
        *bin = lots[size_t(n)];
        return lots[size_t(n)].base;
    }
    if (kind == "lot")
        if (const Lot* l = w.hood.lot(uint32_t(n))) {
            *self = l->id;
            return l->info;
        }
    return std::nullopt;
}

int reportDecor(const DecorResult& r) {
    for (const auto& l : r.lines)
        std::printf("%s\n", l.c_str());
    if (!r.ok)
        std::fprintf(stderr, "failed: %s\n", r.error.c_str());
    return r.ok ? 0 : 1;
}

int cmdLotSpots(const char* path, const std::string& spec) {
    const auto w = loadWorld(path);
    std::optional<BinLot> bin;
    uint32_t self = 0;
    int empty = -1;
    const auto base = what(*w, spec, &bin, &self, &empty);
    if (!base)
        throw std::runtime_error("usage: empty:<0-9>, bin:<n> or lot:<id>");
    size_t n = 0;
    for (int y = 0; y < int(w->terrain.height); ++y)
        for (int x = 0; x < int(w->terrain.width); ++x) {
            if (empty >= 0) {
                for (int r = 0; r < 4; ++r) {
                    const TileRect R = emptyLotRect(emptyLotSize(empty), x, y, r);
                    const Placement p = checkEmptyLot(w->place, emptyLotSize(empty), x, y, r);
                    if (p.ok) {
                        std::printf("  (%d, %d) rotation %d: rect (%d, %d)-(%d, %d), height %.2f\n", x, y, r, R.x0,
                                    R.y0, R.x1, R.y1, p.roadHeight);
                        ++n;
                    }
                }
            } else if (auto f = findFront(w->place, *base, x, y, self)) {
                std::printf("  (%d, %d) front %d, height %.2f\n", x, y, f->first, f->second.roadHeight);
                ++n;
            }
        }
    std::printf("%zu places\n", n);
    return 0;
}

int cmdPlaceLot(int argc, char** argv) {
    const auto w = loadWorld(argv[2]);
    std::optional<BinLot> bin;
    uint32_t self = 0;
    int empty = -1;
    if (!what(*w, argv[3], &bin, &self, &empty) || self)
        throw std::runtime_error("usage: s2pkg hood-place-lot <dir> empty:<0-9>|bin:<n> <x> <y> [rotation|front]");
    const int x = std::stoi(argv[4]), y = std::stoi(argv[5]);
    const char* forbidden = std::getenv("OS2_DATA");
    if (empty >= 0) {
        int r = argc > 6 ? std::stoi(argv[6]) : -1;
        if (r < 0)
            for (int k = 0; k < 4 && r < 0; ++k) {
                if (checkEmptyLot(w->place, emptyLotSize(empty), x, y, k).ok)
                    r = k;
            }
        return reportLot(placeEmptyLot(argv[2], w->place, emptyLotSize(empty), x, y, std::max(r, 0),
                                       forbidden ? forbidden : "", {}, 0,
                                       w->gameRes.empty() ? std::filesystem::path{} : w->gameRes / "LotTemplates"));
    }
    int front = argc > 6 ? std::stoi(argv[6]) : -1;
    if (front < 0)
        if (auto f = findFront(w->place, bin->base, x, y))
            front = f->first;
    return reportLot(placeBinLot(argv[2], w->place, *bin, x, y, std::max(front, 0), forbidden ? forbidden : ""));
}

int cmdMoveLot(int argc, char** argv) {
    const auto w = loadWorld(argv[2]);
    const uint32_t lot = uint32_t(std::stoul(argv[3]));
    const int x = std::stoi(argv[4]), y = std::stoi(argv[5]);
    const Lot* l = w->hood.lot(lot);
    if (!l)
        throw std::runtime_error("no lot " + std::string(argv[3]));
    int front = argc > 6 ? std::stoi(argv[6]) : -1;
    if (front < 0)
        if (auto f = findFront(w->place, l->info, x, y, lot))
            front = f->first;
    const char* forbidden = std::getenv("OS2_DATA");
    return reportLot(moveLot(argv[2], w->place, lot, x, y, std::max(front, 0), forbidden ? forbidden : ""));
}

int cmdLotBin(const char* path) {
    const auto w = loadWorld(path);
    const auto dir = binOf(*w);
    const auto lots = loadLotBin(dir);
    std::printf("%s: %zu lots\n", dir.string().c_str(), lots.size());
    for (size_t i = 0; i < lots.size(); ++i)
        std::printf("  %zu cx_%08u %ux%u roads %u front %u  %lld  %s%s%s\n", i, lots[i].catalogId, lots[i].base.width,
                    lots[i].base.depth, lots[i].base.roads, lots[i].base.creationFrontEdge,
                    (long long)lots[i].price, lots[i].base.name.c_str(), lots[i].familyId ? "  family: " : "",
                    lots[i].familyName.c_str());
    return 0;
}

} // namespace

const char* hoodPlayUsage() {
    return "       s2pkg hood-lots <neighborhood-dir>  (prices, family bin)\n"
           "       s2pkg lot-value <lot.package>...  (the price the neighborhood shows, from the SIMI)\n"
           "       s2pkg hood-move-in <neighborhood-dir> <family> <lot>   (writes the folder: use the user's copy)\n"
           "       s2pkg hood-move-out <neighborhood-dir> <lot>\n"
           "       s2pkg hood-bulldoze <neighborhood-dir> <lot>\n"
           "       s2pkg hood-lot-bin <neighborhood-dir>   (the player's Lots & Houses bin; $OS2_DATA fills it)\n"
           "       s2pkg hood-lot-check <neighborhood-dir>   (every lot's placement checked where it stands)\n"
           "       s2pkg hood-lot-spots <neighborhood-dir> empty:<0-9>|bin:<n>|lot:<id>\n"
           "       s2pkg hood-place-lot <neighborhood-dir> empty:<0-9>|bin:<n> <x> <y> [rotation|front]\n"
           "       s2pkg hood-move-lot <neighborhood-dir> <lot> <x> <y> [front]\n"
           "       s2pkg hood-lot-to-bin <neighborhood-dir> <lot>\n"
           "       s2pkg hood-decor <neighborhood-dir> list | add <guid> <x> <y> [rotation] | remove flora|prop <index>   ($OS2_DATA: the catalog)\n";
}

int hoodPlayCommand(const std::string& cmd, int argc, char** argv) {
    const char* game = std::getenv("OS2_GAME_DIR"); // refused as a destination when set
    const std::filesystem::path forbidden = game ? game : "";
    if (cmd == "lot-value" && argc >= 3)
        return cmdLotValue(argc, argv);
    if (cmd == "hood-lots" && argc == 3)
        return cmdLots(argv[2]);
    if (cmd == "hood-move-in" && argc == 5)
        return report(moveIn(argv[2], uint32_t(std::stoul(argv[3])), uint32_t(std::stoul(argv[4])), forbidden));
    if (cmd == "hood-move-out" && argc == 4)
        return report(moveOut(argv[2], uint32_t(std::stoul(argv[3])), forbidden));
    if (cmd == "hood-bulldoze" && argc == 4)
        return report(bulldoze(argv[2], uint32_t(std::stoul(argv[3])), forbidden));
    if (cmd == "hood-lot-bin" && argc == 3)
        return cmdLotBin(argv[2]);
    if (cmd == "hood-lot-check" && argc == 3) {
        // Every lot checked where it stands, as if moved there.
        const auto w = loadWorld(argv[2]);
        size_t ok = 0;
        for (const auto& l : w->hood.lots) {
            const Placement p = checkLot(w->place, l.info, l.id, int(l.info.x), int(l.info.y), l.info.frontEdge);
            ok += p.ok;
            std::printf("  lot %u: %s", l.id, p.ok ? "ok" : p.why.c_str());
            if (p.ok)
                std::printf(" (road height %.3f, lot elevation %.3f)", p.heightAt(l.info.frontEdge), l.info.heightOffset);
            std::printf("\n");
        }
        std::printf("%zu of %zu lots pass where they stand\n", ok, w->hood.lots.size());
        return 0;
    }
    if (cmd == "hood-lot-spots" && argc == 4)
        return cmdLotSpots(argv[2], argv[3]);
    if (cmd == "hood-decor" && argc >= 4) {
        const auto w = loadWorld(argv[2]);
        if (w->gameRes.empty())
            throw std::runtime_error("hood-decor needs $OS2_DATA");
        ResourceIndex index(w->gameRes);
        const auto catalog = loadDecorCatalog(index);
        const std::string sub = argv[3];
        if (sub == "list") {
            for (const auto& p : catalog)
                std::printf("  %-9s %5u  %-40s %-36s lot=%d road=%d surface=%d\n", p.sort.c_str(), p.guid, p.model.c_str(),
                            p.name.c_str(), p.inLot, p.onRoad, p.surface);
            std::printf("%zu products\n", catalog.size());
            return 0;
        }
        if (sub == "add" && argc >= 7) {
            const uint32_t guid = uint32_t(std::stoul(argv[4]));
            const auto it = std::find_if(catalog.begin(), catalog.end(), [&](const DecorProduct& p) { return p.guid == guid; });
            if (it == catalog.end())
                throw std::runtime_error("no such product");
            OccupantPosition at;
            const float x = std::stof(argv[5]), y = std::stof(argv[6]);
            at.position = {x, y, w->terrain.heightAt(x, y)};
            at.boundsMin = {x - 3.5f, y - 3.5f};
            at.boundsMax = {x + 3.5f, y + 3.5f};
            return reportDecor(addDecoration(argv[2], w->place, *it, at, argc > 7 ? std::stof(argv[7]) : 0.0f, forbidden));
        }
        if (sub == "remove" && argc == 6)
            return reportDecor(removeDecoration(argv[2], w->place, std::string(argv[4]) == "flora", std::stoul(argv[5]), forbidden));
        throw std::runtime_error("usage: s2pkg hood-decor <dir> list | add <guid> <x> <y> [rotation] | remove flora|prop <index>");
    }
    if (cmd == "hood-place-lot" && argc >= 6)
        return cmdPlaceLot(argc, argv);
    if (cmd == "hood-move-lot" && argc >= 6)
        return cmdMoveLot(argc, argv);
    if (cmd == "hood-lot-to-bin" && argc == 4) {
        const auto w = loadWorld(argv[2]);
        return reportLot(moveLotToBin(argv[2], uint32_t(std::stoul(argv[3])), binOf(*w), forbidden));
    }
    return -1;
}

} // namespace os2
