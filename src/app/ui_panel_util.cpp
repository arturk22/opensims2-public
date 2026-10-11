#include "app/ui_panel_util.hpp"

#include <algorithm>
#include <cctype>
#include <utility>

namespace os2 {

const UiElement* findWindow(const UiElement& e, uint32_t id) {
    if (e.attr("id") && e.id() == id)
        return &e;
    for (const auto& c : e.children)
        if (const UiElement* f = findWindow(c, id))
            return f;
    return nullptr;
}

UiElement* findWindow(UiElement& e, uint32_t id) {
    return const_cast<UiElement*>(findWindow(std::as_const(e), id));
}

std::string uiArea(int x0, int y0, int x1, int y1) {
    return "(" + std::to_string(x0) + "," + std::to_string(y0) + "," + std::to_string(x1) + "," + std::to_string(y1) +
           ")";
}

RgbaImage scaledImage(const RgbaImage& src, uint32_t w, uint32_t h) {
    RgbaImage out;
    out.width = w, out.height = h;
    out.pixels.resize(size_t(w) * h * 4);
    if (src.width == 0 || src.height == 0)
        return out;
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            const uint32_t sx = std::min(src.width - 1, uint32_t((x + 0.5f) * float(src.width) / float(w)));
            const uint32_t sy = std::min(src.height - 1, uint32_t((y + 0.5f) * float(src.height) / float(h)));
            std::copy_n(&src.pixels[(size_t(sy) * src.width + sx) * 4], 4, &out.pixels[(size_t(y) * w + x) * 4]);
        }
    return out;
}

std::string formatMoney(int64_t v) {
    std::string digits = std::to_string(v < 0 ? -v : v), out;
    for (size_t i = 0; i < digits.size(); ++i) {
        if (i && (digits.size() - i) % 3 == 0)
            out += '.';
        out += digits[i];
    }
    return std::string(v < 0 ? "-" : "") + "§" + out;
}

uint32_t objectThumbnailInstance(uint32_t group, const std::string& modelName) {
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i << 24;
            for (int k = 0; k < 8; ++k)
                c = (c & 0x80000000u) ? (c << 1) ^ 0x04C11DB7u : c << 1;
            t[i] = c;
        }
        return t;
    }();
    const std::string key = std::to_string(int32_t(group)) + modelName;
    uint32_t h = 0xFFFFFFFFu;
    for (char ch : key)
        h = (h << 8) ^ table[((h >> 24) ^ uint8_t(std::tolower(static_cast<unsigned char>(ch)))) & 0xFF];
    return h;
}

std::vector<QueueSlot> layoutActionQueue(const std::vector<bool>& nested, const QueueMetrics& m) {
    const bool anyNested = std::find(nested.begin(), nested.end(), true) != nested.end();
    const bool anyTop = std::find(nested.begin(), nested.end(), false) != nested.end();
    const bool both = anyNested && anyTop;
    std::vector<QueueSlot> out;
    float x = m.x0, y = m.y0;
    bool firstTop = true, firstSub = true;
    for (bool sub : nested) {
        QueueSlot s;
        if (!sub) {
            s = {x, m.y0, both && firstTop};
            firstTop = false;
            x += m.topWidth + m.margin;
        } else {
            y += m.subHeight + (firstSub ? m.subOffset : 0);
            s = {m.x0, y, both && !firstSub};
            firstSub = false;
        }
        out.push_back(s);
    }
    return out;
}

} // namespace os2
