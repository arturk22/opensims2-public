#pragma once

// The neighborhood view's UI as ui.package's scripts define it, drawn with
// the UI layer over the neighborhood (app/hood_view.hpp):
//
//   0x49000000  the neighborhood HUD: the puck (window 0x4BE6ED7D: families,
//               lots, decorations, story, options, camera) at the bottom
//               left, the chooser (0x4BE6ED7E: main menu button and the
//               city name, text 0x3) at the top left (OpenTS2's
//               NeighborhoodHUD anchors them so)
//   0x49001011  "occupied lot catalog description": shown next to the mouse
//               over a lot (panel 0xA000 for an occupied lot: family name,
//               lot name, size, the family's bio and the lot's picture;
//               0xB000 otherwise)
//   0x49001000  "Lot Info Dialog", opened by clicking a lot: panel 0xB000
//               ("Basic /no bio") for an occupied lot (family snapshot,
//               family name, lot name, Enter Lot 0xB002, Close 0xB00A),
//               0xD000 for an empty one (For Sale, Enter Lot 0xD005, Close
//               0xD007), 0xE000 for a community lot (Enter 0xE005, Close
//               0xE007)
//
// Which popup the game shows on hover and how it places it are inferred
// (the window procedures are not decoded); the dialog is centred. Buttons
// other than Enter Lot and Close do nothing yet.

#include "formats/image.hpp"
#include "formats/ui_screen.hpp"
#include "formats/ui_script.hpp"

#include <simd/simd.h>

#include <array>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace MTL {
class Device;
class RenderCommandEncoder;
} // namespace MTL

namespace os2 {

class FontLibrary;
class HoodView;
class ResourceIndex;
class UiLayer;
class UiResources;
struct HoodLot;

class HoodUi {
public:
    HoodUi(const HoodView& view, ResourceIndex& index, const std::filesystem::path& resDir);
    ~HoodUi();

    // The lot under the mouse (null: none) and the mouse (canvas pixels).
    void hoverLot(const HoodLot* lot, simd_float2 mouse);
    // Opens the Lot Info Dialog of a lot (null closes it).
    void openLot(const HoodLot* lot);
    const HoodLot* dialogLot() const { return dialog_; }

    enum class Action { None, Consumed, EnterLot, Close };
    // A left click (canvas pixels): on the dialog or the HUD it is the UI's.
    Action press(simd_float2 p);
    void hover(simd_float2 p);
    void setCanvas(int width, int height) { canvasW_ = width, canvasH_ = height, dirty_ = true; }

    UiScreen screen(int width, int height);
    void attach(MTL::Device* device, uint32_t colorFormat, uint32_t depthFormat);
    void encode(MTL::RenderCommandEncoder* enc, uint32_t width, uint32_t height, float scale);

private:
    const RgbaImage* familySnapshot(uint32_t family);
    const RgbaImage* lotPicture(const HoodLot& lot);

    const HoodView* view_;
    std::unique_ptr<UiResources> res_;
    std::unique_ptr<FontLibrary> fonts_;
    UiScript hud_, hoverScript_, dialogScript_;
    bool ok_ = false;
    std::filesystem::path familyThumbs_;
    std::map<uint32_t, std::unique_ptr<RgbaImage>> snapshots_; // by family (null: none)
    std::map<uint32_t, std::unique_ptr<RgbaImage>> pictures_;  // by lot (null: none)

    const HoodLot* hover_ = nullptr;
    simd_float2 mouse_{0, 0};
    const HoodLot* dialog_ = nullptr;
    struct Target {
        std::array<float, 4> rect;
        Action action;
    };
    std::vector<Target> targets_;
    std::vector<std::array<float, 4>> solid_; // areas that take clicks
    int canvasW_ = 1024, canvasH_ = 768, laidW_ = 0, laidH_ = 0;
    bool dirty_ = true;
    std::array<std::unique_ptr<UiLayer>, 4> layers_;
    size_t layer_ = 0;
};

} // namespace os2
