#include "camera_input.hpp"

#include <algorithm>
#include <cmath>

namespace os2 {

std::pair<float, float> CameraMouseInput::panAxes(float w, float h) const {
    if (anchored_ || !config_.edgeScroll || !inside_ || w <= 0 || h <= 0)
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

float CameraMouseInput::wheelNotches(double deltaY, bool precise) const {
    if (!precise)
        return deltaY > 0 ? 1.0f : deltaY < 0 ? -1.0f : 0.0f;
    return float(deltaY) / std::max(1.0f, config_.pointsPerNotch);
}

} // namespace os2
