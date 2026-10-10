// XA ADPCM, SPX1, WAV and hit list decoding on hand-built data.
#include "audio/decode.hpp"
#include "formats/audio.hpp"

#include <speex/speex.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace os2;

static int failures = 0;

static void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

struct Writer {
    std::vector<uint8_t> b;
    void u8(uint8_t v) { b.push_back(v); }
    void u16(uint16_t v) { u8(uint8_t(v)), u8(uint8_t(v >> 8)); }
    void u32(uint32_t v) { u16(uint16_t(v)), u16(uint16_t(v >> 16)); }
    void tag(const char* t) { b.insert(b.end(), t, t + std::strlen(t)); }
};

static void xaHeader(Writer& w, uint16_t channels, uint32_t rate, uint32_t pcmBytes) {
    w.tag("XA");
    w.u16(0);
    w.u32(pcmBytes);
    w.u16(1);
    w.u16(channels);
    w.u32(rate);
    w.u32(rate * channels * 2);
    w.u16(uint16_t(channels * 2));
    w.u16(16);
}

// Reference EA XA encoder for one channel block: tries every predictor and
// shift and keeps the one with the smallest squared error (the decoder's
// arithmetic, so the round trip only loses quantization noise).
static void encodeBlock(const int16_t* in, int& cur, int& prev, uint8_t out[15]) {
    static const int coef[4][2] = {{0, 0}, {240, 0}, {460, -208}, {392, -220}};
    double best = INFINITY;
    for (int pred = 0; pred < 4; ++pred)
        for (int lo = 0; lo < 13; ++lo) {
            const int shift = lo + 8;
            int c = cur, p = prev;
            double err = 0;
            uint8_t nib[28];
            for (int i = 0; i < 28; ++i) {
                const int predicted = c * coef[pred][0] + p * coef[pred][1];
                // residual r (4-bit) contributes (r << 28 >> shift) before the >> 8.
                const double want = (double(in[i]) * 256 - predicted - 0x80) / double(1 << (28 - shift));
                int r = int(std::lround(want));
                r = std::max(-8, std::min(7, r));
                const int res = int32_t(uint32_t(r & 15) << 28) >> shift;
                int s = (res + predicted + 0x80) >> 8;
                s = std::max(-32768, std::min(32767, s));
                err += double(s - in[i]) * (s - in[i]);
                nib[i] = uint8_t(r & 15);
                p = c;
                c = s;
            }
            if (err < best) {
                best = err;
                out[0] = uint8_t(pred << 4 | lo);
                for (int i = 0; i < 14; ++i)
                    out[1 + i] = uint8_t(nib[2 * i] << 4 | nib[2 * i + 1]);
            }
        }
    // Advance the history the way the decoder will.
    int16_t tmp[28];
    decodeXaBlock(out, out + 1, 1, cur, prev, tmp, 1);
}

static void testXaBlock() {
    // Predictor 0, shift 8: a residual n gives (n << 20 + 0x80) >> 8.
    uint8_t block[15] = {0x00, 0x17, 0x8F};
    int cur = 0, prev = 0;
    int16_t out[28];
    decodeXaBlock(block, block + 1, 1, cur, prev, out, 1);
    check(out[0] == 4096, "XA: residual 1, shift 8 -> 4096");
    check(out[1] == 28672, "XA: residual 7 -> 28672");
    check(out[2] == -32768, "XA: residual -8 -> -32768");
    check(out[3] == -4096, "XA: residual -1 -> -4096");
    check(out[4] == 0 && out[27] == 0, "XA: zero residuals stay 0 with predictor 0");
    check(cur == 0 && prev == 0, "XA: history follows the last two samples");

    // Predictor 1 (c1 = 240/256): an impulse decays geometrically.
    uint8_t decay[15] = {0x10, 0x10};
    cur = prev = 0;
    decodeXaBlock(decay, decay + 1, 1, cur, prev, out, 1);
    check(out[0] == 4096, "XA: predictor 1 impulse");
    check(out[1] == (4096 * 240 + 0x80) >> 8, "XA: predictor 1, second sample = 240/256 of the first");
    check(out[2] == (out[1] * 240 + 0x80) >> 8, "XA: predictor 1 keeps decaying");

    // Predictor 2 uses both history samples: s = (cur*460 - prev*208 + 0x80) >> 8.
    uint8_t two[15] = {0x20, 0x00};
    cur = 1000, prev = 500;
    decodeXaBlock(two, two + 1, 1, cur, prev, out, 1);
    check(out[0] == (1000 * 460 - 500 * 208 + 0x80) >> 8, "XA: predictor 2 from the carried history");

    // Clipping: predictor 2 with a large history overshoots.
    uint8_t clip[15] = {0x20, 0x70};
    cur = 32000, prev = 0;
    decodeXaBlock(clip, clip + 1, 1, cur, prev, out, 1);
    check(out[0] == 32767, "XA: output clips to int16");

    // Larger shift: shift 20 (low nibble 12) scales residual 7 down to (7 << 8 + 0x80) >> 8 = 7.
    uint8_t small[15] = {0x0C, 0x70};
    cur = prev = 0;
    decodeXaBlock(small, small + 1, 1, cur, prev, out, 1);
    check(out[0] == 7, "XA: shift 20");
}

