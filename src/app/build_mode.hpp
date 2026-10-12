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

#include "app/head_shot.hpp"
#include "app/ui_panel_util.hpp"
#include "formats/build_catalog.hpp"
#include "formats/catalog.hpp"
#include "formats/lot.hpp"
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

class DesignTool;
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
    // UI sounds by name (the ui_build_* sounds of the executable: the wall
    // tool's press, sections and release, floors, coverings, decks, pools,
    // roofs, doors and stairs; ui_error, ui_undo, ui_redo), and the brush's
    // loops (ui_build_terrain_lp, ui_build_terrain_paint_lp) started and
    // stopped by handle.
    std::function<void(const std::string&)> playSound;
    std::function<uint32_t(const std::string&)> startLoop;
    std::function<void(uint32_t)> stopLoop;
    // The object under a mouse ray (the host's picking), for the Design tool.
    std::function<vm::ObjectId(simd_float3 eye, simd_float3 dir)> pickObject;
    // "Alternar Dia/Noite" (0xAA): the host flips the lighting.
    std::function<void()> onDayNight;
    // Renders models for a catalog tile (the host's renderer:
    // renderPartsThumbnail); the modular stairs and dormers have no cached
    // thumbnail, the game renders theirs.
    std::function<std::optional<RgbaImage>(const std::vector<ThumbnailPart>&)> renderThumb;
    // The Design tool (0xAB, app/design_tool.hpp) on an object.
    void selectDesignTool();
    bool design(vm::ObjectId id, simd_float2 at);
    DesignTool& designTool() { return *design_; }

    enum class Tool { None, Wall, Room, Floor, Wallpaper, Object, Fence, Foundation, Terrain, Roof, Pool, Design, Stairs, Dormer, FenceArch, Railing };
    Tool tool() const { return tool_; }
    vm::BuildEditor& editor() { return *editor_; }
    const BuildCatalog& patterns() const { return patterns_; }
    Catalog& objects() { return objects_; }
    void setLevel(int level) { level_ = level; }
    int level() const { return level_; }

    // Picks a tool with what it lays (a pattern for floors and walls).
    void selectWallTool(bool room);
    void selectPattern(const BuildPattern& p);
    void selectFence(const BuildFence& f);
    // deckfoundations.txt product: 0 foundation, 1 lattice deck, 2 deck on columns.
    void selectFoundationTool(int product = 0);
    void selectPoolTool();
    // Modular stairs (B640, modularstairs.txt): click a floor tile's edge
    // (the flight goes down off the side of the tile the click is on, its
    // end found by itself) or drag from it to where the last step goes;
    // Ctrl deletes the flight under the mouse.
    void selectStairsTool(const BuildStairs& product);
    // The Railing Tool (B620): a click on a staircase's side puts a railing
    // on that side of its flight; Ctrl takes it off.
    void selectRailingTool();
    // Dormers (B940, roofs.txt "roof 6-11"): a click on the attic level puts
    // one with its corner at the grid point under the mouse, turned by `,`/`.`.
    void selectDormerTool(const BuildRoofProduct& product);
    // Terrain: a sculpt brush (raise, lower, flatten, smooth) or a paint
    // (the paint's texture); `size` 0-2 small/medium/large.
    void selectTerrainTool(vm::BuildEditor::TerrainOp op, const BuildTerrainPaint* paint = nullptr);
    void setBrushSize(int size) { brushSize_ = size; }
    // Roof tool: Lot::RoofType 1-6 (roofs.txt "roof 0-5").
    void selectRoofTool(uint32_t type);
    // The floor tool's pattern rotation (1, 2, 4, 8; `,`/`.` cycle it).
    int floorRotation() const { return floorRotation_; }
    void setFloorRotation(int r) { floorRotation_ = r; }

    // Headless steps of a run (--wall, --floor, --fence, ... in order; see
    // the comment in build_mode.cpp), from the command line's arguments.
    void runSteps(const std::vector<std::string>& args);
    bool selectObject(const CatalogEntry& e);
    // Headless: places the held object (a door or window) at a tile.
    bool placeObjectAt(int x, int y, int direction);
    void holdObjectAt(int x, int y, int direction);

    // --- The lot (mouse rays in world space) ---------------------------------
    void pointAt(simd_float3 eye, simd_float3 dir);
    // Press, drag and release on the lot; `control` deletes / erases,
    // `shift` draws a room (wall tool) or covers the connected faces with the clicked one's pattern (wallpaper).
    bool mouseDown(simd_float3 eye, simd_float3 dir, bool control, bool shift);
    void mouseDrag(simd_float3 eye, simd_float3 dir);
    bool mouseUp(simd_float3 eye, simd_float3 dir);
    void rotate(int steps);
    void cancel();
    bool undo();
    bool redo();
    const std::string& message() const { return message_; }
    std::optional<simd_float2> pointer() const { return pointer_; } // the lot point under the mouse

    // --- The panel -----------------------------------------------------------
    // The tutorial (app/tutorial.hpp): allowPress before a click on a
    // control (false: turned off); a control by the game's window ids, while
    // the panel is up.
    std::function<bool(simd_float2)> allowPress;
    std::optional<std::array<float, 4>> controlRect(uint32_t window, uint32_t control);
    int pickLevel() const; // the VM level (1 = ground) the mouse picks on with the current tool
    float pickHeight(simd_float2 p) const; // the height the pick meets at p (the cursor's)
    bool press(simd_float2 p);
    void hover(simd_float2 p);
    void setCanvas(int width, int height) { canvasW_ = width, canvasH_ = height, dirty_ = true; }
    UiScreen screen(int width, int height);
    void attach(MTL::Device* device, uint32_t colorFormat, uint32_t depthFormat);
    void encode(MTL::RenderCommandEncoder* enc, uint32_t width, uint32_t height, float scale);
    // Called every frame: the previews follow the tool.
    void update();

    // The auto-roof panel (0xAC, B950): tool 1-5 (C100 hip, C200 gable,
    // C500/C600 the mixes, C700 the tiled slab), 0 removes every roof (C300).
    vm::BuildEditor::Result autoRoof(int tool);
    // The category (0xA?00 button, 1-10) and subsort button shown; for tests.
    void openCategory(int category, int subsort = 0);

