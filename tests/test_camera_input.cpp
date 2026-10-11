// Mouse camera control: anchor scrolling, edge scrolling, wheel notches.
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

    check(in.wheelNotches(3.5, false) == 1 && in.wheelNotches(-0.1, false) == -1, "a wheel event is a notch");
    check(in.wheelNotches(30, true) == 3 && in.wheelNotches(-5, true) == -0.5f, "trackpad points per notch");

    if (failures)
        std::fprintf(stderr, "%d failure(s)\n", failures);
    else
        std::printf("camera_input: all tests passed\n");
    return failures ? 1 : 0;
}
