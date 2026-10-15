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
// - Tab and Y ("Cycle Camera Controller", 0x0CA8D3F3): cCameraLive::DoMessage
//   (BV 0x6a3df0, the Live and the Build camera alike) makes "CameraMan" the
//   current controller in a lot ("CameraManNH" in the neighborhood); the
//   free camera remembers which controller it came from (its SetActive,
//   0x7a7d2c, keeps the previous controller's name) and Tab or Escape
//   (0x8CA297E2, cCameraCameraManController::DoMessage 0x7a6878) make that one
//   current again. Any SetTopDown or mode change that follows asks the
//   manager for "Live" or "Build" and so ends the free camera too.
//   (The "First" controller, the camera on a Sim's head, is not reached by any key.)

namespace os2 {

class CameraSelector {
public:
    enum class Mode { Live, Buy, Build };
    // The controllers of the lot: the "Live" camera and the top-down "Build"
    // one are the same class (cCameraLive) with different configurations.
    enum class Controller { Live, Build, CameraMan };

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

    // Tab / Y: the free camera, and back to the controller it came from.
    void cycleController() {
        if (controller_ == Controller::CameraMan)
            controller_ = returnTo_;
        else
            returnTo_ = controller_, controller_ = Controller::CameraMan;
    }
    // Escape in the free camera (and only there) leaves it too; false elsewhere.
    bool escape() {
        if (controller_ != Controller::CameraMan)
            return false;
        controller_ = returnTo_;
        return true;
    }
    bool freeCamera() const { return controller_ == Controller::CameraMan; }

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
    Controller returnTo_ = Controller::Live; // what the free camera came from
    bool topDown_ = false;
};

} // namespace os2