private:
    enum class Action { None, Category, Subsort, Back, Prev, Next, Item, Hand, Undo, Redo, Design, DayNight, AutoRoof };
    bool preserveRoofs_ = false; // C400 "Preservar Telhados Existentes"
    // One step of Build mode's history (cTSUserToolUndoManager): a change of
    // the editor's (walls, floors, ...: its own undo), an object the object
    // tool bought (a door, window or staircase), or a design choice.
    struct Entry {
        enum class Kind { Editor, Object, Design } kind = Kind::Editor;
        vm::ObjectId id = 0;
        vm::Placement placement;
        int cost = 0;
        std::optional<Lot::Object> cobject; // the cut it makes in the walls
        std::map<std::string, std::string> before, after;
    };
    std::vector<Entry> undo_, redo_;
    size_t editorDepth_ = 0;
    // The editor's new changes join the history.
    void syncHistory();
    std::unique_ptr<DesignTool> design_;
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
        const BuildFence* fence = nullptr;
        const BuildTerrainPaint* paint = nullptr;
        const BuildRoofPattern* roofPattern = nullptr;
        int terrainOp = -1;
        uint32_t roofType = 0;
        int product = 0; // deckfoundations.txt product (foundation tool)
        const BuildStairs* stairs = nullptr;
        const BuildRoofProduct* dormer = nullptr;
        const BuildFenceArch* arch = nullptr;
        std::string name, description;
        int price = 0;
        const RgbaImage* image = nullptr;
    };
    std::vector<Item> items();
    const RgbaImage* patternThumbnail(const BuildPattern& p);
    const RgbaImage* fenceThumbnail(const BuildFence& f);
    const RgbaImage* materialThumbnail(uint32_t key, const std::string& material);
    const RgbaImage* textureThumbnail(uint32_t key, const std::string& texture);
    // A modular staircase's tile (cTSUIThumbnailFactoryModularStairs::
    // SetupScene): its top step, a step and its bottom step (the step when
    // it has none), each 0.25 further along y and 0.1875 lower (the
    // translation (0, 0.25 i, -0.1875 i) the factory gives the i-th).
    const RgbaImage* stairsThumbnail(const BuildStairs& b);
    // A dormer's tile (cTSUIThumbnailFactoryDormer): the product's model,
    // its defaultRoofPattern ("dormerpreview-2wide").
    const RgbaImage* dormerThumbnail(const BuildRoofProduct& d);
    std::string refusalText(const vm::BuildEditor::Result& r) const;
    std::optional<simd_float2> groundPoint(simd_float3 eye, simd_float3 dir) const;
    void applied(const vm::BuildEditor::Result& r);
    void say(const std::string& m);
    void layoutIfNeeded();
    void syncAfterObject();
    void brushTick(bool force);
    // The lot changed: the simulation's heights and the host's drawing follow.
    void redraw();
    void sound(const std::string& name) {
        if (playSound && !name.empty())
            playSound(name);
    }
    // A tool's sounds: on the press, and on the release with what it did
    // (ui_error when refused). `tool` with `erase` (Ctrl) picks them.
    void pressSound(Tool tool, bool erase);
    void releaseSound(Tool tool, bool erase, const vm::BuildEditor::Result& r);
    void stopBrushLoop();
    // The sound of a build object put down (door/window, stairs, plant, the rest).
    std::string objectSound(vm::ObjectId id) const;
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
    const BuildFence* fence_ = nullptr;
    const BuildTerrainPaint* paint_ = nullptr;
    vm::BuildEditor::TerrainOp terrainOp_ = vm::BuildEditor::TerrainOp::Raise;
    int brushSize_ = 1;
    uint32_t roofType_ = 0;
    int deckProduct_ = 0;
    const BuildStairs* stairs_ = nullptr;
    const BuildRoofProduct* dormer_ = nullptr;
    const BuildFenceArch* arch_ = nullptr;
    // The flight the stairs tool would lay from the drag (or the pointer):
    // top tile, direction and the dragged end (none: found by itself).
    struct StairsDrag {
        vm::TilePos top;
        int direction = 0;
        std::optional<std::array<float, 2>> end;
    };
    StairsDrag stairsDrag(simd_float2 from, simd_float2 to) const;
    bool stroking_ = false;
    uint32_t brushLoop_ = 0;
    size_t dragSections_ = 0;   // sections (or tiles) the drag covers, for the section sounds
    std::array<int, 5> hoverFace_{}; // the face the covering tool last showed
    bool lastOk_ = false;            // the held object's last verdict (the "green" sound)
    double lastBrush_ = 0, lastRedraw_ = 0;
    int floorRotation_ = 1;
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
    DrawnWindows windows_; // the panel's windows in the last screen
    std::vector<std::array<float, 4>> solid_;
    std::optional<simd_float2> mouse_;
    std::array<std::unique_ptr<UiLayer>, 3> layers_;
    size_t layer_ = 0;
    bool dirty_ = true;
    int canvasW_ = 0, canvasH_ = 0, laidW_ = 0, laidH_ = 0;
};

} // namespace os2
