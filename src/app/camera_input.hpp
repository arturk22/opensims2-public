#pragma once

// Mouse control of the lot camera, as in the game's camera tools
// (cTSUserToolCameraRightClick: hold the right button and the view scrolls
// towards the mouse, faster the farther it is from the anchor shown where
// the button went down; cTSUserToolCameraMiddleClick: hold the middle button
// to rotate and tilt; cTSWinSceneGraph::DoWindowEdgeScrolling: the mouse at
// the window's edge scrolls; the wheel zooms). Rotation, tilt and the wheel
// go to the camera as the game's requests (pixels dragged, wheel notches:
// LotCamera::rotateRequest / wheel). The scroll speeds and sizes are not in
// the data found (horzedgescrollrate / vertedgescrollrate are named in
// Sims2.exe only), so they are chosen here.
// Plain C++; main.mm feeds it AppKit's events.

#include <utility>

namespace os2 {

class CameraMouseInput {
public:
    struct Config {
        float deadZone = 6;       // points around the anchor that do not scroll
        float fullSpeed = 160;    // points from the anchor at full scroll speed
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

    // Pan axes (right, forward), each in [-1, 1], for a view of w x h
    // points: from the anchor offset while the right button is held, else
    // from the window edges.
    std::pair<float, float> panAxes(float w, float h) const;

    // Wheel notches (positive = closer) of a scroll event: one per mouse
    // wheel event, `pointsPerNotch` of trackpad scroll each.
    float wheelNotches(double deltaY, bool precise) const;

private:
    Config config_;
    float x_ = 0, y_ = 0, ax_ = 0, ay_ = 0;
    bool inside_ = false, anchored_ = false;
};

} // namespace os2
