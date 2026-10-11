#include "app/main_menu.hpp"

#include "app/ui_panel_util.hpp"
#include "formats/dbpf.hpp"
#include "formats/font.hpp"
#include "formats/neighborhood.hpp"
#include "formats/resindex.hpp"
#include "formats/ui_image.hpp"
#include "formats/ui_resources.hpp"
#include "render/ui_layer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>

namespace os2 {

namespace fs = std::filesystem;

namespace {

constexpr uint32_t kMenuScript = 0x49001017, kPreviewScript = 0x49001021, kLearnScript = 0x49001022;
// Main menu windows.
constexpr uint32_t kBackground = 0x0DA36C7D, kThumbs = 0x1006, kPrev = 0x1004, kNext = 0x1005, kLearn = 0xA2,
                   kInitial = 0x8DC06B6A, kQuitInitial = 0xA5, kFromHood = 0x3000, kClose = 0xA3, kQuitFromHood = 0xA4,
                   kShade = 0x100A;
// NH Preview / Learn to play.
constexpr uint32_t kPreviewPicture = 0x2001, kPreviewDescription = 0x2002, kPreviewName = 0x2003, kDelete = 0xB1,
                   kPlay = 0xB2, kPreviewClose = 0xB3, kTutorialPlay = 0xC2, kTutorialClose = 0xC3;

bool inside(const std::array<float, 4>& r, simd_float2 p) { return insideRect(r, p.x, p.y); }

} // namespace

std::vector<MainMenuHood> readMainMenuHoods(const std::vector<fs::path>& dirs) {
    std::vector<MainMenuHood> out;
    for (const auto& dir : dirs) {
        MainMenuHood h;
        h.prefix = dir.filename().string();
        try {
            const hood::Neighborhood n = hood::Neighborhood::load(dir, false);
            h.name = n.name, h.description = n.description;
        } catch (const std::exception& e) {
            std::fprintf(stderr, "opensims2: main menu: %s: %s\n", h.prefix.c_str(), e.what());
        }
        if (h.name.empty())
            h.name = h.prefix;
        std::ifstream in(dir / (h.prefix + "_Neighborhood.png"), std::ios::binary);
        if (in) {
            const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            if (auto img = decodeUiImage(bytes))
                h.picture = std::make_shared<RgbaImage>(std::move(*img));
        }
        out.push_back(std::move(h));
    }
    return out;
}

MainMenu::MainMenu(ResourceIndex& index, const fs::path& resDir, std::vector<MainMenuHood> hoods, bool fromNeighborhood)
    : res_(std::make_unique<UiResources>(index, resDir)), fonts_(std::make_unique<FontLibrary>(resDir / "UI/Fonts")),
      fromHood_(fromNeighborhood), hoods_(std::move(hoods)) {
    auto load = [&](uint32_t id, UiScript& out) {
        if (auto s = res_->script(id); s && !s->roots.empty()) {
            out = std::move(*s);
            return true;
        }
        return false;
    };
    const bool a = load(kMenuScript, menu_), b = load(kPreviewScript, preview_script_), c = load(kLearnScript, learn_);
    ok_ = a && b && c;
}

MainMenu::~MainMenu() = default;

void MainMenu::preview(int i) {
    preview_ = i >= 0 && i < int(hoods_.size()) ? i : -1;
    if (preview_ >= 0)
        page_ = preview_ / kPerPage;
    learnOpen_ = false;
    dirty_ = true;
}

std::array<float, 4> MainMenu::thumbnailRect(int i) {
    layoutIfNeeded();
    if (i < 0 || i >= int(thumbs_.size()))
        return {0, 0, 0, 0};
    return thumbs_[size_t(i)];
}

void MainMenu::layoutIfNeeded() {
    if (dirty_ || laidW_ != canvasW_ || laidH_ != canvasH_)
        screen(canvasW_, canvasH_);
}

MainMenu::Action MainMenu::press(simd_float2 p) {
    layoutIfNeeded();
    // Topmost first: the hits are collected back to front.
    for (auto it = hits_.rbegin(); it != hits_.rend(); ++it) {
        if (!inside(it->rect, p))
            continue;
        if (it->element && playSound_)
            if (const std::string s = uiButtonSound(*it->element); !s.empty())
                playSound_(s);
        dirty_ = true;
        const int pages = std::max(1, (int(hoods_.size()) + kPerPage - 1) / kPerPage);
        switch (it->target) {
        case Target::Prev: page_ = (page_ + pages - 1) % pages; return Action::Consumed;
        case Target::Next: page_ = (page_ + 1) % pages; return Action::Consumed;
        case Target::Thumb: preview(it->value); return Action::Consumed;
        case Target::LearnToPlay: learnOpen_ = true, preview_ = -1; return Action::Consumed;
        case Target::PreviewClose: preview_ = -1; return Action::Consumed;
        case Target::TutorialClose: learnOpen_ = false; return Action::Consumed;
        case Target::PreviewPlay:
            chosen_ = hoods_[size_t(preview_)].prefix;
            return Action::Play;
        case Target::TutorialPlay: chosen_ = "Tutorial"; return Action::Tutorial;
        case Target::Quit: return Action::Quit;
        case Target::Close: return Action::Close;
        }
    }
    return Action::None;
}

void MainMenu::hover(simd_float2) {}

UiScreen MainMenu::screen(int width, int height) {
    UiScreen out;
    out.width = width, out.height = height;
    laidW_ = width, laidH_ = height;
    hits_.clear();
    scaled_.clear();
    thumbs_.assign(hoods_.size(), {0, 0, 0, 0});
    dirty_ = false;
    uploaded_ = false;
    if (!ok_)
        return out;
    const UiElement& root = menu_.roots.front();
    const auto ra = root.area();
    const float rw = float(ra[2] - ra[0]), rh = float(ra[3] - ra[1]);
    const float ox = std::round((float(width) - rw) / 2), oy = std::round((float(height) - rh) / 2);
    std::map<const UiElement*, std::array<float, 4>> rects;
    auto hit = [&](const UiElement* e, Target t, int v = 0) {
        if (auto r = e ? rects.find(e) : rects.end(); r != rects.end())
            hits_.push_back({r->second, t, v, e});
    };

    // The blurred neighborhood behind it all, covering the canvas.
    if (const UiElement* bg = root.child(kBackground))
        if (const std::string* ref = bg->attr("image"))
            if (auto r = uiResourceRef(*ref))
                if (const RgbaImage* img = res_->image(*r); img && img->width && img->height) {
                    UiQuad q;
                    q.image = img;
                    q.x0 = 0, q.y0 = 0, q.x1 = float(width), q.y1 = float(height);
                    // The art is 1024x768 with a 10 px bleed on each side.
                    const float bx = 10.0f / float(img->width), by = 10.0f / float(img->height);
                    const float iw = float(img->width) - 20, ih = float(img->height) - 20;
                    const float sa = iw / ih, ca = float(width) / float(height);
                    float u = 1, v = 1; // the share of the art shown
                    if (ca > sa)
                        v = sa / ca; // wider canvas: crop top and bottom
                    else
                        u = ca / sa;
                    q.u0 = bx + (1 - 2 * bx) * (1 - u) / 2, q.u1 = 1 - q.u0;
                    q.v0 = by + (1 - 2 * by) * (1 - v) / 2, q.v1 = 1 - q.v0;
                    out.quads.push_back(q);
                }

    const bool dialog = preview_ >= 0 || learnOpen_;
    UiDrawHooks hk;
    hk.rects = &rects;
    if (const UiElement* e = root.child(kInitial))
        (fromHood_ ? hk.hide : hk.show).insert(e);
    if (const UiElement* e = root.child(kFromHood))
        (fromHood_ ? hk.show : hk.hide).insert(e);
    if (const UiElement* e = root.child(kShade); e && dialog)
        hk.show.insert(e);
    // The thumbnails, three a page, each the picture over the name (inferred).
    const UiElement* strip = root.child(kThumbs);
    const UiElement* welcome = nullptr; // its text style names the thumbnails
    for (const auto& c : root.children)
        if (c.clsid() == "GZWinText" && c.get("font") == "NeighborhoodWelcome")
            welcome = &c;
    std::vector<std::pair<int, std::array<float, 4>>> placed;
    if (strip) {
        hk.after[strip] = [&](UiScreen& s, float x, float y, float w, float h) {
            const float slot = w / kPerPage;
            const float th = h - 26, tw = std::round(th * 4 / 3);
            for (int k = 0; k < kPerPage; ++k) {
                const int i = page_ * kPerPage + k;
                if (i >= int(hoods_.size()))
                    break;
                const MainMenuHood& hood = hoods_[size_t(i)];
                const float tx = std::round(x + slot * float(k) + (slot - tw) / 2), ty = y + 2;
                // A thin frame, the picture, the name under it.
                UiQuad frame;
                frame.x0 = tx - 2, frame.y0 = ty - 2, frame.x1 = tx + tw + 2, frame.y1 = ty + th + 2;
                frame.color = {0.72f, 0.73f, 1.0f, 1};
                s.quads.push_back(frame);
                if (hood.picture) {
                    scaled_.push_back(std::make_unique<RgbaImage>(
                        scaledImage(*hood.picture, uint32_t(tw), uint32_t(th))));
                    UiQuad q;
                    q.image = scaled_.back().get();
                    q.x0 = tx, q.y0 = ty, q.x1 = tx + tw, q.y1 = ty + th;
                    s.quads.push_back(q);
                }
                if (welcome) {
                    UiElement t = *welcome;
                    t.set("caption", hood.name);
                    t.set("captionres", "");
                    t.set("font", "LiveModePanelPuck");
                    t.set("area", uiArea(0, 0, int(slot), 22));
                    UiDrawHooks th2;
                    th2.captions[&t] = hood.name;
                    layoutUiElement(t, x + slot * float(k), ty + th + 3, *res_, *fonts_, s, th2);
                }
                placed.push_back({i, {tx, ty, tx + tw, ty + th}});
            }
        };
    }
    layoutUiElement(root, ox - float(ra[0]), oy - float(ra[1]), *res_, *fonts_, out, hk);
    if (!dialog) {
        for (const auto& [i, r] : placed) {
            thumbs_[size_t(i)] = r;
            hits_.push_back({r, Target::Thumb, i, nullptr});
        }
        if (hoods_.size() > size_t(kPerPage)) {
            hit(root.child(kPrev), Target::Prev);
            hit(root.child(kNext), Target::Next);
        }
        hit(root.child(kLearn), Target::LearnToPlay);
        if (const UiElement* g = root.child(kInitial); g && !fromHood_)
            hit(g->child(kQuitInitial), Target::Quit);
        if (const UiElement* g = root.child(kFromHood); g && fromHood_) {
            hit(g->child(kClose), Target::Close);
            hit(g->child(kQuitFromHood), Target::Quit);
        }
    } else {
        for (const auto& [i, r] : placed)
            thumbs_[size_t(i)] = r;
    }

    // The dialogs over the menu, at the root's origin (inferred).
    if (preview_ >= 0) {
        const MainMenuHood& hood = hoods_[size_t(preview_)];
        const UiElement& pr = preview_script_.roots.front();
        UiDrawHooks ph;
        ph.rects = &rects;
        if (const UiElement* e = pr.child(kPreviewName))
            ph.captions[e] = hood.name;
        // A text edit draws its initvalue.
        if (UiElement* e = preview_script_.roots.front().child(kPreviewDescription))
            e->set("initvalue", hood.description);
        if (const UiElement* e = pr.child(kDelete))
            ph.hide.insert(e);
        if (const UiElement* e = pr.child(kPreviewPicture); e && hood.picture) {
            const auto a = e->area();
            scaled_.push_back(std::make_unique<RgbaImage>(
                scaledImage(*hood.picture, uint32_t(a[2] - a[0]), uint32_t(a[3] - a[1]))));
            ph.images[e] = scaled_.back().get();
        }
        layoutUiElement(pr, ox - float(pr.area()[0]), oy - float(pr.area()[1]), *res_, *fonts_, out, ph);
        hit(pr.child(kPlay), Target::PreviewPlay);
        hit(pr.child(kPreviewClose), Target::PreviewClose);
    } else if (learnOpen_) {
        const UiElement& lr = learn_.roots.front();
        UiDrawHooks lh;
        lh.rects = &rects;
        layoutUiElement(lr, ox - float(lr.area()[0]), oy - float(lr.area()[1]), *res_, *fonts_, out, lh);
        hit(lr.child(kTutorialPlay), Target::TutorialPlay);
        hit(lr.child(kTutorialClose), Target::TutorialClose);
    }
    return out;
}

void MainMenu::attach(MTL::Device* device, uint32_t colorFormat, uint32_t depthFormat) {
    for (auto& l : layers_)
        l = std::make_unique<UiLayer>(device, colorFormat, depthFormat);
}

void MainMenu::encode(MTL::RenderCommandEncoder* enc, uint32_t width, uint32_t height, float scale) {
    if (!layers_[0] || width == 0 || height == 0 || !ok_)
        return;
    const int cw = int(std::lround(float(width) / std::max(scale, 0.01f)));
    const int ch = int(std::lround(float(height) / std::max(scale, 0.01f)));
    if (cw != canvasW_ || ch != canvasH_)
        canvasW_ = cw, canvasH_ = ch, dirty_ = true;
    if (dirty_ || !uploaded_ || laidW_ != cw || laidH_ != ch) {
        layer_ = (layer_ + 1) % layers_.size();
        const UiScreen sc = screen(cw, ch);
        layers_[layer_]->setScreen(sc);
        uploaded_ = true;
    }
    layers_[layer_]->encode(enc, width, height);
}

} // namespace os2
