#pragma once

// Test aid (OS2_DEBUG_GLIDE=1, headless and live): flags Sims that glide
// over the floor, their drawn pelvis moving on (over `minSpeed` tiles a
// second, kept up for `window` seconds) while their legs hold still (the
// feet, measured from the pelvis in the body's own frame, move less than
// `legRange` over that time): a walk whose clips did not start, a pose or
// overlay over the walk, a route moving a Sim that plays no locomotion.
// Pure bookkeeping: the app feeds it one sample per Sim per frame and
// prints what the Sim was doing when an episode starts.

#include <cstdint>
#include <deque>
#include <map>
#include <vector>

namespace os2 {

class GlideDetector {
public:
    // Forgets the Sims followed (a new scene: its object IDs name other Sims);
    // the totals stay.
    void forgetSims() { tracks_.clear(); }
    struct Sample {
        int16_t id = 0;
        double time = 0;           // scene seconds
        uint64_t tick = 0;
        float pelvis[2] = {0, 0};  // drawn, in the world (tiles)
        float lfoot[3] = {0, 0, 0}, rfoot[3] = {0, 0, 0}; // from the pelvis, in the body's frame
        bool inWorld = true;       // drawn, not carried, not in a container's slot
    };
    struct Event {
        int16_t id = 0;
        double time = 0;
        uint64_t tick = 0;
        float distance = 0; // the pelvis' move over the window
        float legs = 0;     // the feet's largest range over it
    };

    double window = 1.0;    // seconds
    float minSpeed = 0.4f;  // tiles a second
    float legRange = 0.06f; // tiles: the feet's range under which the legs hold still
    float maxFrameStep = 0.5f; // a frame moving farther is a jump (not a glide)

    // Feeds one sample; returns an event when a Sim starts gliding.
    std::vector<Event> add(const Sample& s);
    const std::vector<Event>& events() const { return events_; }
    int episodes() const { return int(events_.size()); }
    // Seconds spent gliding (every frame of the episodes).
    double seconds() const { return seconds_; }
    bool gliding(int16_t id) const;

private:
    struct Track {
        std::deque<Sample> samples;
        bool gliding = false;
        double lastGlide = -1e9;
    };
    std::map<int16_t, Track> tracks_;
    std::vector<Event> events_;
    double seconds_ = 0;
};

} // namespace os2
