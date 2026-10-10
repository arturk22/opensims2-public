#include "audio.hpp"

#include "binreader.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace os2 {

AudioFormat detectAudioFormat(std::span<const uint8_t> d) {
    auto starts = [&](const char* m) {
        const size_t n = std::strlen(m);
        return d.size() >= n && std::memcmp(d.data(), m, n) == 0;
    };
    if (d.size() >= kXaHeaderSize && starts("XA"))
        return AudioFormat::Xa;
    if (starts("SPX1"))
        return AudioFormat::Spx;
    if (starts("ID3") || (d.size() >= 4 && d[0] == 0xFF && (d[1] & 0xE0) == 0xE0))
        return AudioFormat::Mp3;
    if (d.size() >= 12 && starts("RIFF") && std::memcmp(d.data() + 8, "WAVE", 4) == 0)
        return AudioFormat::Wav;
    if (starts("UTM0"))
        return AudioFormat::Utk;
    if (starts("HIT!"))
        return AudioFormat::Hit;
    // Text: the first bytes are printable ASCII or line breaks/tabs.
    const size_t n = std::min<size_t>(d.size(), 64);
    if (n == 0)
        return AudioFormat::Unknown;
    for (size_t i = 0; i < n; ++i) {
        const uint8_t c = d[i];
        if (!(c == '\r' || c == '\n' || c == '\t' || (c >= 0x20 && c < 0x7F)))
            return AudioFormat::Unknown;
    }
    return AudioFormat::Text;
}

const char* audioFormatName(AudioFormat f) {
    switch (f) {
    case AudioFormat::Xa: return "XA";
    case AudioFormat::Spx: return "SPX";
    case AudioFormat::Mp3: return "MP3";
    case AudioFormat::Wav: return "WAV";
    case AudioFormat::Utk: return "UTK";
    case AudioFormat::Hit: return "HIT";
    case AudioFormat::Text: return "text";
    case AudioFormat::Unknown: break;
    }
    return "unknown";
}

int Pcm::peak() const {
    int p = 0;
    for (int16_t s : samples)
        p = std::max(p, std::abs(int(s)));
    return p;
}

double Pcm::rmsDb() const {
    if (samples.empty())
        return -INFINITY;
    double sum = 0;
    for (int16_t s : samples)
        sum += double(s) * s;
    const double rms = std::sqrt(sum / double(samples.size())) / 32768.0;
    return rms > 0 ? 20.0 * std::log10(rms) : -INFINITY;
}

std::optional<XaHeader> parseXaHeader(std::span<const uint8_t> d) {
    if (d.size() < kXaHeaderSize || d[0] != 'X' || d[1] != 'A')
        return std::nullopt;
    XaHeader h;
    h.variant[0] = char(d[2]);
    h.variant[1] = char(d[3]);
    h.pcmBytes = le32(&d[4]);
    h.formatTag = le16(&d[8]);
    h.channels = le16(&d[10]);
    h.sampleRate = le32(&d[12]);
    h.byteRate = le32(&d[16]);
    h.blockAlign = le16(&d[20]);
    h.bits = le16(&d[22]);
    return h;
}

namespace {

// Predictor coefficients (scaled by 256), indexed by the high nibble of a block header.
constexpr int kXaCoef[4][2] = {{0, 0}, {240, 0}, {460, -208}, {392, -220}};

int16_t clip16(int v) { return int16_t(std::clamp(v, -32768, 32767)); }

} // namespace

void decodeXaBlock(const uint8_t* header, const uint8_t* nibbles, size_t stride, int& cur, int& prev,
                   int16_t* out, size_t outStride) {
    const int pred = (*header >> 4) & 3; // only 0..3 occur in the data
    const int c1 = kXaCoef[pred][0], c2 = kXaCoef[pred][1];
    const int shift = (*header & 0x0F) + 8;
    for (int i = 0; i < 14; ++i) {
        const uint8_t b = nibbles[size_t(i) * stride];
        for (int n : {b >> 4, b & 0x0F}) {
            const int residual = int32_t(uint32_t(n) << 28) >> shift;
            const int s = clip16((residual + cur * c1 + prev * c2 + 0x80) >> 8);
            prev = cur;
            cur = s;
            *out = int16_t(s);
            out += outStride;
        }
    }
}

