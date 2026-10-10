#pragma once

#include "rcol.hpp"

#include <span>
#include <string>
#include <utility>
#include <vector>

namespace os2 {

// cMaterialDefinition (TXMT): a shader type plus string properties, e.g.
// stdMatBaseTextureName -> "foo_bar" (resolves to TXTR "foo_bar_txtr").
struct MaterialDefinition {
    uint32_t version = 0;
    SgResource resource;
    std::string description;
    std::string materialType; // "StandardMaterial", "SimStandardMaterial", ...
    std::vector<std::pair<std::string, std::string>> properties;
    std::vector<std::string> textureNames; // version > 8

    static MaterialDefinition read(BinReader& r);
    static MaterialDefinition parse(std::span<const uint8_t> body);

    // Property value, or empty when absent.
    std::string property(std::string_view name) const;
};

} // namespace os2
