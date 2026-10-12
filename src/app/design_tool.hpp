#pragma once

// The Design tool of Buy and Build mode (cTSUserToolObjectDesign, buttons
// 0xCC607030 and 0xAB; docs/buy-build-mac.md §7.12): a click on an object
// that has design choices (formats/design_catalog.hpp; else "This object
// is not designable", UIText STR# 0x8D #15) opens the object design dialog
// (UI script 0x3C605F85, cTSWinProcObjectDesignDialog) by the mouse: the
// product's name, one or two rows of swatches (one per design subset, 5 a
// page with arrows), the cost ("Cost to Update: %s", STR# 0x93 #20: the
// tool's catalog price, §15 from Build.package STR# 0x92), Accept (lit
// only when a choice changed) and Cancel. Picking a swatch shows it on the
// object; Accept charges the price (refused without the funds, STR# 0x8D
// #14) and keeps it as the object's design choice (vm::Object::design, the
// cObject's list when saved); Cancel puts the old one back. Plain C++.

#include "formats/design_catalog.hpp"
#include "formats/ui_screen.hpp"
#include "vm/vm.hpp"

#include <simd/simd.h>

#include <array>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace os2 {

class FontLibrary;
class ResourceIndex;
class UiResources;
class VmObjectView;
struct RgbaImage;

class DesignTool {
public:
    DesignTool(vm::VM& vm, VmObjectView& view, ResourceIndex& index, UiResources& res, FontLibrary& fonts);
    ~DesignTool();

    std::function<void(const std::string&)> say, playSound;
    // An accepted change, for the mode's undo history.
    struct Change {
        vm::ObjectId id = 0;
        std::map<std::string, std::string> before, after;
        int cost = 0;
    };
    std::function<void(const Change&)> onAccepted;

    // Opens the dialog for the object clicked at `at` (canvas point);
    // `name` is its product's name. False (and the game's text) when it
    // cannot be designed or the family cannot pay.
    bool open(vm::ObjectId id, simd_float2 at, const std::string& name);
    bool isOpen() const { return open_.has_value(); }
    void cancel();
    // Picks option `option` (index in the row's list) of row 0 or 1.
    bool choose(int row, size_t option);
    bool accept();
    // The rows of the open dialog: their subsets and option counts.
    size_t rows() const { return open_ ? open_->subsets.size() : 0; }
    size_t optionCount(int row) const;

    // Draws the dialog into `out` (canvas size width x height).
    void layout(UiScreen& out, int width, int height);
    // A click: true if the dialog took it.
    bool press(simd_float2 p);

    // Sets an object's design choice in the VM and in the drawing (undo).
    static void apply(vm::VM& vm, VmObjectView& view, vm::ObjectId id, const std::map<std::string, std::string>& design);

    static constexpr int kDefaultPrice = 15; // Build.package STR# 0x92 #0

private:
    struct Open {
        vm::ObjectId id = 0;    // the object whose model has the choices (the part clicked)
        std::string model, name;
        std::vector<std::string> subsets; // design subsets (the model's names), at most 2
        std::vector<std::vector<const DesignMaterial*>> options;
        std::array<size_t, 2> chosen{0, 0}, original{0, 0}, page{0, 0};
        std::map<std::string, std::string> before; // the design choice when opened
        simd_float2 at{0, 0};
    };
    enum class Action { Swatch, Prev, Next, Accept, Cancel };
    struct Target {
        std::array<float, 4> rect;
        Action action;
        int row = 0;
        size_t value = 0;
    };
    std::map<std::string, std::string> designFor(const Open& o) const;
    const RgbaImage* swatch(const std::string& material);
    int price();
    bool changed() const;
    void message(uint32_t table, int index1, const std::string& fallback);

    vm::VM& vm_;
    VmObjectView& view_;
    ResourceIndex& index_;
    UiResources& res_;
    FontLibrary& fonts_;
    DesignCatalog catalog_;
    UiScript dialog_;
    std::optional<Open> open_;
    std::vector<Target> targets_;
    std::array<float, 4> rect_{0, 0, 0, 0};
    std::map<std::string, std::unique_ptr<RgbaImage>> swatches_;
    int price_ = -1;
};

} // namespace os2
