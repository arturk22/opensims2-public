#include "app/live_ui.hpp"

#include "formats/dbpf.hpp"
#include "formats/font.hpp"
#include "formats/image.hpp"
#include "formats/ui_image.hpp"
#include "formats/ui_resources.hpp"
#include "render/ui_layer.hpp"
#include "vm/animation.hpp"
#include "vm/interactions.hpp"
#include "vm/motives.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace os2 {

namespace {

constexpr uint32_t kPuckScript = 0x4905F85B, kLiveScript = 0x49060005, kMotivesScript = 0x49064905,
                   kBarScript = 0x4906500B, kMemberScript = 0x49060053, kIconScript = 0x0609A1C0,
                   kQueueConstants = 0xCCD02691;
constexpr uint32_t kMoodGradient = 0xE9100002; // docs/ui-live-mac.md §4.2
// Live panel placements (inferred): the mood meter group after the front
// cap (316 + 13), the needs panel's background after the group (its mood
// box ends at +87), 347 px wide.
constexpr int kGroupX = 329, kSubPanelX = kGroupX + 87, kSubPanelWidth = 347;

// Live panel windows (0x49060005).
constexpr uint32_t kPause = 0x27D7258F, kSpeed2 = 0x57E01CC1, kSpeed3 = 0x37E01CC3, kSpeed4 = 0x47E01CC5;
constexpr uint32_t kClock = 0x37D82A93;
// Puck windows (0x4905F85B): mode buttons (live, buy, build, story, options) and funds.
constexpr std::array<uint32_t, 5> kModes = {0x37D97B25, 0x67D97B2A, 0x77D97B28, 0x27D97B2D, 0x67D97B2F};
constexpr uint32_t kFunds = 0x00003000;

// The needs panel's bars: child window -> motive (docs/ui-live-mac.md §3.1),
// and the caption in Live.package's STR# 0x82 (string number = entry + 1).
struct BarInfo {
    uint32_t window;
    int16_t motive;
    int caption;
};
constexpr std::array<BarInfo, 8> kBars = {{{0xA1, vm::motive::Hunger, 2},
                                           {0xA2, vm::motive::Comfort, 6},
                                           {0xA3, vm::motive::Bladder, 14},
                                           {0xA4, vm::motive::Energy, 4},
                                           {0xA5, vm::motive::Fun, 8},
                                           {0xA6, vm::motive::Social, 12},
                                           {0xA7, vm::motive::Hygiene, 10},
                                           {0xA8, vm::motive::Environment, 16}}};

const UiElement* findIn(const UiElement& e, uint32_t id) {
    if (e.attr("id") && e.id() == id)
        return &e;
    for (const auto& c : e.children)
        if (const UiElement* f = findIn(c, id))
            return f;
    return nullptr;
}
UiElement* findIn(UiElement& e, uint32_t id) { return const_cast<UiElement*>(findIn(std::as_const(e), id)); }

std::string area(int x0, int y0, int x1, int y1) {
    return "(" + std::to_string(x0) + "," + std::to_string(y0) + "," + std::to_string(x1) + "," + std::to_string(y1) +
           ")";
}

bool inside(const std::array<float, 4>& r, simd_float2 p) {
    return p.x >= r[0] && p.x < r[2] && p.y >= r[1] && p.y < r[3];
}

// Thousands with '.' (the Brazilian Portuguese format; the game asks the
// locale's format service).
std::string money(int32_t v) {
    std::string digits = std::to_string(v < 0 ? -int64_t(v) : int64_t(v)), out;
    for (size_t i = 0; i < digits.size(); ++i) {
        if (i && (digits.size() - i) % 3 == 0)
            out += '.';
        out += digits[i];
    }
    return std::string(v < 0 ? "-" : "") + "§" + out;
}

RgbaImage scaled(const RgbaImage& src, uint32_t w, uint32_t h) {
    RgbaImage out;
    out.width = w, out.height = h;
    out.pixels.resize(size_t(w) * h * 4);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            const uint32_t sx = std::min(src.width - 1, uint32_t((x + 0.5f) * float(src.width) / float(w)));
            const uint32_t sy = std::min(src.height - 1, uint32_t((y + 0.5f) * float(src.height) / float(h)));
            std::copy_n(&src.pixels[(size_t(sy) * src.width + sx) * 4], 4, &out.pixels[(size_t(y) * w + x) * 4]);
        }
    return out;
}

} // namespace

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

