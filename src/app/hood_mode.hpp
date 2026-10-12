#pragma once

// The app's neighborhood mode (--hood <N001|folder>): the view
// (app/hood_view.hpp) and its UI (app/hood_ui.hpp) behind the mouse and the
// keys, and the way between the neighborhood and a lot.
//
// Entering a lot (the Lot Info Dialog's Enter Lot) and the chooser's Main
// Menu button call onEnterLot / onMainMenu: the app's game flow
// (app/game_flow.hpp) unloads the neighborhood and loads the lot (--hood <N>
// --hood-lot <id> --vm --ui live) or the main menu in the same process.
//
// Headless (--screenshot): --hood-hover <id> shows the hover popup over that
// lot, --hood-select <id> opens its Lot Info Dialog, --hood-panel
// families|lots opens a puck panel, --hood-family <id> selects a family of
// the bin (moving it in) and --hood-tool evict|bulldoze picks a tool; a
// --hood-lot-click <id> then clicks that lot with it.
//
// Moving a family in, out and bulldozing write the neighborhood folder (the
// user's copy; never data/game) through formats/hood_moves.hpp; moving in
// then enters the lot, as cTSUserToolNeighborhoodMoveIn does.

#include "app/hood_ui.hpp"
#include "app/hood_view.hpp"

#include <simd/simd.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace os2 {

class LotCamera;
class Renderer;
class ResourceIndex;

class HoodMode {
public:
    // `gameDir` is data/game; `args` the process arguments (argv[0] first),
    // kept for the restart into a lot.
    HoodMode(Renderer& renderer, ResourceIndex& index, const std::filesystem::path& gameDir, const std::string& query,
             std::vector<std::string> args);
    ~HoodMode();

    HoodView& view() { return *view_; }
    HoodUi& ui() { return *ui_; }
    std::unique_ptr<LotCamera> makeCamera() const;

    // Mouse in canvas pixels (origin top left) of a canvas w x h seen by `camera`.
    void mouseMoved(const LotCamera& camera, simd_float2 p, float w, float h);
    void mouseDown(LotCamera& camera, simd_float2 p, float w, float h);
    // The lot tools press, drag and release (cTSUserToolMoveLot's MouseEvent
    // 4/3/5): while the button is held the lot stays where it was pressed and
    // turns to face the drag (RotateLot, hood::frontFromDrag); the release
    // puts it down (a new empty lot asks its name first).
    void mouseDragged(const LotCamera& camera, simd_float2 p, float w, float h);
    void mouseUp(const LotCamera& camera, simd_float2 p, float w, float h);
    // Headless: a press at the placement's middle dragged by (dx, dy) world units.
    void dragTurn(float dx, float dy);
    // Esc: closes the dialog.
    void escape();
    // Enters a lot (onEnterLot).
    void enterLot(const HoodLot& lot);
    std::function<void(const HoodLot&)> onEnterLot;
    std::function<void()> onMainMenu;
    std::function<void()> onQuit; // the options panel's Quit
    // "Create New Family" in the Families panel: Create-a-Sim for a new
    // family of this neighborhood (its folder, the user's copy). When CAS has
    // written the family into the neighborhood package, familyCreated(id)
    // brings the view up to date and selects it in the family bin, ready to
    // move in. Without a handler the button does nothing.
    std::function<void(const std::filesystem::path& hoodFolder)> onCreateFamily;
    void familyCreated(uint32_t family);
    // A lot was placed, moved or sent to the bin: the view is rebuilt (the
    // terrain under the imposters changes). Without a handler, refresh().
    std::function<void()> onReload;
    // Rereads the neighborhood after it changed on disk.
    void refresh();

    // --hood-hover / --hood-select for screenshots; moves the camera to the lot.
    void scriptedView(LotCamera& camera, float w, float h);

