#pragma once

// Which loading screen a scene gets (docs/loading-mac.md, docs/cinematics-
// mac.md §2.1: cTSWinProcLotIntro::onAttach): the game's start-up Reia
// 0x8DA3ADE7 for the main menu, and one of the lot intro Reias 0x8DA3ADE8-ED
// with an icon IMG (0x8DA3ADF0-F4) or a picture in the middle tile and a
// title under it, by case:
//
//   lot of a family        E8   the family's photo         "The <name> Family"
//   empty residential lot  E9   F2 (build tools)          the lot's name
//   community lot          EA   the image of an object of  the lot's name
//                               the lot (found during the load), else F1
//   neighborhood           EB   F3 (houses)                the neighborhood's name
//                                                          + U(0x84, 0x10) under it
//   back to the hood with  EB   the family's photo         "The <name> Family" (case 6)
//   a family (from CAS)
//   Tutorial               ED   F4                         U(0x84, 0x2C) + U(0x84, 0x10)
//                                                          (inferred: case 7 is the tutorial)
//   Create-a-Sim           EC   F0 (a family)              "Create A Family"
//
// (The base game's neighborhood type only; the expansions' icons are not used.)
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
    std::string name;        // the lot's name
    std::string familyName;  // the family's
    uint32_t familyId = 0;
    std::filesystem::path familyThumbs; // <hood>/Thumbnails/<prefix>_FamilyThumbnails.package
    // The neighborhood being entered (Scene::Kind::Neighborhood).
    std::string hoodName;   // CTSS 1 (empty: the Tutorial)
    bool tutorial = false;  // the Tutorial neighborhood
    bool withFamily = false; // case 6: back to the neighborhood with this family (familyId, familyName)
};

// The lot intro's middle tile, in the 820x620 picture (the glowing
// trapezoid spans x 221-596, y 195-413 in every frame).
constexpr float kLotIntroTileX = 409, kLotIntroTileY = 300, kLotIntroIconW = 250, kLotIntroIconH = 165;

// The title under the middle tile, centred (the game's text is a window of
// the intro; its place and font are inferred).
constexpr float kLotIntroTitleX = 409, kLotIntroTitleY = 470;

LoadingScreen loadingScreenFor(const std::filesystem::path& resDir, const Scene& scene, bool cas, const LotEntry& lot);

// An IMG of ui.package (group 0x499DB772) read from the package itself (not
// through a ResourceIndex, which the loading thread owns meanwhile).
std::optional<RgbaImage> readUiIcon(const std::filesystem::path& resDir, uint32_t instance);

} // namespace os2

namespace os2 {
// While a community lot's loading screen shows, the load looks for the image
// of one of the lot's objects (docs/cinematics-mac.md §2.1): the worker asks
// whether one is wanted and offers it (any thread), the screen takes it.
bool loadingIconWanted();
void offerLoadingIcon(RgbaImage image);
} // namespace os2
