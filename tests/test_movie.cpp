// The .movie decoder (formats/movie.hpp): bit reader, AC codes, IDCT, MAD
// intra/inter frames built by hand, EA ADPCM R1 and the sound header.
#include "formats/movie.hpp"
#include "formats/reia.hpp"
#include "formats/cinematic.hpp"

#include <cstdio>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace os2;

static int failures = 0;

static void check(bool cond, const std::string& what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
        ++failures;
    }
}

// Writes bits MSB first, then swaps byte pairs: the MAD word order.
struct BitWriter {
    std::vector<uint8_t> bytes;
    int used = 8;
    void put(uint32_t v, int n) {
        for (int i = n - 1; i >= 0; --i) {
            if (used == 8) {
                bytes.push_back(0);
                used = 0;
            }
            bytes.back() |= uint8_t(((v >> i) & 1) << (7 - used));
            ++used;
        }
    }
    std::vector<uint8_t> words() const {
        std::vector<uint8_t> b = bytes;
        if (b.size() & 1)
            b.push_back(0);
        b.resize(b.size() + 8, 0); // padding
        for (size_t i = 0; i + 1 < b.size(); i += 2)
            std::swap(b[i], b[i + 1]);
        return b;
    }
};

static void testBitReader() {
    const uint8_t data[] = {0x34, 0x12, 0x78, 0x56, 0xAB};
    MadBitReader br(data);
    check(br.peek(16) == 0x1234, "peek the first word");
    check(br.bits(4) == 0x1, "first nibble is the word's high bits");
    check(br.bits(12) == 0x234, "rest of the word");
    check(br.signedBits(8) == 0x56, "positive signed");
    check(br.signedBits(4) == 0x7, "nibble");
    check(br.signedBits(4) == -8, "negative signed (0x8)");
    check(br.bits(8) == 0, "the odd last byte is not part of a word: zeros");
    check(br.overrun(), "past the end");
}

static void testAcCodes() {
    BitWriter w;
    w.put(0x3, 2);   // run 0 level 1
    w.put(0x1b, 16); // run 31 level 1
    w.put(0x1, 6);   // escape
    w.put(0x6, 5);   // run 4 level 1
    w.put(0x1f, 14); // run 0 level 16
    w.put(0x2, 2);   // end of block
    const auto b = w.words();
    MadBitReader br(b);
    auto c = readMadAcCode(br);
    check(c.kind == MadAcCode::Coefficient && c.run == 1 && c.level == 1 && c.length == 2, "11 = run 0 level 1");
    c = readMadAcCode(br);
    check(c.kind == MadAcCode::Coefficient && c.run == 32 && c.level == 1, "16-bit code = run 31 level 1");
    c = readMadAcCode(br);
    check(c.kind == MadAcCode::Escape && c.length == 6, "escape");
    c = readMadAcCode(br);
    check(c.kind == MadAcCode::Coefficient && c.run == 5 && c.level == 1, "00110 = run 4 level 1");
    c = readMadAcCode(br);
    check(c.kind == MadAcCode::Coefficient && c.run == 1 && c.level == 16, "run 0 level 16");
    c = readMadAcCode(br);
    check(c.kind == MadAcCode::EndOfBlock, "end of block");
    check(br.position() == 2 + 16 + 6 + 5 + 14 + 2, "bits consumed");
    const uint8_t zeros[4] = {};
    MadBitReader bz(zeros);
    check(readMadAcCode(bz).kind == MadAcCode::Invalid, "all zeros is no code");
}

static void testIdct() {
    int16_t block[64] = {};
    uint8_t out[64];
    block[0] = 100 * 16;
    madIdct(block, out, 8);
    bool flat = true;
    for (uint8_t v : out)
        flat = flat && v == 100;
    check(flat, "DC-only block is flat at DC/16");
    std::memset(block, 0, sizeof block);
    block[0] = 128 * 16;
    block[1] = 200; // the first horizontal frequency
    madIdct(block, out, 8);
    bool rows = true;
    for (int r = 1; r < 8; ++r)
        rows = rows && std::memcmp(out, out + r * 8, 8) == 0;
    check(rows, "a horizontal frequency makes equal rows");
    check(out[0] > out[3] && out[3] > 128 && out[4] < 128 && out[7] < out[4], "a falling cosine across the row");
    check(std::abs((out[0] - 128) + (out[7] - 128)) <= 1, "odd symmetry around the centre");
}

