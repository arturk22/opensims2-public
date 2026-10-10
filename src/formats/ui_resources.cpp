#include "ui_resources.hpp"

#include "ui_image.hpp"

#include <cctype>

namespace os2 {

namespace {
constexpr uint32_t kTypeStr = 0x53545223;
}

UiResources::UiResources(ResourceIndex& index, const std::filesystem::path& resRoot, const std::string& locale)
    : index_(index) {
    for (const auto& p : {resRoot / "Locale" / locale / "UI" / "ui.package", resRoot / "UI" / "ui.package"})
        if (std::filesystem::exists(p))
            uiPackages_.push_back(std::make_unique<DbpfPackage>(p));
    for (const auto& de : std::filesystem::recursive_directory_iterator(resRoot)) {
        if (!de.is_regular_file())
            continue;
        std::string ext = de.path().extension().string();
        for (auto& c : ext)
            c = char(std::tolower(static_cast<unsigned char>(c)));
        if (ext == ".package")
            packagesByGroup_.emplace(packageGroupHash(de.path().stem().string()), de.path());
    }
}

std::optional<std::vector<uint8_t>> UiResources::read(const Tgi& tgi) {
    for (auto& p : uiPackages_)
        if (const auto* e = p->find(tgi))
            return p->read(*e);
    return index_.read(tgi);
}

std::optional<UiScript> UiResources::script(uint32_t instance, uint32_t group) {
    auto bytes = read(Tgi{kTypeUiScript, group, instance, 0});
    if (!bytes || isReia(*bytes))
        return std::nullopt;
    return UiScript::parse(*bytes);
}

const RgbaImage* UiResources::image(const UiResourceRef& ref) {
    const auto key = std::make_tuple(ref.type, ref.group, ref.instance);
    auto it = images_.find(key);
    if (it == images_.end()) {
        std::unique_ptr<RgbaImage> img;
        if (auto bytes = read(Tgi{ref.type, ref.group, ref.instance, 0}))
            if (auto decoded = decodeUiImage(*bytes))
                img = std::make_unique<RgbaImage>(std::move(*decoded));
        it = images_.emplace(key, std::move(img)).first;
    }
    return it->second.get();
}

const StringTable* UiResources::table(uint32_t group, uint32_t instance) {
    const auto key = std::make_pair(group, instance);
    auto it = tables_.find(key);
    if (it == tables_.end()) {
        std::unique_ptr<StringTable> table;
        // Resources already in their final group (rare) come straight from the index.
        auto bytes = index_.read(Tgi{kTypeStr, group, instance, 0});
        auto [lo, hi] = packagesByGroup_.equal_range(group);
        for (auto p = lo; !bytes && p != hi; ++p) {
            DbpfPackage pkg(p->second);
            if (const auto* e = pkg.find(Tgi{kTypeStr, 0xFFFFFFFF, instance, 0}))
                bytes = pkg.read(*e);
        }
        if (bytes)
            table = std::make_unique<StringTable>(StringTable::parse(*bytes));
        it = tables_.emplace(key, std::move(table)).first;
    }
    return it->second.get();
}

std::optional<std::string> UiResources::string(const UiStringRef& ref, uint8_t language) {
    const StringTable* t = table(ref.group, ref.instance);
    if (!t || ref.index >= t->entries.size())
        return std::nullopt;
    return t->get(ref.index, language);
}

} // namespace os2
