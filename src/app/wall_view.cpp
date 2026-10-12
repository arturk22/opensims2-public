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

LotViewOptions WallView::options(std::array<float, 2> look, int level) const {
    LotViewOptions v;
    v.walls = walls();
    v.roof = roof();
    v.level = level;
    // The yard of a house on foundations, shown with the house's floor: its
    // walls take the mode too.
    if (level != LotViewOptions::kAllLevels && level - 1 >= minLevel_ && shownLevel(level - 1) == level)
        v.lowestCut = level - 1;
    if (std::hypot(look[0], look[1]) > 1e-3f)
        v.viewDirection = look;
    return v;
}

bool WallView::sameCut(const LotViewOptions& a, const LotViewOptions& b) {
    if (a.walls != b.walls || a.roof != b.roof || a.level != b.level)
        return false;
    if (a.walls != Walls::Cutaway)
        return true;
    // buildLotModel cuts a wall by the sign of its normal along the view
    // (|dot| > 1e-3): walls run along the axes and the diagonals.
    static const float kNormals[4][2] = {{1, 0}, {0, 1}, {1, 1}, {1, -1}};
    auto side = [](const float n[2], const std::array<float, 2>& d) {
        const float len = std::hypot(n[0], n[1]);
        const float f = (n[0] * d[0] + n[1] * d[1]) / len;
        return std::abs(f) <= 1e-3f ? 0 : f > 0 ? 1 : -1;
    };
    for (const auto& n : kNormals)
        if (side(n, a.viewDirection) != side(n, b.viewDirection))
            return false;
    return true;
}

bool WallView::visible(const Lot& lot, int level, float x, float y, float z) {
    if (level == LotViewOptions::kAllLevels || !lot.hasLevel(level + 1))
        return true;
    x = std::clamp(x, 0.0f, float(lot.width)), y = std::clamp(y, 0.0f, float(lot.height));
    // Half a story of slack: wall shelves and tall objects stay with their floor.
    return z < lot.elevationAt(level + 1, x, y) - 0.5f;
}

} // namespace os2
