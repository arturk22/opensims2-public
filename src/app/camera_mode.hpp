#pragma once

// Which camera controller the lot shows, as the game's main toolbar and
// camera controller manager decide it (docs/camera-ui-mac.md §3, from the
// Mac binary). Plain C++; the app (live_setup.mm, app_view.mm) feeds it the
// mode changes and keys and swaps the LotCamera's definition to match.
//
// - cTSMainToolbar::SetTopDown (BV 0x1a9674): on = the camera manager makes
//   "Build" (BuildCamera.txt, the top-down view) the current controller,
//   off = "Live"; the toolbar remembers the flag (+0xa8).
// - cTSMainToolbar::SetMode (BV 0x1ade24), for the play modes Live / Buy /
//   Build: with the flag off the Live camera is made current (Buy and Build
//   note the visit); with it on, Buy and Build make "Build" current and Live
//   makes "Live" current but keeps the flag, so the next Buy or Build is top
//   down again.
// - The key T ("Top down view", 0x2CEE0269) is only handled by the Buy and
//   Build panels (cTSWinProcBuildPanel::DoMessage, BV 0x286f80; the Buy
//   panel's alike): it toggles their top-down button (Build 0xA3, Buy
//   0xCC607036), whose onBtnToggledID calls SetTopDown with the new state.
//   In Live mode T does nothing.

namespace os2 {

class CameraSelector {
public:
    enum class Mode { Live, Buy, Build };
    // The controllers of the lot: the "Live" camera and the top-down "Build"
    // one are the same class (cCameraLive) with different configurations.
    enum class Controller { Live, Build };

    Controller controller() const { return controller_; }
    bool topDown() const { return topDown_; }
    Mode mode() const { return mode_; }

    // SetTopDown(on).
    void setTopDown(bool on) {
        controller_ = on ? Controller::Build : Controller::Live;
        topDown_ = on;
    }

    // SetMode(mode).
    void setMode(Mode m) {
        mode_ = m;
        if (!topDown_) {
            setTopDown(false);
        } else if (m != Mode::Live) {
            setTopDown(true);
        } else {
            setTopDown(false);
            topDown_ = true; // Live shows the Live camera but remembers the flag
        }
    }

    // The key T, or a click on the panel's top-down button: false when the
    // mode has no such button (Live).
    bool toggleTopDown() {
        if (mode_ == Mode::Live)
            return false;
        setTopDown(!topDown_);
        return true;
    }

private:
    Mode mode_ = Mode::Live;
    Controller controller_ = Controller::Live;
    bool topDown_ = false;
};

} // namespace os2
