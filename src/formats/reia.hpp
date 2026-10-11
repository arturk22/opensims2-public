#pragma once

// Reia: Maxis's UI animation format (7 resources of type 0 in ui.package,
// group 0x499DB772, instances 0x8DA3ADE7-0x8DA3ADED; ".reia" files on the
// Mac; played by the UI window class cTSWinReiaPlayer).
//
// RIFF container, form "Reia":
//   "head" (24 bytes): u32 version (1), u32 width, u32 height, u32 time
//          scale (1000000), u32 frame duration in that scale (33333: 30 fps),
//          u32 frame count.
//   "frme" per frame: the picture in 32x32 blocks, left to right and top to
//          bottom (edge blocks are full size and clipped): per block a byte,
//          0 = unchanged from the previous frame, 1 = 1024 BGR values
//          follow, PackBits-style (a byte c < 128: c + 1 literal pixels;
//          c >= 128: one pixel repeated 257 - c times), which are added to
//          the previous frame's pixels per channel, modulo 256 (the first
//          frame adds to black, so it is the picture itself).
// No sound.

#include "formats/image.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <vector>

namespace os2 {

constexpr uint32_t kReiaGroup = 0x499DB772; // their group in ui.package (type 0)

class Reia {
public:
    explicit Reia(std::vector<uint8_t> bytes); // throws if not a Reia
    static bool detect(std::span<const uint8_t> bytes);

    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    size_t frameCount() const { return frames_.size(); }
    double frameSeconds() const { return frameSeconds_; }
    double duration() const { return frameSeconds_ * double(frames_.size()); }

private:
    friend class ReiaDecoder;
    std::vector<uint8_t> bytes_;
    std::vector<std::pair<size_t, size_t>> frames_; // payload offset, size
    uint32_t width_ = 0, height_ = 0;
    double frameSeconds_ = 1.0 / 30;
};

// Decodes frames in order into an RGBA picture (restarting for an earlier frame).
class ReiaDecoder {
public:
    explicit ReiaDecoder(const Reia& reia);
    // Decodes up to frame `index` (clamped); false if a frame was damaged.
    bool seekFrame(size_t index);
    bool seek(double seconds);
    const RgbaImage& picture() const { return picture_; }
    long current() const { return next_ - 1; }

private:
    const Reia& reia_;
    RgbaImage picture_;
    long next_ = 0;
    bool decodeFrame(std::span<const uint8_t> data);
    void reset();
};

// A Reia of ui.package by instance (type 0, group kReiaGroup): the
// language's Locale/<lang>/UI/ui.package first, then UI/ui.package.
std::optional<std::vector<uint8_t>> readUiReia(const std::filesystem::path& resDir, uint32_t instance);

// Where the game shows them (cTSWinReiaPlayer, CLSID 0x8DA279CB, created by
// code, not by UI scripts): the start-up loading screen
// (cTSWinProcGameLoad) plays 0x8DA3ADE7; the lot loading screen
// (cTSWinProcLotIntro::SetupWindow) one of 0x8DA3ADE8-0x8DA3ADED by the kind
// of lot and how it is entered, with an icon IMG (0x8DA3ADF0-0x8DA3ADFA,
// same group) in the middle tile.
constexpr uint32_t kReiaGameLoad = 0x8DA3ADE7;

} // namespace os2