Pcm decodeXa(std::span<const uint8_t> d) {
    const auto h = parseXaHeader(d);
    if (!h)
        throw FormatError("not an XA resource");
    if (h->channels < 1 || h->channels > 2 || h->bits != 16 || h->sampleRate == 0)
        throw FormatError("XA: unsupported format (" + std::to_string(h->channels) + " channels, " +
                          std::to_string(h->bits) + " bits)");
    Pcm pcm;
    pcm.sampleRate = h->sampleRate;
    pcm.channels = h->channels;
    const size_t ch = h->channels;
    const size_t blockSize = 15 * ch;
    const size_t body = d.size() - kXaHeaderSize;
    // Some stereo files end with a 15-byte partial block; it is decoded as
    // if zero-padded and the header's size then trims it.
    const size_t blocks = (body + blockSize - 1) / blockSize;
    const size_t wanted = h->pcmBytes / 2; // total samples over all channels
    pcm.samples.resize(blocks * 28 * ch);
    int cur[2] = {0, 0}, prev[2] = {0, 0};
    uint8_t padded[30];
    for (size_t b = 0; b < blocks; ++b) {
        const uint8_t* p = d.data() + kXaHeaderSize + b * blockSize;
        if (const size_t left = body - b * blockSize; left < blockSize) {
            std::memset(padded, 0, sizeof padded);
            std::memcpy(padded, p, left);
            p = padded;
        }
        int16_t* out = pcm.samples.data() + b * 28 * ch;
        for (size_t c = 0; c < ch; ++c)
            decodeXaBlock(p + c, p + ch + c, ch, cur[c], prev[c], out + c, ch);
    }
    if (wanted && wanted < pcm.samples.size())
        pcm.samples.resize(wanted - wanted % ch);
    return pcm;
}

std::optional<SpxHeader> parseSpxHeader(std::span<const uint8_t> d) {
    if (d.size() < kSpxHeaderSize || std::memcmp(d.data(), "SPX1", 4) != 0)
        return std::nullopt;
    SpxHeader h;
    h.channels = d[4];
    h.pcmBytes = le32(&d[5]);
    h.mode = le32(&d[9]);
    h.frameSamples = le16(&d[13]);
    return h;
}

namespace {

void put16(std::vector<uint8_t>& b, uint16_t v) { b.push_back(uint8_t(v)), b.push_back(uint8_t(v >> 8)); }
void put32(std::vector<uint8_t>& b, uint32_t v) { put16(b, uint16_t(v)), put16(b, uint16_t(v >> 16)); }
void putTag(std::vector<uint8_t>& b, const char* t) { b.insert(b.end(), t, t + 4); }

} // namespace

std::vector<uint8_t> encodeWav(const Pcm& pcm) {
    const uint32_t dataBytes = uint32_t(pcm.samples.size() * 2);
    std::vector<uint8_t> b;
    b.reserve(44 + dataBytes);
    putTag(b, "RIFF");
    put32(b, 36 + dataBytes);
    putTag(b, "WAVE");
    putTag(b, "fmt ");
    put32(b, 16);
    put16(b, 1);
    put16(b, pcm.channels);
    put32(b, pcm.sampleRate);
    put32(b, pcm.sampleRate * pcm.channels * 2);
    put16(b, uint16_t(pcm.channels * 2));
    put16(b, 16);
    putTag(b, "data");
    put32(b, dataBytes);
    for (int16_t s : pcm.samples)
        put16(b, uint16_t(s));
    return b;
}

Pcm decodeWav(std::span<const uint8_t> d) {
    BinReader r(d);
    if (d.size() < 12 || std::memcmp(d.data(), "RIFF", 4) != 0 || std::memcmp(d.data() + 8, "WAVE", 4) != 0)
        throw FormatError("not a WAV file");
    r.seek(12);
    Pcm pcm;
    bool haveFmt = false;
    while (r.remaining() >= 8) {
        const auto tag = r.bytes(4);
        const uint32_t size = r.u32();
        const size_t next = r.pos() + size + (size & 1);
        if (std::memcmp(tag.data(), "fmt ", 4) == 0) {
            const uint16_t fmt = r.u16();
            pcm.channels = r.u16();
            pcm.sampleRate = r.u32();
            r.skip(6);
            const uint16_t bits = r.u16();
            if (fmt != 1 || bits != 16)
                throw FormatError("WAV: only 16-bit PCM is supported");
            haveFmt = true;
        } else if (std::memcmp(tag.data(), "data", 4) == 0) {
            if (!haveFmt)
                throw FormatError("WAV: data before fmt");
            const auto bytes = r.bytes(std::min<size_t>(size, r.remaining()));
            pcm.samples.resize(bytes.size() / 2);
            for (size_t i = 0; i < pcm.samples.size(); ++i)
                pcm.samples[i] = int16_t(le16(&bytes[i * 2]));
            return pcm;
        }
        if (next > d.size())
            break;
        r.seek(next);
    }
    throw FormatError("WAV: no data chunk");
}

HitList parseHitList(std::span<const uint8_t> d) {
    BinReader r(d);
    HitList h;
    h.version = r.u32();
    const uint32_t count = r.u32();
    if (count > r.remaining() / 8)
        throw FormatError("hit list: count past end");
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t lo = r.u32();
        const uint32_t hi = r.u32();
        h.sounds.emplace_back(lo, hi);
    }
    return h;
}

} // namespace os2
