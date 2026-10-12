#include "app/loading_screen.hpp"

#include "formats/dbpf.hpp"
#include "formats/font.hpp"
#include "formats/strings.hpp"
#include "formats/locale.hpp"
#include "formats/reia.hpp"
#include "formats/ui_image.hpp"
#include "formats/ui_script.hpp"

#include <mutex>

namespace os2 {

namespace {

constexpr uint32_t kTypeFamilyThumb = 0x8C3CE95A, kTypeStr = 0x53545223;

std::mutex gOfferMutex;
bool gWanted = false;
std::optional<RgbaImage> gOffered;

// String `number` (1-based, as the game's T(instance, number)) of the UIText STR# `instance`.
std::string uiText(const std::filesystem::path& resDir, uint32_t instance, uint32_t number, const char* fallback) {
    try {
        DbpfPackage pkg(resDir / "Text/UIText.package");
        if (const DbpfEntry* e = pkg.find(Tgi{kTypeStr, 0xFFFFFFFF, instance, 0}))
            if (std::string t = StringTable::parse(pkg.read(*e)).get(number - 1); !t.empty())
                return t;
    } catch (const std::exception&) {
    }
    return fallback;
}

// The title as a transparent picture: white text with a soft shadow.
std::optional<RgbaImage> renderTitle(const std::filesystem::path& resDir, const std::string& text) {
    if (text.empty())
        return std::nullopt;
    try {
        FontLibrary fonts(resDir / "UI/Fonts");
        const FontStyle* style = fonts.style("GenBodyMedium");
        if (!style)
            style = fonts.style("CASText");
        const Font* font = style ? fonts.font(*style) : nullptr;
        if (!font)
            return std::nullopt;
        const float px = 30;
        const std::u32string u = decodeUtf8(text);
        const TextMetrics m = measureText(*font, u, px);
        RgbaImage img;
        img.width = std::min(800u, uint32_t(m.width) + 12), img.height = uint32_t(m.ascent + m.descent) + 12;
        img.pixels.assign(size_t(img.width) * img.height * 4, 0);
        const float x = (float(img.width) - m.width) / 2, base = 6 + m.ascent;
        const uint8_t shadow[4] = {0, 20, 40, 170}, white[4] = {255, 255, 255, 255};
        drawText(img, *font, u, x + 2, base + 2, px, shadow);
        drawText(img, *font, u, x, base, px, white);
        return img;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::optional<RgbaImage> readFamilyPhoto(const LotEntry& lot) {
    if (lot.familyThumbs.empty() || !lot.familyId)
        return std::nullopt;
    std::error_code ec;
    if (!std::filesystem::is_regular_file(lot.familyThumbs, ec))
        return std::nullopt;
    try {
        DbpfPackage pkg(lot.familyThumbs);
        if (const DbpfEntry* e = pkg.find(Tgi{kTypeFamilyThumb, kReiaGroup, lot.familyId, 0}))
            return decodeUiImage(pkg.read(*e));
        for (const auto& e : pkg.entries())
            if (e.tgi.type == kTypeFamilyThumb && e.tgi.instance == lot.familyId)
                return decodeUiImage(pkg.read(e));
    } catch (const std::exception&) {
    }
    return std::nullopt;
}

} // namespace

bool loadingIconWanted() {
    std::lock_guard<std::mutex> lock(gOfferMutex);
    return gWanted;
}

void offerLoadingIcon(RgbaImage image) {
    std::lock_guard<std::mutex> lock(gOfferMutex);
    if (gWanted)
        gOffered = std::move(image);
}

std::optional<RgbaImage> readUiIcon(const std::filesystem::path& resDir, uint32_t instance) {
    std::error_code ec;
    for (const auto& p : {resDir / "Locale" / defaultLocale() / "UI/ui.package", resDir / "UI/ui.package"}) {
        if (!std::filesystem::is_regular_file(p, ec))
            continue;
        try {
            DbpfPackage pkg(p);
            if (const DbpfEntry* e = pkg.find(Tgi{kTypeUiImage, kReiaGroup, instance, 0}))
                if (auto img = decodeUiImage(pkg.read(*e)))
                    return img;
        } catch (const std::exception&) {
        }
    }
    return std::nullopt;
}

LoadingScreen loadingScreenFor(const std::filesystem::path& resDir, const Scene& scene, bool cas, const LotEntry& lot) {
    LoadingScreen s;
    uint32_t icon = 0;
    std::string title;
    bool community = false;
    std::optional<RgbaImage> photo;
    {
        std::lock_guard<std::mutex> lock(gOfferMutex);
        gWanted = false, gOffered.reset();
    }
    if (cas) {
        s.reia = 0x8DA3ADEC, icon = 0x8DA3ADF0; // case 1
        title = uiText(resDir, 7, 3, "Create A Family");
    } else if (scene.kind == Scene::Kind::MainMenu) {
        s.reia = kReiaGameLoad; // full screen, no icon
    } else if (scene.kind == Scene::Kind::Neighborhood) {
        s.reia = 0x8DA3ADEB, icon = 0x8DA3ADF3; // case 3, the base game's neighborhood
    } else if (lot.known && lot.family) {
        s.reia = 0x8DA3ADE8; // case 0 with a family: its photo
        photo = readFamilyPhoto(lot);
        if (!photo)
            icon = 0x8DA3ADF0;
        char b[160];
        std::snprintf(b, sizeof b, uiText(resDir, 7, 4, "The %s Family").c_str(), lot.familyName.c_str());
        title = b;
    } else if (lot.known && lot.type == 1) {
        s.reia = 0x8DA3ADEA, icon = 0x8DA3ADF1; // community lot: the taxi until an object's image is found
        community = true;
        title = lot.name;
    } else if (lot.known) {
        s.reia = 0x8DA3ADE9, icon = 0x8DA3ADF2; // empty residential lot
        title = lot.name;
    } else {
        s.reia = 0x8DA3ADE8, icon = 0x8DA3ADF0;
    }
    if (icon)
        s.icon = readUiIcon(resDir, icon);
    else if (photo)
        s.icon = std::move(photo);
    s.iconX = kLotIntroTileX, s.iconY = kLotIntroTileY;
    s.iconW = kLotIntroIconW, s.iconH = kLotIntroIconH;
    if (!title.empty()) {
        s.title = renderTitle(resDir, title);
        s.titleX = kLotIntroTitleX, s.titleY = kLotIntroTitleY;
    }
    if (community) {
        {
            std::lock_guard<std::mutex> lock(gOfferMutex);
            gWanted = true;
        }
        s.lateIcon = [] {
            std::lock_guard<std::mutex> lock(gOfferMutex);
            std::optional<RgbaImage> img;
            img.swap(gOffered);
            return img;
        };
    }
    return s;
}

} // namespace os2
