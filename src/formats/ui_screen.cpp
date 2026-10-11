#include "ui_screen.hpp"

#include "font.hpp"
#include "ui_resources.hpp"

#include <algorithm>
#include <cmath>

namespace os2 {

namespace {

std::array<float, 4> rgba(const UiElement& e, std::string_view key, std::array<float, 4> fallback) {
    if (auto c = uiColor(e.get(key)))
        return {(*c)[0] / 255.0f, (*c)[1] / 255.0f, (*c)[2] / 255.0f, 1};
    return fallback;
}

struct Layout {
    UiResources& res;
    FontLibrary& fonts;
    const UiLayoutOptions& opt;
    UiScreen out;
    const UiDrawHooks* hooks = nullptr;
    std::array<float, 4> tint{1, 1, 1, 1};

    void quad(const RgbaImage* img, float x0, float y0, float x1, float y1, float sx0, float sy0, float sx1, float sy1,
              std::array<float, 4> color = {1, 1, 1, 1}) {
        if (x1 <= x0 || y1 <= y0)
            return;
        UiQuad q;
        q.image = img;
        q.x0 = x0, q.y0 = y0, q.x1 = x1, q.y1 = y1;
        if (img) {
            q.u0 = sx0 / float(img->width);
            q.v0 = sy0 / float(img->height);
            q.u1 = sx1 / float(img->width);
            q.v1 = sy1 / float(img->height);
        }
        for (size_t k = 0; k < 4; ++k)
            q.color[k] = color[k] * tint[k];
        out.quads.push_back(q);
    }
    void flat(float x0, float y0, float x1, float y1, std::array<float, 4> color) {
        quad(nullptr, x0, y0, x1, y1, 0, 0, 0, 0, color);
    }

    // Source rectangle (sx0, sy0, sx1, sy1) stretched over the window.
    void stretched(const RgbaImage* img, float x, float y, float w, float h, std::array<float, 4> src) {
        quad(img, x, y, x + w, y + h, src[0], src[1], src[2], src[3]);
    }

    void tiled(const RgbaImage* img, float x, float y, float w, float h, std::array<float, 4> src) {
        const float tw = src[2] - src[0], th = src[3] - src[1];
        if (tw <= 0 || th <= 0 || (w / tw) * (h / th) > 4096)
            return stretched(img, x, y, w, h, src);
        for (float ty = 0; ty < h; ty += th)
            for (float tx = 0; tx < w; tx += tw) {
                const float cw = std::min(tw, w - tx), ch = std::min(th, h - ty);
                quad(img, x + tx, y + ty, x + tx + cw, y + ty + ch, src[0], src[1], src[0] + cw, src[1] + ch);
            }
    }

    // Nine-slice: the source cut in thirds; corners keep their size, the rest stretches.
    void edge(const RgbaImage* img, float x, float y, float w, float h, std::array<float, 4> src) {
        const float sw = src[2] - src[0], sh = src[3] - src[1];
        const float cw = std::min(std::floor(sw / 3), std::floor(w / 2)), ch = std::min(std::floor(sh / 3), std::floor(h / 2));
        const float sxs[4] = {src[0], src[0] + cw, src[2] - cw, src[2]};
        const float sys[4] = {src[1], src[1] + ch, src[3] - ch, src[3]};
        const float dxs[4] = {x, x + cw, x + w - cw, x + w};
        const float dys[4] = {y, y + ch, y + h - ch, y + h};
        for (int j = 0; j < 3; ++j)
            for (int i = 0; i < 3; ++i)
                quad(img, dxs[i], dys[j], dxs[i + 1], dys[j + 1], sxs[i], sys[j], sxs[i + 1], sys[j + 1]);
    }

    const RgbaImage* image(const UiElement& e, std::string_view key) {
        if (hooks)
            if (auto it = hooks->images.find(&e); it != hooks->images.end())
                return it->second;
        const auto* v = e.attr(key);
        if (!v)
            return nullptr;
        auto ref = uiResourceRef(*v);
        if (!ref || (ref->group == 0 && ref->instance == 0))
            return nullptr;
        const RgbaImage* img = res.image(*ref);
        if (!img)
            ++out.missingImages;
        return img;
    }

