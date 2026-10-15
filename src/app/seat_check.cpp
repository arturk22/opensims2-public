#include "app/seat_check.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace os2 {

bool SeatCheck::inside(const Box& b, const Vec3& p, float shrink, float slab) {
    const Vec3 l = b.at.inverse().apply(p);
    for (int k = 0; k < 2; ++k)
        if (l[size_t(k)] < b.lo[size_t(k)] + shrink || l[size_t(k)] > b.hi[size_t(k)] - shrink)
            return false;
    if (b.table)
        return l[2] <= b.hi[2] && l[2] >= b.hi[2] - slab;
    return l[2] >= b.lo[2] + shrink && l[2] <= b.hi[2] - shrink;
}

void SeatCheck::flag(Track& t, const Sample& s, const std::string& kind, bool on, float value, const std::string& detail,
                     std::vector<std::string>& out) {
    bool& was = t.on[kind];
    if (on) {
        Count& c = counts_[kind];
        ++c.frames;
        if (!was) {
            ++c.episodes;
            char buf[512];
            std::snprintf(buf, sizeof buf, "%s: tick %llu Sim %d %.2f %s in '%s'", kind.c_str(), (unsigned long long)s.tick, s.id,
                          value, detail.c_str(), s.what.c_str());
            out.push_back(buf);
            if (c.example.empty() || value > c.worst)
                c.example = buf;
        }
        c.worst = std::max(c.worst, value);
    }
    was = on;
}

std::vector<std::string> SeatCheck::add(const Sample& s, const std::vector<Box>& near) {
    std::vector<std::string> out;
    Track& t = tracks_[s.id];
    const bool rest = !s.walking && !s.transitional;
    if (!rest)
        t.restSince = -1;
    else if (t.restSince < 0)
        t.restSince = s.time;
    if (s.container != t.container) {
        t.leftChair = s.container ? 0 : t.container;
        t.container = s.container, t.containedSince = s.time;
    }
    if (s.walking)
        t.leftChair = 0;
    const bool settled = rest && s.time - t.restSince >= settle;
    // In a slot, at rest: the pelvis over the slot, in the slot's frame.
    {
        bool bad = false;
        float value = 0;
        std::string detail;
        if (s.container && settled && s.time - t.containedSince >= settle) {
            const Vec3 l = s.seat.inverse().apply(s.pelvis);
            Range& r = seats_[s.containerName];
            for (int k = 0; k < 3; ++k)
                r.lo[size_t(k)] = std::min(r.lo[size_t(k)], l[size_t(k)]), r.hi[size_t(k)] = std::max(r.hi[size_t(k)], l[size_t(k)]);
            ++r.frames;
            const float side = std::hypot(l[0], l[1]);
            bad = side > (s.lying ? lyingXY : seatXY) || l[2] < -seatBelow || l[2] > seatAbove;
            value = std::max({side, -l[2], l[2] - seatAbove, 0.0f});
            char buf[200];
            std::snprintf(buf, sizeof buf, "pelvis (%.2f %.2f %.2f) in the slot of %d %s", l[0], l[1], l[2], s.container,
                          s.containerName.c_str());
            detail = buf;
        }
        flag(t, s, "seat", bad, value, detail, out);
    }
    // Sitting, the feet ahead of (or under) the pelvis, never behind the seat.
    {
        float behind = 0;
        if (s.container && !s.lying && settled && s.time - t.containedSince >= settle) {
            const float py = s.seat.inverse().apply(s.pelvis)[1];
            for (const Vec3& f : s.feet)
                behind = std::max(behind, py - s.seat.inverse().apply(f)[1]);
        }
        flag(t, s, "legs", behind > feetBehind, behind, "foot behind the pelvis in " + s.containerName, out);
    }
    // A hand short of its IK goal (more slack while getting in or out, the
    // goals on a chair that moves with it; a goal on another Sim is out:
    // the VM's social distances are not this module's).
    const bool onSim = s.ikChain.find(" of a2a-") != std::string::npos || s.ikChain.find(" of a2c-") != std::string::npos ||
                       s.ikChain.find(" of c2a-") != std::string::npos || s.ikChain.find(" of c2c-") != std::string::npos;
    const float reach = s.transitional ? ikReachMoving : ikReach;
    flag(t, s, "ik", !s.walking && !onSim && s.ikMiss > reach, s.ikMiss, "from the goal of " + s.ikChain, out);
    // Through a table's top, or a chair the Sim is not in.
    {
        bool table = false, chair = false;
        std::string tableWhat, chairWhat;
        if (settled) {
            std::vector<std::pair<std::string, Vec3>> points = s.body;
            points.insert(points.begin(), {"pelvis", s.pelvis});
            for (const Box& b : near) {
                if (b.id == s.container || b.id == t.leftChair)
                    continue;
                for (const auto& [name, p] : points)
                    if (inside(b, p, b.table ? 0.02f : shrink, slab)) {
                        if (b.table && !table)
                            table = true, tableWhat = name + " in " + std::to_string(b.id) + " " + b.name;
                        else if (!b.table && !chair)
                            chair = true, chairWhat = name + " in " + std::to_string(b.id) + " " + b.name;
                    }
            }
        }
        flag(t, s, "table", table, 1, tableWhat, out);
        flag(t, s, "chair", chair, 1, chairWhat, out);
    }
    flag(t, s, "carry", s.carryEmpty, 1, "carry pose with empty hands", out);
    return out;
}

int SeatCheck::episodes() const {
    int n = 0;
    for (const auto& [k, c] : counts_)
        n += c.episodes;
    return n;
}

std::string SeatCheck::summary() const {
    std::string s;
    char buf[160];
    for (const char* k : {"seat", "ik", "table", "chair", "carry", "legs"}) {
        const auto it = counts_.find(k);
        std::snprintf(buf, sizeof buf, "%s%s %d (%d frames)", s.empty() ? "" : ", ", k, it == counts_.end() ? 0 : it->second.episodes,
                      it == counts_.end() ? 0 : it->second.frames);
        s += buf;
    }
    return s;
}

} // namespace os2
