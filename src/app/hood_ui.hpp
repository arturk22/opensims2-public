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
// (the window procedures are not decoded); the dialog is centred. The puck's
// Families and Lots & Houses buttons open their panels (app/hood_bins.hpp);
// the zoom and rotate buttons move the camera. The lot's value (hover
// popup) and the family's net worth (dialog) come from the lot file's SIMI.

#include "app/hood_bins.hpp"
#include "formats/image.hpp"
#include "formats/ui_screen.hpp"
#include "formats/ui_script.hpp"

#include <simd/simd.h>

#include <array>
#include <filesystem>
#include <functional>
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

    // After the neighborhood changed (a family moved, a lot went): forgets
    // the lot values and pointers into the view's lots.
    void refresh();
    // The lot under the mouse (null: none) and the mouse (canvas pixels).
    void hoverLot(const HoodLot* lot, simd_float2 mouse);
    // Opens the Lot Info Dialog of a lot (null closes it).
    void openLot(const HoodLot* lot);
    const HoodLot* dialogLot() const { return dialog_; }

    enum class Action { None, Consumed, EnterLot, Close, ZoomIn, ZoomOut, RotateLeft, RotateRight, MainMenu,
                        Families, Lots, Bins };
    // The Families and Lots & Houses panels (app/hood_bins.hpp); after
    // Action::Bins, binRequest() says what they asked for.
    HoodBins& bins() { return *bins_; }
    const HoodBins::Request& binRequest() const { return binRequest_; }
    // A left click (canvas pixels): on the dialog or the HUD it is the UI's.
    Action press(simd_float2 p);
    // UI sounds by name (the buttons' own: their "clicksound" attribute).
    std::function<void(const std::string&)> playSound;
    void hover(simd_float2 p);
    // Whether the HUD, a panel or the dialog is under `p` (canvas pixels): a lot behind it is not hovered.
    bool covers(simd_float2 p);
    void setCanvas(int width, int height) { canvasW_ = width, canvasH_ = height, dirty_ = true; }

    UiScreen screen(int width, int height);
    void attach(MTL::Device* device, uint32_t colorFormat, uint32_t depthFormat);
    void encode(MTL::RenderCommandEncoder* enc, uint32_t width, uint32_t height, float scale);

private:
    const RgbaImage* familySnapshot(uint32_t family);
    const RgbaImage* lotPicture(const HoodLot& lot);
    // The values in a lot's SIMI (docs/save-mac.md §5): the lot, the
    // family's objects and the lot's objects (cached; ok = false without a lot file).
    struct LotValues {
        bool ok = false;
        int64_t lot = 0, familyObjects = 0, lotObjects = 0;
    };
    const LotValues& lotValues(const HoodLot& lot);
    // The family's net worth ("Patrimônio"): its funds, the lot and its objects.
    int64_t netWorth(const HoodLot& lot);
    std::map<uint32_t, LotValues> values_; // by lot

    const HoodView* view_;
    std::unique_ptr<HoodBins> bins_;
    HoodBins::Request binRequest_;
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
        std::string sound; // the button's click sound
    };
    std::vector<Target> targets_;
    std::vector<std::array<float, 4>> solid_; // areas that take clicks
    int canvasW_ = 1024, canvasH_ = 768, laidW_ = 0, laidH_ = 0;
    bool dirty_ = true;
    std::array<std::unique_ptr<UiLayer>, 4> layers_;
    size_t layer_ = 0;
};

} // namespace os2
