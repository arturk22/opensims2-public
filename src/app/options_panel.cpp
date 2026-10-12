#include "app/options_panel.hpp"

#include "app/ui_panel_util.hpp"
#include "app/user_data.hpp"
#include "formats/font.hpp"
#include "formats/resindex.hpp"
#include "formats/ui_resources.hpp"
#include "render/ui_layer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace os2 {

namespace {

constexpr uint32_t kPuckScript = 0x4905F85B, kPanelScript = 0x49060001, kAudioScript = 0x49060F01,
                   kGameScript = 0x49060F02, kGraphicsScript = 0x49060F03, kCameraScript = 0x49060F05,
                   kCreditsScript = 0x49060F08;
constexpr uint32_t kPuckAnchor = 0x4BDEFFAA, kButtons = 0xA200, kAnchor = 0xAAAA;
// Audio options: sliders and mute boxes by group (voice, sfx, music, ambience).
constexpr std::array<uint32_t, 4> kSliders = {0xB31, 0xB01, 0xB41, 0xB21};
constexpr std::array<uint32_t, 4> kMutes = {0x10, 0x11, 0x12, 0x13};
constexpr uint32_t kEdgeScrolling = 0xA1;
// The windows' left column, 156 px (the title icon's x in the scripts): only the lower tab is art, over the puck.
constexpr float kTab = 156;

bool inside(const std::array<float, 4>& r, simd_float2 p) { return insideRect(r, p.x, p.y); }

// The buttons of a window's radio groups: a rectangle with two or more image
// buttons (Off / Medium / High...).
std::vector<std::pair<const UiElement*, std::vector<const UiElement*>>> radioGroups(const UiElement& root) {
    std::vector<std::pair<const UiElement*, std::vector<const UiElement*>>> out;
    for (const auto& c : root.children) {
        if (c.clsid() != "GZWinFlatRect")
            continue;
        std::vector<const UiElement*> btns;
        for (const auto& b : c.children)
            if (b.clsid() == "GZWinBtn" && b.attr("image"))
                btns.push_back(&b);
        if (btns.size() >= 2)
            out.push_back({&c, btns});
    }
    return out;
}

// The option a radio group sets (its buttons' ids: group << 4 | setting):
// Graphics 0x49060F03 and Camera 0x49060F05, named as Graphics Rules.sgr's
// options where it has them (UI Transparency, Square Pixels and the camera's
// snapshot and video groups are named here).
std::string optionName(int window, const std::vector<const UiElement*>& btns) {
    if (btns.empty())
        return {};
    const uint32_t g = btns.front()->id() >> 4;
    if (window == 3)
        switch (g) {
        case 0x9: return "Shadows";
        case 0x8: return "LightingQuality";
        case 0x5: return "EffectsQuality";
        case 0x6: return "ObjectDetail";
        case 0x4: return "MaterialDetail";
        case 0x7: return "Reflection";
        case 0x2: return "UITransparency";
        case 0x1: return "SquarePixels";
        case 0xA: return "ObjectHiding";
        default: return {};
        }
    if (window == 4)
        switch (g) {
        case 0x0: return "LivePIP";
        case 0x1: return "SpecialEventCamera";
        case 0x3: return "NeighborhoodCameraDrift";
        case 0x7: return "SnapshotSize";
        case 0x4: return "SnapshotQuality";
        case 0x8: return "VideoSize";
        case 0x6: return "VideoSound";
        case 0x9: return "VideoQuality";
        default: return {};
        }
    return {};
}

// The default pick of a group: the highest setting the labels name.
const UiElement* defaultPick(const std::vector<const UiElement*>& btns) {
    for (const char* want : {"High", "On", "Enabled", "Medium", "Large"})
        for (const UiElement* b : btns)
            if (b->get("caption") == want)
                return b;
    return btns.front();
}

} // namespace

