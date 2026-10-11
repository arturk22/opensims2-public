#pragma once

// The app's pie menu over the lot's VM (the one LotSimulation runs,
// app/lot_vm.hpp; queue logic in src/vm/interactions.hpp): mouse picking,
// the menu and the selected Sim's queue drawn with the UI layer.
//
// Picking: every lot object the renderer draws gets a world-space box, the
// bounds of its model's vertices transformed by its placement, tagged with
// the object's ID (the low 16 bits of the cObject instance = the saved
// object ID the VM keeps). A click casts a ray from the camera through the
// mouse and takes the nearest box it enters. Boxes are coarse (a chair's box
// covers the space under the table next to it), but need no extra render
// pass; a pick ID pass would be exact.
//
// Layout and behaviour as the Mac binary's cTSWinPieMenu
// (docs/camera-ui-mac.md §2), with the constants of ui.package's UI script
// 0x090617B7 ("Constants Table": kItemRadius, the text colours, the font
// style, the cancel/gesture distances, the sounds; PieMenuStyle): the
// selected Sim's head (the character file's portrait) in the middle, the n
// items at 2*pi/n steps on a circle of kItemRadius starting at the top and
// going clockwise, each window placed from its point by the angle (right of
// it on the right, left of it on the left, centred at the top and bottom);
// the whole menu moved to fit the canvas. An item with children opens a
// child pie centred on the item, the head going along; clicking the head
// there goes back. The left button picks on release; releasing the press
// that opened the menu within kCancelBoundary of the centre leaves it open,
// past kGesturePickBoundary over an item picks it (drag and release),
// anywhere else cancels; the right button or a key cancels. The queue is a
// column of labels at the top left, the running interaction first.

#include "formats/ui_screen.hpp"
#include "vm/interactions.hpp"

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
class UiLayer;
struct ObjectModel;

// Object-space bounds of a model's vertices.
std::pair<simd_float3, simd_float3> modelBounds(const ObjectModel& model);

// The pie menu's constants (ui.package 0x090617B7), with the binary's
// values as defaults. The disabled colour and the item's background are not
// in the table (inferred).
struct PieMenuStyle {
    float itemRadius = 68, cancelBoundary = 30, gesturePickBoundary = 35, headInflate = -60;
    std::array<float, 4> textNormal{178 / 255.0f, 193 / 255.0f, 1, 1};
    std::array<float, 4> textHighlight{0, 1, 0, 1};
    std::string font = "0x2966548b"; // FontStyle.ini GUID ("piemenu")
    std::string mainSound = "ui_piemenu_primary", submenuSound = "ui_piemenu_secondary";
    bool itemAnimations = false;

    static PieMenuStyle fromConstants(const std::map<std::string, std::string>& constants);
};

class PieMenuHost {
public:
    // Over a VM with the lot's objects loaded (not owned; it must outlive
    // this). `simQuery` picks the selected Sim (vm::findSim).
    PieMenuHost(vm::VM& vm, const std::filesystem::path& fontsDir, const std::string& simQuery = {});
    ~PieMenuHost();

    vm::VM& vm() { return *vm_; }
    vm::ObjectId sim() const { return sim_; }

    void setStyle(const PieMenuStyle& style) { style_ = style, dirty_ = true; }
    const PieMenuStyle& style() const { return style_; }
    // The selected Sim's portrait (character file IMG 0x10), drawn as the head.
    void setPortrait(RgbaImage portrait);
    // Plays a UI sound by name (the item sounds).
    void setSoundPlayer(std::function<void(const std::string&)> play) { playSound_ = std::move(play); }

    // --- Picking -------------------------------------------------------------
    void addPickBox(uint32_t lotInstance, simd_float3 lo, simd_float3 hi, const simd_float4x4& at);
    // The nearest object hit by the ray (0: none).
    vm::ObjectId pick(simd_float3 origin, simd_float3 dir) const;
    // Middle of the top of an object's box (where its menu opens).
    std::optional<simd_float3> anchor(vm::ObjectId id) const;

