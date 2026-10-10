#pragma once

#include "image.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace os2 {

// Image files stored whole in IMG resources (0x856DDBAC, mostly the UI's):
// in ui.package, 32/24-bit TGA (raw or RLE, bottom-up), 24/32-bit BMP,
// 8-bit RGB/RGBA PNG and a single JPEG; elsewhere (thumbnails, CAS,
// neighbourhood art) mostly baseline JPEG.
enum class UiImageFormat { Unknown, Tga, Bmp, Png, Jpeg };

const char* uiImageFormatName(UiImageFormat f);
UiImageFormat detectUiImage(std::span<const uint8_t> data);

// Decodes to RGBA8 (top row first). BMPs have no alpha channel and come out
// opaque; JPEGs get theirs from the ALFA segment (see decodeJpeg). `error`,
// when given, says why decoding failed.
std::optional<RgbaImage> decodeUiImage(std::span<const uint8_t> data, std::string* error = nullptr);

} // namespace os2
