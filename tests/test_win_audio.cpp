// The Windows output device pulls the mixer. A looping tone must advance
// the rendered-frame counter; a silent or missing device does not.
#include "audio/mixer.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <thread>

int main() {
    auto pcm = std::make_shared<os2::Pcm>();
    pcm->sampleRate = 44100;
    pcm->channels = 1;
    pcm->samples.resize(44100 / 5);
    for (size_t i = 0; i < pcm->samples.size(); ++i)
        pcm->samples[i] = int16_t(std::sin(2.0 * 3.141592653589793 * 440.0 * double(i) / 44100.0) * 8000.0);

    os2::AudioMixer mixer(44100);
    os2::AudioMixer::Params params;
    params.loop = true;
    mixer.play(pcm, params);

    os2::AudioOutput output(mixer);
    if (!output.start()) {
        std::fprintf(stderr, "audio output: no device\n");
        return 1;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    const uint64_t frames = mixer.framesRendered();
    output.stop();
    std::printf("audio output: %llu frames\n", static_cast<unsigned long long>(frames));
    if (frames < 44100 / 10) {
        std::fprintf(stderr, "audio output: device did not pull samples\n");
        return 1;
    }
    std::printf("win-audio: ok\n");
    return 0;
}
