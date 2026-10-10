#pragma once

#include "image.hpp"

#include <cstdint>
#include <span>
#include <stdexcept>

namespace os2 {

struct JpegError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Baseline (huffman, 8-bit) JPEG decoder: greyscale or YCbCr, any sampling
// factors, restart markers. Maxis' JPEGs may carry an alpha channel in an
// APP0 "ALFA" segment (several are concatenated) (run-length coded, one byte per pixel, top row first:
// a negative count byte -n repeats the next byte n+1 times, a count n copies
// n+1 bytes); without one the image is opaque. Throws JpegError.
RgbaImage decodeJpeg(std::span<const uint8_t> data);

} // namespace os2
