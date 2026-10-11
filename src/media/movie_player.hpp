#pragma once

// Plays a .movie (formats/movie.hpp) by its clock: the sound goes to an
// AudioMixer as one voice, and the picture follows the mixer's position
// (the frames it has rendered since the voice started, less the output's
// buffering), so picture and sound stay together even when frames are
// dropped; without sound (or without a running mixer) it follows the wall
// clock the caller passes. Plain C++.

#include "audio/mixer.hpp"
#include "formats/movie.hpp"

#include <filesystem>
#include <memory>

namespace os2 {

class MoviePlayer {
public:
    struct Options {
        AudioMixer* mixer = nullptr; // nullptr: no sound
        bool audioClock = true;      // the mixer is being pulled (a device or offline renders)
        double outputLatency = 0;    // seconds between the mixer's render and hearing it
        float volume = 1;
    };
    MoviePlayer(const std::filesystem::path& file, const Options& options);
    ~MoviePlayer();
    MoviePlayer(const MoviePlayer&) = delete;
    MoviePlayer& operator=(const MoviePlayer&) = delete;

    // Moves the clock on; `wallSeconds` is the time since the movie started,
    // used when the sound does not drive it. True when the picture changed.
    bool update(double wallSeconds);
    double time() const { return time_; }
    // Video or sound, whichever is longer.
    double duration() const { return duration_; }
    bool finished() const { return skipped_ || time_ >= duration_; }
    void skip();

    const RgbaImage& picture() const { return picture_; }
    long frame() const { return decoder_->current(); }
    const Movie& movie() const { return *movie_; }
    // Seconds of sound the mixer has rendered for the movie so far (-1 without sound).
    double audioTime() const;

private:
    std::unique_ptr<Movie> movie_;
    std::unique_ptr<MovieDecoder> decoder_;
    Options options_;
    AudioMixer::VoiceId voice_ = 0;
    uint64_t startFrames_ = 0;
    double duration_ = 0, time_ = 0;
    bool skipped_ = false;
    RgbaImage picture_;
};

} // namespace os2