std::array<float, 4> moodColor(const RgbaImage& gradient, float mood) {
    if (gradient.height == 0 || gradient.width == 0)
        return {1, 1, 1, 1};
    const float m = std::clamp(std::trunc(mood), -100.0f, 100.0f);
    const int h = int(gradient.height);
    const int y = std::clamp(h - int(std::lround(float(h) * (m + 100) / 200)), 0, h - 1);
    const uint8_t* p = &gradient.pixels[size_t(y) * gradient.width * 4];
    return {p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f, 1};
}

void MotiveTrend::add(float value) {
    values_.push_back(int(std::trunc(value)));
    if (values_.size() > 10)
        values_.pop_front();
    float sum = 0;
    for (int v : values_)
        sum += float(v);
    averages_.push_back(int(std::lround(sum / float(values_.size()))));
    if (averages_.size() > 10)
        averages_.pop_front();
}

int MotiveTrend::arrows() const {
    if (averages_.size() < 2)
        return 0;
    const int d = averages_.back() - averages_.front();
    const int a = std::abs(d);
    const int n = a == 0 ? 0 : a <= 2 ? 1 : a <= 4 ? 2 : 3;
    return d < 0 ? -n : n;
}

LiveUi::LiveUi(vm::VM& vm, ResourceIndex& index, const std::filesystem::path& resDir)
    : vm_(&vm), res_(std::make_unique<UiResources>(index, resDir)),
      fonts_(std::make_unique<FontLibrary>(resDir / "UI/Fonts")) {
    auto load = [&](uint32_t id, UiScript& into) {
        if (auto s = res_->script(id)) {
            into = std::move(*s);
            return true;
        }
        std::fprintf(stderr, "opensims2: live UI: no UI script %08X\n", id);
        return false;
    };
    ok_ = true;
    for (auto [id, into] : {std::pair{kPuckScript, &puck_}, {kLiveScript, &live_}, {kMotivesScript, &motives_},
                            {kBarScript, &bar_}, {kMemberScript, &member_}, {kIconScript, &icon_}})
        ok_ = load(id, *into) && ok_;
    if (auto c = res_->script(kQueueConstants))
        queueConstants_ = uiConstants(*c);
    for (size_t i = 0; i < kBars.size(); ++i) {
        bars_[i] = bar_;
        for (auto& r : bars_[i].roots)
            if (r.attr("captionres"))
                r.set("captionres", "{7f96c284," + [&] {
                    char b[16];
                    std::snprintf(b, sizeof b, "0082%04x", kBars[i].caption);
                    return std::string(b);
                }() + "}");
    }
    try {
        thumbnails_ = std::make_unique<DbpfPackage>(resDir / "UserData/Thumbnails/ObjectThumbnails.package");
    } catch (const std::exception& e) {
        std::fprintf(stderr, "opensims2: live UI: %s\n", e.what());
    }

    if (!ok_)
        return;
    // --- What the panel's code does once (inferred placements: TODO.md) ----
    // The live panel with the wants closed and the needs subpanel open:
    // the caps of the other states (the script keeps them side by side as a
    // palette) are hidden; the mood meter and the subpanel buttons follow
    // the front cap at the offset the expanded palette shows (13 px into
    // it), with no wants area (the wants and fears are not simulated); the
    // subpanel's tile runs from there under the needs panel to its end cap.
    UiElement& root = live_.roots.front();
    auto hideId = [&](uint32_t id) {
        if (UiElement* e = findIn(root, id))
            e->set("winflag_visible", "no");
    };
    // The platinum aspiration glows (0x49064805/07, 0x4BDEFFA2) only show
    // past 74 aspiration points (docs/ui-live-mac.md §4.2); aspiration is
    // not simulated yet.
    for (uint32_t id : {0xA94E97F1u, 0xA94E97E6u, 0xA94E97ECu, 0x49064802u, 0xA94E97F4u, 0xA94E97F2u, 0x49064805u,
                        0x49064807u, 0x4BDEFFA2u, 0xA94E97E5u})
        hideId(id);
    for (uint32_t id : {0x49064801u, 0xA94E97E8u}) // Mood SubPanel / ButtonHolder - minimized
        if (UiElement* e = findIn(root, id)) {
            const auto a = e->area();
            e->set("area", area(kGroupX, a[1], kGroupX + a[2] - a[0], a[3]));
        }
    if (UiElement* e = findIn(root, 0xA94E97F5)) // SubPanel Background - Tile
        e->set("area", area(kGroupX + 13, 136, kSubPanelX + kSubPanelWidth - 3, 283)), e->set("blttype", "tiled"),
            e->set("winflag_visible", "yes");
    if (UiElement* e = findIn(root, 0xA94E97EB)) // End Cap - Collapsed - SubPanel
        e->set("area", area(kSubPanelX + kSubPanelWidth - 3, 136, kSubPanelX + kSubPanelWidth + 32, 283));
    // The portrait's shadow is the portrait's silhouette (not drawn here).
    if (UiElement* e = findIn(root, 0x4BDEFFA4))
        if (UiElement* s = e->child(2))
            s->set("winflag_visible", "no");
    // Family thumbnails: no shadow (as above).
    if (UiElement* g = findIn(member_.roots.front(), 0x1000))
        if (UiElement* s = g->child(2))
            s->set("winflag_visible", "no");
}

