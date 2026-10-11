// s2pkg commands for playing a neighborhood (formats/hood_moves.hpp):
// lot prices, the family bin, moving in and out, bulldozing.
#include "hood_play_cmds.hpp"

#include "formats/dbpf.hpp"
#include "formats/hood_moves.hpp"
#include "formats/lot_objects.hpp"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>

namespace os2 {

namespace {

using namespace hood;

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

} // namespace

const char* hoodPlayUsage() {
    return "       s2pkg hood-lots <neighborhood-dir>  (prices, family bin)\n"
           "       s2pkg hood-move-in <neighborhood-dir> <family> <lot>   (writes the folder: use the user's copy)\n"
           "       s2pkg hood-move-out <neighborhood-dir> <lot>\n"
           "       s2pkg hood-bulldoze <neighborhood-dir> <lot>\n";
}

int hoodPlayCommand(const std::string& cmd, int argc, char** argv) {
    const char* game = std::getenv("OS2_GAME_DIR"); // refused as a destination when set
    const std::filesystem::path forbidden = game ? game : "";
    if (cmd == "hood-lots" && argc == 3)
        return cmdLots(argv[2]);
    if (cmd == "hood-move-in" && argc == 5)
        return report(moveIn(argv[2], uint32_t(std::stoul(argv[3])), uint32_t(std::stoul(argv[4])), forbidden));
    if (cmd == "hood-move-out" && argc == 4)
        return report(moveOut(argv[2], uint32_t(std::stoul(argv[3])), forbidden));
    if (cmd == "hood-bulldoze" && argc == 4)
        return report(bulldoze(argv[2], uint32_t(std::stoul(argv[3])), forbidden));
    return -1;
}

} // namespace os2
