#include "property_set.hpp"

#include "binreader.hpp"

#include <cstdio>

namespace os2 {

namespace {

std::string decodeEntities(std::string_view s) {
    static const std::pair<std::string_view, char> entities[] = {
        {"&quot;", '"'}, {"&apos;", '\''}, {"&lt;", '<'}, {"&gt;", '>'}, {"&amp;", '&'}};
    std::string out;
    for (size_t i = 0; i < s.size();) {
        bool replaced = false;
        if (s[i] == '&')
            for (const auto& [name, c] : entities)
                if (s.substr(i, name.size()) == name) {
                    out += c;
                    i += name.size();
                    replaced = true;
                    break;
                }
        if (!replaced)
            out += s[i++];
    }
    return out;
}

} // namespace

std::map<std::string, std::string> parsePropertySet(std::string_view xml) {
    std::map<std::string, std::string> out;
    constexpr std::string_view keyAttr = "key=\"";
    for (size_t pos = 0; (pos = xml.find('<', pos)) != std::string_view::npos;) {
        const size_t tagEnd = xml.find('>', pos);
        if (tagEnd == std::string_view::npos)
            break;
        const std::string_view tag = xml.substr(pos + 1, tagEnd - pos - 1);
        pos = tagEnd + 1;
        const size_t k = tag.find(keyAttr);
        if (k == std::string_view::npos || tag.ends_with('/'))
            continue;
        const size_t kEnd = tag.find('"', k + keyAttr.size());
        if (kEnd == std::string_view::npos)
            continue;
        const std::string key = decodeEntities(tag.substr(k + keyAttr.size(), kEnd - k - keyAttr.size()));
        const size_t close = xml.find("</", pos);
        if (close == std::string_view::npos)
            break;
        out[key] = decodeEntities(xml.substr(pos, close - pos));
        pos = close;
    }
    return out;
}

std::map<std::string, std::string> readPropertySet(std::span<const uint8_t> data) {
    BinReader r(data);
    if (data.size() < 4 || le32(data.data()) != 0xCBE750E0)
        return parsePropertySet(std::string_view(reinterpret_cast<const char*>(data.data()), data.size()));
    r.u32();
    r.u16(); // version
    const uint32_t n = r.u32();
    if (n > 0x10000)
        throw FormatError("property set: bad item count");
    std::map<std::string, std::string> out;
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t type = r.u32();
        const uint32_t len = r.u32();
        auto nameBytes = r.bytes(len);
        std::string name(reinterpret_cast<const char*>(nameBytes.data()), len);
        std::string value;
        switch (type) {
        case 0xEB61E4F7: value = std::to_string(r.u32()); break;
        case 0x0C264712: value = std::to_string(r.i32()); break;
        case 0xABC78708: {
            char buf[32];
            std::snprintf(buf, sizeof buf, "%g", r.f32());
            value = buf;
            break;
        }
        case 0xCBA908E1: value = r.u8() ? "true" : "false"; break;
        case 0x0B8BEA18: {
            auto b = r.bytes(r.u32());
            value.assign(reinterpret_cast<const char*>(b.data()), b.size());
            break;
        }
        default: throw FormatError("property set: unknown value type " + hexString(type) + " for " + name);
        }
        out[name] = std::move(value);
    }
    return out;
}

} // namespace os2