    std::string caption(const UiElement& e) {
        if (hooks)
            if (auto it = hooks->captions.find(&e); it != hooks->captions.end())
                return it->second;
        if (const auto* r = e.attr("captionres"))
            if (auto ref = uiStringRef(*r))
                if (auto s = res.string(*ref))
                    return *s;
        return e.get("caption");
    }

    const Font* font(const UiElement& e, float& ppem) {
        const FontStyle* st = fonts.style(e.get("font", "Default"));
        if (!st)
            st = fonts.style("Default");
        if (!st)
            return nullptr;
        ppem = float(st->size);
        return fonts.font(*st);
    }

    void text(const UiElement& e, float x, float y, float w, float h, const std::string& str,
              std::array<float, 4> color, bool wrapped) {
        if (str.empty() || w < 1 || h < 1)
            return;
        float ppem = 11;
        const Font* f = font(e, ppem);
        if (!f) {
            out.warnings.push_back("no font for window " + e.get("caption"));
            return;
        }
        const auto g = uiInts(e.get("gutters", "(0,0)"));
        const float gl = g.size() >= 2 ? float(g[0]) : 0, gt = g.size() >= 2 ? float(g[1]) : 0;
        const float gr = g.size() == 4 ? float(g[2]) : gl, gb = g.size() == 4 ? float(g[3]) : gt;
        const float avail = w - gl - gr;

        // Lines: explicit breaks, then greedy word wrap.
        std::vector<std::u32string> lines;
        const std::u32string all = decodeUtf8(str);
        std::u32string para;
        auto flush = [&](const std::u32string& p) {
            if (!wrapped) {
                lines.push_back(p);
                return;
            }
            std::u32string line;
            size_t i = 0;
            while (i <= p.size()) {
                size_t j = p.find(U' ', i);
                if (j == std::u32string::npos)
                    j = p.size();
                const std::u32string word = p.substr(i, j - i);
                std::u32string candidate = line.empty() ? word : line + U' ' + word;
                if (!line.empty() && measureText(*f, candidate, ppem).width > avail) {
                    lines.push_back(line);
                    line = word;
                } else {
                    line = candidate;
                }
                i = j + 1;
            }
            lines.push_back(line);
        };
        for (char32_t c : all) {
            if (c == U'\r')
                continue;
            if (c == U'\n') {
                flush(para);
                para.clear();
            } else {
                para += c;
            }
        }
        flush(para);

        const TextMetrics m = measureText(*f, U"", ppem);
        const float lineH = std::round(m.ascent + m.descent);
        const float blockH = lineH * float(lines.size());
        const std::string align = e.get("align", "lefttop");
        const bool left = align.find("left") != std::string::npos, right = align.find("right") != std::string::npos;
        const bool top = align.find("top") != std::string::npos, bottom = align.find("bottom") != std::string::npos;
        float ty = top ? gt : bottom ? h - gb - blockH : std::round((h - blockH) / 2);

        auto img = std::make_unique<RgbaImage>();
        img->width = uint32_t(std::ceil(w));
        img->height = uint32_t(std::ceil(h));
        img->pixels.assign(size_t(img->width) * img->height * 4, 0);
        // Transparent pixels carry the text colour so filtering at the edges does not darken it.
        for (size_t i = 0; i < img->pixels.size(); i += 4)
            for (int k = 0; k < 3; ++k)
                img->pixels[i + size_t(k)] = uint8_t(std::lround(color[size_t(k)] * 255));
        const uint8_t c[4] = {uint8_t(std::lround(color[0] * 255)), uint8_t(std::lround(color[1] * 255)),
                              uint8_t(std::lround(color[2] * 255)), 255};
        for (const auto& line : lines) {
            const float lw = measureText(*f, line, ppem).width;
            const float tx = left ? gl : right ? w - gr - lw : std::round((w - lw) / 2);
            drawText(*img, *f, line, tx, ty + std::round(m.ascent), ppem, c);
            ty += lineH;
        }
        const RgbaImage* p = img.get();
        out.textImages.push_back(std::move(img));
        quad(p, x, y, x + w, y + h, 0, 0, float(p->width), float(p->height));
        ++out.texts;
    }

