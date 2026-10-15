#include "wall_view.hpp"

#include "formats/lot.hpp"

#include <algorithm>
#include <cmath>

namespace os2 {

void WallView::study(const Lot& lot) {
    minLevel_ = lot.minLevel;
    topLevel_ = lotTopLevel(lot);
    foundationBelow_.assign(size_t(std::max(lot.levelCount, 0)), false);
    // A level whose floors sit less than a story's half over the level
    // below (inferred: foundations are 1 tile, a story 3) is that level's
    // raised ground floor.
    for (int level = lot.minLevel; level + 1 < lot.minLevel + lot.levelCount; ++level) {
        int floors = 0, low = 0;
        for (uint32_t x = 0; x < lot.width; ++x)
            for (uint32_t y = 0; y < lot.height; ++y) {
                if (lot.floor(level + 1, x, y) == std::array<uint16_t, 4>{})
                    continue;
                ++floors;
                const float step = lot.elevation(level + 1, int(x), int(y)) - lot.elevation(level, int(x), int(y));
                low += step < 1.5f;
            }
        foundationBelow_[size_t(level - lot.minLevel)] = floors > 0 && low * 2 > floors;
    }
}

int WallView::shownLevel(int level) const {
    const int i = level - minLevel_;
    if (level == LotViewOptions::kAllLevels || i < 0 || size_t(i) >= foundationBelow_.size())
        return level;
    return foundationBelow_[size_t(i)] ? level + 1 : level;
}

int WallView::lowestCut(int level) const {
    if (level != LotViewOptions::kAllLevels && level - 1 >= minLevel_ && shownLevel(level - 1) == level)
        return level - 1;
    return LotViewOptions::kAllLevels;
}

LotViewOptions WallView::options(std::array<float, 2> look, int level) const {
    LotViewOptions v;
    v.walls = walls();
    v.roof = roof();
    v.level = level;
    // The yard of a house on foundations, shown with the house's floor: its
    // walls take the mode too.
    v.lowestCut = lowestCut(level);
    if (std::hypot(look[0], look[1]) > 1e-3f)
        v.viewDirection = look;
    return v;
}

bool WallView::sameCut(const LotViewOptions& a, const LotViewOptions& b) {
    // In cutaway the app also compares what is cut (lotCutawayWalls), which
    // follows the camera and the selected Sim.
    return a.walls == b.walls && a.roof == b.roof && a.level == b.level && a.lowestCut == b.lowestCut;
}

bool WallView::visible(const Lot& lot, int level, float x, float y, float z, int lowestCut) {
    if (level == LotViewOptions::kAllLevels)
        return true;
    x = std::clamp(x, 0.0f, float(lot.width)), y = std::clamp(y, 0.0f, float(lot.height));
    // Shown with the level below: where the level is the next story of a
    // building on the ground (storyOverGround) what stands on its floor is
    // not drawn (what hangs on its walls goes with the walls:
    // LotModelCache::cut); not over a tile without a floor of that level.
    if (lowestCut < level && lot.hasLevel(level) && z >= lot.elevationAt(level, x, y) - 0.5f) {
        const uint32_t tx = std::min(uint32_t(x), lot.width - 1), ty = std::min(uint32_t(y), lot.height - 1);
        if (lot.floor(level, tx, ty) != std::array<uint16_t, 4>{} && storyOverGround(lot, level, int(tx), int(ty)))
            return false;
    }
    if (!lot.hasLevel(level + 1))
        return true;
    // Half a story of slack: wall shelves and tall objects stay with their floor.
    return z < lot.elevationAt(level + 1, x, y) - 0.5f;
}

} // namespace os2