LiveUi::~LiveUi() = default;

void LiveUi::setSim(vm::ObjectId id) {
    if (id == sim_)
        return;
    sim_ = id;
    for (auto& t : trends_)
        t.clear();
    dirty_ = true;
}

void LiveUi::setPortrait(vm::ObjectId id, RgbaImage portrait) {
    portraits_[id] = std::make_unique<RgbaImage>(std::move(portrait));
    icons_.clear();
    dirty_ = true;
}

const RgbaImage* LiveUi::image(uint32_t instance) {
    return res_->image({kTypeUiImage, kUiImageGroup, instance});
}

std::vector<vm::ObjectId> LiveUi::family() const {
    std::vector<vm::ObjectId> all, family;
    const int16_t current = vm_->globals[vm::global::CurrentFamily];
    for (const auto& [id, o] : vm_->objects()) {
        if (!o.person)
            continue;
        all.push_back(id);
        if (current && o.person->data[vm::persondata::FamilyNumber] == current)
            family.push_back(id);
    }
    return family.empty() ? all : family;
}

void LiveUi::tick(double seconds) {
    if (lastSample_ >= 0 && seconds - lastSample_ < 0.5)
        return;
    lastSample_ = seconds;
    const vm::Object* sim = vm_->object(sim_);
    if (!sim || !sim->person)
        return;
    for (size_t i = 0; i < kBars.size(); ++i)
        trends_[i].add(float(sim->person->motives[size_t(kBars[i].motive)]));
}

const RgbaImage* LiveUi::objectThumbnail(const vm::Object& o) {
    const uint32_t guid = o.guid();
    auto it = objectThumbs_.find(guid);
    if (it == objectThumbs_.end()) {
        std::unique_ptr<RgbaImage> img;
        // ObjectThumbnails.package: JPEG (with ALFA) per object, keyed by
        // the OBJD's GUID (inferred).
        if (thumbnails_ && guid)
            for (const auto& e : thumbnails_->entries())
                if (e.tgi.instance == guid) {
                    if (auto d = decodeUiImage(thumbnails_->read(e)))
                        img = std::make_unique<RgbaImage>(std::move(*d));
                    break;
                }
        if (!img && std::getenv("OS2_LIVE_UI_DUMP"))
            std::fprintf(stderr, "opensims2: live UI: no thumbnail for %s (GUID %08X)\n",
                         o.definition ? o.definition->fileName.c_str() : "?", guid);
        it = objectThumbs_.emplace(guid, std::move(img)).first;
    }
    return it->second.get();
}

