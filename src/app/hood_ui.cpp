#include "hood_ui.hpp"

#include "app/hood_view.hpp"
#include "formats/dbpf.hpp"
#include "formats/font.hpp"
#include "formats/resindex.hpp"
#include "formats/ui_image.hpp"
#include "formats/ui_resources.hpp"
#include "render/ui_layer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace os2 {

namespace {

constexpr uint32_t kHudScript = 0x49000000, kHoverScript = 0x49001011, kDialogScript = 0x49001000;
constexpr uint32_t kPuck = 0x4BE6ED7D, kChooser = 0x4BE6ED7E, kCityName = 0x3;
constexpr uint32_t kTypeImg = 0x856DDBAC, kTypeFamilyThumb = 0x8C3CE95A;
constexpr uint32_t kLotPicture = 0x35CA0000; // the lot package's first snapshot (300x300 JPEG)

bool inside(const std::array<float, 4>& r, simd_float2 p) {
    return p.x >= r[0] && p.x < r[2] && p.y >= r[1] && p.y < r[3];
}

std::string sizeText(const HoodLot& lot) {
    return "(" + std::to_string(lot.width) + "x" + std::to_string(lot.depth) + ")";
}

} // namespace

HoodUi::HoodUi(const HoodView& view, ResourceIndex& index, const std::filesystem::path& resDir)
    : view_(&view), res_(std::make_unique<UiResources>(index, resDir)),
      fonts_(std::make_unique<FontLibrary>(resDir / "UI/Fonts")) {
    auto load = [&](uint32_t id, UiScript& out) {
        if (auto s = res_->script(id); s && !s->roots.empty()) {
            out = std::move(*s);
            return true;
        }
        return false;
    };
    const bool a = load(kHudScript, hud_), b = load(kHoverScript, hoverScript_), c = load(kDialogScript, dialogScript_);
    ok_ = a && b && c;
    const auto& h = view.neighborhood();
    familyThumbs_ = h.package.parent_path() / "Thumbnails" / (h.prefix + "_FamilyThumbnails.package");
}

HoodUi::~HoodUi() = default;

const RgbaImage* HoodUi::familySnapshot(uint32_t family) {
    auto it = snapshots_.find(family);
    if (it != snapshots_.end())
        return it->second.get();
    std::unique_ptr<RgbaImage> img;
    try {
        if (std::filesystem::exists(familyThumbs_)) {
            DbpfPackage pkg(familyThumbs_);
            for (const auto& e : pkg.entries())
                if (e.tgi.type == kTypeFamilyThumb && e.tgi.instance == family)
                    if (auto d = decodeUiImage(pkg.read(e)))
                        img = std::make_unique<RgbaImage>(std::move(*d));
        }
    } catch (const std::exception&) {
    }
    return (snapshots_[family] = std::move(img)).get();
}

const RgbaImage* HoodUi::lotPicture(const HoodLot& lot) {
    auto it = pictures_.find(lot.id);
    if (it != pictures_.end())
        return it->second.get();
    std::unique_ptr<RgbaImage> img;
    try {
        if (!lot.package.empty() && std::filesystem::exists(lot.package)) {
            DbpfPackage pkg(lot.package);
            for (const auto& e : pkg.entries())
                if (e.tgi.type == kTypeImg && e.tgi.instance == kLotPicture)
                    if (auto d = decodeUiImage(pkg.read(e)))
                        img = std::make_unique<RgbaImage>(std::move(*d));
        }
    } catch (const std::exception&) {
    }
    return (pictures_[lot.id] = std::move(img)).get();
}

void HoodUi::hoverLot(const HoodLot* lot, simd_float2 mouse) {
    if (lot != hover_ || (lot && simd_distance(mouse, mouse_) > 0.5f))
        dirty_ = true;
    hover_ = lot;
    mouse_ = mouse;
}

void HoodUi::openLot(const HoodLot* lot) {
    dialog_ = lot;
    dirty_ = true;
}