// A MAD chunk for a 16x16 picture.
static std::vector<uint8_t> madChunk(char kind, const BitWriter& w, uint8_t qscale = 1) {
    std::vector<uint8_t> c(24, 0);
    std::memcpy(c.data(), "MAD", 3);
    c[3] = uint8_t(kind);
    const uint32_t ms = 0x0042AAAB;
    std::memcpy(&c[12], &ms, 4);
    c[16] = 16; c[18] = 16;
    c[20] = 2; c[21] = qscale;
    const auto b = w.words();
    c.insert(c.end(), b.begin(), b.end());
    const uint32_t size = uint32_t(c.size());
    std::memcpy(&c[4], &size, 4);
    return c;
}

static void testMadFrames() {
    MadDecoder mad;
    // Intra: six DC-only blocks; Y blocks 128 + 10*j, chroma 128 - 20 and 128 + 30.
    BitWriter k;
    const int dc[6] = {0, 10, 20, 30, -20, 30};
    for (int j = 0; j < 6; ++j) {
        k.put(uint32_t(dc[j]) & 0xFF, 8);
        k.put(0x2, 2); // end of block
    }
    check(mad.decode(madChunk('k', k)), "intra frame decodes");
    check(mad.width() == 16 && mad.height() == 16, "size");
    check(std::abs(mad.frameMs() - 66.6667) < 0.01, "frame duration from 16.16 ms");
    const uint8_t* y = mad.plane(0);
    check(y[0] == 128 && y[8] == 138 && y[8 * 16] == 148 && y[8 * 16 + 8] == 158, "luma blocks in raster order");
    check(mad.plane(1)[0] == 108 && mad.plane(2)[0] == 158, "chroma blocks");

    // Inter: the macroblock copied (map 63, vector 0,0) with +2*(1+v) per block.
    auto copyAll = [](int add4) {
        BitWriter w;
        w.put(1, 1); // macroblock type: all blocks copied
        w.put(0, 1); // mv x = 0
        w.put(0, 1); // mv y = 0
        for (int j = 0; j < 6; ++j) {
            w.put(1, 1);
            w.put(0, 1);
            w.put(uint32_t(add4), 4); // offset (1 + add4) * 2
        }
        return w;
    };
    check(mad.decode(madChunk('e', copyAll(2))), "MADe decodes");
    check(mad.plane(0)[0] == 134 && mad.plane(1)[0] == 114, "MADe adds its offset");
    check(mad.decode(madChunk('m', copyAll(0))), "MADm decodes");
    check(mad.plane(0)[0] == 130, "MADe was not the reference: MADm builds on the intra frame");
    check(mad.decode(madChunk('m', copyAll(0))), "second MADm");
    check(mad.plane(0)[0] == 132 && mad.plane(0)[8] == 142, "MADm became the reference");

    // Partly copied: type 01 + 6-bit map (only block 0 copied), vector (-1, 0)
    // from outside the picture: left as it was; the others intra.
    BitWriter p;
    p.put(0, 1);
    p.put(1, 1);
    p.put(0x01, 6);
    p.put(0b11, 2); p.put(15, 4); // mv x = -17 + 15 + 1 = -1
    p.put(0, 1);                  // mv y = 0
    p.put(0b10, 2); p.put(0, 4);  // block 0 offset 2
    for (int j = 1; j < 6; ++j) {
        p.put(5, 8);
        p.put(0x2, 2);
    }
    check(mad.decode(madChunk('m', p)), "partly intra macroblock");
    check(mad.plane(0)[8] == 133 && mad.plane(1)[0] == 133, "intra blocks of an inter frame");

    RgbaImage img;
    mad.toRgba(img);
    check(img.width == 16 && img.height == 16 && img.pixels.size() == 16 * 16 * 4, "RGBA size");
    check(img.pixels[3] == 255, "opaque");

    BitWriter bad;
    bad.put(0, 8);
    bad.put(0, 16); // no valid AC code
    check(!mad.decode(madChunk('k', bad)), "damaged block is reported");
}