static void testXaFile() {
    // Mono: two blocks, pcmBytes trims the second block to 30 samples in all.
    Writer w;
    xaHeader(w, 1, 22050, 60);
    uint8_t b1[15] = {0x00, 0x10}, b2[15] = {0x00, 0x70};
    w.b.insert(w.b.end(), b1, b1 + 15);
    w.b.insert(w.b.end(), b2, b2 + 15);
    check(detectAudioFormat(w.b) == AudioFormat::Xa, "XA detected");
    Pcm pcm = decodeXa(w.b);
    check(pcm.sampleRate == 22050 && pcm.channels == 1, "XA mono header");
    check(pcm.samples.size() == 30, "XA trimmed to the header's PCM size");
    check(pcm.samples[0] == 4096 && pcm.samples[28] == 28672, "XA blocks decode in order");

    // Stereo: header bytes for left and right, then left/right byte pairs.
    Writer s;
    xaHeader(s, 2, 32000, 28 * 2 * 2);
    s.u8(0x00); // left: predictor 0, shift 8
    s.u8(0x0C); // right: predictor 0, shift 20
    for (int i = 0; i < 14; ++i) {
        s.u8(i == 0 ? 0x17 : 0x00);
        s.u8(i == 0 ? 0x7F : 0x00);
    }
    pcm = decodeXa(s.b);
    check(pcm.channels == 2 && pcm.frames() == 28, "XA stereo frames");
    check(pcm.samples[0] == 4096 && pcm.samples[1] == 7, "XA stereo first frame (L, R)");
    check(pcm.samples[2] == 28672 && pcm.samples[3] == -1 + 0, "XA stereo second frame");

    // A trailing 15-byte partial stereo block (13 of the game's stereo files
    // end like this) still yields the frames the header asks for.
    Writer part = s;
    part.b[4] = uint8_t((28 + 6) * 4); // pcmBytes: one block plus 6 frames
    part.u8(0x00), part.u8(0x00);
    part.u8(0x11); // left 1,1  right 1,1 (nibble pairs)
    part.u8(0x11);
    for (int i = 0; i < 11; ++i)
        part.u8(0);
    pcm = decodeXa(part.b);
    check(pcm.frames() == 34, "XA partial stereo block decoded and trimmed");
    check(pcm.samples[28 * 2] == 4096 && pcm.samples[28 * 2 + 1] == 4096, "XA partial block first frame");

    // Round trip a 440 Hz tone through the reference encoder.
    const int n = 28 * 200;
    std::vector<int16_t> tone(n);
    for (int i = 0; i < n; ++i)
        tone[size_t(i)] = int16_t(12000 * std::sin(2 * M_PI * 440 * i / 22050.0));
    Writer t;
    xaHeader(t, 1, 22050, uint32_t(n * 2));
    int cur = 0, prev = 0;
    for (int b = 0; b < n / 28; ++b) {
        uint8_t blk[15];
        encodeBlock(&tone[size_t(b) * 28], cur, prev, blk);
        t.b.insert(t.b.end(), blk, blk + 15);
    }
    pcm = decodeXa(t.b);
    double sig = 0, noise = 0;
    for (int i = 0; i < n; ++i) {
        sig += double(tone[size_t(i)]) * tone[size_t(i)];
        const double e = double(pcm.samples[size_t(i)]) - tone[size_t(i)];
        noise += e * e;
    }
    const double snr = 10 * std::log10(sig / std::max(noise, 1.0));
    check(pcm.samples.size() == size_t(n), "XA tone length");
    check(snr > 30, ("XA tone SNR > 30 dB (" + std::to_string(snr) + ")").c_str());

    // Bad header.
    Writer bad;
    xaHeader(bad, 3, 22050, 0);
    bool threw = false;
    try {
        decodeXa(bad.b);
    } catch (const std::exception&) {
        threw = true;
    }
    check(threw, "XA rejects 3 channels");
}

