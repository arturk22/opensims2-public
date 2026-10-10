#include "property_set.hpp"

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

} // namespace os2
