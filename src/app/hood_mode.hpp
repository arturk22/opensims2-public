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
    // Esc: closes the dialog.
    void escape();
    // Enters a lot (onEnterLot).
    void enterLot(const HoodLot& lot);
    std::function<void(const HoodLot&)> onEnterLot;
    std::function<void()> onMainMenu;
    // "Create New Family" in the Families panel: Create-a-Sim for a new
    // family of this neighborhood (its folder, the user's copy). When CAS has
    // written the family into the neighborhood package, familyCreated(id)
    // brings the view up to date and selects it in the family bin, ready to
    // move in. Without a handler the button does nothing.
    std::function<void(const std::filesystem::path& hoodFolder)> onCreateFamily;
    void familyCreated(uint32_t family);
    // Rereads the neighborhood after it changed on disk.
    void refresh();

    // --hood-hover / --hood-select for screenshots; moves the camera to the lot.
    void scriptedView(LotCamera& camera, float w, float h);

private:
    const HoodLot* lotAt(const LotCamera& camera, simd_float2 p, float w, float h) const;
    void carryOut(const HoodBins::Request& r);
    void lotClicked(const HoodLot& lot);

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