OptionsPanel::OptionsPanel(ResourceIndex& index, const std::filesystem::path& resDir, GameSettings settings)
    : res_(std::make_unique<UiResources>(index, resDir)), fonts_(std::make_unique<FontLibrary>(resDir / "UI/Fonts")),
      settings_(settings) {
    auto load = [&](uint32_t id, UiScript& out) {
        if (auto s = res_->script(id); s && !s->roots.empty()) {
            out = std::move(*s);
            return true;
        }
        return false;
    };
    const bool a = load(kPuckScript, puck_), b = load(kPanelScript, panel_), c = load(kAudioScript, audio_),
               d = load(kGameScript, game_);
    ok_ = a && b && c && d;
    load(kGraphicsScript, graphics_), load(kCameraScript, camera_), load(kCreditsScript, credits_);
    syncPicks();
}

// The radio groups' picks from the settings (Options.txt), else the defaults.
void OptionsPanel::syncPicks() {
    for (const UiScript* sc : {&graphics_, &camera_})
        if (!sc->roots.empty())
            for (const auto& [g, btns] : radioGroups(sc->roots.front())) {
                picks_[g] = defaultPick(btns);
                const std::string name = optionName(sc == &graphics_ ? 3 : 4, btns);
                if (!name.empty())
                    if (const int v = settings_.pick(name, 0); v >= 1)
                        for (const UiElement* btn : btns)
                            if (int(btn->id() & 0xF) == v)
                                picks_[g] = btn;
            }
}

const UiScript& OptionsPanel::windowScript(int window) const {
    switch (window) {
    case 1: return audio_;
    case 3: return graphics_;
    case 4: return camera_;
    case 5: return credits_;
    default: return game_;
    }
}

OptionsPanel::~OptionsPanel() = default;

void OptionsPanel::toggleWindow(int window) {
    if (window < 1 || window > 5 || windowScript(window).roots.empty())
        window = 0;
    window_ = window_ == window ? 0 : window;
    dirty_ = true;
}

void OptionsPanel::closeWindow() {
    window_ = 0;
    dirty_ = true;
}

void OptionsPanel::changed() {
    if (onChanged)
        onChanged(settings_);
}

bool OptionsPanel::covers(simd_float2 p) {
    if (window_ == 0 || !ok_)
        return false;
    if (dirty_ || laidW_ != canvasW_ || laidH_ != canvasH_)
        screen(canvasW_, canvasH_);
    for (const auto& h : hits_)
        if (h.target == Target::Window && inside(h.rect, p))
            return true;
    return false;
}

OptionsPanel::Action OptionsPanel::press(simd_float2 p) {
    if (window_ == 0 || !ok_)
        return Action::None;
    if (dirty_ || laidW_ != canvasW_ || laidH_ != canvasH_)
        screen(canvasW_, canvasH_);
    for (auto it = hits_.rbegin(); it != hits_.rend(); ++it) {
        if (!inside(it->rect, p))
            continue;
        if (it->element && playSound)
            if (const std::string s = uiButtonSound(*it->element); !s.empty())
                playSound(s);
        dirty_ = true;
        switch (it->target) {
        case Target::Slider: {
            const auto& r = it->rect;
            const float v = std::clamp((p.x - r[0]) / std::max(1.0f, r[2] - r[0]), 0.0f, 1.0f);
            settings_.volume[size_t(it->value)] = std::round(v * 100) / 100;
            std::printf("opensims2: options: %s volume %.2f\n", GameSettings::groupKey(it->value),
                        settings_.volume[size_t(it->value)]);
            changed();
            return Action::Consumed;
        }
        case Target::Mute:
            settings_.muted[size_t(it->value)] = !settings_.muted[size_t(it->value)];
            std::printf("opensims2: options: %s %s\n", GameSettings::groupKey(it->value),
                        settings_.muted[size_t(it->value)] ? "muted" : "on");
            changed();
            return Action::Consumed;
        case Target::EdgeOff:
        case Target::EdgeOn:
            settings_.edgeScrolling = it->target == Target::EdgeOn;
            std::printf("opensims2: options: edge scrolling %s\n", settings_.edgeScrolling ? "on" : "off");
            changed();
            return Action::Consumed;
        case Target::Window: return Action::Consumed;
        case Target::Radio:
            for (auto& [g, pick] : picks_)
                for (const auto& b : g->children)
                    if (&b == it->element) {
                        pick = it->element;
                        std::vector<const UiElement*> btns;
                        for (const auto& c : g->children)
                            if (c.clsid() == "GZWinBtn" && c.attr("image"))
                                btns.push_back(&c);
                        const std::string name = optionName(window_, btns);
                        if (!name.empty()) {
                            settings_.picks[name] = int(it->element->id() & 0xF);
                            if (name == "Reflection") // the renderer's setting (option "reflection")
                                settings_.reflections = settings_.picks[name] == 2;
                            std::printf("opensims2: options: %s %d\n", name.c_str(), settings_.picks[name]);
                            changed();
                        }
                    }
            return Action::Consumed;
        }
    }
    return Action::None;
}

