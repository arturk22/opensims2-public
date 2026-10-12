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
#include "app/options_panel.hpp"
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
    void setForceRenderHeads(bool on) { forceRenderHeads = on; }
    // The dialog's "Informações" panel (headless: --hood-info [member]).
    void showInfo(bool on, size_t member = 0) { advanced_ = on, member_ = member, dirty_ = true; }
    // The story panel (F4; headless: --hood-story).
    void showStory(bool on, bool edit = false) { story_ = on, storyEdit_ = edit, dirty_ = true; }
    bool storyOpen() const { return story_; }
    // The decorations' pictures for the products the bin shows (call when
    // the panel opens or its page changes, never while drawing); a
    // product without a UI image is drawn by `renderDecorThumb`.
    void prefetchDecorations();
    std::function<std::optional<RgbaImage>(const hood::DecorProduct&)> renderDecorThumb;
    // The "Transferir Lote" dialog (headless: --hood-package).
    void showPackage(bool on) { packaging_ = on, dirty_ = true; }
    bool packageOpen() const { return packaging_; }

    enum class Action { None, Consumed, EnterLot, Close, ZoomIn, ZoomOut, RotateLeft, RotateRight, MainMenu,
                        Families, Lots, Bins, Options, Info, Member, Friends, Package, Decorations, Story, StoryClose, StoryEdit };
    // The Families and Lots & Houses panels (app/hood_bins.hpp); after
    // Action::Bins, binRequest() says what they asked for.
    HoodBins& bins() { return *bins_; }
    // The option windows the Options panel opens (app/options_panel.hpp).
    OptionsPanel& options() { return *options_; }
    const HoodBins::Request& binRequest() const { return binRequest_; }
    // A left click (canvas pixels): on the dialog or the HUD it is the UI's.
    Action press(simd_float2 p);
    // Renders a head shot for a character file without one (set by the app:
    // app/head_shot.hpp); called when a lot's dialog opens, not while drawing.
    std::function<std::optional<RgbaImage>(const std::filesystem::path&)> renderHead;
    // UI sounds by name (the buttons' own: their "clicksound" attribute).
    std::function<void(const std::string&)> playSound;
    void hover(simd_float2 p);
    // Whether the HUD, a panel or the dialog is under `p` (canvas pixels): a lot behind it is not hovered.
    bool covers(simd_float2 p);
    void setCanvas(int width, int height) { canvasW_ = width, canvasH_ = height, dirty_ = true; }
    void refreshLayout() { dirty_ = true; }
    // The puck's accelerators (their tips: "Famílias - F1", "Lotes & Casas -
    // F2", "Opções - F5"): open or close that panel.
    void togglePanel(HoodBins::Panel p) {
        bins_->togglePanel(p), dirty_ = true;
        if (p == HoodBins::Panel::Decorations && bins_->panel() == p)
            prefetchDecorations();
    }
    void toggleStory() { story_ = !story_, storyEdit_ = false, dirty_ = true; }

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
    std::unique_ptr<OptionsPanel> options_;
    HoodBins::Request binRequest_;
    std::unique_ptr<UiResources> res_;
    std::unique_ptr<FontLibrary> fonts_;
    UiScript hud_, hoverScript_, dialogScript_, packageScript_, storyScript_;
    // "História da Vizinhança" (F4, 0x49001030): the picture and the description
    // of the neighborhood (CTSS), or "no story"; Pronto closes it. The
    // story's own pages (the game's Storytelling folder) are not read.
    bool story_ = false, storyEdit_ = false;
    std::unique_ptr<RgbaImage> storyPicture_;
    bool storyPictureLoaded_ = false;
    // Decorations' pictures by product: UI images (effects) or renders.
    std::map<uint32_t, std::unique_ptr<RgbaImage>> decorThumbs_;
    const RgbaImage* decorThumb(uint32_t guid) const;
    std::string text(uint32_t table, uint32_t index, const char* fallback) const;
    // "Transferir Lote" (EFFF4500): the dialog the Lot Info Dialog's button opens; the
    // pick (1 TheSims2.com, 2 a file) is only remembered, nothing is exported.
    bool packaging_ = false;
    int packagePick_ = 2;
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
        int value = 0;     // Member: the member's index
    };
    // The Lot Info Dialog's "Informações" (B004/C004): the advanced panel
    // C000 of an occupied lot (the members' head shots, name, aspiration).
    bool advanced_ = false;
    size_t member_ = 0;
    const RgbaImage* memberThumb(const std::filesystem::path& character);
    std::map<std::string, std::unique_ptr<RgbaImage>> memberThumbs_;
    bool forceRenderHeads = false; // test aid (--hood-render-heads): never read the files' own head shots
    bool renderHeads_ = false; // only while a lot opens, never inside a frame
    std::vector<Target> targets_;
    std::vector<std::array<float, 4>> solid_; // areas that take clicks
    int canvasW_ = 1024, canvasH_ = 768, laidW_ = 0, laidH_ = 0;
    bool dirty_ = true;
    std::array<std::unique_ptr<UiLayer>, 4> layers_;
    size_t layer_ = 0;
};

} // namespace os2
