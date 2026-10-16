#include "camera_input.hpp"

#include <algorithm>
#include <cmath>

namespace os2 {

std::pair<float, float> CameraMouseInput::panAxes(float w, float h) const {
    if (anchored_ || !config_.edgeScroll || !inside_ || w <= 0 || h <= 0)
        return {0, 0};
    const float m = (prevSideways_ != 0 || prevForward_ != 0) ? config_.edgeMarginScrolling : config_.edgeMargin;
    float right = 0, forward = 0;
    if (x_ < m)
        right = -1;
    else if (x_ > w - m)
        right = 1;
    if (y_ < m)
        forward = 1;
    else if (y_ > h - m)
        forward = -1;
    return {right, forward};
}

CameraMouseInput::EdgeSteps CameraMouseInput::edgeSteps(float w, float h) {
    const auto [right, forward] = panAxes(w, h);
    EdgeSteps e;
    e.sideways = -right * config_.edgeRate;
    e.forward = forward * config_.edgeRate;
    e.sendSideways = e.sideways != 0 || prevSideways_ != 0;
    e.sendForward = e.forward != 0 || prevForward_ != 0;
    prevSideways_ = e.sideways, prevForward_ = e.forward;
    return e;
}

float CameraMouseInput::wheelNotches(double deltaY, bool precise) const {
    if (!precise)
        return deltaY > 0 ? 1.0f : deltaY < 0 ? -1.0f : 0.0f;
    return float(deltaY) / std::max(1.0f, config_.pointsPerNotch);
}

} // namespace os2
