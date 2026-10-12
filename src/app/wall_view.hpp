#pragma once

// The lot's wall and roof view (the puck's wall button and fan, Home/End,
// the floor the camera shows): which walls are cut down and which levels are
// drawn. The app rebuilds the lot model (buildLotModel with a LotModelCache)
// only when the view changes what is cut: the wall mode (with or without the
// roof), the level, or, in cutaway, the camera turning past a wall direction. Plain C++.

#include "formats/lot_model.hpp"

#include <array>
#include <vector>

namespace os2 {

struct Lot;

class WallView {
public:
    using Walls = LotViewOptions::Walls;

    // The game's wall modes (cITSMainToolbar::eWallMode, docs/ui-live-mac.md
    // 8c): 0 walls down, 1 cutaway, 2 up, 3 up with the roof; the roofs show
    // only in mode 3 (cTSMainToolbar::SetWallMode). A lot is entered in mode
    // 1, as the game does (InitForNewLot).
    int mode() const { return mode_; }
    void setMode(int mode) { mode_ = mode < 0 ? 0 : mode > 3 ? 3 : mode; }
    Walls walls() const { return walls(mode_); }
    bool roof() const { return mode_ == 3; }
    static Walls walls(int mode) { return mode <= 0 ? Walls::Down : mode == 1 ? Walls::Cutaway : Walls::Up; }
    // Home / End ("Paredes ... - Origem/Fim"): one mode up (down, cutaway,
    // up, roof) for +1 or down for -1, wrapping around (inferred: the
    // accelerator's handler was not traced).
    void cycle(int step) { mode_ = (mode_ + (step >= 0 ? 1 : 3)) % 4; }

    // A house on a foundation has its ground floor one level up from the
    // yard (the foundation's top is the next level): a camera on the yard's
    // level shows that floor too, as in the game. study() finds such levels
    // (call it again after the lot changed); shownLevel() is the level drawn
    // with the wall mode for the camera's level.
    void study(const Lot& lot);
    int shownLevel(int level) const;
    // The highest level with walls or floors (lotTopLevel): the floor
    // buttons go no higher, and there the roof shows.
    int topLevel() const { return topLevel_; }

    // The view for a camera looking along `look` (horizontal, into the
    // scene) at `level` (the lot's numbering).
    LotViewOptions options(std::array<float, 2> look, int level) const;
    // Whether a lot model built for `a` also shows `b`: the same mode, roof
    // and level, and in cutaway the same side of every wall direction (axes
    // and diagonals) facing the camera.
    static bool sameCut(const LotViewOptions& a, const LotViewOptions& b);
    // Whether a point (an object's or a Sim's placement) is drawn when the
    // camera shows `level`: below the floor of the next level up.
    static bool visible(const Lot& lot, int level, float x, float y, float z);

private:
    int mode_ = 1;
    int minLevel_ = 0, topLevel_ = 0;
    std::vector<bool> foundationBelow_; // per level from minLevel_: the next level up stands on foundations
};

} // namespace os2
