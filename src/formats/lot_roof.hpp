// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Port of OpenTS2's roof geometry (Assets/Scripts/OpenTS2/Scenes/Lot/Roof/,
// MPL-2.0): https://github.com/LazyDuchess/OpenTS2
#pragma once

#include "gmdc.hpp"
#include "lot_roof_game.hpp"

#include <memory>
#include <string>
#include <vector>

namespace os2 {

struct Lot;

// The roofs of a lot, built the way the game draws them: every roof face is a
// grid of half-tile roof tiles that pick their picture from the roof
// pattern's 5 x 5 texture atlas (plain, ridge, hip and valley pieces), cut
// where the faces of different roofs meet. Units are tiles, Z up.
class LotRoofs {
public:
    explicit LotRoofs(const Lot& lot);
    ~LotRoofs();
    LotRoofs(LotRoofs&&) noexcept;
    LotRoofs& operator=(LotRoofs&&) noexcept;

    struct Meshes {
        MeshGroup top;   // atlas texture ("texturetop")
        MeshGroup edges; // the roof's thickness along its borders ("textureedges")
        MeshGroup trim;  // strip along the top of the borders ("texturetrim")
        MeshGroup under; // the underside ("textureunder")
    };
    Meshes build() const;

    // Height of the highest roof based on `level` over (x, y), less `offset`;
    // `fallback` where no roof covers the point. Attic walls rise to it.
    float heightAt(float x, float y, int level, float fallback, float offset = 0) const;

    // Catalog GUID of the roof pattern (the last roof's, as in the game);
    // the default "Classic" pattern when no roof names one.
    uint32_t pattern() const { return pattern_; }
    size_t count() const { return roofs_.size(); }
    std::vector<std::string> warnings;

    // The lot's roofs of types 1-6 as the game's nTSWorld::cWorldRoof holds
    // them (docs/roof-fence-water-mac.md §1.5).
    const roofgame::World& world() const { return world_; }

    struct Roof;

private:
    std::vector<std::unique_ptr<Roof>> roofs_; // OpenTS2 port: the diagonal and pagoda types
    roofgame::World world_;
    uint32_t pattern_ = 0;
};

} // namespace os2
