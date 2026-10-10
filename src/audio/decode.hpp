#pragma once

#include "formats/audio.hpp"

#include <span>

namespace os2 {

// True when MP3 decoding is available (macOS AudioToolbox).
bool haveMp3Decoder();

// MP3 to 16-bit PCM through AudioToolbox (AudioFile + ExtAudioFile on the
// bytes in memory). Throws FormatError when unavailable or on failure.
Pcm decodeMp3(std::span<const uint8_t> data);

// Any sample resource (XA, SPX, MP3, WAV) to PCM; throws FormatError for
// the others (HIT, text, ...).
Pcm decodeSound(std::span<const uint8_t> data);

} // namespace os2