    void window(const UiElement& e, float px, float py, float pw, float ph, bool root) {
        ++out.windows;
        const uint32_t id = e.attr("id") ? e.id() : 0;
        const bool forced = id && opt.show.count(id);
        bool visible = e.visible() || forced;
        if (hooks) {
            if (hooks->hide.count(&e))
                visible = false;
            else if (hooks->show.count(&e))
                visible = true;
        }
        if (!visible)
            return;
        const auto a = e.area();
        float x = px + float(a[0]), y = py + float(a[1]);
        const float w = float(a[2] - a[0]), h = float(a[3] - a[1]);
        if (root || (forced && (w > pw || h > ph))) {
            x = std::round((float(opt.canvasWidth) - w) / 2);
            y = std::round((float(opt.canvasHeight) - h) / 2);
        }
        std::string cls = e.clsid();
        if (!cls.empty() && cls[0] == '0') { // known only by GUID: draw as its interface
            const std::string iid = e.get("iid");
            if (iid == "IGZWinBtn")
                cls = "GZWinBtn";
            else if (iid == "IGZWinBMP")
                cls = "GZWinBMP";
        }
        const auto savedTint = tint;
        if (hooks) {
            if (hooks->rects)
                (*hooks->rects)[&e] = {x, y, x + w, y + h};
            if (auto it = hooks->colors.find(&e); it != hooks->colors.end())
                for (size_t k = 0; k < 4; ++k)
                    tint[k] *= it->second[k];
        }
        bool drew = false;
        auto src = [&](const RgbaImage* img) {
            std::array<float, 4> s{0, 0, float(img->width), float(img->height)};
            const auto r = uiInts(e.get("imagerect"));
            if (r.size() == 4 && r[2] > r[0] && r[3] > r[1])
                s = {float(r[0]), float(r[1]), float(r[2]), float(r[3])};
            return s;
        };
        if (cls == "GZWinBMP" || cls == "GZWinGen") {
            if (const RgbaImage* img = image(e, "image")) {
                const std::string blt = e.get("blttype", "normal");
                if (e.flag("edgeimage") || blt == "edge")
                    edge(img, x, y, w, h, src(img));
                else if (cls == "GZWinGen" && blt == "tiled")
                    tiled(img, x, y, w, h, src(img));
                else
                    stretched(img, x, y, w, h, src(img));
                drew = true;
            } else if (cls == "GZWinGen" && e.flag("fill")) {
                flat(x, y, x + w, y + h, rgba(e, "fillcolor", {0, 0, 0, 1}));
                drew = true;
            }
        } else if (cls == "0x4d9ccdb1") { // animation strip: first frame
            if (const RgbaImage* img = image(e, "image")) {
                const float fw = std::min(w, float(img->width));
                stretched(img, x, y, w, h, {0, 0, fw, float(img->height)});
                drew = true;
            }
        } else if (cls == "GZWinBtn") {
            if (const RgbaImage* img = image(e, "image")) {
                const float sw = float(img->width) / 4; // disabled, normal, pressed, hover
                float frame = 1;
                if (hooks)
                    if (auto it = hooks->buttonState.find(&e); it != hooks->buttonState.end())
                        frame = float(std::clamp(it->second, 0, 3));
                const std::array<float, 4> s{frame * sw, 0, (frame + 1) * sw, float(img->height)};
                if (e.flag("edgeimage"))
                    edge(img, x, y, w, h, s);
                else
                    stretched(img, x, y, w, h, s);
                drew = true;
            }
            if (e.flag("showcaption")) {
                text(e, x, y, w, h, caption(e), rgba(e, "colorfontnormal", {0, 0, 0, 1}), e.flag("wrapcaption"));
                drew = true;
            }
        } else if (cls == "GZWinFlatRect") {
            if (e.get("style") != "nofill") {
                flat(x, y, x + w, y + h, rgba(e, "fillcolor", {0, 0, 0, 1}));
                drew = true;
            }
            if (e.flag("outline")) {
                flat(x, y, x + 1, y + h, rgba(e, "colorleft", {0, 0, 0, 1}));
                flat(x, y, x + w, y + 1, rgba(e, "colortop", {0, 0, 0, 1}));
                flat(x + w - 1, y, x + w, y + h, rgba(e, "colorright", {0, 0, 0, 1}));
                flat(x, y + h - 1, x + w, y + h, rgba(e, "colorbottom", {0, 0, 0, 1}));
                drew = true;
            }
        } else if (cls == "GZWinText") {
            if (e.flag("opaque"))
                flat(x, y, x + w, y + h, rgba(e, "bkgcolor", {0, 0, 0, 1}));
            text(e, x, y, w, h, caption(e), rgba(e, "forecolor", {0, 0, 0, 1}), e.flag("wrapped"));
            drew = true;
        } else if (cls == "GZWinTextEdit") {
            // The text typed so far: a hook's caption (code-driven screens), else the initial value.
            auto it = hooks ? hooks->captions.find(&e) : decltype(hooks->captions)::const_iterator{};
            const bool hooked = hooks && it != hooks->captions.end();
            text(e, x, y, w, h, hooked ? it->second : e.get("initvalue"), rgba(e, "caretcolor", {0, 0, 0, 1}), true);
            drew = true;
        }
        out.drawn += drew;
        if (hooks)
            if (auto it = hooks->after.find(&e); it != hooks->after.end())
                it->second(out, x, y, w, h);
        for (const auto& c : e.children)
            window(c, x, y, w, h, false);
        tint = savedTint;
    }
};

} // namespace

UiScreen layoutUi(const UiScript& script, UiResources& res, FontLibrary& fonts, const UiLayoutOptions& opt) {
    Layout l{res, fonts, opt, {}};
    l.out.width = opt.canvasWidth;
    l.out.height = opt.canvasHeight;
    for (const auto& r : script.roots)
        l.window(r, 0, 0, float(opt.canvasWidth), float(opt.canvasHeight), true);
    return std::move(l.out);
}

void layoutUiElement(const UiElement& e, float parentX, float parentY, UiResources& res, FontLibrary& fonts,
                     UiScreen& out, const UiDrawHooks& hooks) {
    const UiLayoutOptions opt{out.width, out.height, {}};
    Layout l{res, fonts, opt, std::move(out)};
    l.hooks = &hooks;
    l.window(e, parentX, parentY, float(opt.canvasWidth), float(opt.canvasHeight), false);
    out = std::move(l.out);
}

RgbaImage composeUi(const UiScreen& screen, std::array<uint8_t, 4> clear) {
    RgbaImage img;
    img.width = uint32_t(screen.width);
    img.height = uint32_t(screen.height);
    img.pixels.resize(size_t(img.width) * img.height * 4);
    for (size_t i = 0; i < img.pixels.size(); i += 4)
        std::copy(clear.begin(), clear.end(), img.pixels.begin() + long(i));
    for (const auto& q : screen.quads) {
        const int x0 = std::max(0, int(std::floor(q.x0))), x1 = std::min(int(img.width), int(std::ceil(q.x1)));
        const int y0 = std::max(0, int(std::floor(q.y0))), y1 = std::min(int(img.height), int(std::ceil(q.y1)));
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x) {
                float s[4] = {1, 1, 1, 1};
                if (q.image) {
                    // Nearest texel at the pixel centre.
                    const float u = q.u0 + (float(x) + 0.5f - q.x0) / (q.x1 - q.x0) * (q.u1 - q.u0);
                    const float v = q.v0 + (float(y) + 0.5f - q.y0) / (q.y1 - q.y0) * (q.v1 - q.v0);
                    const int tx = std::clamp(int(u * float(q.image->width)), 0, int(q.image->width) - 1);
                    const int ty = std::clamp(int(v * float(q.image->height)), 0, int(q.image->height) - 1);
                    const uint8_t* t = &q.image->pixels[(size_t(ty) * q.image->width + size_t(tx)) * 4];
                    for (int k = 0; k < 4; ++k)
                        s[k] = float(t[k]) / 255.0f;
                }
                const float a = s[3] * q.color[3];
                uint8_t* o = &img.pixels[(size_t(y) * img.width + size_t(x)) * 4];
                for (int k = 0; k < 3; ++k)
                    o[k] = uint8_t(std::lround(std::clamp(s[k] * q.color[size_t(k)] * a + float(o[k]) / 255.0f * (1 - a), 0.0f, 1.0f) * 255));
                o[3] = uint8_t(std::lround(std::clamp(a + float(o[3]) / 255.0f * (1 - a), 0.0f, 1.0f) * 255));
            }
    }
    return img;
}

} // namespace os2