static void testAdpcm() {
    // Stereo, 28 samples per channel; predictor 0 and shift 8 make each
    // nibble the sample itself; the right channel starts at offset 20.
    std::vector<uint8_t> p = {28, 0, 0, 0, 0, 0, 0, 0, 20, 0, 0, 0};
    auto channel = [&](int base) {
        for (int i = 0; i < 4; ++i)
            p.push_back(0); // current, previous
        p.push_back(0x0C);  // coefficients 0, shift 20 - 12 = 8
        for (int i = 0; i < 14; ++i) {
            const int a = (base + 2 * i) & 15, b = (base + 2 * i + 1) & 15;
            p.push_back(uint8_t(a << 4 | b));
        }
        p.push_back(0); // pad to 20 bytes
    };
    channel(0);
    channel(8);
    std::vector<int16_t> out;
    decodeEaAdpcmR1(p, 2, out);
    check(out.size() == 56, "28 samples x 2 channels");
    bool ok = out.size() == 56;
    for (int i = 0; ok && i < 28; ++i) {
        const int l = int8_t(uint8_t(i & 15) << 4) >> 4, r = int8_t(uint8_t((8 + i) & 15) << 4) >> 4;
        ok = out[size_t(2 * i)] == l && out[size_t(2 * i + 1)] == r;
    }
    check(ok, "nibbles decode to signed samples, interleaved");

    // Prediction: coefficient pair 1 (240/256, 0) on a constant input.
    std::vector<uint8_t> q = {28, 0, 0, 0, 0, 0, 0, 0, 0x00, 0x10, 0x00, 0x10, 0x1C};
    q.resize(q.size() + 14, 0);
    out.clear();
    decodeEaAdpcmR1(q, 1, out);
    check(out.size() == 28 && out[0] == (4096 * 240) >> 8 && out[1] == (out[0] * 240) >> 8, "first-order prediction");
}

static void testSoundHeader() {
    const uint8_t h[] = {'P', 'T', 0, 0, 0x06, 0x01, 0x65, 0x0b, 0x01, 0x02, 0xfd, 0x80, 0x01, 0x01, 0x85,
                         0x03, 0x01, 0x13, 0xe6, 0x82, 0x01, 0x02, 0x8c, 0x01, 0x04, 0xff, 0x00, 0x00};
    const auto s = parseSoundHeader(h);
    check(s.revision == 1 && s.channels == 2 && s.samples == 0x0113e6, "header values");
    check(s.sampleRate == 22050 && s.compression == -1, "defaults: 22050 Hz, ADPCM by revision");
}