UiScreen OptionsPanel::screen(int width, int height) {
    UiScreen out;
    out.width = width, out.height = height;
    laidW_ = width, laidH_ = height;
    hits_.clear();
    dirty_ = false;
    uploaded_ = false;
    if (!ok_ || window_ == 0)
        return out;
    std::map<const UiElement*, std::array<float, 4>> rects;
    auto hit = [&](const UiElement* e, Target t, int v = 0) {
        if (auto r = e ? rects.find(e) : rects.end(); r != rects.end())
            hits_.push_back({r->second, t, v, e});
    };
    // Joined to the puck (at the bottom left, 8 px in) by the anchors.
    const UiElement& puck = puck_.roots.front();
    const UiElement& root = panel_.roots.front();
    const auto pa = puck.area(), ra = root.area();
    const float puckX = 8, puckY = float(height) - float(pa[3] - pa[1]);
    float x0 = puckX, y0 = puckY - 120;
    const UiElement* buttons = root.child(kButtons);
    if (const UiElement* a = puck.child(kPuckAnchor))
        if (const UiElement* b = buttons ? buttons->child(kAnchor) : nullptr) {
            const auto ba = buttons->area();
            x0 = puckX + float(a->area()[0] - ba[0] - b->area()[0]);
            y0 = puckY + float(a->area()[1] - ba[1] - b->area()[1]);
        }
    // The option window over the panel (inferred).
    if (window_ != 0) {
        const UiElement& wr = windowScript(window_).roots.front();
        const auto wa = wr.area();
        // Its bottom left on the panel's top left (inferred: its lower left
        // tab is as tall as the panel and rises from it; the body, with
        // Apply Changes at its foot, must not sit under the panel's buttons,
        // which stay usable, as the window on the panel hid half of Apply).
        float wx = std::max(0.0f, x0), wy = std::max(0.0f, y0 - float(wa[3] - wa[1]));
        if (anchored_)
            wx = std::max(0.0f, anchorX_), wy = std::max(0.0f, anchorY_ - float(wa[3] - wa[1]));
        UiDrawHooks wh;
        wh.rects = &rects;
        if (window_ == 1) {
            // The mute boxes: pressed = muted.
            for (const auto& c : wr.children)
                for (const auto& b : c.children)
                    for (size_t g = 0; g < 4; ++g)
                        if (b.id() == kMutes[g] && b.clsid() == "GZWinBtn")
                            wh.buttonState[&b] = settings_.muted[g] ? 2 : 1;
            // Sliders: the track and the thumb at the volume.
            for (size_t g = 0; g < 4; ++g)
                if (const UiElement* s = wr.child(kSliders[g])) {
                    const RgbaImage* track = nullptr;
                    const RgbaImage* thumb = nullptr;
                    if (const std::string* ref = s->attr("containerimage"))
                        if (auto r = uiResourceRef(*ref))
                            track = res_->image(*r);
                    if (const std::string* ref = s->attr("thumbimage"))
                        if (auto r = uiResourceRef(*ref))
                            thumb = res_->image(*r);
                    const float v = settings_.volume[g];
                    wh.after[s] = [track, thumb, v](UiScreen& o, float x, float y, float w, float h) {
                        UiQuad t;
                        t.x0 = x, t.y0 = y, t.x1 = x + w, t.y1 = y + h;
                        if (track)
                            t.image = track;
                        else
                            t.color = {0.3f, 0.35f, 0.6f, 1};
                        o.quads.push_back(t);
                        UiQuad q;
                        const float tw = thumb ? float(thumb->width) : 12;
                        const float cx = x + v * (w - tw);
                        q.x0 = cx, q.y0 = y, q.x1 = cx + tw, q.y1 = y + h;
                        if (thumb) {
                            q.image = thumb;
                            q.v0 = 0.25f, q.v1 = 0.5f; // the normal frame of four
                        } else {
                            q.color = {0.9f, 0.9f, 1, 1};
                        }
                        o.quads.push_back(q);
                    };
                }
        } else if (window_ >= 3) {
            if (window_ != 5)
                for (const auto& [g, btns] : radioGroups(wr))
                    for (const UiElement* b : btns)
                        wh.buttonState[b] = picks_[g] == b ? 2 : 1;
            if (window_ == 5)
                wh.hide.insert(wr.child(0xA0));
        } else {
            if (const UiElement* r = wr.child(kEdgeScrolling)) {
                if (const UiElement* off = r->child(1))
                    wh.buttonState[off] = settings_.edgeScrolling ? 1 : 2;
                if (const UiElement* on = r->child(2))
                    wh.buttonState[on] = settings_.edgeScrolling ? 2 : 1;
            }
        }
        if (const UiElement* e = wr.child(0x999)) // "WingenPlacement"
            wh.hide.insert(e);
        layoutUiElement(wr, wx - float(wa[0]), wy - float(wa[1]), *res_, *fonts_, out, wh);
        if (auto r = rects.find(&wr); r != rects.end()) {
            // The body takes the clicks it covers (not the transparent column
            // left of it, where the tab's art lets the puck's buttons through).
            std::array<float, 4> body = r->second;
            body[0] += kTab;
            hits_.push_back({body, Target::Window, 0, nullptr});
        }
        if (window_ == 1) {
            for (size_t g = 0; g < 4; ++g)
                hit(wr.child(kSliders[g]), Target::Slider, int(g));
            for (const auto& c : wr.children)
                for (const auto& b : c.children)
                    for (size_t g = 0; g < 4; ++g)
                        if (b.id() == kMutes[g] && b.clsid() == "GZWinBtn")
                            hit(&b, Target::Mute, int(g));
        } else if (window_ >= 3) {
            if (window_ != 5)
                for (const auto& [g, btns] : radioGroups(wr))
                    for (const UiElement* b : btns)
                        hit(b, Target::Radio);
        } else if (const UiElement* r = wr.child(kEdgeScrolling)) {
            hit(r->child(1), Target::EdgeOff);
            hit(r->child(2), Target::EdgeOn);
        }
    }
    return out;
}

void OptionsPanel::attach(MTL::Device* device, uint32_t colorFormat, uint32_t depthFormat) {
    for (auto& l : layers_)
        l = std::make_unique<UiLayer>(device, colorFormat, depthFormat);
}

void OptionsPanel::encode(MTL::RenderCommandEncoder* enc, uint32_t width, uint32_t height, float scale) {
    if (!layers_[0] || width == 0 || height == 0 || !ok_ || window_ == 0)
        return;
    const int cw = int(std::lround(float(width) / std::max(scale, 0.01f)));
    const int ch = int(std::lround(float(height) / std::max(scale, 0.01f)));
    if (cw != canvasW_ || ch != canvasH_)
        canvasW_ = cw, canvasH_ = ch, dirty_ = true;
    if (dirty_ || !uploaded_ || laidW_ != cw || laidH_ != ch) {
        layer_ = (layer_ + 1) % layers_.size();
        layers_[layer_]->setScreen(screen(cw, ch));
        uploaded_ = true;
    }
    layers_[layer_]->encode(enc, width, height);
}

} // namespace os2
