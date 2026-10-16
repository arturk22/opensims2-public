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
    // Family members out of the world (at work) are greyed in the column and
    // cannot be picked there (null: none are).
    std::function<bool(vm::ObjectId)> simAway;
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
    // The Live button (mode 0) only works with Sims on the lot: the UCP's
    // `this[0x80]` is "the lot has a selectable Sim" and `HandleLiveModeButton`
    // does nothing without it, and `onSelectedPersonChanged` disables the
    // button (0x37D97B25) the same way (BV 0x1a0654 / 0x1a131c). A lot
    // without Sims (no family moved in) opens in Buy mode and stays out of Live.
    void setLiveEnabled(bool on) { liveEnabled_ = on, dirty_ = true; }
    bool liveEnabled() const { return liveEnabled_; }
    // The options panel (puck button 4, script 0x49060001: the lot's
    // cTSWinProcPanelOptions): "Vizinhança" leaves the lot for the
    // neighborhood (only shown when setCanLeave(true): the lot came from
    // one), "Sair" quits, "Salvar" saves (onSave), the audio and game
    // options buttons open their windows (onOptionWindow: 1 audio, 2 game, 3 graphics, 4 camera, 5 credits;
    // app/options_panel.hpp).
    std::function<void()> onNeighborhood, onQuit, onSave;
    std::function<void(int)> onOptionWindow;
    void setCanLeave(bool can) { canLeave_ = can, dirty_ = true; }
    bool optionsOpen() const { return optionsOpen_; }
    // Esc: the options panel closes (as its puck button would).
    void closeOptions() {
        if (optionsOpen_)
            optionsOpen_ = false, dirty_ = true;
    }
    // The puck's floor buttons ("Subir um Andar (Page Up)" 0x2001, "Descer
    // um Andar (Page Down)" 0x2000): +1 / -1.
    std::function<void(int)> onFloor;
    // The puck's zoom ("Mais Zoom +" 0x1002 = +1, "Menos Zoom -" 0x1003 = -1)
    // and rotate buttons (0x1001 on the left = -1, 0x1000 = +1, one step).
    std::function<void(int)> onZoom, onRotate;
    // The puck's wall button (0x1100 walls down, 0x1101 cutaway, 0x1102 up,
    // 0x1103 up with the roof: the one of the current mode is shown) opens
    // the wall fan (0x4000, over the camera controls: 1 walls down, 2
    // cutaway, 3 up, 4 up with the roof); a choice closes it, as does a
    // click anywhere else. The four are exclusive modes, as
    // cTSWinProcUCP::onWallChanged lights them (docs/ui-live-mac.md 8c).
    // setWallMode shows the mode (0-3); onWallMode is called with the one
    // the player picks.
    void setWallMode(int mode) {
        if (mode != wallMode_)
            wallMode_ = mode, dirty_ = true;
    }
    bool wallFanOpen() const { return wallFan_; }
    std::function<void(int)> onWallMode;

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
    // Whether p is on one of the puck's buttons (the modes Live, Buy, Build,
    // ..., the walls, zoom, rotate and floors).
    // They stay clickable over Buy and Build mode's panels, whose backdrops
    // would otherwise take the click.
    bool overModeButton(simd_float2 p);
    // A control or a panel's body is under p (the game's arrow cursor there).
    bool overPanel(simd_float2 p);
    void hover(simd_float2 p);
    // The tutorial (app/tutorial.hpp): allowPress is asked before a left
    // click on a control (false: the tutorial turned it off, the click does
    // nothing). A control by the game's window ids ("Button GUIDs": a window, and a
    // button in it; kMainWindow holds them all): its rectangle in the last
    // screen, if drawn.
    std::function<bool(simd_float2)> allowPress;
    static constexpr uint32_t kMainWindow = 0x480E8304, kSimChooser = 0x4BDEFFAB, kFirstSimInChooser = 0x2000;
    std::optional<std::array<float, 4>> controlRect(uint32_t window, uint32_t control);
    bool controlAt(simd_float2 p, uint32_t window, uint32_t control);
    // The canvas size clicks are laid out against before the first frame (headless).
    void setCanvas(int width, int height) { canvasW_ = width, canvasH_ = height, dirty_ = true; }

    // The family members shown (the Sims of the lot's current family, or
    // every Sim when none is in it).
    std::vector<vm::ObjectId> family() const;

    // --- Drawing -------------------------------------------------------------
    UiScreen screen(int width, int height);
#ifndef _WIN32
    void attach(MTL::Device* device, uint32_t colorFormat, uint32_t depthFormat);
    // Rebuilds the overlay when what it shows changed (canvas = the drawable
    // size divided by `scale`) and encodes it.
    void encode(MTL::RenderCommandEncoder* enc, uint32_t width, uint32_t height, float scale);
#endif

