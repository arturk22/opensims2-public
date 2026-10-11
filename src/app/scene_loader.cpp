#include "app/scene_loader.hpp"

#include <pthread.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <stdexcept>
#include <string>

namespace os2 {

struct BackgroundJob::State {
    std::function<void()> work;
    std::exception_ptr error;
    std::atomic<bool> finished{false};
    pthread_t thread{};
};

namespace {

void* runJob(void* arg) {
    auto* state = static_cast<BackgroundJob::State*>(arg);
    try {
        state->work();
    } catch (...) {
        state->error = std::current_exception();
    }
    state->work = nullptr;
    state->finished.store(true, std::memory_order_release);
    return nullptr;
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
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 64u << 20); // 64 MB, as reserved address space
    const int rc = pthread_create(&state->thread, &attr, runJob, state.get());
    pthread_attr_destroy(&attr);
    if (rc != 0)
        throw std::runtime_error("BackgroundJob: pthread_create failed");
    state_ = std::move(state);
}

bool BackgroundJob::active() const { return state_ != nullptr; }

bool BackgroundJob::done() const { return state_ && state_->finished.load(std::memory_order_acquire); }

std::exception_ptr BackgroundJob::collect() {
    if (!state_)
        return nullptr;
    pthread_join(state_->thread, nullptr);
    std::exception_ptr error = state_->error;
    state_.reset();
    return error;
}

namespace loading {

namespace {

using Clock = std::chrono::steady_clock;

std::atomic<float> gReported{0};
std::atomic<int64_t> gChanged{0}; // Clock ticks of the last change
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
// of each phase main.mm laps.
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
};

} // namespace

void begin() {
    gReported.store(0);
    gChanged.store(nowTicks());
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