    // --- Menu ------------------------------------------------------------------
    // Opens the menu of `object` (its interaction leader's) for the selected
    // Sim at a canvas point. False if it has no available entries.
    bool open(vm::ObjectId object, simd_float2 at);
    void close();
    bool isOpen() const { return open_; }
    const std::vector<vm::PieEntry>& entries() const { return entries_; } // available, not debug
    // Mouse in canvas pixels. press/release: the left button while the menu
    // is open (the press that opened it counts as the gesture's start):
    // releasing on an item opens its child pie or queues its interaction
    // (and closes the menu, unless `keepOpen`), on the head of a child pie
    // goes back, elsewhere cancels. Returns whether the event was the menu's.
    void hover(simd_float2 p);
    bool press(simd_float2 p);
    bool release(simd_float2 p, bool keepOpen = false);
    // A whole click (press and release at p, no gesture); answers a dialog first.
    bool click(simd_float2 p, bool keepOpen = false);
    // Queues entry `index` of entries() (as numbered by `s2pkg pie`) and
    // highlights it (opening its submenu). False if there is none.
    bool select(size_t index);

    // --- Dialogs (Dialog, vm/dialog.hpp) -------------------------------------
    // The blocking dialog waiting for the player (null: none): drawn over
    // everything, centred, with the game's title, message and buttons. It is
    // modal: while it is up, click() answers it with the button hit (or
    // ignores the click).
    const vm::Dialog* dialog() const;
    bool dialogOpen() const { return dialog() != nullptr; }
    // Answers the open dialog with the button at `p`; false if no button is there.
    bool clickDialog(simd_float2 p);
    // The canvas size clicks are laid out against before the first frame (headless).
    void setCanvas(int width, int height) { canvasW_ = width, canvasH_ = height; }

    // --- Drawing ---------------------------------------------------------------
    // The menu and the queue on a canvas of this size.
    UiScreen screen(int width, int height);
    // Rebuilds the overlay when something changed and encodes it (canvas =
    // the drawable size divided by `scale`).
    void attach(MTL::Device* device, uint32_t colorFormat, uint32_t depthFormat);
    void encode(MTL::RenderCommandEncoder* enc, uint32_t width, uint32_t height, float scale);

private:
    struct Label {
        std::string text;
        std::vector<size_t> entries; // indices into entries_ (several: a submenu)
        float x0 = 0, y0 = 0, x1 = 0, y1 = 0; // the drawn box
        float px = 0, py = 0;                 // its point on the circle
    };
    struct Layout {
        std::vector<Label> labels;
        float cx = 0, cy = 0; // the head's centre (after fitting on the canvas)
    };
    Layout layout() const; // of the current ring
    std::vector<Label> labels() const { return layout().labels; }
    int labelAt(const Layout& l, simd_float2 p) const;
    bool headAt(const Layout& l, simd_float2 p) const;
    bool pickLabel(const Layout& l, int index, bool keepOpen);
    std::string queueText(const vm::QueuedInteraction& q) const;
    struct DialogLayout {
        float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        std::vector<std::string> lines;
        std::vector<std::array<float, 4>> buttons; // x0, y0, x1, y1
    };
    DialogLayout dialogLayout(const vm::Dialog& d, int width, int height) const;
    void queueEntry(size_t index);

    vm::VM* vm_;
    std::unique_ptr<FontLibrary> fonts_;
    vm::ObjectId sim_ = 0;

    struct PickBox {
        vm::ObjectId id;
        simd_float3 lo, hi;
    };
    std::vector<PickBox> boxes_;

    bool open_ = false;
    vm::ObjectId object_ = 0;
    std::string title_;
    simd_float2 at_{0, 0};
    std::vector<vm::PieEntry> entries_;
    std::string submenu_; // path[0] of the open submenu, empty at the top
    simd_float2 subAt_{0, 0}; // the child pie's centre (its item's point)
    bool gesture_ = false;    // the press that opened the menu is still down
    PieMenuStyle style_;
    std::unique_ptr<RgbaImage> head_;
    std::function<void(const std::string&)> playSound_;
    int hover_ = -1;      // label index
    int chosen_ = -1;     // entry index highlighted by select()

    // Overlay: a ring of layers, so one is rebuilt while the GPU may still
    // read the ones of the frames in flight.
    std::array<std::unique_ptr<UiLayer>, 4> layers_;
    size_t layer_ = 0;
    bool dirty_ = true;
    int canvasW_ = 0, canvasH_ = 0;
    uint64_t queueStamp_ = 0;
};

} // namespace os2