private:
    enum class Action { None, SelectSim, Centre, Speed, Pause, Cancel, Mode, SubPanel, Callback, Floor, Zoom, Rotate,
                        WallFan, Walls };
    struct Target {
        std::array<float, 4> rect; // x0, y0, x1, y1
        Action action = Action::None;
        int value = 0;
        const UiElement* element = nullptr; // the button (its click sound)
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
    UiScript puck_, live_, motives_, bar_, member_, icon_, options_;
    bool optionsOpen_ = false, canLeave_ = false, liveEnabled_ = true;
    int wallMode_ = 2;
    bool wallFan_ = false;
    std::map<std::string, std::string> queueConstants_;
    std::array<UiScript, 8> bars_; // a copy of the motive bar per motive (its caption)

    vm::ObjectId sim_ = 0, followed_ = 0;
    std::unique_ptr<LivePanels> panels_;
    int subPanel_ = 0;
    std::vector<std::function<void()>> callbacks_;          // Action::Callback targets of the current screen
    AgeMeter ageMeter_; // the selected Sim's age meter (this frame)
    std::vector<std::pair<std::array<float, 4>, std::string>> tips_; // tooltips of the current screen
    int tip_ = -1;                                          // the one under the mouse
    // Tooltip timing (cGZWinToolTipMgr, docs/ui-live-mac.md §7.6): shown
    // 0.75 s after the mouse came over the control. The binary hides it at
    // 2.25 s; we keep it up while the mouse stays over the control (user: it
    // vanished before it could be read).
    double now_ = 0, tipSince_ = 0;
    int tipHovered_ = -1;
    bool tipVisible() const { return now_ - tipSince_ >= 0.75; }
    UiScript pause_;                                        // 0x8C364B93, the pause indicator
    // The queue's icons slide to their slots (kAnimationSteps steps) and a
    // new one plays ui_queue (docs/ui-live-mac.md §6.4).
    struct IconSlide {
        float fromX = 0, fromY = 0, toX = 0, toY = 0;
        double since = 0;
    };
    std::map<uint32_t, IconSlide> slides_; // by (nested << 16 | uid)
    vm::ObjectId slidesSim_ = 0;
    bool sliding_ = false;
    // A popup (the mood popup, the relationship description): a script
    // drawn over the panel with its tail on an anchor, opened by a click.
    struct Popup {
        uint32_t script = 0;
        std::array<float, 4> anchor{}; // x0, y0, x1, y1: the tail points at its top centre
        std::vector<std::pair<uint32_t, std::string>> texts; // caption of the child window by id
        double since = 0, delay = 0;
        int kind = 0;
        std::vector<std::pair<uint32_t, const RgbaImage*>> images; // picture of the child window by id
    } popup_;
    std::map<uint32_t, UiScript> popupScripts_;
    std::array<float, 4> popupRect_{};
    int closedPopupKind_ = 0; // the kind closed by the click being handled
    void openPopup(uint32_t script, std::array<float, 4> anchor, std::vector<std::pair<uint32_t, std::string>> texts,
                   double delay, int kind);
    void closePopup();
    std::map<int16_t, std::unique_ptr<RgbaImage>> neighborPortraits_; // by neighbor ID (null: none)
    const RgbaImage* portraitOfNeighbor(int16_t id);
    void sound(const std::string& name) {
        if (playSound)
            playSound(name);
    }
    std::map<vm::ObjectId, std::unique_ptr<RgbaImage>> portraits_;
    std::map<vm::ObjectId, std::unique_ptr<RgbaImage>> awayPortraits_; // greyed copies, Sims out of the world
    std::map<uint32_t, std::unique_ptr<RgbaImage>> objectThumbs_; // by GUID (null: none)
    std::map<uint32_t, const DbpfEntry*> thumbIndex_;             // ObjectThumbnails.package by instance
    std::map<std::tuple<vm::ObjectId, bool, bool>, std::unique_ptr<RgbaImage>> icons_; // masked (grey when waiting), by variant
    std::vector<std::unique_ptr<RgbaImage>> frameImages_; // fills made for the current screen

    std::array<MotiveTrend, 8> trends_;
    double lastSample_ = -1;
    std::vector<Target> targets_;
    std::vector<std::pair<uint32_t, std::array<float, 4>>> windows_; // drawn windows by id (last screen)
    static constexpr uint32_t kChooserThumbnail = 0xFFFF2000; // windows_ key of the first family thumbnail
    static constexpr uint32_t kPuckRoot = 0xFFFF0001;         // ... and of the puck's root
    std::vector<std::array<float, 4>> solid_; // panel areas that take clicks
    int hoverIcon_ = -1;                      // queue icon under the mouse
    std::optional<simd_float2> mouse_;
    std::set<std::pair<vm::ObjectId, uint16_t>> cancelled_; // (Sim, queue uid) cancelled from the panel

#ifndef _WIN32
    std::array<std::unique_ptr<UiLayer>, 4> layers_;
    size_t layer_ = 0;
#endif
    bool dirty_ = true;
    int canvasW_ = 0, canvasH_ = 0, laidW_ = 0, laidH_ = 0;
    uint64_t stamp_ = 0;
    int mode_ = 0;
};

} // namespace os2
