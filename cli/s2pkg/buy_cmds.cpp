// s2pkg catalog: the Buy-mode catalog (formats/catalog.hpp).

#include "buy_cmds.hpp"

#include "formats/catalog.hpp"
#include "formats/image.hpp"
#include "formats/resindex.hpp"

#include <cstdio>
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

} // namespace

int buyCommand(const std::string& cmd, int argc, char** argv) {
    if (cmd == "catalog")
        return cmdCatalog({argv + 2, argv + argc});
    return -1;
}

const char* buyUsage() {
    return "       s2pkg catalog <Res-dir> [--function bit [--sub bit] | --room bit [--sub function-bit]] [-v]\n"
           "       s2pkg catalog <Res-dir> --thumb <guid|name> <out.png>\n";
}

} // namespace os2