// The icon of a queue entry: the target Sim's portrait or the object's
// thumbnail, scaled to the icon window, cut by the circular mask (child 3's
// image) and grey while waiting (docs/ui-live-mac.md §6.2).
const RgbaImage* LiveUi::queueIcon(const vm::QueuedInteraction& q, bool running) {
    const auto key = std::make_pair(q.object, running);
    if (auto it = icons_.find(key); it != icons_.end())
        return it->second.get();
    const RgbaImage* src = nullptr;
    if (const vm::Object* o = vm_->object(q.object)) {
        if (o->person) {
            if (auto p = portraits_.find(q.object); p != portraits_.end())
                src = p->second.get();
        } else {
            src = objectThumbnail(*o);
        }
    }
    std::unique_ptr<RgbaImage> out;
    const UiElement* top = icon_.roots.empty() ? nullptr : &icon_.roots.front();
    const UiElement* maskWin = top ? top->child(3) : nullptr;
    const RgbaImage* mask = nullptr;
    if (maskWin)
        if (auto ref = uiResourceRef(maskWin->get("image")))
            mask = res_->image(*ref);
    if (src && mask) {
        out = std::make_unique<RgbaImage>(scaled(*src, mask->width, mask->height));
        for (size_t i = 0; i < out->pixels.size(); i += 4) {
            uint8_t* p = &out->pixels[i];
            if (!running) {
                const uint8_t l = uint8_t((p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8);
                p[0] = p[1] = p[2] = l;
            }
            p[3] = uint8_t(unsigned(p[3]) * mask->pixels[i + 3] / 255);
        }
    }
    return (icons_[key] = std::move(out)).get();
}

uint64_t LiveUi::stamp() const {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](uint64_t v) { h = (h ^ v) * 1099511628211ull; };
    mix(uint64_t(sim_));
    mix(uint64_t(laidW_) << 16 | uint64_t(laidH_));
    const auto& g = vm_->globals;
    mix(uint64_t(uint16_t(g[vm::global::Hour])) << 32 | uint16_t(g[vm::global::Minute]) << 16 |
        uint16_t(g[vm::global::DayOfWeek]));
    mix(uint64_t(uint16_t(g[vm::global::Speed])) << 16 | uint16_t(g[vm::global::Paused]));
    const int16_t fam = g[vm::global::CurrentFamily];
    if (auto it = vm_->families.find(fam); it != vm_->families.end())
        mix(uint64_t(uint32_t(it->second.funds)));
    if (const vm::Object* sim = vm_->object(sim_)) {
        if (sim->person)
            for (int16_t m : sim->person->motives)
                mix(uint64_t(uint16_t(m)));
        mix(uint64_t(sim->activeInteraction + 1));
        for (const auto& q : sim->queue)
            mix(uint64_t(q.uid) << 2 | uint64_t(q.cancelled) << 1), mix(uint64_t(uint16_t(q.object)));
    }
    for (const auto& t : trends_)
        mix(uint64_t(t.arrows() + 8));
    for (vm::ObjectId id : family()) {
        mix(uint64_t(uint16_t(id)));
        if (const vm::Object* o = vm_->object(id); o && o->person)
            mix(uint64_t(uint16_t(o->person->motives[vm::motive::Mood])));
    }
    mix(uint64_t(hoverIcon_ + 1));
    mix(uint64_t(mode_ + 1));
    mix(uint64_t(uint16_t(g[vm::global::BudgetDiv10000])) << 16 | uint16_t(g[vm::global::BudgetMod10000]));
    return h;
}

