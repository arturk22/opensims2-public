#include "app/tv_program.hpp"

#include <cstdio>
#include <random>
#include <string>

using os2::TvProgram;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

int main() {
    std::mt19937 rng(7);
    TvProgram<std::string> p;
    p.shows = {"a", "b", "c"};
    p.commercials = {"x", "y", "z"};
    std::string last, k;
    bool ad = false;
    // Shows in order (wrapping), a commercial between each two, never repeated.
    const char* expect[] = {"a", "b", "c", "a", "b"};
    for (int round = 0; round < 5; ++round) {
        CHECK(p.next(rng, k, &ad) && !ad && k == expect[round]);
        CHECK(p.next(rng, k, &ad) && ad);
        CHECK(k != last);
        last = k;
    }
    // Many commercials in a row of rounds: never the same twice.
    for (int i = 0; i < 500; ++i) {
        p.next(rng, k, &ad);
        p.next(rng, k, &ad);
        CHECK(k != last);
        last = k;
    }
    // One commercial only: it repeats (nothing else to draw).
    TvProgram<int> q;
    q.shows = {1};
    q.commercials = {9};
    int v = 0;
    q.next(rng, v);
    CHECK(v == 1);
    q.next(rng, v, &ad);
    CHECK(ad && v == 9);
    q.next(rng, v);
    q.next(rng, v);
    CHECK(v == 9);
    // No commercials (a games broadcast): the fixed file, forever.
    TvProgram<int> g;
    g.shows = {5};
    for (int i = 0; i < 3; ++i)
        CHECK(g.next(rng, v, &ad) && v == 5 && !ad);
    TvProgram<int> none;
    CHECK(!none.next(rng, v));
    std::printf(fails ? "tv_program: %d failures\n" : "tv_program: ok\n", fails);
    return fails ? 1 : 0;
}
