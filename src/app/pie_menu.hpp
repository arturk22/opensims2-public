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
// Layout (the game's own pie menu is drawn by code, UIPieMenu in Sims2.exe,
// with the Sim's head in the middle; only its font style "piemenu" =
// ITC Benguiat Gothic 14 bold, FontStyle.ini, is in the data): the entries
// are labels on a circle around the clicked point, starting at the top and
// going clockwise; entries sharing a "Sub.../" prefix are one label that
// opens a second ring with the rest of their names. The queue is a column of
// labels at the top left, the running interaction first.

#include "formats/ui_screen.hpp"
#include "vm/interactions.hpp"

#include <simd/simd.h>

#include <array>
#include <filesystem>
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

class PieMenuHost {
public:
    // Over a VM with the lot's objects loaded (not owned; it must outlive
    // this). `simQuery` picks the selected Sim (vm::findSim).
    PieMenuHost(vm::VM& vm, const std::filesystem::path& fontsDir, const std::string& simQuery = {});
    ~PieMenuHost();

    vm::VM& vm() { return *vm_; }
    vm::ObjectId sim() const { return sim_; }

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
    // Mouse in canvas pixels. A click on a label opens its submenu or queues
    // its interaction (and closes the menu, unless `keepOpen`); a click
    // elsewhere closes the menu (in a submenu: goes back). Returns whether
    // the click was the menu's.
    void hover(simd_float2 p);
    bool click(simd_float2 p, bool keepOpen = false);
    // Queues entry `index` of entries() (as numbered by `s2pkg pie`) and
    // highlights it (opening its submenu). False if there is none.
    bool select(size_t index);

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
        float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    };
    std::vector<Label> labels() const; // of the current ring, laid out
    std::string queueText(const vm::QueuedInteraction& q) const;
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