HoodUi::Action HoodUi::press(simd_float2 p) {
    if (laidW_ != canvasW_ || laidH_ != canvasH_ || dirty_)
        screen(canvasW_, canvasH_); // targets for the current canvas
    for (const auto& t : targets_)
        if (inside(t.rect, p)) {
            if (t.action == Action::Close)
                openLot(nullptr);
            return t.action;
        }
    for (const auto& r : solid_)
        if (inside(r, p))
            return Action::Consumed;
    return Action::None;
}

void HoodUi::hover(simd_float2 p) {
    mouse_ = p;
}

UiScreen HoodUi::screen(int width, int height) {
    UiScreen out;
    out.width = width, out.height = height;
    laidW_ = width, laidH_ = height;
    targets_.clear();
    solid_.clear();
    if (!ok_)
        return out;
    std::map<const UiElement*, std::array<float, 4>> rects;
    auto place = [&](const UiElement& e, float x, float y, UiDrawHooks& hk) {
        const auto a = e.area();
        hk.rects = &rects;
        layoutUiElement(e, x - float(a[0]), y - float(a[1]), *res_, *fonts_, out, hk);
        if (auto r = rects.find(&e); r != rects.end())
            solid_.push_back(r->second);
    };
    auto target = [&](const UiElement* e, Action a) {
        if (auto r = e ? rects.find(e) : rects.end(); r != rects.end())
            targets_.push_back({r->second, a});
    };

    // HUD: the chooser at the top left, the puck at the bottom left.
    const UiElement& hudRoot = hud_.roots.front();
    if (const UiElement* chooser = hudRoot.child(kChooser)) {
        UiDrawHooks hk;
        if (const UiElement* name = chooser->child(kCityName))
            hk.captions[name] = view_->displayName();
        place(*chooser, 4, 0, hk);
    }
    if (const UiElement* puck = hudRoot.child(kPuck)) {
        const auto a = puck->area();
        UiDrawHooks hk;
        place(*puck, 2, float(height - (a[3] - a[1]) - 2), hk);
    }

    // Hover popup next to the mouse (not while the dialog is open).
    if (hover_ && !dialog_) {
        const HoodLot& lot = *hover_;
        const UiElement& root = hoverScript_.roots.front();
        UiDrawHooks hk;
        const uint32_t panel = lot.type == 1 ? 0xC000 : lot.family ? 0xA000 : 0xB000;
        for (uint32_t id : {0xA000u, 0xB000u, 0xC000u})
            if (const UiElement* e = root.child(id))
                (id == panel ? hk.show : hk.hide).insert(e);
        if (const UiElement* pe = root.child(panel)) {
            auto caption = [&](uint32_t id, const std::string& text) {
                if (const UiElement* e = pe->child(id))
                    hk.captions[e] = text;
            };
            auto hide = [&](uint32_t id) {
                if (const UiElement* e = pe->child(id))
                    hk.hide.insert(e);
            };
            const RgbaImage* pic = lotPicture(lot);
            if (panel == 0xA000) {
                caption(0xA001, lot.familyName);
                caption(0xA002, lot.name);
                hide(0xA003); // value (not computed)
                caption(0xA005, sizeText(lot));
                const auto* fam = view_->neighborhood().family(lot.family);
                if (fam && !fam->description.empty())
                    caption(0xA006, fam->description);
                if (const UiElement* e = pe->child(0xA007); e && pic)
                    hk.images[e] = pic;
            } else {
                const uint32_t base = panel; // B000 / C000
                caption(base + 1, lot.name);
                hide(base + 2);
                caption(base + 3, sizeText(lot));
                if (const UiElement* e = pe->child(base + 4); e && pic)
                    hk.images[e] = pic;
                if (panel == 0xC000 && !lot.description.empty())
                    caption(0xC005, lot.description);
            }
        }
        const auto a = root.area();
        const float w = float(a[2] - a[0]), h = float(a[3] - a[1]);
        float x = mouse_.x + 16, y = mouse_.y + 16 - 72; // the popup's art starts 72 px down
        x = std::clamp(x, 0.0f, std::max(0.0f, float(width) - w));
        y = std::clamp(y, -72.0f, std::max(-72.0f, float(height) - h));
        const size_t solidBefore = solid_.size();
        place(root, x, y, hk);
        solid_.resize(solidBefore); // the popup does not take clicks
    }

    // The Lot Info Dialog, centred.
    if (dialog_) {
        const HoodLot& lot = *dialog_;
        const UiElement& root = dialogScript_.roots.front();
        UiDrawHooks hk;
        const uint32_t panel = lot.type == 1 ? 0xE000 : lot.family ? 0xB000 : 0xD000;
        for (uint32_t id : {0xA000u, 0xB000u, 0xC000u, 0xD000u, 0xE000u})
            if (const UiElement* e = root.child(id))
                (id == panel ? hk.show : hk.hide).insert(e);
        const UiElement* pe = root.child(panel);
        auto child = [&](uint32_t id) { return pe ? pe->child(id) : nullptr; };
        if (panel == 0xB000) {
            if (auto* e = child(0xB006))
                hk.captions[e] = lot.familyName;
            if (auto* e = child(0xB007))
                hk.captions[e] = lot.name;
            if (auto* e = child(0xB008))
                hk.hide.insert(e);
            if (auto* e = child(0xB005))
                if (const RgbaImage* snap = familySnapshot(lot.family))
                    hk.images[e] = snap;
        } else if (panel == 0xD000) {
            if (auto* e = child(0xD003))
                hk.captions[e] = lot.name;
            if (auto* e = child(0xD004))
                hk.hide.insert(e);
        } else {
            if (auto* e = child(0xE003))
                hk.captions[e] = lot.name;
            if (auto* e = child(0xE004); e && !lot.description.empty())
                hk.captions[e] = lot.description;
        }
        const auto a = pe ? pe->area() : root.area();
        const float w = float(a[2] - a[0]), h = float(a[3] - a[1]);
        place(root, std::round((float(width) - w) / 2), std::round((float(height) - h) / 2), hk);
        const uint32_t enter = panel == 0xB000 ? 0xB002 : panel == 0xD000 ? 0xD005 : 0xE005;
        const uint32_t close = panel == 0xB000 ? 0xB00A : panel == 0xD000 ? 0xD007 : 0xE007;
        target(child(enter), Action::EnterLot);
        target(child(close), Action::Close);
        // The dialog panel takes the clicks (the root is larger than what is drawn).
        solid_.pop_back();
        if (pe)
            if (auto r = rects.find(pe); r != rects.end())
                solid_.push_back(r->second);
    }
    dirty_ = false;
    return out;
}

void HoodUi::attach(MTL::Device* device, uint32_t colorFormat, uint32_t depthFormat) {
    for (auto& l : layers_)
        l = std::make_unique<UiLayer>(device, colorFormat, depthFormat);
}

void HoodUi::encode(MTL::RenderCommandEncoder* enc, uint32_t width, uint32_t height, float scale) {
    if (!layers_[0] || width == 0 || height == 0 || !ok_)
        return;
    const int cw = int(std::lround(float(width) / std::max(scale, 0.01f)));
    const int ch = int(std::lround(float(height) / std::max(scale, 0.01f)));
    if (cw != canvasW_ || ch != canvasH_)
        canvasW_ = cw, canvasH_ = ch, dirty_ = true;
    if (dirty_ || laidW_ != cw || laidH_ != ch) {
        layer_ = (layer_ + 1) % layers_.size();
        const UiScreen sc = screen(cw, ch);
        layers_[layer_]->setScreen(sc);
    }
    layers_[layer_]->encode(enc, width, height);
}

} // namespace os2
