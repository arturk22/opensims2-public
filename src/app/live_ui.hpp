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

#include "app/ui_panel_util.hpp"
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
#include <tuple>
#include <vector>

namespace MTL {
class Device;
class RenderCommandEncoder;
} // namespace MTL

namespace os2 {

class DbpfPackage;
struct DbpfEntry;
class FontLibrary;
class ResourceIndex;
class UiLayer;
class UiResources;
class LivePanels;

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
    // The camera and a Sim (docs/ui-live-mac.md §4.3): a left click on the
    // selected Sim's thumbnail (or the panel's portrait) centres on it; a
    // right click on a thumbnail follows that Sim, again on the same Sim
    // stops (follow = 0).
    std::function<void(vm::ObjectId)> onCentreSim;
    std::function<void(vm::ObjectId)> onFollowSim;
    vm::ObjectId followed() const { return followed_; }
    // UI sounds by name (the lot audio's UI group): speeds, pause, queue,
    // panel buttons.
    std::function<void(const std::string&)> playSound;
    // The portrait of a Sim by the GUID of its OBJD (relationship
    // thumbnails of Sims not on the lot); empty if none.
    std::function<std::optional<RgbaImage>(uint32_t guid)> neighborPortrait;
    // The open subpanel (LivePanels::Panel: 0 needs, 1 relationships, 2
    // skills and career, 3 simology, 4 rewards), as its button does.
    void selectSubPanel(int panel);
    int subPanel() const { return subPanel_; }

    // The puck's mode buttons (0 live, 1 buy, 2 build, 3 story, 4 options):
    // the one shown pressed; outside live mode only the puck and the family
    // column are drawn (Buy mode draws its panel itself, app/buy_mode.hpp).
    // onMode is called when the player presses one.
    void setMode(int mode) { mode_ = mode, dirty_ = true; }
    int mode() const { return mode_; }
    std::function<void(int)> onMode;

    // The speed buttons (docs/ui-live-mac.md §1.3): simulator speed 2-4
    // (unpauses), pause toggled.
    void setSpeed(int speed);
    void togglePause();

    // The real clock (seconds), for the trend arrows' 500 ms samples.
    void tick(double seconds);

    // Mouse in canvas pixels (origin top left). press: the left (or right)
    // button down; returns whether the press was the panel's (then the
    // world below is not picked).
    bool press(simd_float2 p, bool right = false);
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
    enum class Action { None, SelectSim, Centre, Speed, Pause, Cancel, Mode, SubPanel, Callback };
    struct Target {
        std::array<float, 4> rect; // x0, y0, x1, y1
        Action action = Action::None;
        int value = 0;
    };
    const RgbaImage* image(uint32_t instance);
    const RgbaImage* objectThumbnail(const vm::Object& o);
    const RgbaImage* queueIcon(const vm::QueuedInteraction& q, bool running, bool nested);
    uint64_t stamp() const;
    void layoutIfNeeded();
    void drawTooltip(UiScreen& out, const std::string& text, simd_float2 at);

    vm::VM* vm_;
    std::unique_ptr<UiResources> res_;
    std::unique_ptr<FontLibrary> fonts_;
    std::unique_ptr<DbpfPackage> thumbnails_;
    bool ok_ = false;
    UiScript puck_, live_, motives_, bar_, member_, icon_;
    std::map<std::string, std::string> queueConstants_;
    std::array<UiScript, 8> bars_; // a copy of the motive bar per motive (its caption)

    vm::ObjectId sim_ = 0, followed_ = 0;
    std::unique_ptr<LivePanels> panels_;
    int subPanel_ = 0;
    std::vector<std::function<void()>> callbacks_;          // Action::Callback targets of the current screen
    std::vector<std::pair<std::array<float, 4>, std::string>> tips_; // tooltips of the current screen
    int tip_ = -1;                                          // the one under the mouse
    UiScript pause_;                                        // 0x8C364B93, the pause indicator
    std::map<int16_t, std::unique_ptr<RgbaImage>> neighborPortraits_; // by neighbor ID (null: none)
    const RgbaImage* portraitOfNeighbor(int16_t id);
    void sound(const std::string& name) {
        if (playSound)
            playSound(name);
    }
    std::map<vm::ObjectId, std::unique_ptr<RgbaImage>> portraits_;
    std::map<uint32_t, std::unique_ptr<RgbaImage>> objectThumbs_; // by GUID (null: none)
    std::map<uint32_t, const DbpfEntry*> thumbIndex_;             // ObjectThumbnails.package by instance
    std::map<std::tuple<vm::ObjectId, bool, bool>, std::unique_ptr<RgbaImage>> icons_; // masked (grey when waiting), by variant
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
    int mode_ = 0;
};

} // namespace os2
