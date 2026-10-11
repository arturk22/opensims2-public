#pragma once

// The live-mode control panel as the game's UI scripts define it, drawn over
// the lot with the UI layer and wired to the lot's VM. The layout of every
// window comes from ui.package; what the game's window procedures do in
// code follows docs/ui-live-mac.md (Mac binary). Scripts:
//
//   0x4905F85B  the lot puck (mode buttons, camera, walls, funds) and the
//               family column above it (window 0x4BDEFFAB)
//   0x49060005  the live panel (cTSWinProcLivePanel): the selected Sim's
//               portrait, age, game speed and pause, clock, aspiration, mood
//               meter, the subpanel buttons and the panel's caps
//   0x49064905  the needs subpanel (cTSWinProcPanelMotives), one group per
//               age (0xA003 toddler, 0xA004 child, 0xA005 the rest)
//   0x4906500B  one motive bar (cTSWinProcMotiveBar): trend arrows, the
//               gradient fill bar (child 7) and the caption
//   0x49060053  a family member's thumbnail (cTSWinProcSimChooserThumbnail)
//   0x0609A1C0  an action queue icon (cTSWinActionQueueIcon), laid out by
//               the constants of 0xCCD02691 (cTSUIActionQueue)
//
// Where the game places the scripts' roots relative to each other is code
// that is not documented yet; those placements are inferred (see
// placePanels in live_ui.cpp and TODO.md).
//
// Interactive: the speed buttons and pause set the VM's Speed/Paused globals
// (the app's LotSimulation scales its clock by them), a family thumbnail
// selects that Sim (onSelectSim), a queue icon cancels its interaction on
// the press, the mode buttons other than Live do nothing.

#include "formats/ui_screen.hpp"
#include "vm/vm.hpp"

#include <simd/simd.h>

#include <array>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace MTL {
class Device;
class RenderCommandEncoder;
} // namespace MTL

namespace os2 {

class DbpfPackage;
class FontLibrary;
class ResourceIndex;
class UiLayer;
class UiResources;

// The colour of a gradient fill bar (cTSWinProcGradientFillBar,
// docs/ui-live-mac.md §5): the mask's size, each row (horizontal) or column
// (vertical) a copy of the gradient's row/column i = round(N (v - min) /
// (max - min)), limited to N - 1, alpha from the mask.
RgbaImage gradientFill(const RgbaImage& gradient, const RgbaImage& mask, float value, float min, float max,
                       bool vertical);

// The mood colour of a family thumbnail (docs/ui-live-mac.md §4.2): pixel
// (0, y) of the gradient, y = H - round(H (m + 100) / 200), limited to H - 1.
std::array<float, 4> moodColor(const RgbaImage& gradient, float mood);

// The trend arrows of a motive bar (docs/ui-live-mac.md §3.2): values taken
// every 500 ms, the running average of the last 10 kept too; the arrows
// count from the change between the newest and oldest averages.
class MotiveTrend {
public:
    void add(float value);
    void clear() { values_.clear(), averages_.clear(); }
    // -3..3: the number of arrows, negative for down.
    int arrows() const;

private:
    std::deque<int> values_, averages_;
};

class LiveUi {
public:
    // Over a VM with the lot loaded (not owned; it must outlive this).
    // `resDir` is TSData/Res (fonts, the object thumbnails).
    LiveUi(vm::VM& vm, ResourceIndex& index, const std::filesystem::path& resDir);
    ~LiveUi();

    // False if the scripts are missing (then nothing is drawn).
    bool ok() const { return ok_; }

    vm::ObjectId sim() const { return sim_; }
    void setSim(vm::ObjectId id);
    // A Sim's portrait (character file IMG 0x10): the panel, the family
    // thumbnails and the queue icons of interactions with that Sim.
    void setPortrait(vm::ObjectId id, RgbaImage portrait);
    // Called when the player picks another Sim in the family column.
    std::function<void(vm::ObjectId)> onSelectSim;

    // The real clock (seconds), for the trend arrows' 500 ms samples.
    void tick(double seconds);

    // Mouse in canvas pixels (origin top left). press: the left (or right)
    // button down; returns whether the press was the panel's (then the
    // world below is not picked).
    bool press(simd_float2 p);
    void hover(simd_float2 p);
    // The canvas size clicks are laid out against before the first frame (headless).
    void setCanvas(int width, int height) { canvasW_ = width, canvasH_ = height, dirty_ = true; }

    // The family members shown (the Sims of the lot's current family, or
    // every Sim when none is in it).
    std::vector<vm::ObjectId> family() const;

    // --- Drawing -------------------------------------------------------------
    UiScreen screen(int width, int height);
    void attach(MTL::Device* device, uint32_t colorFormat, uint32_t depthFormat);
    // Rebuilds the overlay when what it shows changed (canvas = the drawable
    // size divided by `scale`) and encodes it.
    void encode(MTL::RenderCommandEncoder* enc, uint32_t width, uint32_t height, float scale);

private:
    enum class Action { None, SelectSim, Speed, Pause, Cancel, Mode };
    struct Target {
        std::array<float, 4> rect; // x0, y0, x1, y1
        Action action = Action::None;
        int value = 0;
    };
    const RgbaImage* image(uint32_t instance);
    const RgbaImage* objectThumbnail(const vm::Object& o);
    const RgbaImage* queueIcon(const vm::QueuedInteraction& q, bool running);
    uint64_t stamp() const;
    void layoutIfNeeded();

    vm::VM* vm_;
    std::unique_ptr<UiResources> res_;
    std::unique_ptr<FontLibrary> fonts_;
    std::unique_ptr<DbpfPackage> thumbnails_;
    bool ok_ = false;
    UiScript puck_, live_, motives_, bar_, member_, icon_;
    std::map<std::string, std::string> queueConstants_;
    std::array<UiScript, 8> bars_; // a copy of the motive bar per motive (its caption)

    vm::ObjectId sim_ = 0;
    std::map<vm::ObjectId, std::unique_ptr<RgbaImage>> portraits_;
    std::map<uint32_t, std::unique_ptr<RgbaImage>> objectThumbs_; // by GUID (null: none)
    std::map<std::pair<vm::ObjectId, bool>, std::unique_ptr<RgbaImage>> icons_; // masked (and grey when waiting)
    std::vector<std::unique_ptr<RgbaImage>> frameImages_; // fills made for the current screen

    std::array<MotiveTrend, 8> trends_;
    double lastSample_ = -1;
    std::vector<Target> targets_;
    std::vector<std::array<float, 4>> solid_; // panel areas that take clicks
    int hoverIcon_ = -1;                      // queue icon under the mouse
    std::optional<simd_float2> mouse_;
    std::set<std::pair<vm::ObjectId, uint16_t>> cancelled_; // (Sim, queue uid) cancelled from the panel

    std::array<std::unique_ptr<UiLayer>, 4> layers_;
    size_t layer_ = 0;
    bool dirty_ = true;
    int canvasW_ = 0, canvasH_ = 0, laidW_ = 0, laidH_ = 0;
    uint64_t stamp_ = 0;
};

} // namespace os2
