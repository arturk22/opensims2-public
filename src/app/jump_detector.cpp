#include "app/jump_detector.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace os2 {

namespace {
float dist2(const float* a, const float* b) { return std::hypot(a[0] - b[0], a[1] - b[1]); }
} // namespace

std::vector<JumpDetector::Event> JumpDetector::add(const Sample& s) {
    std::vector<Event> out;
    Track& t = tracks_[s.id];
    if (!t.has || !s.inWorld || !t.last.inWorld) {
        t.has = s.inWorld;
        t.last = s;
        return out;
    }
    const Sample& p = t.last;
    float vmMove = dist2(p.vm, s.vm);
    if (s.container && p.container == s.container) {
        const float a[2] = {p.vm[0] - p.holder[0], p.vm[1] - p.holder[1]};
        const float b[2] = {s.vm[0] - s.holder[0], s.vm[1] - s.holder[1]};
        vmMove = dist2(a, b);
    }
    const bool levelChange = p.level != s.level;
    const bool vmSnap = (vmMove > vmStep && !(p.walking && s.walking && vmMove < 2 * vmStep)) || levelChange;
    const bool seatChange = p.container != s.container;
    if (vmSnap) {
        t.snaps.push_back(s.time);
        t.snaps.erase(std::remove_if(t.snaps.begin(), t.snaps.end(), [&](double x) { return x < s.time - snapWindow; }),
                      t.snaps.end());
        if (int(t.snaps.size()) >= snapRepeats && !t.snapFlagged) {
            Event e;
            e.id = s.id, e.time = s.time, e.tick = s.tick, e.distance = vmMove, e.vmSnap = true, e.kind = "vm-snap";
            std::copy(p.drawn, p.drawn + 3, e.from), std::copy(s.drawn, s.drawn + 3, e.to);
            e.animation = s.animation;
            t.snapFlagged = true;
            out.push_back(e);
        }
    } else if (t.snaps.empty() || t.snaps.back() < s.time - snapWindow) {
        t.snapFlagged = false;
    }
    float d = dist2(p.drawn, s.drawn);
    if (s.container && p.container == s.container) { // moved with its container (a car): relative to it
        const float a[2] = {p.drawn[0] - p.seat[0], p.drawn[1] - p.seat[1]};
        const float b[2] = {s.drawn[0] - s.seat[0], s.drawn[1] - s.seat[1]};
        d = dist2(a, b);
    }
    if (d > maxStep && !vmSnap && !seatChange) {
        Event e;
        e.id = s.id, e.time = s.time, e.tick = s.tick, e.distance = d, e.kind = "drawn";
        std::copy(p.drawn, p.drawn + 3, e.from), std::copy(s.drawn, s.drawn + 3, e.to);
        e.animation = s.animation;
        t.recent.erase(std::remove_if(t.recent.begin(), t.recent.end(), [&](const Event& x) { return x.time < s.time - backWindow; }),
                       t.recent.end());
        for (const Event& r : t.recent)
            if (dist2(r.from, e.to) < maxStep * 0.5f)
                e.back = true;
        t.recent.push_back(e);
        out.push_back(e);
    }
    t.last = s;
    events_.insert(events_.end(), out.begin(), out.end());
    return out;
}

int JumpDetector::drawnJumps() const {
    return int(std::count_if(events_.begin(), events_.end(), [](const Event& e) { return e.kind == "drawn"; }));
}
int JumpDetector::backAndForth() const {
    return int(std::count_if(events_.begin(), events_.end(), [](const Event& e) { return e.kind == "drawn" && e.back; }));
}
int JumpDetector::repeatedSnaps() const {
    return int(std::count_if(events_.begin(), events_.end(), [](const Event& e) { return e.kind == "vm-snap"; }));
}

std::string JumpDetector::describe(const Event& e) {
    char buf[512];
    std::snprintf(buf, sizeof buf, "%s Sim %d tick %llu (%.2f s): %.2f tiles (%.2f %.2f %.2f) -> (%.2f %.2f %.2f)%s anim '%s'",
                  e.kind == "drawn" ? "drawn jump:" : "repeated VM snaps:", e.id, (unsigned long long)e.tick, e.time,
                  e.distance, e.from[0], e.from[1], e.from[2], e.to[0], e.to[1], e.to[2], e.back ? " (back and forth)" : "",
                  e.animation.c_str());
    return buf;
}

} // namespace os2
