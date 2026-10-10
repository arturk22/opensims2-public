#include "txmt.hpp"

namespace os2 {

MaterialDefinition MaterialDefinition::parse(std::span<const uint8_t> body) {
    BinReader r(body);
    auto m = read(r);
    r.expectEnd("cMaterialDefinition");
    return m;
}

MaterialDefinition MaterialDefinition::read(BinReader& r) {
    MaterialDefinition m;
    m.version = r.u32();
    m.resource = SgResource::read(r);
    m.description = r.str7();
    m.materialType = r.str7();
    const uint32_t n = r.u32();
    if (n > 4096)
        throw FormatError("cMaterialDefinition: bad property count");
    for (uint32_t i = 0; i < n; ++i) {
        auto name = r.str7();
        m.properties.emplace_back(std::move(name), r.str7());
    }
    if (m.version > 8) {
        const uint32_t t = r.u32();
        if (t > 4096)
            throw FormatError("cMaterialDefinition: bad texture count");
        for (uint32_t i = 0; i < t; ++i)
            m.textureNames.push_back(r.str7());
    }
    return m;
}

std::string MaterialDefinition::property(std::string_view name) const {
    for (const auto& [k, v] : properties)
        if (k == name)
            return v;
    return {};
}

} // namespace os2
