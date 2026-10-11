#include "app/ui_panel_util.hpp"

#include "formats/resindex.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
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
    // The game formats the group with "%d": signed (catalog.hpp's
    // thumbnailKey writes it unsigned; the same for every shipped group).
    return nameInstanceHiHash(std::to_string(int32_t(group)) + modelName);
}

std::string uiButtonSound(const UiElement& button, std::string_view attr) {
    static const char* const kNames[] = {"ui_album_close", "ui_album_open", "ui_aspiration_powerdown", "ui_aspiration_powerup", "ui_build_deck_mdown", "ui_build_deck_mup", "ui_build_deck_section", "ui_build_door_green", "ui_build_door_plop", "ui_build_flooring_plop", "ui_build_garden_plop", "ui_build_pool_mdown", "ui_build_pool_mup", "ui_build_rail_plop", "ui_build_roof_mdown", "ui_build_roof_mup", "ui_build_stair_green", "ui_build_stair_plop", "ui_build_terrain_level_mdown", "ui_build_terrain_level_mup", "ui_build_terrain_lp", "ui_build_terrain_paint_lp", "ui_build_wall_mdown", "ui_build_wall_mup", "ui_build_wall_section", "ui_build_wallcovering_appear", "ui_build_wallcovering_plop", "ui_build_walldelete_mdown", "ui_build_walldelete_section", "ui_build_water_lp", "ui_button_ffw", "ui_button_fw", "ui_button_pause", "ui_button_play", "ui_button_rotate_lp", "ui_button_zoom_in", "ui_button_zoom_out", "ui_camera_photo", "ui_cas_camera", "ui_cas_personalityeight", "ui_cas_personalityfive", "ui_cas_personalityfour", "ui_cas_personalitynine", "ui_cas_personalityone", "ui_cas_personalityseven", "ui_cas_personalitysix", "ui_cas_personalityten", "ui_cas_personalitythree", "ui_cas_personalitytwo", "ui_cas_randomize_mup", "ui_error", "ui_flatten_lot", "ui_hardwindow_close", "ui_hardwindow_open", "ui_object_buy", "ui_object_pickup", "ui_object_plop", "ui_object_rotate", "ui_object_sell", "ui_pane_primary", "ui_panel_close", "ui_panel_collapse", "ui_panel_expand", "ui_panel_open", "ui_panel_shift", "ui_piemenu_primary", "ui_piemenu_secondary", "ui_primary_button", "ui_queue", "ui_queue_delete", "ui_redo", "ui_secondary_button", "ui_softwindow_close", "ui_softwindow_open", "ui_tertiary_button", "ui_text_description_close", "ui_text_description_open", "ui_text_notification_close", "ui_text_notification_open", "ui_undo", "ui_wf_fear_glow", "ui_wf_want_glow", };
    const std::string* v = button.attr(attr);
    if (!v)
        return {};
    const auto comma = v->find(',');
    if (comma == std::string::npos)
        return {};
    const uint32_t key = uint32_t(std::stoul(v->substr(comma + 1), nullptr, 16));
    if (key == 0)
        return {};
    for (const char* n : kNames)
        if (nameInstanceHash(n) == key)
            return n;
    return {};
}

RgbaImage gradientFill(const RgbaImage& gradient, const RgbaImage& mask, float value, float min, float max,
                       bool vertical) {
    RgbaImage out;
    out.width = mask.width, out.height = mask.height;
    out.pixels.assign(size_t(out.width) * out.height * 4, 0);
    const uint32_t n = vertical ? gradient.width : gradient.height;
    if (n == 0 || max <= min)
        return out;
    const float t = std::clamp((value - min) / (max - min), 0.0f, 1.0f);
    const uint32_t i = std::min(n - 1, uint32_t(std::lround(float(n) * t)));
    for (uint32_t y = 0; y < out.height; ++y)
        for (uint32_t x = 0; x < out.width; ++x) {
            const uint32_t gx = vertical ? i : std::min(x, gradient.width - 1);
            const uint32_t gy = vertical ? std::min(y, gradient.height - 1) : i;
            const uint8_t* g = &gradient.pixels[(size_t(gy) * gradient.width + gx) * 4];
            uint8_t* o = &out.pixels[(size_t(y) * out.width + x) * 4];
            o[0] = g[0], o[1] = g[1], o[2] = g[2];
            o[3] = uint8_t(unsigned(g[3]) * mask.pixels[(size_t(y) * mask.width + x) * 4 + 3] / 255);
        }
    return out;
}

RgbaImage segmentFill(const RgbaImage& full, float value, float max, int divisions) {
    RgbaImage out = full;
    if (divisions <= 0 || max <= 0)
        return out;
    const int n = std::clamp(int(std::floor(float(divisions) * value / max)), 0, divisions);
    const uint32_t keep = uint32_t(std::lround(float(full.width) * float(n) / float(divisions)));
    for (uint32_t y = 0; y < out.height; ++y)
        for (uint32_t x = keep; x < out.width; ++x)
            out.pixels[(size_t(y) * out.width + x) * 4 + 3] = 0;
    return out;
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
