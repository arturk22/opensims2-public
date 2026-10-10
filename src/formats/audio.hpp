#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace os2 {

// Sound resource types (TSData/Res/Sound/*.package; see docs/audio.md).
namespace type {
constexpr uint32_t AUDIO = 0x2026960B;          // samples (XA, SPX, MP3) and also INI, HIT, FSC, ...
constexpr uint32_t TRACK_SETTINGS = 0x0B9EB87E; // XML property set of a track (sample rate, ...)
constexpr uint32_t HITLIST = 0x7B1ACFCD;        // .hls: samples a track picks from
constexpr uint32_t TRACK_LOGIC = 0x9D796DB4;    // .tlo, binary, not decoded
constexpr uint32_t SOUND_PROPERTIES = 0xEBFEE33F; // XML property sets (ambience scores, music categories)
} // namespace type

// What an AUDIO (0x2026960B) resource holds, from its first bytes.
enum class AudioFormat {
    Unknown,
    Xa,   // "XA" + 2 bytes: EA XA ADPCM
    Spx,  // "SPX1": Speex frames
    Mp3,  // MPEG audio frame sync or ID3 tag
    Wav,  // "RIFF....WAVE" (none in the base game)
    Utk,  // "UTM0" (TS1/TSO voice codec; none in the base game)
    Hit,  // "HIT!" compiled HIT sound script
    Text, // INI/script text stored as AUDIO (TSData/Sys tunables, HIT sources, FSC, UI scripts)
};

AudioFormat detectAudioFormat(std::span<const uint8_t> data);
const char* audioFormatName(AudioFormat f);

// Interleaved signed 16-bit PCM.
struct Pcm {
    uint32_t sampleRate = 0;
    uint16_t channels = 0;
    std::vector<int16_t> samples;

    size_t frames() const { return channels ? samples.size() / channels : 0; }
    double seconds() const { return sampleRate ? double(frames()) / sampleRate : 0.0; }
    // Largest |sample| (0..32768) and RMS in dBFS (-inf for silence).
    int peak() const;
    double rmsDb() const;
};

// XA header: "XA", 2 bytes (0x0000; 0x0012 on some SFX/ambience; "J\0" on
// the ambience loops), u32 size in bytes of the decoded PCM, then a
// WAVEFORMATEX of the decoded stream (tag 1, channels, rate, byte rate,
// align, bits = 16). 24 bytes in all.
struct XaHeader {
    char variant[2] = {0, 0}; // bytes 2..3
    uint32_t pcmBytes = 0;
    uint16_t formatTag = 0;
    uint16_t channels = 0;
    uint32_t sampleRate = 0;
    uint32_t byteRate = 0;
    uint16_t blockAlign = 0;
    uint16_t bits = 0;
};
constexpr size_t kXaHeaderSize = 24;
std::optional<XaHeader> parseXaHeader(std::span<const uint8_t> data);

// EA XA ADPCM: per channel, 15-byte blocks of one header byte (high nibble:
// predictor 0..3, low nibble: shift - 8) and 14 bytes of 4-bit residuals
// (high nibble first), 28 samples a block. Stereo interleaves the two
// channels byte by byte (30-byte blocks: two headers, then left/right
// pairs). Decodes up to header.pcmBytes; throws FormatError on a bad header.
Pcm decodeXa(std::span<const uint8_t> data);

// Decodes 28 samples of one channel from a 15-byte mono block (or any
// block whose bytes for this channel are `stride` apart), updating the
// predictor history (cur, prev). Exposed for the tests.
void decodeXaBlock(const uint8_t* header, const uint8_t* nibbles, size_t stride, int& cur, int& prev,
                   int16_t* out, size_t outStride);

// SPX1 header: "SPX1", u8 channels, u32 size in bytes of the decoded PCM,
// u32 Speex mode (2 = ultra-wideband, 32 kHz), u16 samples per frame (640);
// 15 bytes. Then frames of u8 length + one Speex packet.
struct SpxHeader {
    uint8_t channels = 0;
    uint32_t pcmBytes = 0;
    uint32_t mode = 0;
    uint16_t frameSamples = 0;
};
constexpr size_t kSpxHeaderSize = 15;
std::optional<SpxHeader> parseSpxHeader(std::span<const uint8_t> data);
// Throws FormatError; needs the vendored libspeex (third_party/speex).
Pcm decodeSpx(std::span<const uint8_t> data);

// PCM WAV ("RIFF"/"WAVE", fmt tag 1, 16-bit).
std::vector<uint8_t> encodeWav(const Pcm& pcm);
Pcm decodeWav(std::span<const uint8_t> data);

// Hit list (0x7B1ACFCD, ".hls"): u32 version (56), u32 count, then count
// (instance, instanceHi) pairs naming the samples (AUDIO) a track picks from.
struct HitList {
    uint32_t version = 0;
    std::vector<std::pair<uint32_t, uint32_t>> sounds;
};
HitList parseHitList(std::span<const uint8_t> data);

} // namespace os2