UiScreen LiveUi::screen(int width, int height) {
    UiScreen out;
    out.width = width, out.height = height;
    laidW_ = width, laidH_ = height;
    targets_.clear();
    solid_.clear();
    frameImages_.clear();
    if (!ok_)
        return out;
    const vm::Object* sim = vm_->object(sim_);
    const auto& g = vm_->globals;
    const bool paused = g[vm::global::Paused] != 0;
    const int speed = g[vm::global::Speed] >= 2 && g[vm::global::Speed] <= 4 ? g[vm::global::Speed] : 2;
    std::map<const UiElement*, std::array<float, 4>> rects;
    auto place = [&](const UiElement& e, float x, float y, UiDrawHooks& hk) {
        const auto a = e.area();
        hk.rects = &rects;
        layoutUiElement(e, x - float(a[0]), y - float(a[1]), *res_, *fonts_, out, hk);
    };
    auto keep = [&](RgbaImage img) {
        frameImages_.push_back(std::make_unique<RgbaImage>(std::move(img)));
        return frameImages_.back().get();
    };
    auto rectOf = [&](const UiElement* e) -> std::optional<std::array<float, 4>> {
        if (!e)
            return std::nullopt;
        if (auto it = rects.find(e); it != rects.end())
            return it->second;
        return std::nullopt;
    };
    auto target = [&](const UiElement* e, Action a, int value) {
        if (auto r = rectOf(e))
            targets_.push_back({*r, a, value});
    };
    // A gradient fill bar (docs/ui-live-mac.md §5): child 3's image is the
    // mask, child 4's the gradient; the output replaces child 3's image.
    auto fill = [&](const UiElement& bar, float value, UiDrawHooks& hk) {
        const UiElement* maskWin = bar.child(3);
        const UiElement* gradWin = bar.child(4);
        if (!maskWin || !gradWin)
            return;
        const auto mref = uiResourceRef(maskWin->get("image")), gref = uiResourceRef(gradWin->get("image"));
        const RgbaImage* mask = mref ? res_->image(*mref) : nullptr;
        const RgbaImage* grad = gref ? res_->image(*gref) : nullptr;
        if (!mask || !grad)
            return;
        // kFillType is not in these scripts; a bar taller than wide fills
        // by columns (the mood meter: inferred, the rows of its gradient
        // never reach the green).
        const auto a = maskWin->area();
        const bool vertical = (a[3] - a[1]) > (a[2] - a[0]);
        hk.images[maskWin] = keep(gradientFill(*grad, *mask, value, -100, 100, vertical));
        hk.hide.insert(gradWin);
    };

    // --- Placement (inferred) ------------------------------------------------
    // The puck at the bottom left; the live panel joined to it by their
    // anchors (the panel's hidden rectangle captioned "4bdeffaa:bl" over the
    // puck's window 0x4BDEFFAA); the needs subpanel after the mood meter.
    const UiElement& puck = puck_.roots.front();
    const UiElement& live = live_.roots.front();
    const auto pa = puck.area();
    const float puckX = 8, puckY = float(height) - float(pa[3] - pa[1]);
    float liveX = puckX + 5, liveY = puckY - 86;
    if (const UiElement* a = puck.child(0x4BDEFFAA))
        if (const UiElement* b = live.child(0x0000AAAA)) {
            liveX = puckX + float(a->area()[0] - b->area()[0]);
            liveY = puckY + float(a->area()[1] - b->area()[1]);
        }

    // --- The live panel (only in live mode: Buy mode draws its own) ----------
    if (mode_ == 0) {
        UiDrawHooks hk;
        // Speed and pause: the marked one pressed (docs/ui-live-mac.md §1.3).
        const std::array<std::pair<uint32_t, int>, 3> speeds = {{{kSpeed2, 2}, {kSpeed3, 3}, {kSpeed4, 4}}};
        for (auto [id, v] : speeds)
            if (const UiElement* b = findIn(live, id))
                hk.buttonState[b] = !paused && speed == v ? 2 : 1;
        if (const UiElement* b = findIn(live, kPause))
            hk.buttonState[b] = paused ? 2 : 1;
        // The clock: the day's name and the time (24 h, as the Brazilian
        // Portuguese locale; the game asks the locale's format service).
        if (const UiElement* c = findIn(live, kClock)) {
            char t[16];
            std::snprintf(t, sizeof t, "%02d:%02d", g[vm::global::Hour], g[vm::global::Minute]);
            std::string day;
            const int dow = std::clamp<int>(g[vm::global::DayOfWeek], 0, 6);
            if (auto s = res_->string({0x7F96C284, 0x8A, uint32_t(7 + dow)}))
                day = *s + " ";
            hk.captions[c] = day + t;
        }
        if (sim && sim->person) {
            // The portrait.
            if (const UiElement* thumb = findIn(live, 0x4BDEFFA4))
                if (const UiElement* t = thumb->child(1))
                    if (auto p = portraits_.find(sim_); p != portraits_.end())
                        hk.images[t] = p->second.get();
            // The age: only the Sim's age shows.
            const std::string age = vm::agePrefix(*sim);
            const std::map<std::string, uint32_t> ages = {
                {"p", 0xAD8A9F0B}, {"c", 0xAD8A9F0C}, {"t", 0xAD8A9F0D}, {"a", 0xAD8A9F0E}, {"e", 0xAD8A9F0F}};
            for (const auto& [prefix, id] : ages)
                if (const UiElement* e = findIn(live, id))
                    (prefix == age ? hk.show : hk.hide).insert(e);
            // The mood meter (motive 3, docs/ui-live-mac.md §4.1).
            if (const UiElement* m = findIn(live, 0x49064803))
                fill(*m, float(sim->person->motives[vm::motive::Mood]), hk);
        }
        // The needs button of the open subpanel, pressed.
        if (const UiElement* holder = findIn(live, 0xA94E97E8))
            if (const UiElement* b = holder->child(0x0D8AB291))
                hk.buttonState[b] = 2;
        place(live, liveX, liveY, hk);
        for (auto [id, v] : speeds)
            target(findIn(live, id), Action::Speed, v);
        target(findIn(live, kPause), Action::Pause, 0);
        for (uint32_t id : {0xA94E97E9u, 0xA94E97F3u, 0xA94E97E5u, 0xA94E97F5u, 0xA94E97EBu})
            if (auto r = rectOf(findIn(live, id)))
                solid_.push_back(*r);
        if (auto r = rectOf(findIn(live, 0xA94E97E4))) // Background: its lower part (the top is the portrait's sky)
            solid_.push_back({(*r)[0], (*r)[1] + 60, (*r)[2], (*r)[3]});
    }

    // --- The needs subpanel ------------------------------------------------
    if (mode_ == 0 && sim && sim->person) {
        const UiElement& panel = motives_.roots.front();
        const std::string age = vm::agePrefix(*sim);
        const uint32_t group = age == "p" || age == "b" ? 0xA003 : age == "c" ? 0xA004 : 0xA005;
        UiDrawHooks hk;
        for (uint32_t id : {0xA003u, 0xA004u, 0xA005u})
            if (const UiElement* e = panel.child(id))
                (id == group ? hk.show : hk.hide).insert(e);
        const UiElement* bg = panel.child(0xA002);
        const float bgX = bg ? float(bg->area()[0]) : 8;
        place(panel, liveX + float(kSubPanelX) - bgX, liveY + 140, hk);
        if (bg)
            if (auto r = rectOf(bg))
                solid_.push_back(*r);
        if (const UiElement* grp = panel.child(group))
            for (size_t i = 0; i < kBars.size(); ++i) {
                auto r = rectOf(grp->child(kBars[i].window));
                if (!r)
                    continue;
                const float v = float(sim->person->motives[size_t(kBars[i].motive)]);
                const int arrows = trends_[i].arrows();
                for (const UiElement& e : bars_[i].roots) {
                    UiDrawHooks bh;
                    const uint32_t id = e.id();
                    if (id >= 1 && id <= 3) // up: children 3, 2, 1 in that order
                        (arrows >= int(4 - id) ? bh.show : bh.hide).insert(&e);
                    else if (id >= 4 && id <= 6) // down: 4, 5, 6
                        (-arrows >= int(id - 3) ? bh.show : bh.hide).insert(&e);
                    else if (id == 7)
                        fill(e, v, bh);
                    bh.rects = &rects;
                    layoutUiElement(e, (*r)[0], (*r)[1], *res_, *fonts_, out, bh);
                }
            }
    }

    // --- The puck ----------------------------------------------------------
    const std::vector<vm::ObjectId> members = family();
    {
        UiDrawHooks hk;
        for (size_t i = 0; i < kModes.size(); ++i)
            if (const UiElement* b = puck.child(kModes[i]))
                hk.buttonState[b] = int(i) == mode_ ? 2 : 1;
        if (const UiElement* f = puck.child(kFunds)) {
            const int16_t fam = g[vm::global::CurrentFamily];
            const auto it = vm_->families.find(fam);
            const int32_t funds = it != vm_->families.end()
                                      ? it->second.funds
                                      : int32_t(g[vm::global::BudgetDiv10000]) * 10000 + g[vm::global::BudgetMod10000];
            hk.captions[f] = money(funds);
        }
        // The family column grows up from the puck by the members' thumbnails (inferred).
        UiScript column;
        if (const UiElement* c = puck.child(0x4BDEFFAB)) {
            column.roots.push_back(*c);
            UiElement& col = column.roots.front();
            const int n = int(members.size());
            const int mid = std::max(31, n * 43);
            const auto a = c->area();
            const int bottom = a[1] + 44; // the middle piece's end
            col.set("area", area(a[0], bottom - mid - 13, a[2], bottom));
            if (UiElement* top = col.child(1))
                top->set("area", area(0, 0, 18, 13));
            if (UiElement* m = col.child(2))
                m->set("area", area(0, 13, 18, 13 + mid));
            hk.hide.insert(c);
        }
        place(puck, puckX, puckY, hk);
        for (size_t i = 0; i < kModes.size(); ++i)
            target(puck.child(kModes[i]), Action::Mode, int(i));
        if (const UiElement* bmp = puck.child(0))
            if (auto r = rectOf(bmp))
                solid_.push_back(*r);
        for (const auto& c : puck.children)
            if (c.clsid() == "GZWinBMP" && c.visible())
                if (auto r = rectOf(&c))
                    solid_.push_back(*r);
        if (!column.roots.empty()) {
            UiDrawHooks ch;
            const auto ca = column.roots.front().area();
            place(column.roots.front(), puckX + float(ca[0]), puckY + float(ca[1]), ch);
            // The thumbnails, centred on the column's middle piece.
            const RgbaImage* moodGradient = image(kMoodGradient);
            const UiElement& m = member_.roots.front();
            const float mx = puckX + float(ca[0]) + 9 - float(m.area()[2] - m.area()[0]) / 2;
            float my = puckY + float(ca[1]) + 13;
            for (vm::ObjectId id : members) {
                const vm::Object* o = vm_->object(id);
                UiDrawHooks th;
                if (const UiElement* grp = m.child(0x1000)) {
                    if (const UiElement* t = grp->child(1)) {
                        if (auto p = portraits_.find(id); p != portraits_.end())
                            th.images[t] = p->second.get();
                        else
                            th.hide.insert(t);
                    }
                    if (const UiElement* bgw = grp->child(3); bgw && moodGradient && o && o->person)
                        th.colors[bgw] = moodColor(*moodGradient, o->person->motives[vm::motive::Mood]);
                }
                // 0x3000 marks the selected Sim, 0x2000 the others (inferred).
                if (const UiElement* b = m.child(0x2000))
                    (id == sim_ ? th.hide : th.show).insert(b);
                if (const UiElement* b = m.child(0x3000))
                    (id == sim_ ? th.show : th.hide).insert(b);
                place(m, mx, my, th);
                targets_.push_back({{mx, my, mx + 40, my + 43}, Action::SelectSim, id});
                solid_.push_back({mx, my, mx + 40, my + 43});
                my += 43;
            }
        }
    }

    // --- The action queue (docs/ui-live-mac.md §6) ----------------------------
    hoverIcon_ = -1;
    if (mode_ == 0 && sim && !icon_.roots.empty()) {
        auto constant = [&](const char* k, float d) {
            auto it = queueConstants_.find(k);
            return it == queueConstants_.end() ? d : std::stof(it->second);
        };
        const float x0 = constant("kLocationX", 60), y0 = constant("kLocationY", 10),
                    margin = constant("kIconMarginX", 5);
        std::vector<int> order;
        if (sim->activeInteraction >= 0 && size_t(sim->activeInteraction) < sim->queue.size())
            order.push_back(sim->activeInteraction);
        for (size_t i = 0; i < sim->queue.size() && order.size() < 12; ++i)
            if (int(i) != sim->activeInteraction)
                order.push_back(int(i));
        const UiElement& top = icon_.roots.front();
        const float w = float(top.area()[2] - top.area()[0]), h = float(top.area()[3] - top.area()[1]);
        float x = x0;
        std::string hoverName;
        std::array<float, 4> hoverRect{};
        for (size_t k = 0; k < order.size(); ++k) {
            const auto& q = sim->queue[size_t(order[k])];
            const bool running = order[k] == sim->activeInteraction;
            const std::array<float, 4> r{x, y0, x + w, y0 + h};
            const bool hovered = mouse_ && inside(r, *mouse_);
            if (hovered)
                hoverIcon_ = int(k), hoverName = q.name, hoverRect = r;
            UiDrawHooks hk;
            const RgbaImage* ic = queueIcon(q, running);
            if (const UiElement* c = top.child(1))
                hk.show.insert(c);
            if (const UiElement* c = top.child(2))
                hk.hide.insert(c);
            if (const UiElement* c = top.child(3)) {
                if (ic)
                    hk.images[c] = ic;
                else
                    hk.hide.insert(c);
            }
            if (const UiElement* c = top.child(7))
                (ic ? hk.hide : hk.show).insert(c);
            const bool cancelled = q.cancelled || cancelled_.count({sim_, q.uid});
            if (const UiElement* c = top.child(4))
                (cancelled ? hk.show : hk.hide).insert(c);
            if (const UiElement* c = top.child(5))
                (running ? hk.show : hk.hide).insert(c);
            if (const UiElement* c = top.child(6))
                (hovered ? hk.show : hk.hide).insert(c);
            place(top, x, y0, hk);
            targets_.push_back({r, Action::Cancel, q.uid});
            solid_.push_back(r);
            x += w + margin;
        }
        // The interaction's name under the hovered icon (the game opens a
        // cTSWinCalloutBox; its look is inferred).
        if (!hoverName.empty()) {
            const std::string name = hoverName.substr(hoverName.rfind('/') == std::string::npos ? 0 : hoverName.rfind('/') + 1);
            UiElement t;
            t.tag = "LEGACY";
            t.attrs = {{"clsid", "GZWinText"},          {"caption", name},         {"font", "LiveModePanelPuck"},
                       {"forecolor", "(5,6,113)"},       {"opaque", "yes"},         {"bkgcolor", "(178,193,255)"},
                       {"align", "center"},              {"gutters", "(4,2)"},      {"winflag_visible", "yes"}};
            float ppem = 12;
            float tw = 120;
            if (const FontStyle* st = fonts_->style("LiveModePanelPuck"))
                if (const Font* f = fonts_->font(*st))
                    ppem = float(st->size), tw = measureText(*f, decodeUtf8(name), ppem).width + 12;
            const float tx = std::max(0.0f, hoverRect[0] + w / 2 - tw / 2), ty = hoverRect[3] + 2;
            t.attrs.emplace_back("area", area(0, 0, int(std::ceil(tw)), int(std::ceil(ppem * 1.6f))));
            UiDrawHooks th;
            layoutUiElement(t, tx, ty, *res_, *fonts_, out, th);
        }
    }
    return out;
}

