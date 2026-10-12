#pragma once

// The game's start-to-play flow in one process: main menu (the neighborhood
// chooser, app/main_menu.hpp) -> neighborhood view (app/hood_mode.hpp) ->
// lot (live mode) -> back, as cTSGameStateController goes (docs/
// neighborhood-mac.md §6), without restarting the app: a scene is unloaded
// and the next one loaded in place.
//
// A scene is chosen by the app's arguments, so the same flags pick it at
// start-up and in a switch:
//
//   --menu                                   the main menu
//   --hood <N001|folder>                     the neighborhood view
//   --hood <N001|folder> --hood-lot <id>     a lot of it (with --vm --ui live)
//
// With no scene flag (and nothing else to show: --object, --lot, --sim,
// --skinned, --character, --ui) the app opens the main menu, as the game
// does. Neighborhoods named by prefix ("N001") are the user's copies
// (app/user_data.hpp), made on first use; a folder path is used as given.
//
// Headless, the --click x,y / --right-click x,y / --wait <ticks> steps are a
// script that runs across scenes: each scene takes them in order until one
// switches scenes, and the next scene gets the rest. --flow-shots also saves
// each scene, as it is just before it is left, to <shot>-1.png, -2.png, ...
// (the last scene goes to the --screenshot file itself). Plain C++.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace os2 {

struct Scene {
    enum class Kind { MainMenu, Neighborhood, Lot, Cas, Other };
    Kind kind = Kind::Other;
    std::string hood; // --hood value (prefix or folder)
    uint32_t lot = 0; // --hood-lot
    bool fromNeighborhood = false; // main menu opened from a neighborhood (Close goes back)
    std::string returnHood;        // that neighborhood
    // Cas: Create-a-Sim for a new family of the neighborhood folder `hood`
    // (--cas --cas-hood), returning to the neighborhood `returnHood`.
};

// The scene `args` pick (args[0] is the executable).
Scene sceneOf(const std::vector<std::string>& args);

// One scripted step: a click (left or right) at canvas x,y, a wait of n VM
// ticks, or (--pie-step <object>:<entry>) the selected Sim's pie menu on an
// object (as --pie names it) and an entry picked: by its index, or the one
// whose name holds <entry>; or (--tutorial-auto <n>) up to n turns of playing
// along with a tutorial: a click on the control its arrow points at, else on
// its panel's Next, through the UI as the mouse does, 40 ticks apart (stops
// when neither is on); or (--key space) a key: the space bar selects the
// next Sim, (--key pageup / pagedown) the level shown, (--key escape) Esc.
struct FlowStep {
    enum class Kind { Click, RightClick, Wait, Pie, TutorialAuto, Key } kind = Kind::Click;
    float x = 0, y = 0;
    long ticks = 0;
    // --pie-step <object>:<entry>: the selected Sim picks that entry of the
    // object's pie menu (as a click on it and on the entry would).
    std::string object;
    int entry = 0;      // -1: the entry whose name holds `text`
    std::string text;   // what follows the colon
};
// The steps in `args`, in order.
std::vector<FlowStep> flowSteps(const std::vector<std::string>& args);

// `args` for scene `next`: the scene flags of the current one replaced by
// next's, and the first `stepsUsed` steps dropped (the rest stay, in order).
std::vector<std::string> sceneArguments(const std::vector<std::string>& args, const Scene& next, size_t stepsUsed);

// The flag's value / presence in `args` (args[0] skipped).
std::optional<std::string> argValue(const std::vector<std::string>& args, const std::string& flag);
bool hasArg(const std::vector<std::string>& args, const std::string& flag);

// <shot>-<n>.<ext> for --flow-shots.
std::string numberedShot(const std::string& shot, int n);

} // namespace os2
