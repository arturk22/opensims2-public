// SeatCheck (app/seat_check.hpp): the seat battery's per-frame checks.
#include "app/seat_check.hpp"

#include <cmath>
#include <cstdio>

static int failures = 0;
static void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

using os2::SeatCheck;
using os2::Xform;

static SeatCheck::Sample seated(double t, float px, float py, float pz) {
    SeatCheck::Sample s;
    s.id = 5, s.time = t, s.tick = uint64_t(t * 24);
    s.container = 40, s.containerName = "Chair";
    s.seat.t = {10, 10, 0};
    s.pelvis = {px, py, pz};
    return s;
}

int main() {
    {
        // Seated over the slot: nothing; then 0.5 off to the side, at rest: one "seat" episode.
        SeatCheck c;
        for (int i = 0; i < 40; ++i)
            c.add(seated(i / 24.0, 10.0f, 9.97f, 0.71f), {});
        check(c.episodes() == 0, "seated over the slot");
        for (int i = 40; i < 80; ++i)
            c.add(seated(i / 24.0, 10.5f, 10.0f, 0.71f), {});
        check(c.counts().count("seat") && c.counts().at("seat").episodes == 1, "pelvis off the slot");
        check(c.seats().at("Chair").frames > 0, "seat range kept");
    }
    {
        // In the air over a slot on the floor (a stool's 0.92 is fine, 1.3 is not), but only once settled.
        SeatCheck c;
        for (int i = 0; i < 40; ++i)
            c.add(seated(i / 24.0, 10.0f, 10.0f, 0.92f), {});
        check(c.episodes() == 0, "a bar stool's height");
        auto s = seated(2.0, 10.0f, 10.0f, 1.3f);
        s.transitional = true; // getting out: not checked
        c.add(s, {});
        check(c.episodes() == 0, "not while getting out");
        for (int i = 0; i < 40; ++i)
            c.add(seated(3.0 + i / 24.0, 10.0f, 10.0f, 1.3f), {});
        check(c.episodes() == 1, "sitting in the air");
    }
    {
        // Through a table's top: a point in its top slab counts, under it not.
        SeatCheck::Box table;
        table.id = 9, table.name = "Table", table.table = true;
        table.lo = {-0.5f, -0.5f, 0}, table.hi = {0.5f, 0.5f, 0.75f};
        table.at.t = {12, 10, 0};
        check(SeatCheck::inside(table, {12, 10, 0.72f}, 0.02f, 0.06f), "in the top slab");
        check(!SeatCheck::inside(table, {12, 10, 0.4f}, 0.02f, 0.06f), "knees under the table");
        SeatCheck c;
        SeatCheck::Sample s;
        s.id = 3, s.pelvis = {12, 10, 0.72f};
        for (int i = 0; i < 40; ++i)
            s.time = i / 24.0, c.add(s, {table});
        check(c.counts().at("table").episodes == 1, "standing through the table");
    }
    {
        // Seated with the feet ahead of the pelvis: fine; folded back behind it
        // (the child kneeling through the chair's back): one "legs" episode.
        SeatCheck c;
        for (int i = 0; i < 40; ++i) {
            auto s = seated(i / 24.0, 10.0f, 10.0f, 0.6f);
            s.feet = {{9.93f, 10.3f, 0.05f}, {10.07f, 10.3f, 0.05f}};
            c.add(s, {});
        }
        check(c.episodes() == 0, "feet ahead of the seat");
        for (int i = 40; i < 80; ++i) {
            auto s = seated(i / 24.0, 10.0f, 10.0f, 0.6f);
            s.feet = {{9.93f, 9.6f, 0.3f}, {10.07f, 9.6f, 0.3f}};
            c.add(s, {});
        }
        check(c.counts().count("legs") && c.counts().at("legs").episodes == 1, "feet behind the seat");
    }
    {
        // A hand short of a goal fully on.
        SeatCheck c;
        SeatCheck::Sample s;
        s.id = 2, s.ikMiss = 0.2f, s.ikChain = "r_hand";
        c.add(s, {});
        check(c.counts().at("ik").episodes == 1, "hand off its goal");
    }
    if (failures)
        return 1;
    std::printf("seat check: ok\n");
    return 0;
}
