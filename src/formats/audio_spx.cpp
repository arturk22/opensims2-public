// SPX1 (Speex) sound resources, decoded with the vendored libspeex 1.2.1
// (third_party/speex, BSD license).
#include "audio.hpp"

#include "binreader.hpp"

#include <speex/speex.h>

#include <memory>

namespace os2 {

Pcm decodeSpx(std::span<const uint8_t> d) {
    const auto h = parseSpxHeader(d);
    if (!h)
        throw FormatError("not an SPX1 resource");
    if (h->channels != 1)
        throw FormatError("SPX: " + std::to_string(h->channels) + " channels");
    if (h->mode > SPEEX_MODEID_UWB)
        throw FormatError("SPX: unknown Speex mode " + std::to_string(h->mode));

    const SpeexMode* mode = speex_lib_get_mode(int(h->mode));
    std::unique_ptr<void, void (*)(void*)> dec(speex_decoder_init(mode), speex_decoder_destroy);
    if (!dec)
        throw FormatError("SPX: speex_decoder_init failed");
    int frameSize = 0, rate = 0;
    speex_decoder_ctl(dec.get(), SPEEX_GET_FRAME_SIZE, &frameSize);
    speex_decoder_ctl(dec.get(), SPEEX_GET_SAMPLING_RATE, &rate);
    if (frameSize <= 0 || (h->frameSamples && h->frameSamples != frameSize))
        throw FormatError("SPX: frame size " + std::to_string(h->frameSamples) + " != Speex's " +
                          std::to_string(frameSize));

    SpeexBits bits;
    speex_bits_init(&bits);
    std::unique_ptr<SpeexBits, void (*)(SpeexBits*)> bitsGuard(&bits, speex_bits_destroy);

    Pcm pcm;
    pcm.sampleRate = uint32_t(rate);
    pcm.channels = 1;
    pcm.samples.reserve(h->pcmBytes / 2);
    std::vector<spx_int16_t> frame(static_cast<size_t>(frameSize));
    size_t pos = kSpxHeaderSize;
    while (pos < d.size()) {
        const size_t len = d[pos++];
        if (len == 0 || pos + len > d.size())
            throw FormatError("SPX: truncated frame at " + std::to_string(pos - 1));
        speex_bits_read_from(&bits, reinterpret_cast<const char*>(d.data() + pos), int(len));
        pos += len;
        if (speex_decode_int(dec.get(), &bits, frame.data()) != 0)
            throw FormatError("SPX: bad Speex frame at " + std::to_string(pos - len - 1));
        pcm.samples.insert(pcm.samples.end(), frame.begin(), frame.end());
    }
    const size_t wanted = h->pcmBytes / 2;
    if (wanted && wanted < pcm.samples.size())
        pcm.samples.resize(wanted);
    return pcm;
}

} // namespace os2
