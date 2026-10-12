// The tutorial's UI state (src/vm/tutorial.*): what the UI Effects leave on
// the controls, and the progress window made anew dropping its buttons'.

#include "vm/vm.hpp"

#include <cstdio>

using namespace os2;
using namespace os2::vm;

static int failures = 0;

static void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

int main() {
    CodeLibrary code;
    VM vm{code};
    TutorialUi ui;
    auto push = [&](int mode, uint32_t w, uint32_t c, uint16_t tree = 0) {
        TutorialUiCommand cmd;
        cmd.mode = mode, cmd.window = w, cmd.control = c, cmd.tree = tree, cmd.object = 1;
        vm.tutorial.commands.push_back(cmd);
    };
    constexpr uint32_t kMain = 0x480E8304, kZoomIn = 0x1002, kBuy = 0x67D97B2A;
    push(uieffect::Disable, kProgressWindow, kProgressNext);
    push(uieffect::Highlight, kMain, kZoomIn);
    push(uieffect::EventTree, kMain, kZoomIn, 0x1031);
    push(uieffect::EventTree, kProgressWindow, kProgressBack, 0x1032);
    push(uieffect::Press, kMain, kBuy);
    push(uieffect::Hide, kMain, kBuy);
    ui.update(vm);
    check(!ui.enabled(kProgressWindow, kProgressNext), "Next disabled");
    check(ui.enabled(kMain, kZoomIn), "others enabled");
    check(ui.highlight() == TutorialUi::Key(kMain, kZoomIn), "zoom highlighted");
    check(ui.hasEventTree(kMain, kZoomIn) && ui.hasEventTree(kProgressWindow, kProgressBack), "click trees");
    check(!ui.visible(kMain, kBuy), "Buy hidden");
    const auto presses = ui.takePresses();
    check(presses.size() == 1 && presses[0] == TutorialUi::Key(kMain, kBuy), "the press");
    check(ui.blocked().size() == 2, "blocked: Next and Buy");
    // A new progress window: its buttons' state and trees go, the others stay.
    push(uieffect::ProgressShown, kProgressWindow, 0);
    ui.update(vm);
    check(ui.enabled(kProgressWindow, kProgressNext), "a new window's Next is on");
    check(!ui.hasEventTree(kProgressWindow, kProgressBack), "a new window's Back has no tree");
    check(ui.hasEventTree(kMain, kZoomIn), "the puck's tree stays");
    // Highlight with no control: none; Reset Trees: none; Restore: back to on.
    push(uieffect::Highlight, 0, 0);
    push(uieffect::ResetTrees, 0, 0);
    push(uieffect::Show, kMain, kBuy);
    ui.update(vm);
    check(ui.highlight() == TutorialUi::Key(0, 0), "no highlight");
    check(!ui.hasEventTree(kMain, kZoomIn), "trees reset");
    check(ui.visible(kMain, kBuy), "Buy shown");
    // A click on a disabled control does nothing.
    push(uieffect::Disable, kMain, kZoomIn);
    ui.update(vm);
    check(!ui.click(vm, kMain, kZoomIn), "disabled click refused");
    // Modal progress: Next closes it.
    vm.tutorial.progress.visible = true, vm.tutorial.progress.modal = true;
    check(dialogPausesGame(vm), "modal pauses the game");
    check(ui.click(vm, kProgressWindow, kProgressNext) && !vm.tutorial.progress.visible, "Next ends the modal window");
    if (failures)
        return 1;
    std::printf("tutorial: ok\n");
    return 0;
}
