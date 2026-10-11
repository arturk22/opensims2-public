#pragma once

// Build mode over the lot (plain C++, driven from main.mm): the game's
// Build panel (UI script 0x49060003, cTSWinProcBuildPanel) on the puck's
// Build button, its tool categories (0xA100 walls, 0xA300 doors and
// windows, 0xA400 floors, 0xA500 wall coverings; the others are listed but
// not built yet) with their subsort panels 0xB?00 and the catalog grid,
// and the tools: walls and the room tool (drag; Ctrl deletes), floors
// (drag a rectangle; Ctrl erases), wall coverings (click a wall face, Shift
// covers the whole room) and doors and windows (the object tool on the
// walls, cutting their holes). Every change goes through vm::BuildEditor
// (prices, rules, rooms, routing, undo); the host redraws the lot's model
// and light maps when onGeometryChanged is called. What the game does is
// docs/buy-build-mac.md §7. Build mode pauses the simulation.

#include "formats/build_catalog.hpp"
#include "formats/catalog.hpp"
#include "formats/ui_screen.hpp"
#include "vm/build_edit.hpp"
#include "vm/placement.hpp"

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
class LotSimulation;
class Renderer;
class ResourceIndex;
class UiLayer;
class UiResources;
class VmObjectView;
struct Lot;

class BuildMode {
public:
    // `lot` is the lot as drawn (the host's copy): Build mode changes it.
    BuildMode(LotSimulation& simulation, VmObjectView& view, Renderer& renderer, ResourceIndex& index,
              const std::filesystem::path& resDir, Lot& lot);
    ~BuildMode();

    bool ok() const { return ok_; }
    bool active() const { return active_; }
    void enter();
    void exit();

    // The lot's walls, floors or doors changed: the host redraws it.
    std::function<void()> onGeometryChanged;

    enum class Tool { None, Wall, Room, Floor, Wallpaper, Object };
    Tool tool() const { return tool_; }
    vm::BuildEditor& editor() { return *editor_; }
    const BuildCatalog& patterns() const { return patterns_; }
    Catalog& objects() { return objects_; }
    void setLevel(int level) { level_ = level; }
    int level() const { return level_; }

    // Picks a tool with what it lays (a pattern for floors and walls).
    void selectWallTool(bool room);
    void selectPattern(const BuildPattern& p);
    bool selectObject(const CatalogEntry& e);
    // Headless: places the held object (a door or window) at a tile.
    bool placeObjectAt(int x, int y, int direction);
    void holdObjectAt(int x, int y, int direction);

    // --- The lot (mouse rays in world space) ---------------------------------
    void pointAt(simd_float3 eye, simd_float3 dir);
    // Press, drag and release on the lot; `control` deletes / erases,
    // `shift` draws a room (wall tool) or covers the room (wallpaper).
    bool mouseDown(simd_float3 eye, simd_float3 dir, bool control, bool shift);
    void mouseDrag(simd_float3 eye, simd_float3 dir);
    bool mouseUp(simd_float3 eye, simd_float3 dir);
    void rotate(int steps);
    void cancel();
    bool undo();
    bool redo();
    const std::string& message() const { return message_; }

    // --- The panel -----------------------------------------------------------
    bool press(simd_float2 p);
    void hover(simd_float2 p);
    void setCanvas(int width, int height) { canvasW_ = width, canvasH_ = height, dirty_ = true; }
    UiScreen screen(int width, int height);
    void attach(MTL::Device* device, uint32_t colorFormat, uint32_t depthFormat);
    void encode(MTL::RenderCommandEncoder* enc, uint32_t width, uint32_t height, float scale);
    // Called every frame: the previews follow the tool.
    void update();

    // The category (0xA?00 button, 1-10) and subsort button shown; for tests.
    void openCategory(int category, int subsort = 0);

private:
    enum class Action { None, Category, Subsort, Back, Prev, Next, Item, Hand, Undo, Redo };
    struct Target {
        std::array<float, 4> rect;
        Action action = Action::None;
        int value = 0;
    };
    // A catalog cell: a tool, a pattern or an object.
    struct Item {
        Tool tool = Tool::None;
        const BuildPattern* pattern = nullptr;
        const CatalogEntry* object = nullptr;
        std::string name, description;
        int price = 0;
        const RgbaImage* image = nullptr;
    };
    std::vector<Item> items();
    const RgbaImage* patternThumbnail(const BuildPattern& p);
    std::optional<simd_float2> groundPoint(simd_float3 eye, simd_float3 dir) const;
    void applied(const vm::BuildEditor::Result& r);
    void say(const std::string& m);
    void layoutIfNeeded();
    void syncAfterObject();
    // The wall face under a point: the wall's ends and the side (+1 right of a -> b).
    std::optional<std::pair<std::array<int, 4>, int>> faceAt(simd_float2 p) const;

    LotSimulation& sim_;
    vm::VM& vm_;
    VmObjectView& view_;
    Renderer& renderer_;
    ResourceIndex& index_;
    Lot& lot_;
    std::unique_ptr<UiResources> res_;
    std::unique_ptr<FontLibrary> fonts_;
    BuildCatalog patterns_;
    Catalog objects_;
    std::unique_ptr<vm::BuildEditor> editor_;
    UiScript panel_, puck_, description_;
    bool ok_ = false, active_ = false;
    int16_t savedPaused_ = 0;

    int category_ = 0; // 0: the category buttons; 1-10 the 0xA?00 button
    int subsort_ = 0;  // the subsort button (0xB?n0: n), 0 for all
    size_t page_ = 0;
    const BuildPattern* pattern_ = nullptr;
    const CatalogEntry* object_ = nullptr;
    std::optional<Item> hovered_;
    std::map<uint32_t, std::shared_ptr<RgbaImage>> thumbs_;

    Tool tool_ = Tool::None;
    int level_ = 1;
    bool control_ = false, shift_ = false;
    std::optional<simd_float2> dragFrom_, pointer_;
    // The object tool (doors and windows), as Buy mode's.
    vm::ObjectId held_ = 0;
    vm::Placement last_;
    int direction_ = 2;
    std::string message_;

    std::array<int, 2> previewModels_{-1, -1};
    std::vector<int> previewInstances_;

    std::vector<Target> targets_;
    std::vector<std::array<float, 4>> solid_;
    std::optional<simd_float2> mouse_;
    std::array<std::unique_ptr<UiLayer>, 3> layers_;
    size_t layer_ = 0;
    bool dirty_ = true;
    int canvasW_ = 0, canvasH_ = 0, laidW_ = 0, laidH_ = 0;
};

} // namespace os2
