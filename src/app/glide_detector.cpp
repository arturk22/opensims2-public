#include "app/glide_detector.hpp"

#include <algorithm>
#include <cmath>

namespace os2 {

std::vector<GlideDetector::Event> GlideDetector::add(const Sample& s) {
    std::vector<Event> out;
    Track& t = tracks_[s.id];
    if (!s.inWorld) {
        t.samples.clear();
        t.gliding = false;
        return out;
    }
    if (!t.samples.empty()) {
        const Sample& p = t.samples.back();
        if (std::hypot(s.pelvis[0] - p.pelvis[0], s.pelvis[1] - p.pelvis[1]) > maxFrameStep || s.time < p.time)
            t.samples.clear(); // a jump (a Snap): start over from here
    }
    t.samples.push_back(s);
    while (t.samples.size() > 2 && t.samples[1].time <= s.time - window)
        t.samples.pop_front();
    const Sample& first = t.samples.front();
    const double span = s.time - first.time;
    bool glide = false;
    float moved = 0, legs = 0;
    if (span >= window * 0.9) {
        moved = std::hypot(s.pelvis[0] - first.pelvis[0], s.pelvis[1] - first.pelvis[1]);
        if (moved > minSpeed * span) {
            auto range = [&](const float(Sample::*foot)[3]) {
                float lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f};
                for (const Sample& x : t.samples)
                    for (int i = 0; i < 3; ++i)
                        lo[i] = std::min(lo[i], (x.*foot)[i]), hi[i] = std::max(hi[i], (x.*foot)[i]);
                return std::hypot(hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]);
            };
            legs = std::max(range(&Sample::lfoot), range(&Sample::rfoot));
            glide = legs < legRange;
        }
    }
    if (glide) {
        if (t.samples.size() >= 2)
            seconds_ += s.time - t.samples[t.samples.size() - 2].time;
        if (!t.gliding && s.time - t.lastGlide > 1.0) { // (a pause under a second is the same episode)
            Event e;
            e.id = s.id, e.time = s.time, e.tick = s.tick, e.distance = moved, e.legs = legs;
            out.push_back(e);
            events_.push_back(e);
        }
    }
    if (glide)
        t.lastGlide = s.time;
    t.gliding = glide;
    return out;
}

bool GlideDetector::gliding(int16_t id) const {
    const auto it = tracks_.find(id);
    return it != tracks_.end() && it->second.gliding;
}

} // namespace os2
