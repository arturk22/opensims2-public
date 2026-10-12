#pragma once

// The app's pie menu over the lot's VM (the one LotSimulation runs,
// app/lot_vm.hpp; queue logic in src/vm/interactions.hpp): mouse picking,
// the menu and the selected Sim's queue drawn with the UI layer.
//
// Picking: every lot object the renderer draws is picked by its model's
// triangles (PickMesh: no shadow planes) where it is drawn now, tagged with
// the object's ID (the low 16 bits of the cObject instance = the saved
// object ID the VM keeps). A click casts a ray from the camera through the
// mouse and takes the nearest triangle it hits, testing an object's
// triangles only when the ray enters their box. The Sims are picked by the
// box around their posed skeleton.
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
#include "formats/ui_script.hpp"
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
class ResourceIndex;
class UiLayer;
class UiResources;
struct ObjectModel;

// Object-space bounds of a model's vertices.
std::pair<simd_float3, simd_float3> modelBounds(const ObjectModel& model);

// A model's triangles for picking (object space, three corners each) and
// their bounds: what is drawn of it, less the shadow planes ("groundshadow",
// "*wallshadow") and the flat patches on the ground of a model that has
// more than that (the grass under a tree), which the player does not see as
// the object. Empty when the model has none.
// Alpha-tested parts (leaves, fences, lace) keep their texture's alpha:
// a ray through a hole in the leaves goes on to what is behind.
struct PickMesh {
    std::vector<simd_float3> corners;
    simd_float3 lo{0, 0, 0}, hi{0, 0, 0};
    struct Mask {
        uint32_t width = 0, height = 0;
        std::vector<uint8_t> alpha; // row-major
        uint8_t ref = 127;          // passes when alpha >= ref
    };
    std::vector<Mask> masks;
    std::vector<int> triangleMask;     // per triangle: index into masks, -1 solid
    std::vector<simd_float2> uvs;      // per corner, where triangleMask is used
};
// `index` (to read the alpha-tested textures; null: every part is solid).
std::shared_ptr<const PickMesh> makePickMesh(const ObjectModel& model, ResourceIndex* index = nullptr);
// The nearest hit of a ray (origin, dir: any length) on the mesh's
// triangles, both sides, as a multiple of dir; nothing if none.
std::optional<float> rayHit(const PickMesh& mesh, simd_float3 origin, simd_float3 dir);
// A bounding volume tree over a posed model's triangles (a Sim's skinned
// mesh, already in world space, less its shadow parts): built once from one
// pose (median splits, at most 8 triangles a leaf) and refitted to each new
// pose, so that a ray tests only the triangles near it.
struct PosedPick {
    struct Tri {
        uint32_t part, index; // index of the triangle's first corner in the part's triangle list
    };
    struct Node {
        simd_float3 lo, hi;
        uint32_t first, count; // a leaf: tris[first .. first + count); else count 0 and first = right child
    };
    std::vector<Tri> tris;
    std::vector<Node> nodes; // pre-order: a node's left child follows it
    uint64_t shape = 0;      // the model's parts and triangle counts it was built for (shapeOf)
    // The bounds to `model` as posed now (same topology as built).
    void refit(const ObjectModel& model);
};
PosedPick makePosedPick(const ObjectModel& model);
uint64_t shapeOf(const ObjectModel& model);
// The nearest hit of a ray on those triangles, both sides, as a multiple of
// dir; nothing if none.
std::optional<float> rayHit(const ObjectModel& model, const PosedPick& tree, simd_float3 origin, simd_float3 dir);

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
    // Another selected Sim (its portrait comes by setPortrait).
    void setSim(vm::ObjectId id) { sim_ = id, dirty_ = true; }
    // The debug queue (a column of labels at the top left); off when the
    // live-mode panel draws the game's queue.
    void setShowQueue(bool show) { showQueue_ = show, dirty_ = true; }

    void setStyle(const PieMenuStyle& style) { style_ = style, dirty_ = true; }
    const PieMenuStyle& style() const { return style_; }
    // The selected Sim's portrait (character file IMG 0x10), drawn as the head.
    void setPortrait(RgbaImage portrait);
    // Plays a UI sound by name (the item sounds).
    void setSoundPlayer(std::function<void(const std::string&)> play) { playSound_ = std::move(play); }

    // --- Picking -------------------------------------------------------------
    // An object of the lot at `at`, picked by its model's triangles (`mesh`;
    // null: by its box). Where `placement` (below) says the object is drawn
    // now, it is picked there (objects the VM moves).
    void addPickBox(uint32_t lotInstance, simd_float3 lo, simd_float3 hi, const simd_float4x4& at,
                    std::shared_ptr<const PickMesh> mesh = nullptr);
    // An object saved in a slot (a cake on a table, a plate on a counter):
    // its model-space box goes where `placement` says the object is drawn
    // now, each time a ray is cast (nothing: not pickable).
    void addFollowingPickBox(uint32_t lotInstance, simd_float3 lo, simd_float3 hi,
                             std::shared_ptr<const PickMesh> mesh = nullptr);
    void setPlacementSource(std::function<std::optional<simd_float4x4>(vm::ObjectId)> placement) {
        placement_ = std::move(placement);
    }
    // A box that moves (a Sim: the host sets it from the posed skeleton every
    // frame), in world space, in place of the one set before; lo == hi removes it.
    // `posed`: the mesh as drawn now, in world space (a Sim's skinned
    // model, SimActor::posed; it must outlive the box): a ray that enters
    // the box picks it only where it hits one of its triangles.
    void setMovingBox(vm::ObjectId id, simd_float3 lo, simd_float3 hi, const ObjectModel* posed = nullptr);
    // Boxes this refuses are not picked (objects on the levels the camera
    // hides, app/wall_view.hpp); null picks all.
    std::function<bool(simd_float3 lo, simd_float3 hi)> pickable;
    // Objects this refuses are not picked (not drawn now: the windows and
    // doors of a wall cut down); null picks all.
    std::function<bool(vm::ObjectId)> drawn;
    // The nearest object hit by the ray (0: none).
    vm::ObjectId pick(simd_float3 origin, simd_float3 dir) const;
    // Middle of the top of an object's box (where its menu opens).
    std::optional<simd_float3> anchor(vm::ObjectId id) const;

    // --- Menu ------------------------------------------------------------------
    // Opens the menu of `object` (its interaction leader's) for the selected
    // Sim at a canvas point. False if it has no available entries.
    // `merged`: a second object whose entries follow (a rug's go-here
    // destination, vm::objectClick); onClose is called for it too.
    bool open(vm::ObjectId object, simd_float2 at, vm::ObjectId merged = 0);
    void close();
    bool isOpen() const { return open_; }
    // Called with the menu's object when an open menu closes (picked or
    // cancelled): the host lets a go-here destination go (vm/go_here.hpp).
    std::function<void(vm::ObjectId)> onClose;
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
    // The game's dialog scripts (the Sims 2 yes/no box 0x6CA51466 and the
    // baby-name box 0x2D91050A, from ui.package) draw the dialog; without
    // them it is drawn with plain boxes.
    void setResources(ResourceIndex& index, const std::filesystem::path& resDir);
    // A key for the open dialog (Mac key code and the characters typed): the
    // text of a text-entry dialog, Return = its first button, Esc = its last
    // (yes/no, yes/no/cancel; inferred). False if no dialog is open.
    bool dialogKey(int keyCode, const std::string& typed);
    // Test aid: sets the text of the open text-entry dialog as if typed.
    void setDialogText(const std::string& text);
    // The open dialog has a text box.
    bool dialogHasTextBox() const;
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
    // The dialog as the game's UI scripts draw it (null without them).
    struct GameDialog {
        float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        std::vector<std::string> lines;
        UiElement root; // laid out at (x0, y0)
        std::vector<uint32_t> buttonIds; // answer order
        std::vector<std::array<float, 4>> buttons; // canvas rectangles
        std::array<float, 4> textBox{0, 0, 0, 0};
        uint32_t textId = 0, titleId = 0, messageId = 0;
        bool baby = false;
    };
    std::optional<GameDialog> gameDialog(const vm::Dialog& d, int width, int height) const;
    void drawGameDialog(UiScreen& out, const vm::Dialog& d, const GameDialog& g);
    std::unique_ptr<UiResources> dlgRes_;
    UiScript yesNoScript_, nameScript_;
    int dialogHover_ = -1;
    uint32_t editedDialog_ = 0; // the dialog whose default text was replaced by typing
    void queueEntry(size_t index);

    vm::VM* vm_;
    std::unique_ptr<FontLibrary> fonts_;
    vm::ObjectId sim_ = 0;

    struct PickBox {
        vm::ObjectId id;
        simd_float3 lo, hi;
        bool follows = false; // lo/hi in model space, placed by placement_
        std::shared_ptr<const PickMesh> mesh; // model-space triangles, null: the box
        simd_float4x4 at = matrix_identity_float4x4; // the mesh's placement
        simd_float3 modelLo{0, 0, 0}, modelHi{0, 0, 0}; // lo/hi in model space (static boxes)
        const ObjectModel* posed = nullptr; // a moving box's world-space mesh (setMovingBox)
        mutable bool fitted = false; // its tree refitted to this pose (at the first pick)
    };
    std::vector<PickBox> boxes_;
    std::function<std::optional<simd_float4x4>(vm::ObjectId)> placement_;
    // A box in world space (a following box where its object is drawn now).
    std::optional<PickBox> worldBox(const PickBox& b) const;
    std::map<vm::ObjectId, PickBox> moving_;
    mutable std::map<const ObjectModel*, PosedPick> posedTrees_; // by the actor's posed model

    bool open_ = false;
    vm::ObjectId object_ = 0;
    vm::ObjectId merged_ = 0;
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
    bool showQueue_ = true;
};

} // namespace os2
