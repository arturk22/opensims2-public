#pragma once

// A software mixer for the game's sounds: any number of voices (16-bit PCM
// at any rate, mono or stereo) resampled and mixed to one stereo stream, with
// a volume per voice, per group (the game's SFX / voice / music / ambience
// channels; SoundMasterGroup*Enabled in the options) and a master volume.
// Plain C++: render() is what an output device pulls from (AudioOutput on
// macOS), and also what tests and headless runs call directly.

#include "formats/audio.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace os2 {

// The mixing groups. The game keeps a volume per group (cTSAudioSoundManager::
// SetGroupVolume) and the option to turn each one off.
enum class SoundGroup : uint8_t { Sfx, Vox, Music, Ambience, Ui, Count };
const char* soundGroupLabel(SoundGroup g);

class AudioMixer {
public:
    using VoiceId = uint32_t; // 0 is never a voice

    explicit AudioMixer(uint32_t sampleRate = 44100);

    uint32_t sampleRate() const { return rate_; }

    struct Params {
        SoundGroup group = SoundGroup::Sfx;
        float volume = 1;     // linear gain
        float pan = 0;        // -1 left .. 1 right (constant power)
        bool loop = false;
        float pitch = 1;      // playback rate multiplier
        double startDelay = 0; // seconds of silence before it starts
        double startAt = 0;    // seconds into the sound it starts from (a broadcast joined late)
    };
    // Starts a voice; the PCM is shared, so a cached sound is not copied.
    VoiceId play(std::shared_ptr<const Pcm> pcm, const Params& params);
    void stop(VoiceId id);
    void stopAll();
    void stopGroup(SoundGroup g);
    // Fades a voice to `volume` over `seconds` (0: at once); a fade to 0 with
    // `stopAtEnd` removes the voice when it gets there.
    void fade(VoiceId id, float volume, double seconds, bool stopAtEnd = false);
    void setVolume(VoiceId id, float volume) { fade(id, volume, 0); }
    void setPan(VoiceId id, float pan);
    bool playing(VoiceId id) const;
    size_t voiceCount() const;

    void setGroupVolume(SoundGroup g, float volume);
    float groupVolume(SoundGroup g) const;
    void setMasterVolume(float volume);
    // Muted: voices still advance (so looping ambience and timing stay
    // right), but the output is silence.
    void setMuted(bool muted);
    bool muted() const;

    // Mixes the next `frames` frames into `out` (interleaved stereo int16,
    // 2 * frames samples), advancing every voice; finished voices go away.
    void render(int16_t* out, size_t frames);
    // The same as floats (-1..1), for tests that check levels.
    void renderFloat(float* out, size_t frames);
    // Frames rendered so far.
    uint64_t framesRendered() const;

private:
    struct Voice {
        VoiceId id = 0;
        std::shared_ptr<const Pcm> pcm;
        Params params;
        double position = 0; // in source frames
        double step = 1;     // source frames per output frame
        float gain = 1, target = 1, gainStep = 0;
        bool stopAtEnd = false;
        int64_t delayFrames = 0;
    };
    uint32_t rate_;
    mutable std::mutex mutex_;
    std::vector<Voice> voices_;
    std::array<float, size_t(SoundGroup::Count)> groupVolume_;
    float master_ = 1;
    bool muted_ = false;
    VoiceId next_ = 1;
    uint64_t rendered_ = 0;
    std::vector<float> scratch_;

    Voice* find(VoiceId id);
    const Voice* find(VoiceId id) const;
};

// Pulls a mixer onto the default output device (macOS AudioQueue, 44.1 kHz
// stereo, ~70 ms of buffering, refilled on AudioToolbox's thread). start()
// returns false when there is no device; the mixer then just is not heard.
class AudioOutput {
public:
    explicit AudioOutput(AudioMixer& mixer);
    ~AudioOutput();
    AudioOutput(const AudioOutput&) = delete;
    AudioOutput& operator=(const AudioOutput&) = delete;
    bool start();
    void stop();
    bool running() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace os2