void LiveUi::layoutIfNeeded() {
    if (canvasW_ > 0 && canvasH_ > 0 && (laidW_ != canvasW_ || laidH_ != canvasH_ || targets_.empty()))
        screen(canvasW_, canvasH_);
}

void LiveUi::setSpeed(int speed) {
    // SetSimulatorSpeed: already at v and paused only unpauses.
    vm_->globals[vm::global::Speed] = int16_t(std::clamp(speed, 2, 4)), vm_->globals[vm::global::Paused] = 0;
    std::printf("opensims2: live UI: speed %d\n", speed);
    dirty_ = true;
}

void LiveUi::togglePause() {
    auto& paused = vm_->globals[vm::global::Paused];
    paused = paused ? 0 : 1;
    std::printf("opensims2: live UI: %s\n", paused ? "paused" : "unpaused");
    dirty_ = true;
}

bool LiveUi::press(simd_float2 p) {
    if (!ok_)
        return false;
    layoutIfNeeded();
    for (const Target& t : targets_) {
        if (!inside(t.rect, p))
            continue;
        switch (t.action) {
        case Action::Speed: setSpeed(t.value); break;
        case Action::Pause: togglePause(); break;
        case Action::SelectSim:
            if (vm::ObjectId(t.value) != sim_) {
                std::printf("opensims2: live UI: select Sim %d\n", t.value);
                if (onSelectSim)
                    onSelectSim(vm::ObjectId(t.value));
                setSim(vm::ObjectId(t.value));
            }
            break;
        case Action::Cancel:
            if (vm::Object* sim = vm_->object(sim_); sim && !cancelled_.count({sim_, uint16_t(t.value)})) {
                const bool ok = vm::cancelInteraction(*vm_, *sim, uint16_t(t.value));
                std::printf("opensims2: live UI: cancel queue entry %d: %s\n", t.value, ok ? "ok" : "refused");
                if (ok)
                    cancelled_.insert({sim_, uint16_t(t.value)});
            }
            break;
        case Action::Mode:
            std::printf("opensims2: live UI: mode button %d%s\n", t.value,
                        t.value == 0 ? " (live)" : onMode && (t.value == 1) ? " (buy)" : " (inert)");
            if (onMode)
                onMode(t.value);
            break;
        case Action::None: break;
        }
        std::fflush(stdout);
        dirty_ = true;
        return true;
    }
    for (const auto& r : solid_)
        if (inside(r, p))
            return true;
    return false;
}

