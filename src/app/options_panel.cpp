#include "app/options_panel.hpp"

#include "app/ui_panel_util.hpp"
#include "app/user_data.hpp"
#include "formats/font.hpp"
#include "formats/resindex.hpp"
#include "formats/ui_resources.hpp"
#include "render/ui_layer.hpp"

#include <CoreGraphics/CoreGraphics.h>

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
// The Graphics window's list boxes (class 0xAC17CE2A): screen sizes, refresh rates.
constexpr std::array<uint32_t, 2> kLists = {0x35, 0x45};
constexpr float kListRowH = 17; // rows of the list font (inferred: the game's list boxes use their own constants)
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

// The main display's modes (sizes in points, as the window is laid out)
// and the one in use, for the Graphics window's lists.
std::vector<OptionsPanel::DisplayMode> mainDisplayModes(OptionsPanel::DisplayMode& current) {
    std::vector<OptionsPanel::DisplayMode> out;
    const CGDirectDisplayID display = CGMainDisplayID();
    if (CGDisplayModeRef m = CGDisplayCopyDisplayMode(display)) {
        current = {int(CGDisplayModeGetWidth(m)), int(CGDisplayModeGetHeight(m)), int(std::lround(CGDisplayModeGetRefreshRate(m)))};
        CGDisplayModeRelease(m);
    }
    // The scaled modes too (a Retina display's "looks like" sizes).
    const void* keys[] = {kCGDisplayShowDuplicateLowResolutionModes};
    const void* values[] = {kCFBooleanTrue};
    CFDictionaryRef options =
        CFDictionaryCreate(nullptr, keys, values, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFArrayRef all = CGDisplayCopyAllDisplayModes(display, options);
    if (options)
        CFRelease(options);
    if (all) {
        for (CFIndex i = 0; i < CFArrayGetCount(all); ++i) {
            auto m = (CGDisplayModeRef)CFArrayGetValueAtIndex(all, i);
            // From 800 x 600 up (the game's smallest mode; inferred for the Mac).
            const int w = int(CGDisplayModeGetWidth(m)), h = int(CGDisplayModeGetHeight(m));
            if (w >= 800 && h >= 600 && CGDisplayModeIsUsableForDesktopGUI(m))
                out.push_back({w, h, int(std::lround(CGDisplayModeGetRefreshRate(m)))});
        }
        CFRelease(all);
    }
    if (current.width > 0 && std::none_of(out.begin(), out.end(), [&](const OptionsPanel::DisplayMode& m) {
            return m.width == current.width && m.height == current.height && m.refresh == current.refresh;
        }))
        out.push_back(current);
    return out;
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
    DisplayMode current;
    modes_ = mainDisplayModes(current);
    currentMode_ = current;
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

void OptionsPanel::setDisplayModes(std::vector<DisplayMode> modes, DisplayMode current) {
    modes_ = std::move(modes);
    currentMode_ = current;
    listTop_ = {0, 0}, scrolled_ = {false, false};
    dirty_ = true;
}

std::vector<std::pair<int, int>> OptionsPanel::sizes() const {
    std::vector<std::pair<int, int>> out;
    for (const auto& m : modes_)
        if (m.width > 0 && m.height > 0)
            out.push_back({m.width, m.height});
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::pair<int, int> OptionsPanel::pickedSize() const {
    if (settings_.screenWidth > 0 && settings_.screenHeight > 0)
        return {settings_.screenWidth, settings_.screenHeight};
    return {currentMode_.width, currentMode_.height};
}

std::vector<int> OptionsPanel::rates() const {
    const auto size = pickedSize();
    std::vector<int> out;
    for (const auto& m : modes_)
        if (m.width == size.first && m.height == size.second)
            out.push_back(m.refresh);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

int OptionsPanel::pickedRate() const {
    const auto r = rates();
    if (settings_.refreshRate > 0 && std::find(r.begin(), r.end(), settings_.refreshRate) != r.end())
        return settings_.refreshRate;
    if (std::find(r.begin(), r.end(), currentMode_.refresh) != r.end())
        return currentMode_.refresh;
    return r.empty() ? 0 : r.back();
}

std::vector<std::string> OptionsPanel::screenSizeRows() const {
    std::vector<std::string> out;
    char buf[32];
    for (const auto& [w, h] : sizes()) {
        std::snprintf(buf, sizeof buf, "%u x %u", unsigned(w), unsigned(h)); // the binary's "%u x %u"
        out.push_back(buf);
    }
    return out;
}

std::vector<std::string> OptionsPanel::refreshRateRows() const {
    std::vector<std::string> out;
    char buf[32];
    for (int r : rates()) {
        // A display that gives no rate (most built-in panels) runs at 60 Hz (inferred).
        std::snprintf(buf, sizeof buf, "%u Hz", unsigned(r > 0 ? r : 60));
        out.push_back(buf);
    }
    return out;
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
        case Target::ListRow: {
            const int list = it->value >> 16, row = it->value & 0xFFFF;
            if (list == 0) {
                const auto all = sizes();
                if (size_t(row) < all.size()) {
                    settings_.screenWidth = all[size_t(row)].first, settings_.screenHeight = all[size_t(row)].second;
                    settings_.refreshRate = pickedRate(); // a rate the new size has
                    listTop_[1] = 0, scrolled_[1] = false;
                    std::printf("opensims2: options: screen size %d x %d\n", settings_.screenWidth, settings_.screenHeight);
                    changed();
                }
            } else {
                const auto all = rates();
                if (size_t(row) < all.size()) {
                    if (settings_.screenWidth <= 0)
                        settings_.screenWidth = pickedSize().first, settings_.screenHeight = pickedSize().second;
                    settings_.refreshRate = all[size_t(row)] > 0 ? all[size_t(row)] : 60;
                    std::printf("opensims2: options: refresh rate %d Hz\n", settings_.refreshRate);
                    changed();
                }
            }
            return Action::Consumed;
        }
        case Target::ListScroll: {
            // The scroll bar: a click above the middle goes up a page, below it down.
            const int list = it->value >> 16, rows = it->value & 0xFFFF;
            const int page = std::max(1, int((it->rect[3] - it->rect[1]) / kListRowH));
            const int count = int(list == 0 ? sizes().size() : rates().size());
            const bool up = p.y < (it->rect[1] + it->rect[3]) / 2;
            listTop_[size_t(list)] = std::clamp(listTop_[size_t(list)] + (up ? -page : page), 0, std::max(0, count - rows));
            scrolled_[size_t(list)] = true; // the player's scroll stays (the pick may be out of view)
            return Action::Consumed;
        }
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
        // The Graphics window's two lists: rows of the display's modes, the
        // picked one highlighted (the main menu's list box colours), and the
        // script's hidden scroll bar shown when the rows overflow.
        std::vector<Hit> listHits;
        auto listHooks = [&](const UiElement& wroot, UiDrawHooks& hooks) {
            for (size_t li = 0; li < kLists.size(); ++li) {
                const UiElement* box = nullptr;
                const UiElement* bar = nullptr;
                for (const auto& c : wroot.children)
                    for (const auto& b : c.children)
                        if (b.id() == kLists[li]) {
                            box = &b;
                            for (const auto& o : c.children)
                                if (o.clsid() == "GZWinScrollbar2")
                                    bar = &o;
                        }
                if (!box)
                    continue;
                const std::vector<std::string> rows = li == 0 ? screenSizeRows() : refreshRateRows();
                int picked = -1;
                if (li == 0) {
                    const auto all = sizes();
                    picked = int(std::find(all.begin(), all.end(), pickedSize()) - all.begin());
                } else {
                    const auto all = rates();
                    picked = int(std::find(all.begin(), all.end(), pickedRate()) - all.begin());
                }
                const int fit = std::max(1, int(float(box->area()[3] - box->area()[1]) / kListRowH));
                int& top = listTop_[li];
                top = std::clamp(top, 0, std::max(0, int(rows.size()) - fit));
                if (picked >= 0 && picked < int(rows.size()) && (picked < top || picked >= top + fit) && !scrolled_[li])
                    top = std::clamp(picked - fit / 2, 0, std::max(0, int(rows.size()) - fit));
                hooks.after[box] = [this, rows, picked, top, fit, li, &listHits](UiScreen& sc, float x, float y, float w, float) {
                    for (int r = top; r < int(rows.size()) && r < top + fit; ++r) {
                        const float ry = y + float(r - top) * kListRowH;
                        const bool on = r == picked;
                        if (on) {
                            UiQuad q;
                            q.x0 = x, q.y0 = ry, q.x1 = x + w, q.y1 = ry + kListRowH;
                            q.color = {0.31f, 0.48f, 0.56f, 1};
                            sc.quads.push_back(q);
                        }
                        UiElement t;
                        t.tag = "LEGACY";
                        t.set("clsid", "GZWinText");
                        t.set("iid", "IGZWinText");
                        t.set("area", uiArea(0, 0, int(w) - 6, int(kListRowH)));
                        t.set("font", "0x6c17c8fd");
                        t.set("align", "leftcenter");
                        t.set("winflag_visible", "yes");
                        t.set("forecolor", on ? "(0,255,0)" : "(5,5,112)");
                        UiDrawHooks th;
                        th.captions[&t] = rows[size_t(r)];
                        layoutUiElement(t, x + 4, ry, *res_, *fonts_, sc, th);
                        listHits.push_back({{x, ry, x + w, ry + kListRowH}, Target::ListRow, int(li << 16 | unsigned(r)), nullptr});
                    }
                };
                if (bar && int(rows.size()) > fit) {
                    hooks.show.insert(bar);
                    const int count = int(rows.size());
                    hooks.after[bar] = [top, fit, count, li, &listHits](UiScreen& sc, float x, float y, float w, float h) {
                        UiQuad t;
                        t.x0 = x, t.y0 = y, t.x1 = x + w, t.y1 = y + h;
                        t.color = {0.3f, 0.35f, 0.6f, 1};
                        sc.quads.push_back(t);
                        UiQuad q;
                        const float th = std::max(8.0f, h * float(fit) / float(count));
                        const float ty = y + (h - th) * float(top) / float(std::max(1, count - fit));
                        q.x0 = x, q.y0 = ty, q.x1 = x + w, q.y1 = ty + th;
                        q.color = {0.9f, 0.9f, 1, 1};
                        sc.quads.push_back(q);
                        listHits.push_back({{x, y, x + w, y + h}, Target::ListScroll, int(li << 16 | unsigned(fit)), nullptr});
                    };
                }
            }
        };
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
            if (window_ == 3)
                listHooks(wr, wh);
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
            for (const auto& h : listHits)
                hits_.push_back(h);
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
