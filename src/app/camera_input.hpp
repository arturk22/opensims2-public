#pragma once

// Mouse control of the lot camera, as in the game's camera tools
// (docs/camera-ui-mac.md §1.4, from the Mac binary):
// cTSUserToolCameraRightClick: hold the right button and the camera moves
// at the mouse's offset from the anchor shown where the button went down,
// x 0.25 (DragCamera: a move request of that many pixels, taken here per
// 50 ms frame as the keyboard's); cTSUserToolCameraMiddleClick: the middle
// button (or the right + modifier 1, code 0x81000002: Control, as the
// tutorial says "CTRL + botão DIREITO"; Option too) rotates (x) and tilts
// (y) by the pixels dragged, the cursor held where it was clicked;
// cTSWinSceneGraph::DoWindowEdgeScrolling (BV 0x1fbe2c, once a frame): the
// mouse within 4 points of the window's edge (16 while it already scrolls)
// sends the camera's step requests with horzedgescrollrate /
// vertedgescrollrate (1.25, registered at BV 0x1ff3a0), and once 0 when it
// leaves; the wheel zooms by notches (the binary's wheel axis x 1/120 = one
// per Windows notch).
// Plain C++; main.mm feeds it AppKit's events.

#include <utility>

namespace os2 {

class CameraMouseInput {
public:
    enum class Button { Left, Right, Middle };
    // Whether a press starts the rotate/tilt tool (cTSUserToolCameraMiddleClick)
    // instead of the button's own: the middle button always; the right
    // with Control (the binary's modifier 1, the tutorial's "CTRL + botão
    // DIREITO") or Option (ours, kept); the left with Control (ours: a Mac
    // trackpad or one-button mouse, where Ctrl-click stands for the right
    // button; the host only asks for presses on the lot, not on the UI or
    // in Build mode's Ctrl tools).
    static bool rotatePress(Button b, bool control, bool option) {
        switch (b) {
        case Button::Middle: return true;
        case Button::Right: return control || option;
        case Button::Left: return control;
        }
        return false;
    }

    struct Config {
        float dragFactor = 0.25f; // move request per point of offset from the anchor (DragCamera)
        float edgeMargin = 4;     // points from the window edge that scroll
        float edgeMarginScrolling = 16; // ... while it already scrolls
        float edgeRate = 1.25f;   // horzedgescrollrate / vertedgescrollrate
        bool edgeScroll = true;
        float pointsPerNotch = 10;   // trackpad scroll per wheel notch
    };
    CameraMouseInput() = default;
    explicit CameraMouseInput(Config c) : config_(c) {}
    Config& config() { return config_; }

    // Points in view coordinates, origin top left.
    void mouseAt(float x, float y, bool inside = true) { x_ = x, y_ = y, inside_ = inside; }
    void beginAnchor() { anchored_ = true, ax_ = x_, ay_ = y_; }
    void endAnchor() { anchored_ = false; }
    bool anchored() const { return anchored_; }
    std::pair<float, float> anchor() const { return {ax_, ay_}; }

    // Edge scrolling: pan axes (right, forward), each -1, 0 or 1, for a
    // view of w x h points (none while the right button is held).
    std::pair<float, float> panAxes(float w, float h) const;
    // This frame's step requests of the edge (the binary's senses: sideways +
    // = left, forward + = forward, x the rate): an axis is sent while it is not
    // 0 or was not in the frame before (then 0 once).
    struct EdgeSteps {
        float sideways = 0, forward = 0;
        bool sendSideways = false, sendForward = false;
    };
    EdgeSteps edgeSteps(float w, float h);
    // Right button held: the move request (dx, dy points, y down) the
    // anchor's offset asks for.
    std::pair<float, float> dragMove() const {
        if (!anchored_)
            return {0, 0};
        return {(x_ - ax_) * config_.dragFactor, (y_ - ay_) * config_.dragFactor};
    }

    // Wheel notches (positive = closer) of a scroll event: one per mouse
    // wheel event, `pointsPerNotch` of trackpad scroll each.
    float wheelNotches(double deltaY, bool precise) const;

private:
    Config config_;
    float x_ = 0, y_ = 0, ax_ = 0, ay_ = 0;
    bool inside_ = false, anchored_ = false;
    float prevSideways_ = 0, prevForward_ = 0; // the cTSWinSceneGraph's +0x120 / +0x124
};

} // namespace os2
