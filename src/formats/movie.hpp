#pragma once

// The game's .movie files (TSData/Res/Movies, the EA logo, the intro, the TV
// channels and the arcade games): not VP6 but EA's older "Madcow" video in an
// EA chunk stream, with EA ADPCM (revision 1) sound interleaved.
//
// Chunk stream: tag (4 ASCII), u32 LE size including the 8-byte header.
//   MADk  intra frame             MADm  inter frame (becomes the reference)
//   MADe  inter frame that is shown but not kept as the reference
//   SCHl  sound header: "PT\0\0" then tagged values (see parseSoundHeader)
//   SCCl  u32 number of SCDl chunks     SCDl  sound data     SCEl  end
//
// MAD frame (after the 8-byte header): 4 unknown bytes (0), u32 frame
// duration in ms as 16.16 fixed point (0x0042AAAB = 66.67 ms, 15 fps;
// 0x00215555 = 33.33 ms, 30 fps), u16 width, u16 height, a byte (2), the
// quantiser scale, 2 bytes, then the bitstream: 16-bit little-endian words
// read most significant bit first. Macroblocks of 16x16 (4 Y + U + V 8x8
// blocks, 4:2:0), each block MPEG-1 intra coded (8-bit DC, MPEG-1 AC
// run/level VLC with its own escape) or, in inter frames, copied from the
// reference with a motion vector and a DC offset. The format is as FFmpeg's
// eamad decoder describes it; this is an independent implementation.

#include "formats/audio.hpp"
#include "formats/image.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace os2 {

// MSB-first reader over 16-bit little-endian words (the MAD bitstream).
// Past the end it reads zeros.
class MadBitReader {
public:
    explicit MadBitReader(std::span<const uint8_t> data) : data_(data), usable_(data.size() & ~size_t(1)) {}
    uint32_t bits(int n);          // 0..24
    int32_t signedBits(int n);     // two's complement
    uint32_t peek(int n) const;    // 0..24, without consuming
    void skip(int n) { pos_ += size_t(n); }
    size_t position() const { return pos_; }
    bool overrun() const { return pos_ > usable_ * 8; }

private:
    std::span<const uint8_t> data_;
    size_t usable_;
    size_t pos_ = 0;
    uint8_t byte(size_t i) const { return (i ^ 1) < usable_ ? data_[i ^ 1] : 0; }
};

// One MPEG-1 AC coefficient code: run (zeros before it, +1 as the decoder
// adds it to the index), level (>0), or the escape / end of block.
struct MadAcCode {
    enum Kind : uint8_t { Invalid, Coefficient, Escape, EndOfBlock };
    Kind kind = Invalid;
    uint8_t run = 0, level = 0, length = 0;
};
// Reads the next AC code (without its sign bit).
MadAcCode readMadAcCode(MadBitReader& br);

// The block IDCT (EA's TGQ/TQI/MAD integer transform): coefficients in
// natural order, quantised as the decoder leaves them, out to 8x8 pixels.
void madIdct(int16_t block[64], uint8_t* dst, ptrdiff_t stride);

class MadDecoder {
public:
    // Decodes one MADk/MADm/MADe chunk (with its 8-byte header). Returns
    // false on a damaged frame (the picture keeps what was decoded).
    bool decode(std::span<const uint8_t> chunk);
    int width() const { return width_; }
    int height() const { return height_; }
    double frameMs() const { return frameMs_; }
    // The last decoded picture as RGBA (BT.601, video range).
    void toRgba(RgbaImage& out) const;
    // Plane access for tests: 0 Y, 1 U, 2 V.
    const uint8_t* plane(int i) const;
    int stride(int i) const { return i ? stride_ / 2 : stride_; }

private:
    int width_ = 0, height_ = 0, stride_ = 0, rows_ = 0; // rows_: padded luma height
    double frameMs_ = 0;
    std::vector<uint8_t> frames_[3]; // the picture being built, the reference, scratch
    int out_ = 0, ref_ = 1;
    bool haveRef_ = false;
    uint16_t quant_[64] = {};
    void resize(int w, int h);
    uint8_t* planePtr(int frame, int p);
};

struct MovieSoundHeader {
    int revision = -1, compression = -1, channels = 1, sampleRate = -1;
    uint32_t samples = 0;
};
// The SCHl payload after its 8-byte header (starts with "PT\0\0").
MovieSoundHeader parseSoundHeader(std::span<const uint8_t> payload);
// One SCDl payload (after the 8-byte header) of EA ADPCM revision 1:
// u32 samples per channel, u32 per-channel offsets (from the end of that
// table), then per channel: s16 current, s16 previous, and 15-byte frames
// of 28 samples (a predictor/shift byte, 14 bytes of 4-bit nibbles).
// Appends interleaved samples to `out`.
void decodeEaAdpcmR1(std::span<const uint8_t> payload, int channels, std::vector<int16_t>& out);

// A whole .movie file: its frames (chunk offsets) and its sound.
class Movie {
public:
    explicit Movie(std::vector<uint8_t> bytes);
    static Movie load(const std::filesystem::path& path);

    struct Frame {
        size_t offset, size; // the chunk, header included
        char kind;           // 'k', 'm' or 'e'
    };
    const std::vector<Frame>& frames() const { return frames_; }
    int width() const { return width_; }
    int height() const { return height_; }
    double frameSeconds() const { return frameMs_ / 1000.0; }
    double duration() const { return frames_.size() * frameSeconds(); }
    const MovieSoundHeader& soundHeader() const { return sound_; }
    bool hasSound() const { return soundChunks_.size() > 0; }
    // All the sound decoded to 16-bit PCM (empty without sound).
    Pcm decodeSound() const;
    std::span<const uint8_t> chunk(const Frame& f) const { return {bytes_.data() + f.offset, f.size}; }

private:
    std::vector<uint8_t> bytes_;
    std::vector<Frame> frames_;
    std::vector<std::pair<size_t, size_t>> soundChunks_; // SCDl payloads
    MovieSoundHeader sound_;
    int width_ = 0, height_ = 0;
    double frameMs_ = 0;
};

// Plays a Movie by time: decodes forward to the frame shown at `seconds`
// (restarting from the start when asked for an earlier time).
class MovieDecoder {
public:
    explicit MovieDecoder(const Movie& movie) : movie_(movie) {}
    // Index of the frame shown at `seconds` (clamped to the last).
    size_t frameAt(double seconds) const;
    // Decodes up to that frame; true if the picture changed.
    bool seek(double seconds);
    bool finished(double seconds) const { return seconds >= movie_.duration(); }
    void picture(RgbaImage& out) const { mad_.toRgba(out); }
    long current() const { return next_ - 1; }
    const MadDecoder& mad() const { return mad_; }

private:
    const Movie& movie_;
    MadDecoder mad_;
    long next_ = 0; // next frame to decode
};

// Finds a movie by name ("intro_eng_audio", "Games/SSX/ssx_INTRO",
// "ealogo_audio.movie", or a path) under a TSData/Res directory: the
// language's Locale/<lang>/Movies first, then Movies (recursively).
// Empty when not found.
std::filesystem::path findMovie(const std::filesystem::path& resDir, const std::string& name);

} // namespace os2
