#pragma once

// The tutorial's side of the VM (docs/tutorial-mac.md). The base game's
// tutorial is run by trees: "Controller - Tutorial - Main" (GUID 0x4C99AA97,
// group 0x7FA412A9) walks the steps of the episode of the lot it is on, and
// talks to the UI through two primitives, as the Mac binary does:
//
// - UI Effect (0x22, cTSObject::TryUIEffect): the window and control are
//   indices into an STR# 0x96 ("Button GUIDs") whose strings are window IDs
//   ("0x480e8304  -- W - Main Window"); the tutorial manager then presses,
//   disables, enables, highlights, shows or hides that control, or makes a
//   click on it run an event tree on the object (cTSTutorialManager).
// - Dialog (0x24) types 22/25 "TutorialNext": not a dialog that waits but
//   the tutorial's progress window (cTSTutorialManager::ShowProgressDisplay,
//   ui.package 0x2CDB7CBD): title, message, a picture, "Step %d of %d" from
//   temps 1 and 2; temp 1 = 0 hides it. Type 25 is modal: the game pauses
//   until its button is pressed.
//
// The host (the app) presents them; headless they are only recorded.

#include <cstdint>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace os2::vm {

class VM;
struct Context;
using ObjectId = int16_t;

// Operand byte 0 of UI Effect (the editor's labels).
namespace uieffect {
constexpr int ProgressShown = -2; // not an operand: the progress window was (re)created
constexpr int Press = 0;        // ActivateButton: as if the player clicked it
constexpr int Disable = 1;      // SaveControlEnableState + SetControlEnableState(false)
constexpr int Enable = 2;       // SaveControlEnableState + SetControlEnableState(true)
constexpr int Highlight = 3;    // SetTutorialHighlight (no window: none)
constexpr int EventTree = 4;    // SetControlCallback(control, tree, me)
constexpr int Restore = 5;      // RestoreControlEnableState
constexpr int DisableAll = 6;   // (no handler in the binary's switch)
constexpr int ResetTrees = 7;   // ResetControlCallbacks
constexpr int TnsEventTree = 8; // an event tree on a text notification's button
constexpr int Show = 9;         // SetControlVisible(true)
constexpr int Hide = 10;        // SetControlVisible(false)
} // namespace uieffect

// A UI Effect a tree ran, with its strings looked up: `window` and
// `control` are window IDs (0 for both: "none", as RESET GUID).
struct TutorialUiCommand {
    int mode = 0;
    uint32_t window = 0, control = 0;
    uint16_t effect = 0;
    uint16_t tree = 0;     // EventTree: the tree a click runs
    ObjectId object = 0;   // ... on this object (the one that ran the UI Effect)
    uint64_t tick = 0;
};

// The tutorial's progress window (cTSWinProcTutorialProgressDialog::Show).
struct TutorialProgress {
    bool visible = false;
    std::string title, message;
    uint32_t image = 0; // ui.package image (group 0x499DB772) for the picture, 0: none
    int step = 0, total = 0; // "Step %d of %d" when total > 1
    bool modal = false;      // type 25: the game waits for its button
    ObjectId owner = 0;      // the object whose tree showed it
    uint32_t serial = 0;     // changes every time it is shown again
};

struct TutorialState {
    TutorialProgress progress;
    std::deque<TutorialUiCommand> commands; // for the host to take (the latest 512)
};

// The episode controllers' GUIDs (OBJD type 7 "sim type").
constexpr uint32_t kTutorialControllerGuid = 0x4C99AA97;   // Controller - Tutorial - Main
constexpr uint32_t kTutorialDeathCheckerGuid = 0x4E03E8AD; // Controller - Tutorial - Death Checker

// Registers UI Effect (0x22) over the engine primitives' recorder.
void registerTutorialPrimitives(VM& vm);
// Dialog types 22/25 (dialog.cpp hands them over): always true at once.
int tutorialNextDialog(Context& c, int type, uint32_t stringsGroup);

// The tutorial's controllers on a lot that has none: the game's lots get the
// global "sim type" objects they lack when they load (inferred: the
// controller is in no lot's save and nothing in the data creates it; its Main
// runs the episode of the Tutorial neighborhood's lot and, on any other lot,
// the first-time Wants & Fears hints, and then removes itself). Returns how
// many were created.
int ensureTutorialControllers(VM& vm);

// One turn of the tutorial controllers' Main threads alone, for the host to
// call at the tick rate while the game is paused (Buy and Build mode pause
// it, and the tutorial's first step presses Buy mode): the episode goes on
// with time stopped (inferred: the steps of Buy and Build mode advance in
// the game, so the controller cannot stop with the clock).
void tickTutorialControllers(VM& vm);

// A click on a control with an event tree: runs it on `object` at once
// (cTSEdithWinEventListener, inferred to run the tree to its end); false if
// it failed or the object is gone.
bool runTutorialEventTree(VM& vm, ObjectId object, uint16_t tree);

// The progress window's script and buttons (ui.package 0x2CDB7CBD).
constexpr uint32_t kProgressWindow = 0x4D2B68A1;
constexpr uint32_t kProgressBack = 0xA9000005, kProgressNext = 0xA9000006;

// What the tutorial manager keeps of the UI Effects, for any host: the
// highlighted control, the ones disabled or hidden, the event trees of
// clicks. The progress window is made anew each time it is shown
// (cTSWinProcTutorialProgressDialog::Show), which drops what was set on
// its buttons.
class TutorialUi {
public:
    using Key = std::pair<uint32_t, uint32_t>; // window, control
    // Takes the VM's new UI Effects.
    void update(VM& vm);
    // A click on a control: false if the tutorial disabled it (the click
    // does nothing); otherwise its event tree, if any, runs. The progress
    // window's buttons are the host's to draw: Next of a modal one closes it.
    bool click(VM& vm, uint32_t window, uint32_t control);
    bool enabled(uint32_t window, uint32_t control) const;
    bool visible(uint32_t window, uint32_t control) const;
    bool hasEventTree(uint32_t window, uint32_t control) const;
    // The controls turned off (disabled or hidden) and those with a click tree.
    std::vector<Key> blocked() const;
    std::vector<Key> withTrees() const;
    // The highlighted control (0, 0: none).
    Key highlight() const { return highlight_; }
    // Presses (UI Effect mode 0) the host has not acted on yet.
    std::vector<Key> takePresses() { return std::exchange(presses_, {}); }
    void reset();

private:
    struct Callback {
        ObjectId object = 0;
        uint16_t tree = 0;
    };
    std::map<Key, Callback> callbacks_;
    std::map<Key, bool> enabled_;    // set by Disable/Enable; others are enabled
    std::set<Key> hidden_;
    Key highlight_{0, 0};
    std::vector<Key> presses_;
};

} // namespace os2::vm
