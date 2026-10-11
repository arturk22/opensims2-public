#pragma once

// Loading a scene off the main thread (docs/loading-mac.md).
//
// The app shell (main.mm) splits each scene's start into a part that only
// builds data and GPU resources (resource decoding, models, textures, the
// VM, light maps: run here, on a worker thread, into a renderer that is not
// being drawn yet) and a part that touches AppKit or the view (run on the
// main thread once the worker is done). While the worker runs, the main
// thread keeps drawing the loading screen (app/movie_host.hpp) and ignores
// scene input; nothing of the scene being built is read on the main thread
// until the worker has finished (BackgroundJob::collect joins it, which
// orders its writes before the main thread's reads).
//
// Progress: the load reports named phases (the OS2_PERF lap names of
// main.mm) as fractions, so the loading screen can pace its animation. Plain
// C++.

#include <exception>
#include <functional>
#include <memory>
#include <string_view>

namespace os2 {

// One piece of work on a thread of its own, with a large stack (the VM's
// tree walks and the scene builders recurse deeper than the 512 KB default
// of secondary threads allows).
class BackgroundJob {
public:
    BackgroundJob() = default;
    ~BackgroundJob(); // waits for a running job
    BackgroundJob(const BackgroundJob&) = delete;
    BackgroundJob& operator=(const BackgroundJob&) = delete;

    // Starts `work` (only when no job is running or waiting to be collected).
    void start(std::function<void()> work);
    // Started and not collected yet.
    bool active() const;
    // The work has returned (or thrown).
    bool done() const;
    // Waits for the work, joins the thread and returns what it threw (null if
    // nothing). The job can then be started again.
    std::exception_ptr collect();

    struct State; // (shared with the thread)

private:
    std::shared_ptr<State> state_;
};

namespace loading {

// Starts counting a new load (progress 0).
void begin();
// A phase of the load ended: a known phase name (main.mm's perf laps:
// "Lot::load", "loadLotObjects", "  Sim to the GPU", ...) moves the progress
// to its share of a typical load. Unknown names are ignored. Any thread.
void phase(std::string_view name);
// Sets the progress (0..1) directly; it never goes back. Any thread.
void set(float fraction);
// The load is complete (progress 1).
void end();
// The reported progress (0..1).
float reported();
// What to show: the reported progress, crept slowly forwards while a phase
// takes long (never past the next 10% or 97%), and never going back. Read
// it from one thread (the main thread).
float shown();

} // namespace loading

} // namespace os2
