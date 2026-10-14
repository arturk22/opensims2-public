#include "cas_thumbnails.hpp"
#include "formats/log_once.hpp"

#include "cas_catalog.hpp"
#include "dbpf.hpp"
#include "resindex.hpp"
#include "ui_image.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace os2::cas {

namespace {

constexpr const char* kNullGuid = "00000000-0000-0000-0000-000000000000";

std::string lowerGuid(std::string s) {
    for (auto& c : s)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return s.empty() ? kNullGuid : s;
}

std::string keyText(const Tgi& k) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%d-%d-%d", int32_t(k.group), int32_t(k.type), int32_t(k.instance));
    return buf;
}

} // namespace

std::string casFileName(uint32_t ageBit, uint32_t genderBit) {
    std::string s;
    switch (ageBit) {
    case age::Toddler: s = "toddler"; break;
    case age::Child: s = "child"; break;
    case age::Teen: s = "teen"; break;
    case age::Adult: s = "adult"; break;
    case age::Elder: s = "elder"; break;
    case age::YoungAdult: s = "youngadult"; break;
    default: break;
    }
    if (genderBit == gender::Female)
        s += "female";
    else if (genderBit == gender::Male)
        s += "male";
    return s;
}

uint32_t clothingThumbnailId(uint32_t ageBit, uint32_t genderBit, const Tgi& gzps) {
    return nameInstanceHiHash(casFileName(ageBit, genderBit) + "_" + keyText(gzps));
}

uint32_t hairThumbnailId(uint32_t ageBit, uint32_t genderBit, const Tgi& gzps, const std::string& hairTone) {
    return nameInstanceHiHash(casFileName(ageBit, genderBit) + "_" + keyText(gzps) + "_" + lowerGuid(hairTone));
}

uint32_t overlayThumbnailId(uint32_t ageBit, uint32_t genderBit, const std::string& fam, const std::string& hairTone) {
    return nameInstanceHiHash(casFileName(ageBit, genderBit) + "_" + lowerGuid(fam) + "_" + lowerGuid(hairTone));
}

uint32_t archetypeThumbnailId(uint32_t ageBit, uint32_t genderBit, const std::string& fam, const std::string& region) {
    return nameInstanceHiHash(casFileName(ageBit, genderBit) + lowerGuid(fam) + region);
}

uint32_t outfitDataThumbnailId(const Tgi& aged, bool body) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%x-%x-%x-%d", aged.type, aged.group, aged.instance, body ? 1 : 0);
    return nameInstanceHiHash(buf);
}

uint32_t entryThumbnailId(const SimPart& e, uint32_t ageBit, uint32_t genderBit) {
    if (e.tgi.type == type::TextureOverlay || e.tgi.type == type::MeshOverlay)
        return overlayThumbnailId(ageBit, genderBit, family(e), e.property("hairtone"));
    const std::string tone = lowerGuid(e.property("hairtone"));
    if (tone != kNullGuid)
        return hairThumbnailId(ageBit, genderBit, e.tgi, tone);
    return clothingThumbnailId(ageBit, genderBit, e.tgi);
}

std::optional<RgbaImage> loadNamedTexture(ResourceIndex& index, const std::string& name) {
    try {
        if (auto loc = index.findByName(name + "_txtr", 0x1C4A276C)) {
            RgbaImage img = loadTexture(index.read(*loc), &index);
            if (img.width && img.height)
                return img;
        }
    } catch (const std::exception& err) {
        os2::logOnce("CAS texture", name, err);
    }
    return std::nullopt;
}

std::optional<RgbaImage> overlayPreview(ResourceIndex& index, const std::string& faceTexture,
                                        const std::string& overlayTexture, uint32_t subtype) {
    auto face = loadNamedTexture(index, faceTexture);
    auto over = loadNamedTexture(index, overlayTexture);
    if (!face || !over)
        return std::nullopt;
    // The crop, in units of the 512x512 face layout.
    float cx0 = 56, cy0 = 56, side = 400; // the whole face (blush, costume makeup, masks)
    if (subtype == overlay::Eyes)
        cx0 = 0, cy0 = 24, side = 68; // the eyeball at the top left
    else if (subtype == overlay::EyeShadow)
        cx0 = 92, cy0 = 112, side = 140; // the right eye on the screen's left
    else if (subtype == overlay::Lipstick)
        cx0 = 170, cy0 = 270, side = 170;
    else if (subtype == overlay::Brows)
        cx0 = 92, cy0 = 90, side = 150;
    auto sample = [](const RgbaImage& im, float u, float v, float out[4]) {
        const uint32_t x = std::min(im.width - 1, uint32_t(std::max(0.0f, u) * float(im.width)));
        const uint32_t y = std::min(im.height - 1, uint32_t(std::max(0.0f, v) * float(im.height)));
        const uint8_t* p = &im.pixels[(size_t(y) * im.width + x) * 4];
        for (int c = 0; c < 4; ++c)
            out[c] = float(p[c]) / 255.0f;
    };
    constexpr uint32_t kSize = 96, kSub = 3;
    RgbaImage out;
    out.width = out.height = kSize;
    out.pixels.assign(size_t(kSize) * kSize * 4, 255);
    for (uint32_t y = 0; y < kSize; ++y)
        for (uint32_t x = 0; x < kSize; ++x) {
            float acc[3] = {0, 0, 0};
            for (uint32_t sy = 0; sy < kSub; ++sy)
                for (uint32_t sx = 0; sx < kSub; ++sx) {
                    const float u = (cx0 + side * (float(x) + (float(sx) + 0.5f) / kSub) / kSize) / 512.0f;
                    const float v = (cy0 + side * (float(y) + (float(sy) + 0.5f) / kSub) / kSize) / 512.0f;
                    float f[4], o[4];
                    sample(*face, u, v, f);
                    sample(*over, u, v, o);
                    for (int c = 0; c < 3; ++c)
                        acc[c] += f[c] * (1 - o[3]) + o[c] * o[3];
                }
            uint8_t* d = &out.pixels[(size_t(y) * kSize + x) * 4];
            for (int c = 0; c < 3; ++c)
                d[c] = uint8_t(std::clamp(acc[c] / float(kSub * kSub), 0.0f, 1.0f) * 255.0f + 0.5f);
        }
    return out;
}

CasThumbnails::CasThumbnails(const std::filesystem::path& package) {
    try {
        pkg_ = std::make_unique<DbpfPackage>(package);
    } catch (const std::exception& err) {
        os2::logOnce("CAS thumbnails", package.string(), err);
        pkg_.reset();
        return;
    }
    const auto& entries = pkg_->entries();
    for (size_t i = 0; i < entries.size(); ++i)
        if (entries[i].tgi.type == kThumbnailType)
            byInstance_.emplace(entries[i].tgi.instance, i);
}

CasThumbnails::~CasThumbnails() = default;

size_t CasThumbnails::cachedBytes() const {
    size_t n = 0;
    for (const auto& [instance, img] : cache_)
        if (img)
            n += img->pixels.size();
    return n;
}

const RgbaImage* CasThumbnails::image(uint32_t instance) {
    if (auto it = cache_.find(instance); it != cache_.end())
        return it->second.get();
    std::unique_ptr<RgbaImage> img;
    if (auto it = byInstance_.find(instance); pkg_ && it != byInstance_.end())
        if (auto d = decodeUiImage(pkg_->read(pkg_->entries()[it->second])))
            img = std::make_unique<RgbaImage>(std::move(*d));
    return cache_.emplace(instance, std::move(img)).first->second.get();
}

} // namespace os2::cas
