#include "play_sound.hpp"

#include "decode.hpp"
#include "player.hpp"
#include "sound_bank.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>

namespace os2 {

int playSoundCommand(const std::filesystem::path& resDir, const std::string& name, size_t index,
                     const std::string& wavOut) {
    try {
        SoundBank bank(resDir);
        const auto samples = bank.resolve(name);
        if (samples.empty()) {
            std::fprintf(stderr, "--play-sound: no sample or track named %s\n", name.c_str());
            return 1;
        }
        if (index >= samples.size()) {
            std::fprintf(stderr, "--play-sound: %s has %zu samples\n", name.c_str(), samples.size());
            return 1;
        }
        const Tgi& t = samples[index].entry->tgi;
        Pcm pcm = bank.decode(samples[index]);
        const char* group = soundGroupName(t.group);
        std::printf("%s: sample %zu of %zu, %08X-%08X-%08X-%08X (%s): %u Hz, %u ch, %.3f s, peak %d, RMS %.1f dBFS\n",
                    name.c_str(), index + 1, samples.size(), t.type, t.group, t.instanceHi, t.instance,
                    group ? group : "?", pcm.sampleRate, pcm.channels, pcm.seconds(), pcm.peak(), pcm.rmsDb());
        if (!wavOut.empty()) {
            const auto wav = encodeWav(pcm);
            std::ofstream f(wavOut, std::ios::binary);
            f.write(reinterpret_cast<const char*>(wav.data()), std::streamsize(wav.size()));
            if (!f) {
                std::fprintf(stderr, "--play-sound: cannot write %s\n", wavOut.c_str());
                return 1;
            }
        }
        const double seconds = pcm.seconds();
        const size_t frames = pcm.frames();
        SoundPlayer player;
        const auto start = std::chrono::steady_clock::now();
        player.play(std::move(pcm));
        const bool finished = player.wait(seconds + 5.0);
        const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        std::printf("played %zu of %zu frames in %.3f s%s\n", player.framesQueued(), frames, elapsed,
                    finished ? "" : " (timed out)");
        return finished ? 0 : 1;
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "--play-sound: %s\n", ex.what());
        return 1;
    }
}

} // namespace os2
