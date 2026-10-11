#include "media/movie_player.hpp"

#include <algorithm>

namespace os2 {

namespace {
constexpr double kMaxAudioLag = 0.3; // seconds
}

MoviePlayer::MoviePlayer(const std::filesystem::path& file, const Options& options)
    : movie_(std::make_unique<Movie>(Movie::load(file))), options_(options) {
    decoder_ = std::make_unique<MovieDecoder>(*movie_);
    duration_ = movie_->duration();
    if (options_.mixer && movie_->hasSound()) {
        auto pcm = std::make_shared<Pcm>(movie_->decodeSound());
        duration_ = std::max(duration_, pcm->seconds());
        AudioMixer::Params p;
        p.group = SoundGroup::Music;
        p.volume = options_.volume;
        startFrames_ = options_.mixer->framesRendered();
        voice_ = options_.mixer->play(std::move(pcm), p);
    }
    decoder_->seek(0);
    decoder_->picture(picture_);
}

MoviePlayer::~MoviePlayer() {
    if (voice_)
        options_.mixer->stop(voice_);
}

double MoviePlayer::audioTime() const {
    if (!voice_)
        return -1;
    return double(options_.mixer->framesRendered() - startFrames_) / options_.mixer->sampleRate();
}

bool MoviePlayer::update(double wallSeconds) {
    if (skipped_)
        return false;
    // The sound's clock, but never far behind the wall clock: an output that
    // stalls (or has no device behind it) does not hold the picture.
    time_ = voice_ && options_.audioClock
                ? std::max({0.0, audioTime() - options_.outputLatency, wallSeconds - kMaxAudioLag})
                : wallSeconds;
    if (!decoder_->seek(time_))
        return false;
    decoder_->picture(picture_);
    return true;
}

void MoviePlayer::skip() {
    skipped_ = true;
    if (voice_)
        options_.mixer->fade(voice_, 0, 0.15, true);
    voice_ = 0;
}

} // namespace os2
