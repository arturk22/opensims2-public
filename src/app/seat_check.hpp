#pragma once

// Test aid (OS2_DEBUG_SEATS=1, headless and live; the seat battery,
// tests/seat_battery.sh): checks each Sim's DRAWN body against where it
// should be, frame by frame, and counts what looks broken:
//   - "seat": in an object's slot and at rest (no get-in/get-out playing for
//     a while), the pelvis far from the slot in the slot's frame (sideways
//     more than seatXY, or below/above the slot by more than the seated
//     height range);
//   - "ik": a hand whose IK goal is fully on (contact 1) ending farther
//     than ikReach from it (AnimPlayer::lastIkMisses; ikReachMoving while
//     getting in or out; goals on another Sim left out);
//   - "table": a body point (pelvis, spine, knees) inside the top slab of a
//     table or counter (passing through it), at rest;
//   - "chair": a body point inside a chair or stool the Sim is not in, at rest;
//   - "carry": a carry pose with nothing in the hands.
// Pure bookkeeping on world-space samples; the app feeds it.

#include "formats/xform.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace os2 {

class SeatCheck {
public:
    // An object's box (its model's bounds) where it is drawn.
    struct Box {
        int16_t id = 0;
        std::string name;
        Vec3 lo{0, 0, 0}, hi{0, 0, 0}; // model space
        Xform at;                      // model -> world
        bool table = false;            // a table or counter: only its top slab counts
    };
    struct Sample {
        int16_t id = 0;
        double time = 0;
        uint64_t tick = 0;
        std::string what;          // what it plays (for the report)
        bool walking = false;      // the VM routes it
        bool transitional = false; // a get-in, get-out, start or stop plays
        int16_t container = 0;     // the non-Sim object whose slot holds it (0 none)
        std::string containerName;
        bool lying = false;        // a bed or a tub: the pelvis lies away from the slot
        Xform seat;                // that slot, drawn (world)
        Vec3 pelvis{0, 0, 0};
        std::vector<std::pair<std::string, Vec3>> body; // other points (knees, spine)
        float ikMiss = 0;          // AnimPlayer::lastIkMiss (tiles)
        std::string ikChain;
        bool carryEmpty = false;
    };

    float seatXY = 0.25f;           // tiles
    float lyingXY = 1.0f;           // in a bed or a tub
    float seatBelow = 0.05f;        // the pelvis this far under the slot at most
    float seatAbove = 1.05f;        // and this far over it (a bar stool's slot is on the floor)
    float ikReach = 0.06f;          // tiles
    float ikReachMoving = 0.12f;    // getting in or out
    float slab = 0.06f;             // a table top's thickness that counts
    float shrink = 0.05f;           // chairs' boxes shrunk by this
    double settle = 0.6;            // seconds at rest before the seat and contacts count

    // Feeds one Sim's sample with the boxes near it; returns the problems
    // that started with it ("kind: detail").
    std::vector<std::string> add(const Sample& s, const std::vector<Box>& near);

    struct Count {
        int frames = 0, episodes = 0;
        float worst = 0;
        std::string example;
    };
    const std::map<std::string, Count>& counts() const { return counts_; }
    // Per container name: the pelvis in the slot's frame at rest (min, max).
    struct Range {
        Vec3 lo{1e9f, 1e9f, 1e9f}, hi{-1e9f, -1e9f, -1e9f};
        int frames = 0;
    };
    const std::map<std::string, Range>& seats() const { return seats_; }
    int episodes() const;
    std::string summary() const;

    // Whether `p` (world) is inside the box, shrunk by `shrink`; for a
    // table, inside its top `slab`.
    static bool inside(const Box& b, const Vec3& p, float shrink, float slab);

private:
    struct Track {
        double restSince = -1;      // at rest (not transitional, not walking) since
        int16_t container = 0;
        double containedSince = -1;
        std::map<std::string, bool> on; // kind -> flagged now
    };
    void flag(Track& t, const Sample& s, const std::string& kind, bool on, float value, const std::string& detail,
              std::vector<std::string>& out);
    std::map<int16_t, Track> tracks_;
    std::map<std::string, Count> counts_;
    std::map<std::string, Range> seats_;
};

} // namespace os2
