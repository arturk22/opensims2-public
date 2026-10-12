#pragma once

// Test aid (OS2_DEBUG_JUMPS=1, headless and live): flags Sims whose DRAWN
// place (the pelvis as the renderer poses it: LotSimulation's actor
// placement, seat bones, route interpolation, reframes) jumps more than a
// tile between two frames while the VM gave no reason (no Snap, no move of
// more than a walk step, no seat in or out, not just back in the world),
// jumps that go back and forth, and Sims the VM itself snaps over and over
// (a route failing and snapping every tick). Pure bookkeeping: the app
// feeds it one sample per Sim per frame.

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace os2 {

class JumpDetector {
public:
    struct Sample {
        int16_t id = 0;
        double time = 0;           // scene seconds
        uint64_t tick = 0;         // VM tick
        float drawn[3] = {0, 0, 0}; // the pelvis in the world (tiles)
        float vm[2] = {0, 0};       // the VM's position
        int level = 0;
        bool walking = false;      // the VM walks it (its route moves it a step a tick)
        int container = 0;         // the object whose slot holds it (0 none)
        bool inWorld = true;       // drawn at all (not at work, not carried away)
        std::string animation;     // what Animate Sim plays (for the report)
    };
    struct Event {
        int16_t id = 0;
        double time = 0;
        uint64_t tick = 0;
        float distance = 0;
        float from[3] = {0, 0, 0}, to[3] = {0, 0, 0};
        bool vmSnap = false;   // the VM moved it as far (a Snap or teleport: not counted as a bug)
        bool back = false;     // a jump back near where an unexplained jump started
        std::string kind;      // "drawn" (no VM reason), "vm-snap" (repeated VM snaps)
        std::string animation;
    };

    float maxStep = 1.0f;      // tiles between two frames
    float vmStep = 0.6f;       // a VM move this far in a frame without walking is a Snap
    double backWindow = 3.0;   // seconds for a jump back to count as back-and-forth
    double snapWindow = 10.0;  // seconds: this many VM snaps in it are flagged
    int snapRepeats = 4;

    // Feeds one Sim's sample; returns the events it raised (also kept).
    std::vector<Event> add(const Sample& s);
    const std::vector<Event>& events() const { return events_; }
    // Unexplained drawn jumps (and how many went back and forth).
    int drawnJumps() const;
    int backAndForth() const;
    int repeatedSnaps() const;
    static std::string describe(const Event& e);

private:
    struct Track {
        bool has = false;
        Sample last;
        std::vector<Event> recent;     // unexplained jumps within backWindow
        std::vector<double> snaps;     // VM snap times within snapWindow
        bool snapFlagged = false;
    };
    std::map<int16_t, Track> tracks_;
    std::vector<Event> events_;
};

} // namespace os2