    // The lot tools (formats/hood_lots.hpp): where the picked lot would go
    // with its corner near tile (x, y), shown as the footprint highlight;
    // place() writes it when it may go there.
    struct LotPlacement {
        bool any = false, ok = false;
        hood::TileRect rect;
        int x = 0, y = 0, turn = 0; // empty lot: rotation; bin or moved lot: front edge
        float height = 0;
        std::string why;
    };
    LotPlacement placementAt(int x, int y) const;
    // Writes the placement; a new empty lot first asks its name and zone
    // (HoodBins::openPlaceLot, as cTSUserToolPlaceNewLot::OnRelease shows
    // the place-lot dialog) and is placed when it is accepted.
    bool place(const LotPlacement& p);
    bool placeNamed(const LotPlacement& p, const std::string& name, uint8_t zone);
    // , and . while placing or moving a lot (key 0xACB3A629: KeyCommand of
    // cTSUserToolPlaceNewLot / PlaceLotFromInventory / MoveLot): the
    // rotation or front edge one quarter on (+1) or back (-1). Until a key
    // is pressed the tool takes the first turn that fits (a convenience).
    void turnLot(int direction);
    // The decorations tools' state for scripted runs: --hood-decor-place guid,tx,ty[,rotation].
    bool decorating() const;
    // Ctrl-Z / Ctrl-Y in the Decorations panel (its Undo and Redo buttons).
    void undoDecoration(bool redo);
    // A key for the place-lot dialog (HoodBins::typeKey); Return places the lot.
    void typeKey(int keyCode, const std::string& typed);
    bool placingLot() const;

private:
    std::optional<simd_int2> tileAt(const LotCamera& camera, simd_float2 p, float w, float h) const;
    void updatePlacement(const LotCamera& camera, simd_float2 p, float w, float h);
    void lotsChanged();
    LotPlacement pending_;
    uint32_t movingLot_ = 0; // the move tool's lot
    int turn_ = -1;          // the turn the keys chose (-1: none yet)
    LotPlacement armed_;     // the empty lot waiting for the place-lot dialog
    std::optional<simd_int2> lastTile_; // the corner tile under the mouse
    std::optional<simd_float2> pressGround_; // the ground under the press (this+0xf8), while the button is held
    std::optional<simd_float2> groundAt(const LotCamera& camera, simd_float2 p, float w, float h) const;
    void turnTo(int front);
    const HoodLot* lotAt(const LotCamera& camera, simd_float2 p, float w, float h) const;
    void carryOut(const HoodBins::Request& r);
    void lotClicked(const HoodLot& lot);
    void showFriends(const HoodLot& from);
    // Decorations (F3, docs/neighborhood-mac.md 14): the product's tile centre under the
    // mouse, and what a click does with the Decorate, Hand and Eyedropper tools.
    struct DecorOp {
        bool added = true; // false: a removal
        hood::DecorProduct product;
        hood::OccupantPosition at;
        float rotation = 0;
    };
    std::vector<DecorOp> decorUndo_, decorRedo_;
    float decorRotation_ = 0; // degrees, quarter turns (the keys , and .)
    std::optional<simd_float2> decorSpot_; // tile centre under the mouse
    const hood::DecorProduct* decorProduct(uint32_t guid) const;
    void updateDecoration(const LotCamera& camera, simd_float2 p, float w, float h);
    bool decorClick(const LotCamera& camera, simd_float2 p, float w, float h);
    bool placeDecoration(const hood::DecorProduct& product, float x, float y, float rotation, bool record = true);


    std::filesystem::path gameDir_;
    std::string query_;
    std::vector<std::string> args_;
    std::unique_ptr<HoodView> view_;
    std::unique_ptr<HoodUi> ui_;
};

// Process arguments with the neighborhood ones replaced: for a lot of
// neighborhood `hood` (--hood <hood> --hood-lot <id>, plus --vm --ui live)
// or back to the neighborhood (--hood <hood>).
std::vector<std::string> hoodLotArguments(const std::vector<std::string>& args, const std::string& hood, uint32_t lot);
std::vector<std::string> hoodArguments(const std::vector<std::string>& args, const std::string& hood);
// Replaces the process with `args` (args[0] is the executable).
[[noreturn]] void restartWith(const std::vector<std::string>& args);
// The lot package of --hood <hood> --hood-lot <id>.
std::filesystem::path hoodLotPackage(const std::filesystem::path& resDir, const std::string& hood, uint32_t lot);

} // namespace os2