// A 40x36 Reia (2x2 blocks of 32, clipped): frame 0 paints every block,
// frame 1 changes only block 1 (top right) by a delta.
static void testReia() {
    auto le = [](std::vector<uint8_t>& v, uint32_t x) {
        for (int i = 0; i < 4; ++i)
            v.push_back(uint8_t(x >> (8 * i)));
    };
    std::vector<uint8_t> f0, f1;
    for (int b = 0; b < 4; ++b) { // each block: one colour, 1024 = 8 runs of 128
        f0.push_back(1);
        for (int r = 0; r < 8; ++r) {
            f0.push_back(129); // 257 - 129 = 128 copies
            f0.push_back(uint8_t(10 * b)); f0.push_back(20); f0.push_back(200); // B, G, R
        }
    }
    f1.push_back(0);
    f1.push_back(1);
    f1.push_back(2); // 3 literal pixels
    for (int i = 0; i < 3; ++i) { f1.push_back(1); f1.push_back(uint8_t(-5)); f1.push_back(0); }
    for (int left = 1024 - 3; left > 0; left -= 128) { // the rest: zero delta
        const int n = std::min(left, 128);
        f1.push_back(uint8_t(257 - n));
        f1.push_back(0); f1.push_back(0); f1.push_back(0);
    }
    f1.push_back(0);
    f1.push_back(0);
    std::vector<uint8_t> r = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'R', 'e', 'i', 'a', 'h', 'e', 'a', 'd'};
    le(r, 24);
    for (uint32_t v : {1u, 40u, 36u, 1000000u, 33333u, 2u})
        le(r, v);
    for (auto* f : {&f0, &f1}) {
        r.insert(r.end(), {'f', 'r', 'm', 'e'});
        le(r, uint32_t(f->size()));
        r.insert(r.end(), f->begin(), f->end());
        if (f->size() & 1)
            r.push_back(0);
    }
    check(Reia::detect(r), "Reia detected");
    const Reia reia(r);
    check(reia.width() == 40 && reia.height() == 36 && reia.frameCount() == 2, "Reia head");
    check(std::abs(reia.frameSeconds() - 0.033333) < 1e-5, "Reia frame time");
    ReiaDecoder d(reia);
    check(d.seekFrame(0), "Reia frame 0");
    const auto px = [&](int x, int y) { return d.picture().pixels.data() + (y * 40 + x) * 4; };
    check(px(0, 0)[0] == 200 && px(0, 0)[1] == 20 && px(0, 0)[2] == 0, "BGR to RGB");
    check(px(39, 0)[2] == 10 && px(0, 35)[2] == 20 && px(39, 35)[2] == 30, "blocks clipped at the edges");
    check(d.seekFrame(1), "Reia frame 1");
    check(px(32, 0)[0] == 200 && px(32, 0)[1] == uint8_t(20 - 5) && px(32, 0)[2] == 11, "delta added per channel");
    check(px(35, 0)[1] == 20 && px(0, 0)[1] == 20, "zero delta and unchanged blocks");
    check(d.seekFrame(0) && px(32, 0)[2] == 10, "seeking back restarts from black");
}

// A CINE as the game writes it (cCinematicScene::WriteTo).
static void testCinematic() {
    std::vector<uint8_t> b = {0x01, 0x00, 0xFF, 0xFF, 0, 0, 0, 0, 1, 0, 0, 0, 0x42, 0xF0, 0x51, 0x4D};
    auto u32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back(uint8_t(v >> (8 * i))); };
    auto str = [&](const std::string& t) { b.push_back(uint8_t(t.size())); b.insert(b.end(), t.begin(), t.end()); };
    str("cCinematicScene");
    u32(kTypeCinematic);
    u32(4);
    str("cSGResource");
    u32(0);
    u32(2);
    str("test_cine");
    str("o-cinecam-test");
    u32(2);
    str("sim"); b.push_back(1); str("auskel::1"); u32(7); b.push_back(0); b.push_back(0); str(""); str("");
    str("rig"); b.push_back(1); str("lightrig::1"); u32(0); b.push_back(1); b.push_back(1);
    str("cineLightRig-template"); str("o-rig-1_anim,o-rig-2_anim");
    u32(1); // origin actor
    u32(2); // sequences
    const auto c = CinematicScene::parse(b);
    check(c.version == 4 && c.name == "test_cine" && c.cameraAnimation == "o-cinecam-test", "CINE header");
    check(c.cameraAnimationResource() == "o-cinecam-test_anim", "camera ANIM name");
    check(c.actors.size() == 2 && c.actors[0].name == "sim" && c.actors[0].required &&
              c.actors[0].sgObjectName == "auskel::1" && c.actors[0].renderGroups == 7 && !c.actors[0].autoCreate,
          "CINE actor 0");
    check(c.actors[1].autoCreate && c.actors[1].autoOrigin && c.actors[1].autoModel == "cineLightRig-template" &&
              c.actors[1].autoAnimations.size() == 2 && c.actors[1].autoAnimations[1] == "o-rig-2_anim",
          "CINE actor 1 with its animation list");
    check(c.originActor == 1 && c.sequencedAnimations == 2, "CINE origin and sequences");
}

int main() {
    testCinematic();
    testReia();
    testBitReader();
    testAcCodes();
    testIdct();
    testMadFrames();
    testAdpcm();
    testSoundHeader();
    if (failures)
        std::fprintf(stderr, "%d failure(s)\n", failures);
    else
        std::printf("movie: all tests passed\n");
    return failures ? 1 : 0;
}
