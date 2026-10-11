#pragma once

// Mouse control of the lot camera, as in the game's camera tools
// (cTSUserToolCameraRightClick: hold the right button and the view scrolls
// towards the mouse, faster the farther it is from the anchor shown where
// the button went down; cTSUserToolCameraMiddleClick: hold the middle button
// to rotate and tilt; cTSWinSceneGraph::DoWindowEdgeScrolling: the mouse at
// the window's edge scrolls; the wheel steps through the zoom levels). The
// speeds and sizes are not in the data found (horzedgescrollrate /
// vertedgescrollrate are named in Sims2.exe only), so they are chosen here.
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
        float rotatePerPoint = 0.3f; // degrees of yaw per point dragged (middle button)
        float tiltPerPoint = 0.2f;   // degrees of pitch per point dragged
        float pointsPerZoomLevel = 40; // trackpad scroll per zoom level
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

    // Wheel: zoom levels to step (positive = closer). Mouse wheels step one
    // level a notch; trackpads accumulate `pointsPerZoomLevel`.
    int wheel(double deltaY, bool precise);

    // Middle-button drag by (dx, dy) points: (yaw degrees, tilt degrees).
    std::pair<float, float> rotateDrag(float dx, float dy) const {
        return {dx * config_.rotatePerPoint, dy * config_.tiltPerPoint};
    }

private:
    Config config_;
    float x_ = 0, y_ = 0, ax_ = 0, ay_ = 0;
    bool inside_ = false, anchored_ = false;
    double scroll_ = 0;
};

} // namespace os2
