// s2pkg commands for the sound packages (TSData/Res/Sound).
#include "audio_cmds.hpp"

#include "audio/decode.hpp"
#include "audio/sound_bank.hpp"
#include "formats/audio.hpp"
#include "formats/binreader.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace os2 {

namespace {

std::string keyString(const Tgi& t) {
    char buf[40];
    std::snprintf(buf, sizeof buf, "%08X-%08X-%08X-%08X", t.type, t.group, t.instanceHi, t.instance);
    return buf;
}

std::string groupLabel(uint32_t g) {
    if (const char* n = soundGroupName(g))
        return n;
    char buf[12];
    std::snprintf(buf, sizeof buf, "0x%08X", g);
    return buf;
}

// Format and length from the header alone (no decoding).
std::string headerSummary(std::span<const uint8_t> d) {
    char buf[128];
    switch (const auto f = detectAudioFormat(d)) {
    case AudioFormat::Xa: {
        const auto h = *parseXaHeader(d);
        const double secs = h.sampleRate && h.channels ? h.pcmBytes / (2.0 * h.channels * h.sampleRate) : 0;
        std::snprintf(buf, sizeof buf, "XA%s %u Hz %uch %.2fs", h.variant[0] == 0x12 ? "(12)" : h.variant[0] == 'J' ? "J" : "",
                      h.sampleRate, h.channels, secs);
        return buf;
    }
    case AudioFormat::Spx: {
        const auto h = *parseSpxHeader(d);
        std::snprintf(buf, sizeof buf, "SPX mode %u %uch %.2fs", h.mode, h.channels, h.pcmBytes / (2.0 * 32000));
        return buf;
    }
    default: return audioFormatName(f);
    }
}

void writeFile(const std::string& path, const std::vector<uint8_t>& bytes) {
    std::ofstream f(path, std::ios::binary);
    if (!f)
        throw std::runtime_error("cannot write " + path);
    f.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
}

void printPcm(const Pcm& pcm) {
    std::printf("%u Hz, %u channel(s), %zu frames, %.3f s, peak %d (%.1f dBFS), RMS %.1f dBFS\n", pcm.sampleRate,
                pcm.channels, pcm.frames(), pcm.seconds(), pcm.peak(),
                pcm.peak() ? 20 * std::log10(pcm.peak() / 32768.0) : -INFINITY, pcm.rmsDb());
}

int cmdSoundList(const char* dir, const std::string& filter) {
    SoundBank bank(dir);
    size_t shown = 0;
    for (const auto& loc : bank.samples()) {
        const Tgi& t = loc.entry->tgi;
        const std::string group = groupLabel(t.group);
        const std::string key = keyString(t);
        if (!filter.empty() && group.find(filter) == std::string::npos && key.find(filter) == std::string::npos)
            continue;
        const auto bytes = bank.read(loc);
        std::printf("%s  %-24s %8zu  %s\n", key.c_str(), group.c_str(), bytes.size(), headerSummary(bytes).c_str());
        ++shown;
    }
    std::printf("%zu of %zu AUDIO resources, %zu tracks, %zu packages\n", shown, bank.samples().size(),
                bank.tracks().size(), bank.packageCount());
    return 0;
}

int cmdSoundInfo(const char* dir, const std::string& name) {
    SoundBank bank(dir);
    const auto k = SoundBank::keyOf(name);
    std::printf("%s: instance 0x%08X, instanceHi 0x%08X\n", name.c_str(), k.first, k.second);
    if (auto ts = bank.trackSettings(k)) {
        std::printf("track settings %s (%s):\n", keyString(ts->entry->tgi).c_str(), groupLabel(ts->entry->tgi.group).c_str());
        for (const auto& [key, value] : bank.readTrackSettings(k))
            std::printf("  %s = %s\n", key.c_str(), value.c_str());
    }
    if (auto hl = bank.hitList(k)) {
        const auto list = parseHitList(bank.read(*hl));
        std::printf("hit list %s, version %u, %zu entries\n", keyString(hl->entry->tgi).c_str(), list.version,
                    list.sounds.size());
    }
    const auto samples = bank.resolve(name);
    for (size_t i = 0; i < samples.size(); ++i) {
        const auto bytes = bank.read(samples[i]);
        std::printf("  [%zu] %s  %-20s %s\n", i, keyString(samples[i].entry->tgi).c_str(),
                    groupLabel(samples[i].entry->tgi.group).c_str(), headerSummary(bytes).c_str());
    }
    if (samples.empty()) {
        std::fprintf(stderr, "no sample or track named %s\n", name.c_str());
        return 1;
    }
    return 0;
}

int cmdSoundExtract(const char* dir, const std::string& name, const std::string& out, size_t which) {
    SoundBank bank(dir);
    const auto samples = bank.resolve(name);
    if (samples.empty()) {
        std::fprintf(stderr, "no sample or track named %s\n", name.c_str());
        return 1;
    }
    if (which >= samples.size()) {
        std::fprintf(stderr, "%s has %zu samples\n", name.c_str(), samples.size());
        return 1;
    }
    const auto& loc = samples[which];
    const auto bytes = bank.read(loc);
    std::printf("%s (%s, %s): ", keyString(loc.entry->tgi).c_str(), groupLabel(loc.entry->tgi.group).c_str(),
                headerSummary(bytes).c_str());
    const Pcm pcm = decodeSound(bytes);
    printPcm(pcm);
    writeFile(out, encodeWav(pcm));
    return 0;
}

struct FormatStats {
    size_t count = 0, decoded = 0, failed = 0, silent = 0, lengthMismatch = 0;
    double seconds = 0;
    std::map<std::string, size_t> variants; // rate/channels/variant
    std::string firstError;
};

int cmdSoundSweep(const char* dir) {
    SoundBank bank(dir);
    std::map<std::string, FormatStats> stats;
    for (const auto& loc : bank.samples()) {
        std::vector<uint8_t> bytes;
        try {
            bytes = bank.read(loc);
        } catch (const std::exception& ex) {
            auto& s = stats["(read error)"];
            ++s.count, ++s.failed;
            if (s.firstError.empty())
                s.firstError = keyString(loc.entry->tgi) + ": " + ex.what();
            continue;
        }
        const AudioFormat f = detectAudioFormat(bytes);
        auto& s = stats[audioFormatName(f)];
        ++s.count;
        if (f != AudioFormat::Xa && f != AudioFormat::Spx && f != AudioFormat::Mp3 && f != AudioFormat::Wav)
            continue;
        size_t expected = 0;
        char variant[64] = "";
        if (auto h = parseXaHeader(bytes)) {
            expected = h->pcmBytes / 2;
            std::snprintf(variant, sizeof variant, "XA%s %u Hz %uch",
                          h->variant[0] == 0x12 ? "(12)" : h->variant[0] == 'J' ? "J" : "", h->sampleRate, h->channels);
        } else if (auto h = parseSpxHeader(bytes)) {
            expected = h->pcmBytes / 2;
            std::snprintf(variant, sizeof variant, "mode %u frame %u", h->mode, h->frameSamples);
        }
        try {
            const Pcm pcm = decodeSound(bytes);
            ++s.decoded;
            s.seconds += pcm.seconds();
            if (pcm.peak() == 0)
                ++s.silent;
            if (expected && pcm.samples.size() != expected)
                ++s.lengthMismatch;
            if (!variant[0])
                std::snprintf(variant, sizeof variant, "%u Hz %uch", pcm.sampleRate, pcm.channels);
            ++s.variants[variant];
        } catch (const std::exception& ex) {
            ++s.failed;
            if (s.firstError.empty())
                s.firstError = keyString(loc.entry->tgi) + ": " + ex.what();
        }
    }
    std::printf("%-8s %7s %8s %7s %7s %9s %10s\n", "format", "count", "decoded", "failed", "silent", "len!=hdr", "hours");
    for (const auto& [name, s] : stats) {
        std::printf("%-8s %7zu %8zu %7zu %7zu %9zu %10.2f\n", name.c_str(), s.count, s.decoded, s.failed, s.silent,
                    s.lengthMismatch, s.seconds / 3600);
        for (const auto& [v, n] : s.variants)
            std::printf("         %7zu  %s\n", n, v.c_str());
        if (!s.firstError.empty())
            std::printf("         first error: %s\n", s.firstError.c_str());
    }
    // Tracks: how many hit list entries resolve to a sample.
    size_t entries = 0, resolved = 0, empty = 0;
    std::vector<std::string> missing;
    for (const auto& [k, loc] : bank.tracks()) {
        const auto list = parseHitList(bank.read(loc));
        entries += list.sounds.size();
        empty += list.sounds.empty();
        for (const auto& e : list.sounds) {
            if (bank.sample({e.first, e.second}) || bank.hitList({e.first, e.second}))
                ++resolved;
            else if (missing.size() < 8)
                missing.push_back(keyString(loc.entry->tgi) + " -> " + keyString(Tgi{type::AUDIO, 0, e.first, e.second}));
        }
    }
    std::printf("tracks: %zu hit lists (%zu empty), %zu entries, %zu resolve to a sample or hit list\n",
                bank.tracks().size(), empty, entries, resolved);
    for (const auto& m : missing)
        std::printf("  unresolved: %s\n", m.c_str());
    return 0;
}

// Reads candidate names from stdin, one per line, and prints those that are
// a track (T) or a sample (S): for guessing how the game composes names.
int cmdSoundNames(const char* dir) {
    SoundBank bank(dir);
    std::string line;
    size_t found = 0, tried = 0;
    char buf[4096];
    while (std::fgets(buf, sizeof buf, stdin)) {
        line = buf;
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
            line.pop_back();
        if (line.empty())
            continue;
        ++tried;
        const auto k = SoundBank::nameKey(line);
        const bool track = bank.hitList(k).has_value(), sample = bank.sample(k).has_value();
        if (track || sample) {
            ++found;
            std::printf("%s %s\n", track ? "T" : "S", line.c_str());
        }
    }
    std::fprintf(stderr, "%zu of %zu names exist\n", found, tried);
    return 0;
}

} // namespace

const char* audioUsage() {
    return "       s2pkg sound-list <Res-dir|sound-dir> [filter]\n"
           "       s2pkg sound-info <Res-dir|sound-dir> <name>\n"
           "       s2pkg sound-extract <Res-dir|sound-dir> <name|TGI> <out.wav> [index]\n"
           "       s2pkg sound-sweep <Res-dir|sound-dir>\n"
           "       s2pkg sound-names <Res-dir|sound-dir> < names.txt\n";
}

int audioCommand(const std::string& cmd, int argc, char** argv) {
    if (cmd == "sound-list" && (argc == 3 || argc == 4))
        return cmdSoundList(argv[2], argc == 4 ? argv[3] : "");
    if (cmd == "sound-info" && argc == 4)
        return cmdSoundInfo(argv[2], argv[3]);
    if (cmd == "sound-extract" && (argc == 5 || argc == 6))
        return cmdSoundExtract(argv[2], argv[3], argv[4], argc == 6 ? std::stoul(argv[5]) : 0);
    if (cmd == "sound-sweep" && argc == 3)
        return cmdSoundSweep(argv[2]);
    if (cmd == "sound-names" && argc == 3)
        return cmdSoundNames(argv[2]);
    return -1;
}

} // namespace os2
