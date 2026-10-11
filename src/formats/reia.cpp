#include "formats/reia.hpp"

#include "formats/dbpf.hpp"
#include "formats/locale.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace os2 {

namespace {
uint32_t u32At(const uint8_t* p) { return uint32_t(p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24); }
constexpr int kBlock = 32;
} // namespace

bool Reia::detect(std::span<const uint8_t> b) {
    return b.size() >= 16 && std::memcmp(b.data(), "RIFF", 4) == 0 && std::memcmp(b.data() + 8, "Reiahead", 8) == 0;
}

Reia::Reia(std::vector<uint8_t> bytes) : bytes_(std::move(bytes)) {
    if (!detect(bytes_))
        throw std::runtime_error("not a Reia (RIFF ... Reiahead)");
    size_t o = 12;
    while (o + 8 <= bytes_.size()) {
        const uint8_t* c = bytes_.data() + o;
        const size_t size = u32At(c + 4);
        if (o + 8 + size > bytes_.size())
            break;
        if (std::memcmp(c, "head", 4) == 0 && size >= 24) {
            width_ = u32At(c + 12);
            height_ = u32At(c + 16);
            const uint32_t scale = u32At(c + 20), duration = u32At(c + 24);
            if (scale && duration)
                frameSeconds_ = double(duration) / scale;
        } else if (std::memcmp(c, "frme", 4) == 0) {
            frames_.push_back({o + 8, size});
        }
        o += 8 + size + (size & 1);
    }
    if (!width_ || !height_ || width_ > 8192 || height_ > 8192)
        throw std::runtime_error("Reia without a valid head chunk");
}

ReiaDecoder::ReiaDecoder(const Reia& reia) : reia_(reia) {
    picture_.width = reia.width_;
    picture_.height = reia.height_;
    reset();
}

void ReiaDecoder::reset() {
    picture_.pixels.assign(size_t(reia_.width_) * reia_.height_ * 4, 0);
    for (size_t i = 3; i < picture_.pixels.size(); i += 4)
        picture_.pixels[i] = 255;
    next_ = 0;
}

bool ReiaDecoder::decodeFrame(std::span<const uint8_t> d) {
    const int w = int(reia_.width_), h = int(reia_.height_);
    const int bw = (w + kBlock - 1) / kBlock, bh = (h + kBlock - 1) / kBlock;
    size_t p = 0;
    for (int by = 0; by < bh; ++by) {
        for (int bx = 0; bx < bw; ++bx) {
            if (p >= d.size())
                return false;
            const uint8_t flag = d[p++];
            if (flag == 0)
                continue;
            int n = 0;
            auto put = [&](const uint8_t* bgr) {
                const int x = bx * kBlock + n % kBlock, y = by * kBlock + n / kBlock;
                if (x < w && y < h) {
                    uint8_t* o = picture_.pixels.data() + (size_t(y) * size_t(w) + size_t(x)) * 4;
                    o[0] = uint8_t(o[0] + bgr[2]);
                    o[1] = uint8_t(o[1] + bgr[1]);
                    o[2] = uint8_t(o[2] + bgr[0]);
                }
                ++n;
            };
            while (n < kBlock * kBlock) {
                if (p >= d.size())
                    return false;
                const uint8_t c = d[p++];
                if (c < 128) {
                    const int count = c + 1;
                    if (p + size_t(count) * 3 > d.size() || n + count > kBlock * kBlock)
                        return false;
                    for (int i = 0; i < count; ++i, p += 3)
                        put(d.data() + p);
                } else {
                    const int count = 257 - c;
                    if (p + 3 > d.size() || n + count > kBlock * kBlock)
                        return false;
                    for (int i = 0; i < count; ++i)
                        put(d.data() + p);
                    p += 3;
                }
            }
        }
    }
    return true;
}

bool ReiaDecoder::seekFrame(size_t index) {
    if (reia_.frames_.empty())
        return false;
    const long want = long(std::min(index, reia_.frames_.size() - 1));
    if (want < next_ - 1)
        reset(); // every frame builds on the one before: from the start
    bool ok = true;
    for (; next_ <= want; ++next_) {
        const auto [o, n] = reia_.frames_[size_t(next_)];
        ok = decodeFrame({reia_.bytes_.data() + o, n}) && ok;
    }
    return ok;
}

bool ReiaDecoder::seek(double seconds) {
    return seekFrame(size_t(std::max(0.0, std::floor(seconds / reia_.frameSeconds() + 1e-6))));
}

std::optional<std::vector<uint8_t>> readUiReia(const std::filesystem::path& resDir, uint32_t instance) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path localized = resDir / "Locale" / defaultLocale() / "UI/ui.package";
    for (const fs::path& p : {localized, resDir / "UI/ui.package"}) {
        if (!fs::is_regular_file(p, ec))
            continue;
        DbpfPackage pkg(p);
        if (const DbpfEntry* e = pkg.find(Tgi{0, kReiaGroup, instance, 0})) {
            auto bytes = pkg.read(*e);
            if (Reia::detect(bytes))
                return bytes;
        }
    }
    return std::nullopt;
}

} // namespace os2
