// BackgroundJob and the load progress (app/scene_loader.hpp).
#include "app/scene_loader.hpp"

#include <chrono>
#include <cstdio>
#include <functional>
#include <stdexcept>
#include <string>
#include <thread>

using namespace os2;

static int failures = 0;

static void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

// Recurses `depth` frames of ~1 KB each (more than a default 512 KB secondary stack).
static int deep(int depth) {
    volatile char pad[1024];
    pad[0] = char(depth);
    return depth == 0 ? pad[0] : deep(depth - 1) + (pad[0] & 1);
}

int main() {
    BackgroundJob job;
    check(!job.active() && !job.done(), "idle job");
    int result = 0;
    job.start([&] { result = deep(4000); });
    check(job.active(), "active after start");
    while (!job.done())
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    check(!job.collect(), "no error");
    check(result == 2000, "work ran with a deep stack");
    check(!job.active(), "collected");

    job.start([] { throw std::runtime_error("boom"); });
    std::exception_ptr err = job.collect();
    bool thrown = false;
    try {
        if (err)
            std::rethrow_exception(err);
    } catch (const std::runtime_error& e) {
        thrown = std::string(e.what()) == "boom";
    }
    check(thrown, "exception carried to collect");

    loading::begin();
    check(loading::reported() == 0, "begin at 0");
    loading::phase("loadLotObjects");
    const float a = loading::reported();
    check(a > 0.3f && a < 0.5f, "a known phase moves the progress");
    loading::phase("Lot::load"); // earlier phase: never back
    check(loading::reported() == a, "progress never goes back");
    loading::phase("no such phase");
    check(loading::reported() == a, "unknown phases are ignored");
    loading::phase("  Sim to the GPU");
    check(loading::reported() > a, "each Sim moves it on");
    const float s = loading::shown();
    check(s >= loading::reported() && s <= loading::reported() + 0.1001f, "shown creeps at most 10%");
    loading::end();
    check(loading::reported() == 1 && loading::shown() == 1, "end at 1");

    if (failures)
        std::fprintf(stderr, "%d failures\n", failures);
    else
        std::printf("scene_loader: ok\n");
    return failures ? 1 : 0;
}