void LiveUi::hover(simd_float2 p) {
    mouse_ = p;
    layoutIfNeeded();
    int over = -1, k = 0;
    for (const Target& t : targets_)
        if (t.action == Action::Cancel) {
            if (inside(t.rect, p))
                over = k;
            ++k;
        }
    if (over != hoverIcon_)
        dirty_ = true;
}

void LiveUi::attach(MTL::Device* device, uint32_t colorFormat, uint32_t depthFormat) {
    for (auto& l : layers_)
        l = std::make_unique<UiLayer>(device, colorFormat, depthFormat);
}

void LiveUi::encode(MTL::RenderCommandEncoder* enc, uint32_t width, uint32_t height, float scale) {
    if (!layers_[0] || width == 0 || height == 0 || !ok_)
        return;
    const int cw = int(std::lround(float(width) / std::max(scale, 0.01f)));
    const int ch = int(std::lround(float(height) / std::max(scale, 0.01f)));
    if (cw != canvasW_ || ch != canvasH_)
        canvasW_ = cw, canvasH_ = ch, dirty_ = true;
    if (const uint64_t s = stamp(); s != stamp_)
        stamp_ = s, dirty_ = true;
    if (dirty_) {
        layer_ = (layer_ + 1) % layers_.size();
        const UiScreen sc = screen(cw, ch);
        if (const char* dump = std::getenv("OS2_LIVE_UI_DUMP")) // debug: the screen drawn on the CPU
            writePng(dump, composeUi(sc, {0, 0, 0, 0}));
        layers_[layer_]->setScreen(sc);
        stamp_ = stamp();
        dirty_ = false;
    }
    layers_[layer_]->encode(enc, width, height);
}

} // namespace os2
