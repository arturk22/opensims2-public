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

// The roofs of a lot. Types 1-6 (the base game's) as the game builds them
// (lot_roof_game.*, docs/roof-fence-water-mac.md §1.5): a grid of heights per
// half tile, two triangles per cell, each hidden under a higher roof or
// picking its 5 x 5 atlas cell from its vertices' properties. The diagonal
// and pagoda types stay on the OpenTS2 port (faces of half-tile tiles cut
// where they meet). Units are tiles, Z up.
class LotRoofs {
public:
    explicit LotRoofs(const Lot& lot);
    ~LotRoofs();
    LotRoofs(LotRoofs&&) noexcept;
    LotRoofs& operator=(LotRoofs&&) noexcept;

    struct Meshes {
        MeshGroup top;   // atlas texture ("texturetop")
        MeshGroup edges; // the upper strip of the borders, RoofEdges ("textureedges")
        MeshGroup trim;  // the lower strip, RoofTrim ("texturetrim")
        MeshGroup under; // the underside ("textureunder")
    };
    Meshes build() const;

    // Height of the highest roof based on `level` over (x, y), less `offset`;
    // `fallback` where no roof covers the point. Attic walls rise to it.
    float heightAt(float x, float y, int level, float fallback, float offset = 0) const;

    // Catalog GUID of the roof pattern (the last roof's, as in the game);
    // the default "Classic" pattern when no roof names one.
    uint32_t pattern() const { return pattern_; }
    size_t count() const { return roofs_.size() + world_.roofs().size(); }
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
