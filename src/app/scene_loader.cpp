#include "app/scene_loader.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <process.h>
#include <windows.h>
#else
#include <pthread.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <stdexcept>
#include <string>

namespace os2 {

struct BackgroundJob::State {
    std::function<void()> work;
    std::exception_ptr error;
    std::atomic<bool> finished{false};
#if defined(_WIN32)
    uintptr_t thread = 0;
#else
    pthread_t thread{};
#endif
};

namespace {

#if defined(_WIN32)
unsigned __stdcall runJob(void* arg) {
#else
void* runJob(void* arg) {
#endif
    auto* state = static_cast<BackgroundJob::State*>(arg);
    try {
        state->work();
    } catch (...) { // any type: carried to the main thread (collect), not swallowed; one escaping a thread terminates
        state->error = std::current_exception();
    }
    state->work = nullptr;
    state->finished.store(true, std::memory_order_release);
#if defined(_WIN32)
    return 0;
#else
    return nullptr;
#endif
}

} // namespace

BackgroundJob::~BackgroundJob() {
    if (state_)
        collect();
}

void BackgroundJob::start(std::function<void()> work) {
    if (state_)
        throw std::logic_error("BackgroundJob: a job is still active");
    auto state = std::make_shared<State>();
    state->work = std::move(work);
#if defined(_WIN32)
    // 64 MB, as reserved address space: the VM's tree walks recurse deeper than
    // the default stack of a secondary thread.
    state->thread = _beginthreadex(nullptr, 64u << 20, runJob, state.get(), 0, nullptr);
    if (state->thread == 0)
        throw std::runtime_error("BackgroundJob: _beginthreadex failed");
#else
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 64u << 20);
    const int rc = pthread_create(&state->thread, &attr, runJob, state.get());
    pthread_attr_destroy(&attr);
    if (rc != 0)
        throw std::runtime_error("BackgroundJob: pthread_create failed");
#endif
    state_ = std::move(state);
}

bool BackgroundJob::active() const { return state_ != nullptr; }

bool BackgroundJob::done() const { return state_ && state_->finished.load(std::memory_order_acquire); }

std::exception_ptr BackgroundJob::collect() {
    if (!state_)
        return nullptr;
#if defined(_WIN32)
    WaitForSingleObject(reinterpret_cast<HANDLE>(state_->thread), INFINITE);
    CloseHandle(reinterpret_cast<HANDLE>(state_->thread));
#else
    pthread_join(state_->thread, nullptr);
#endif
    std::exception_ptr error = state_->error;
    state_.reset();
    return error;
}

namespace loading {

namespace {

using Clock = std::chrono::steady_clock;

std::atomic<float> gReported{0};
std::atomic<int64_t> gChanged{0}; // Clock ticks of the last change
std::atomic<int64_t> gBegan{0};   // ... of begin()
float gShown = 0;                 // main thread only

int64_t nowTicks() { return Clock::now().time_since_epoch().count(); }

double secondsSince(int64_t ticks) {
    return std::chrono::duration<double>(Clock::duration(nowTicks() - ticks)).count();
}

void raise(float f) {
    f = std::clamp(f, 0.0f, 1.0f);
    float cur = gReported.load();
    while (f > cur && !gReported.compare_exchange_weak(cur, f)) {
    }
    if (f > cur)
        gChanged.store(nowTicks());
}

// Share of a typical lot load (N001 lot 11, docs/performance.md) at the end
// of each phase the app shell laps (perfLap, app_args.mm).
struct Phase {
    const char* name;
    float at;
};
constexpr Phase kPhases[] = {
    {"ResourceIndex", 0.30f},     // only at start-up (then the main menu)
    {"loadObjects", 0.02f},       {"Lot::load", 0.04f},
    {"leftover cObjects", 0.05f}, {"PatternCatalog", 0.06f},
    {"buildLotModel", 0.10f},     {"lot model to the GPU", 0.13f},
    {"loadLotObjects", 0.40f},    {"  loadSimParts", 0.42f},
    {"loadLotSims", 0.72f},       {"startAudio", 0.73f},
    {"  LotSimulation", 0.83f},   {"  LotLighting", 0.85f},
    {"  live UI", 0.87f},         {"  Buy mode", 0.92f},
    {"  Build mode", 0.97f},      {"startSimulation", 0.97f},
    {"fences", 0.98f},
    // The other scenes (after "ResourceIndex" when it is the first):
    {"hood: terrain", 0.40f},     {"hood: lots", 0.75f},       {"hood: ui", 0.93f},
    {"hood: bins", 0.98f},
    {"menu: scripts", 0.80f},
    {"cas: catalog", 0.42f},      {"cas: ui", 0.44f},          {"cas: studio", 0.72f},
};

// OS2_PERF=1: when each reported phase came, for tuning kPhases.
bool traceOn() {
    static const bool on = std::getenv("OS2_PERF") != nullptr;
    return on;
}

} // namespace

void begin() {
    gReported.store(0);
    gChanged.store(nowTicks());
    gBegan.store(nowTicks());
    gShown = 0;
}

void phase(std::string_view name) {
    // Each Sim built moves the bar on by a step, up to where the Sims end.
    if (name == "  Sim to the GPU") {
        raise(std::min(gReported.load() + 0.06f, 0.70f));
        return;
    }
    for (const auto& p : kPhases)
        if (name == p.name) {
            if (traceOn() && gBegan.load())
                std::printf("opensims2: perf: progress %3.0f%% at %7.1f ms (%s)\n", p.at * 100,
                            secondsSince(gBegan.load()) * 1000, p.name);
            raise(p.at);
            return;
        }
}

void set(float fraction) { raise(fraction); }

void end() { raise(1); }

float reported() { return gReported.load(); }

float shown() {
    const float rep = gReported.load();
    // Creep towards the next 10% while nothing is reported (fast at first,
    // then slower), so a long phase still moves.
    const double idle = secondsSince(gChanged.load());
    const float cap = rep >= 1 ? 1.0f : std::min(rep + 0.10f, 0.97f);
    const float crept = rep + (cap - rep) * float(1 - std::exp(-idle / 1.5));
    gShown = std::max(gShown, std::min(std::max(rep, crept), 1.0f));
    return gShown;
}

} // namespace loading

} // namespace os2
