#pragma once

#include "image.hpp"
#include "resindex.hpp"
#include "strings.hpp"
#include "ui_script.hpp"

#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace os2 {

// What a UI script points at: scripts and images through the resource index
// over TSData/Res, strings by the group of their package. The STR# tables of
// Text/*.package are stored in the local group 0xFFFFFFFF, which the game
// replaces by packageGroupHash of the file name, so captionres={7fa7dc7c,...}
// means Text/UIText.package.
class UiResources {
public:
    // `index` must cover `resRoot` (normally TSData/Res).
    UiResources(ResourceIndex& index, const std::filesystem::path& resRoot);

    ResourceIndex& index() { return index_; }

    // UI script by instance (group 0xA99D8A11). Locale overrides come first
    // in the index (Locale/Brazilian Portuguese sorts before UI/).
    std::optional<UiScript> script(uint32_t instance, uint32_t group = kUiGroup);

    // Decoded image (cached; nullopt if missing or undecodable).
    const RgbaImage* image(const UiResourceRef& ref);

    // Localized string, or nullopt if the table or entry is missing.
    std::optional<std::string> string(const UiStringRef& ref, uint8_t language = lang::BrazilianPortuguese);
    const StringTable* table(uint32_t group, uint32_t instance);

private:
    ResourceIndex& index_;
    std::multimap<uint32_t, std::filesystem::path> packagesByGroup_;
    std::map<std::pair<uint32_t, uint32_t>, std::unique_ptr<StringTable>> tables_; // null if missing
    std::map<std::tuple<uint32_t, uint32_t, uint32_t>, std::unique_ptr<RgbaImage>> images_;
};

} // namespace os2
