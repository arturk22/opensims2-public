// The terrain under the lots (hood_lot_terrain.hpp).

#include "hood_lot_terrain.hpp"

#include <cmath>
#include <set>

namespace os2::hood {

size_t flattenTerrainUnderLots(Terrain& terrain, const std::vector<Lot>& lots, const Occupants& occupants) {
    const int gw = int(terrain.gridWidth), gh = int(terrain.gridHeight);
    if (gw <= 0 || gh <= 0 || terrain.heights.size() != size_t(gw) * size_t(gh))
        return 0;
    // Vertices of road tiles.
    std::set<std::pair<int, int>> road;
    for (const auto& r : occupants.roads) {
        const int x0 = int(std::floor(r.at.boundsMin[0] / kHoodCellSize + 0.01f));
        const int y0 = int(std::floor(r.at.boundsMin[1] / kHoodCellSize + 0.01f));
        const int x1 = int(std::ceil(r.at.boundsMax[0] / kHoodCellSize - 0.01f));
        const int y1 = int(std::ceil(r.at.boundsMax[1] / kHoodCellSize - 0.01f));
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x)
                road.insert({x, y});
    }
    auto inside = [](const Lot& l, int x, int y) {
        return x >= int(l.info.x) && x <= int(l.info.x + l.info.width) && y >= int(l.info.y) &&
               y <= int(l.info.y + l.info.depth);
    };
    size_t moved = 0;
    for (const auto& l : lots) {
        const int x0 = int(l.info.x), y0 = int(l.info.y);
        const int x1 = x0 + int(l.info.width), y1 = y0 + int(l.info.depth);
        if (x0 < 0 || y0 < 0 || x1 >= gw || y1 >= gh || (l.info.flags & LotInfo::kFlagBeach))
            continue; // beach lots keep their ground
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                // Only this lot within one tile of the vertex.
                bool alone = true;
                for (const auto& o : lots)
                    if (&o != &l)
                        for (int dy = -1; dy <= 1 && alone; ++dy)
                            for (int dx = -1; dx <= 1 && alone; ++dx)
                                if (inside(o, x + dx, y + dy))
                                    alone = false;
                if (!alone || road.count({x, y}))
                    continue;
                float& h = terrain.heights[size_t(y) * size_t(gw) + size_t(x)];
                if (h != l.info.heightOffset)
                    h = l.info.heightOffset, ++moved;
            }
    }
    return moved;
}

} // namespace os2::hood
