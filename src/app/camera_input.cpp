#include "camera_input.hpp"

#include <algorithm>
#include <cmath>

namespace os2 {

namespace {
float axis(float d, float dead, float full) {
    const float a = std::fabs(d);
    if (a <= dead)
        return 0;
    return std::copysign(std::min(1.0f, (a - dead) / std::max(1.0f, full - dead)), d);
}
} // namespace

std::pair<float, float> CameraMouseInput::panAxes(float w, float h) const {
    if (anchored_)
        return {axis(x_ - ax_, config_.deadZone, config_.fullSpeed),
                -axis(y_ - ay_, config_.deadZone, config_.fullSpeed)};
    if (!config_.edgeScroll || !inside_ || w <= 0 || h <= 0)
        return {0, 0};
    const float m = config_.edgeMargin;
    float right = 0, forward = 0;
    if (x_ <= m)
        right = -1;
    else if (x_ >= w - m)
        right = 1;
    if (y_ <= m)
        forward = 1;
    else if (y_ >= h - m)
        forward = -1;
    return {right, forward};
}

int CameraMouseInput::wheel(double deltaY, bool precise) {
    if (!precise)
        return deltaY > 0 ? 1 : deltaY < 0 ? -1 : 0;
    scroll_ += deltaY;
    int steps = 0;
    const double notch = config_.pointsPerZoomLevel;
    while (scroll_ >= notch)
        ++steps, scroll_ -= notch;
    while (scroll_ <= -notch)
        --steps, scroll_ += notch;
    return steps;
}

} // namespace os2