static void testSpx() {
    // Encode 0.2 s of a tone with libspeex (ultra-wideband) into an SPX1 resource.
    void* enc = speex_encoder_init(speex_lib_get_mode(SPEEX_MODEID_UWB));
    int frame = 0, quality = 8;
    speex_encoder_ctl(enc, SPEEX_GET_FRAME_SIZE, &frame);
    speex_encoder_ctl(enc, SPEEX_SET_QUALITY, &quality);
    check(frame == 640, "Speex UWB frame is 640 samples");
    const int frames = 10;
    Writer w;
    w.tag("SPX1");
    w.u8(1);
    w.u32(uint32_t(frames * frame * 2 - 100)); // header size trims the tail
    w.u32(2);
    w.u16(uint16_t(frame));
    SpeexBits bits;
    speex_bits_init(&bits);
    std::vector<spx_int16_t> in(static_cast<size_t>(frame));
    double energyIn = 0;
    for (int f = 0; f < frames; ++f) {
        for (int i = 0; i < frame; ++i) {
            in[size_t(i)] = spx_int16_t(8000 * std::sin(2 * M_PI * 300 * (f * frame + i) / 32000.0));
            if (f >= 2)
                energyIn += double(in[size_t(i)]) * in[size_t(i)];
        }
        speex_bits_reset(&bits);
        speex_encode_int(enc, in.data(), &bits);
        char buf[256];
        const int len = speex_bits_write(&bits, buf, sizeof buf);
        w.u8(uint8_t(len));
        w.b.insert(w.b.end(), buf, buf + len);
    }
    speex_bits_destroy(&bits);
    speex_encoder_destroy(enc);

    check(detectAudioFormat(w.b) == AudioFormat::Spx, "SPX detected");
    const auto h = parseSpxHeader(w.b);
    check(h && h->mode == 2 && h->frameSamples == 640, "SPX header");
    const Pcm pcm = decodeSpx(w.b);
    check(pcm.sampleRate == 32000 && pcm.channels == 1, "SPX decodes at 32 kHz mono");
    check(pcm.samples.size() == size_t(frames * frame - 50), "SPX trimmed to the header's PCM size");
    double energyOut = 0;
    for (size_t i = size_t(2 * frame); i < pcm.samples.size(); ++i)
        energyOut += double(pcm.samples[i]) * pcm.samples[i];
    const double ratio = energyOut / energyIn;
    check(ratio > 0.3 && ratio < 3, ("SPX keeps the tone's energy (" + std::to_string(ratio) + ")").c_str());

    Writer cut = w;
    cut.b.resize(cut.b.size() - 3);
    bool threw = false;
    try {
        decodeSpx(cut.b);
    } catch (const std::exception&) {
        threw = true;
    }
    check(threw, "SPX rejects a truncated frame");
}

static void testWavAndHitList() {
    Pcm pcm;
    pcm.sampleRate = 44100;
    pcm.channels = 2;
    pcm.samples = {1, -1, 32767, -32768, 0, 5};
    const auto wav = encodeWav(pcm);
    check(wav.size() == 44 + 12, "WAV size");
    check(detectAudioFormat(wav) == AudioFormat::Wav, "WAV detected");
    const Pcm back = decodeWav(wav);
    check(back.sampleRate == 44100 && back.channels == 2 && back.samples == pcm.samples, "WAV round trip");
    check(pcm.peak() == 32768, "peak");

    Writer h;
    h.u32(56);
    h.u32(2);
    h.u32(0xFF000001), h.u32(0x12345678);
    h.u32(0xFF000002), h.u32(0x9ABCDEF0);
    const HitList list = parseHitList(h.b);
    check(list.version == 56 && list.sounds.size() == 2, "hit list count");
    check(list.sounds[1].first == 0xFF000002 && list.sounds[1].second == 0x9ABCDEF0, "hit list (instance, instanceHi)");

    const uint8_t mp3[] = {0xFF, 0xFB, 0x90, 0x40};
    const uint8_t ini[] = "[Admin]\r\nEnabled=ON\r\n";
    const uint8_t hit[] = "HIT!\x01\x00";
    check(detectAudioFormat(mp3) == AudioFormat::Mp3, "MP3 detected");
    check(detectAudioFormat({ini, sizeof ini - 1}) == AudioFormat::Text, "INI detected as text");
    check(detectAudioFormat({hit, sizeof hit - 1}) == AudioFormat::Hit, "HIT detected");
    check(decodeSound(wav).samples == pcm.samples, "decodeSound dispatches WAV");
}

int main() {
    testXaBlock();
    testXaFile();
    testSpx();
    testWavAndHitList();
    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("audio: all tests passed\n");
    return 0;
}
