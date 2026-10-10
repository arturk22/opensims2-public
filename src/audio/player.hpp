#pragma once

#include "formats/audio.hpp"

#include <memory>

namespace os2 {

// Plays 16-bit PCM on the default output device through an AudioToolbox
// AudioQueue (macOS). One sound at a time; play() replaces the current one.
// The queue runs on AudioToolbox's own thread, so no run loop is needed.
class SoundPlayer {
public:
    SoundPlayer();
    ~SoundPlayer();
    SoundPlayer(const SoundPlayer&) = delete;
    SoundPlayer& operator=(const SoundPlayer&) = delete;

    // Starts playback; throws std::runtime_error when the queue cannot be
    // created or started (e.g. no output device).
    void play(Pcm pcm, float volume = 1.0f);
    void stop();
    bool playing() const;
    // Blocks until the sound ends (or `timeoutSeconds` pass); false on timeout.
    bool wait(double timeoutSeconds);
    // Frames handed to the device so far.
    size_t framesQueued() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace os2
