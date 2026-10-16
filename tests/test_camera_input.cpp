// Mouse camera control: right-button drag, edge scrolling, wheel notches.
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
    in.mouseAt(440, 280);
    check(in.dragMove() == std::pair<float, float>{10, -5}, "right drag: offset x 0.25");
    in.mouseAt(0, 300);
    check(in.panAxes(800, 600) == std::pair<float, float>{0, 0}, "no edge scrolling while dragging");
    in.endAnchor();
    check(in.dragMove() == std::pair<float, float>{0, 0}, "released: still");
    check(in.panAxes(800, 600) == std::pair<float, float>{-1, 0}, "left edge scrolls left");
    in.mouseAt(799, 599);
    check(in.panAxes(800, 600) == std::pair<float, float>{1, -1}, "bottom right corner");
    in.mouseAt(799, 599, false);
    check(in.panAxes(800, 600) == std::pair<float, float>{0, 0}, "outside the window: still");
    in.config().edgeScroll = false;
    in.mouseAt(0, 0);
    check(in.panAxes(800, 600) == std::pair<float, float>{0, 0}, "edge scrolling off");

    // DoWindowEdgeScrolling (BV 0x1fbe2c): within 4 points of the edge, 16
    // while it already scrolls; the step requests in the binary's senses (+
    // = left / forward) x 1.25, and one 0 when the mouse leaves.
    {
        CameraMouseInput e;
        e.mouseAt(10, 300);
        CameraMouseInput::EdgeSteps s = e.edgeSteps(800, 600);
        check(!s.sendSideways && !s.sendForward, "10 points in: no edge");
        e.mouseAt(2, 300);
        s = e.edgeSteps(800, 600);
        check(s.sendSideways && s.sideways == 1.25f && !s.sendForward, "the left edge: sideways +1.25 (left)");
        e.mouseAt(10, 300);
        s = e.edgeSteps(800, 600);
        check(s.sendSideways && s.sideways == 1.25f, "scrolling: the edge is 16 points wide");
        e.mouseAt(20, 300);
        s = e.edgeSteps(800, 600);
        check(s.sendSideways && s.sideways == 0, "out of the edge: one 0");
        s = e.edgeSteps(800, 600);
        check(!s.sendSideways, "then nothing");
        e.mouseAt(400, 1);
        s = e.edgeSteps(800, 600);
        check(s.sendForward && s.forward == 1.25f, "the top edge: forward");
    }

    check(in.wheelNotches(3.5, false) == 1 && in.wheelNotches(-0.1, false) == -1, "a wheel event is a notch");
    check(in.wheelNotches(30, true) == 3 && in.wheelNotches(-5, true) == -0.5f, "trackpad points per notch");

    if (failures)
        std::fprintf(stderr, "%d failure(s)\n", failures);
    else
        std::printf("camera_input: all tests passed\n");
    return failures ? 1 : 0;
}
