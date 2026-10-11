#include "app/loading_screen.hpp"

#include "formats/dbpf.hpp"
#include "formats/locale.hpp"
#include "formats/reia.hpp"
#include "formats/ui_image.hpp"
#include "formats/ui_script.hpp"

namespace os2 {

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
    if (cas) {
        s.reia = 0x8DA3ADEC, icon = 0x8DA3ADF0;
    } else if (scene.kind == Scene::Kind::MainMenu) {
        s.reia = kReiaGameLoad; // full screen, no icon
    } else if (scene.kind == Scene::Kind::Neighborhood) {
        s.reia = 0x8DA3ADED, icon = 0x8DA3ADF3;
    } else if (lot.known && lot.type == 1) {
        s.reia = 0x8DA3ADEA, icon = 0x8DA3ADF1;
    } else if (lot.known && !lot.family) {
        s.reia = 0x8DA3ADE9, icon = 0x8DA3ADF2;
    } else {
        s.reia = 0x8DA3ADE8, icon = 0x8DA3ADF0;
    }
    if (icon) {
        s.icon = readUiIcon(resDir, icon);
        s.iconX = kLotIntroTileX, s.iconY = kLotIntroTileY;
        s.iconW = kLotIntroIconW, s.iconH = kLotIntroIconH;
    }
    return s;
}

} // namespace os2
