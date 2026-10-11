#pragma once

// Mouse control of the lot camera, as in the game's camera tools
// (docs/camera-ui-mac.md §1.4, from the Mac binary):
// cTSUserToolCameraRightClick: hold the right button and the camera moves
// at the mouse's offset from the anchor shown where the button went down,
// x 0.25 (DragCamera: a move request of that many pixels, taken here per
// 50 ms frame as the keyboard's); cTSUserToolCameraMiddleClick: the middle
// button (or right + a modifier, Option here) rotates and tilts by the
// pixels dragged, the cursor held where it was clicked;
// cTSWinSceneGraph::DoWindowEdgeScrolling: the mouse at the window's edge
// scrolls (its rates, horzedgescrollrate / vertedgescrollrate, are named in
// Sims2.exe only, so chosen here); the wheel zooms by notches (the binary's
// wheel axis x 1/120 = one per Windows notch).
// Plain C++; main.mm feeds it AppKit's events.

#include <utility>

namespace os2 {

class CameraMouseInput {
public:
    struct Config {
        float dragFactor = 0.25f; // move request per point of offset from the anchor (DragCamera)
        float edgeMargin = 4;     // points from the window edge that scroll
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

    // Edge scrolling: pan axes (right, forward), each in [-1, 1], for a
    // view of w x h points (none while the right button is held).
    std::pair<float, float> panAxes(float w, float h) const;
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
};

} // namespace os2
