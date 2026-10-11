#pragma once

// The lot's wall and roof view (the puck's wall button and fan, Home/End,
// the floor the camera shows): which walls are cut down and which levels are
// drawn. The app rebuilds the lot model (buildLotModel with a LotModelCache)
// only when the view changes what is cut: the wall mode, the roof toggle, the
// level, or, in cutaway, the camera turning past a wall direction. Plain C++.

#include "formats/lot_model.hpp"

#include <array>
#include <vector>

namespace os2 {

struct Lot;

class WallView {
public:
    using Walls = LotViewOptions::Walls;

    // A lot is entered with walls cutaway, as the game does.
    Walls walls() const { return walls_; }
    void setWalls(Walls w) { walls_ = w; }
    // Home / End ("Paredes ... - Origem/Fim"): one mode up (down, cutaway,
    // up) for +1 or down for -1, wrapping around.
    void cycle(int step);
    bool roof() const { return roof_; }
    void setRoof(bool on) { roof_ = on; }
    // The puck's numbering: 0 down, 1 cutaway, 2 up.
    int puckMode() const { return int(walls_ == Walls::Down ? 0 : walls_ == Walls::Cutaway ? 1 : 2); }
    static Walls fromPuck(int mode) { return mode <= 0 ? Walls::Down : mode == 1 ? Walls::Cutaway : Walls::Up; }

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
    Walls walls_ = Walls::Cutaway;
    bool roof_ = true;
    int minLevel_ = 0, topLevel_ = 0;
    std::vector<bool> foundationBelow_; // per level from minLevel_: the next level up stands on foundations
};

} // namespace os2
