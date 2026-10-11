// Mouse camera control: anchor scrolling, edge scrolling, wheel steps.
#include "app/camera_input.hpp"

#include <cstdio>

using namespace os2;

static int failures = 0;
static void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

int main() {
    CameraMouseInput in;
    in.mouseAt(400, 300);
    check(in.panAxes(800, 600) == std::pair<float, float>{0, 0}, "middle of the window: still");
    in.beginAnchor();
    in.mouseAt(403, 298);
    check(in.panAxes(800, 600) == std::pair<float, float>{0, 0}, "inside the dead zone");
    in.mouseAt(400 + 83, 300 - 160 - 50);
    auto [r, f] = in.panAxes(800, 600);
    check(r > 0.49f && r < 0.51f && f == 1.0f, "half speed right, full speed up (forward)");
    in.mouseAt(0, 300);
    check(in.panAxes(800, 600).first == -1.0f, "anchor wins over the edge");
    in.endAnchor();
    check(in.panAxes(800, 600) == std::pair<float, float>{-1, 0}, "left edge scrolls left");
    in.mouseAt(799, 599);
    check(in.panAxes(800, 600) == std::pair<float, float>{1, -1}, "bottom right corner");
    in.mouseAt(799, 599, false);
    check(in.panAxes(800, 600) == std::pair<float, float>{0, 0}, "outside the window: still");
    in.config().edgeScroll = false;
    in.mouseAt(0, 0);
    check(in.panAxes(800, 600) == std::pair<float, float>{0, 0}, "edge scrolling off");

    check(in.wheel(3.5, false) == 1 && in.wheel(-0.1, false) == -1, "a wheel notch is a level");
    check(in.wheel(30, true) == 0 && in.wheel(30, true) == 1 && in.wheel(-100, true) == -2, "trackpad accumulates");
    auto [yaw, tilt] = in.rotateDrag(10, -5);
    check(yaw == 3.0f && tilt == -1.0f, "middle drag rotates and tilts");

    if (failures)
        std::fprintf(stderr, "%d failure(s)\n", failures);
    else
        std::printf("camera_input: all tests passed\n");
    return failures ? 1 : 0;
}
