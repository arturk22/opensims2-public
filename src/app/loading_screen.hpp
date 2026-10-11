#pragma once

// Which loading screen a scene gets (docs/loading-mac.md): the game's
// start-up Reia 0x8DA3ADE7 for the main menu, and one of the lot intro
// Reias 0x8DA3ADE8-ED with an icon IMG (0x8DA3ADF0-F4) in the middle tile
// for the rest. The game's case -> animation map (cTSWinProcLotIntro::
// onAttach) was not followed; the choice is by what the animations and
// icons show (inferred):
//
//   lot of a family        E8 (Sims, social icons)      + F0 (a family)
//   empty residential lot  E9 (build and garden icons)  + F2 (build tools)
//   community lot          EA (furniture, plants)       + F1 (the taxi)
//   neighborhood           ED (houses, plants)          + F3 (houses)
//   Create-a-Sim           EC (clothes)                 + F0 (a family)
//
// Plain C++.

#include "app/game_flow.hpp"
#include "app/movie_host.hpp"

#include <filesystem>
#include <optional>

namespace os2 {

// What is known of the lot being entered (from the neighborhood's lot info).
struct LotEntry {
    bool known = false;
    uint8_t type = 0;    // 0 residential, 1 community
    bool family = false; // a family lives there
};

// The lot intro's middle tile, in the 820x620 picture (the glowing
// trapezoid spans x 221-596, y 195-413 in every frame).
constexpr float kLotIntroTileX = 409, kLotIntroTileY = 300, kLotIntroIconW = 250, kLotIntroIconH = 165;

LoadingScreen loadingScreenFor(const std::filesystem::path& resDir, const Scene& scene, bool cas, const LotEntry& lot);

// An IMG of ui.package (group 0x499DB772) read from the package itself (not
// through a ResourceIndex, which the loading thread owns meanwhile).
std::optional<RgbaImage> readUiIcon(const std::filesystem::path& resDir, uint32_t instance);

} // namespace os2
