#include "hood_ui.hpp"

#include "app/hood_view.hpp"
#include "app/ui_panel_util.hpp"
#include "formats/binreader.hpp"
#include "formats/lot_objects.hpp"
#include "formats/dbpf.hpp"
#include "formats/font.hpp"
#include "formats/hood_moves.hpp"
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
constexpr uint32_t kFamiliesButton = 0x2BE6F563, kLotsButton = 0x4BE6F571;
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
    bins_ = std::make_unique<HoodBins>(*res_, *fonts_);
    bins_->setNeighborhood(&view.neighborhood());
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

const HoodUi::LotValues& HoodUi::lotValues(const HoodLot& lot) {
    auto it = values_.find(lot.id);
    if (it != values_.end())
        return it->second;
    LotValues v;
    try {
        if (!lot.package.empty() && std::filesystem::exists(lot.package)) {
            DbpfPackage pkg(lot.package);
            for (const auto& e : pkg.entries())
                if (e.tgi.type == 0x53494D49 && e.tgi.instance == 1) {
                    // cTSSimulator::Read (docs/save-mac.md §5): the name, "SIMI",
                    // the version, a u32, the globals, then the offsets 0xA0, 0x11C,
                    // 0x98, 0xB4 and the random seed, 0x9C, and from version 0x25
                    // the lot's value (0xA4), the family's objects' (0xAC) and
                    // (from 0xAD) the lot's objects' (0xA8).
                    const auto bytes = pkg.read(e);
                    BinReader r(bytes);
                    r.skip(64);
                    r.u32();
                    const uint32_t version = r.u32();
                    r.u32();
                    r.skip(simulatorGlobalCount(version) * 2);
                    if (version < 0x12) // the speed
                        r.u32();
                    r.u32(); // 0xA0
                    r.u16(); // 0x11C
                    if (version > 1)
                        r.u32();
                    if (version > 0xD)
                        r.skip(8);
                    if (version >= 0xF)
                        r.u32();
                    if (version >= 0x25) {
                        v.lot = r.u32();
                        v.familyObjects = r.u32();
                        if (version >= 0xAD)
                            v.lotObjects = r.u32();
                        v.ok = true;
                    }
                    break;
                }
        }
    } catch (const std::exception&) {
    }
    return values_[lot.id] = v;
}

int64_t HoodUi::netWorth(const HoodLot& lot) {
    // cTSFamilyManager::GetFamilyNetWorth: funds + the lot's price.
    return hood::netWorth(view_->neighborhood(), lot.family);
}

void HoodUi::refresh() {
    values_.clear();
    pictures_.clear();
    hover_ = dialog_ = nullptr;
    bins_->setNeighborhood(&view_->neighborhood());
    dirty_ = true;
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
    if (laidW_ != canvasW_ || laidH_ != canvasH_ || dirty_ || bins_->dirty())
        screen(canvasW_, canvasH_); // targets for the current canvas
    binRequest_ = {};
    if (bins_->modal()) {
        binRequest_ = bins_->press(p);
        dirty_ = true;
        return binRequest_.kind == HoodBins::Request::Kind::Consumed ? Action::Consumed : Action::Bins;
    }
    for (const auto& t : targets_)
        if (inside(t.rect, p)) {
            if (playSound && !t.sound.empty())
                playSound(t.sound);
            if (t.action == Action::Close)
                openLot(nullptr);
            if (t.action == Action::Families || t.action == Action::Lots) {
                bins_->togglePanel(t.action == Action::Families ? HoodBins::Panel::Families : HoodBins::Panel::Lots);
                dirty_ = true;
            }
            return t.action;
        }
    if (!dialog_)
        if (auto r = bins_->press(p); r.kind != HoodBins::Request::Kind::None) {
            binRequest_ = r;
            dirty_ = true;
            return r.kind == HoodBins::Request::Kind::Consumed ? Action::Consumed : Action::Bins;
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
            targets_.push_back({r->second, a, uiButtonSound(*e)});
    };

    // HUD: the chooser at the top left, the puck at the bottom left.
    const UiElement& hudRoot = hud_.roots.front();
    if (const UiElement* chooser = hudRoot.child(kChooser)) {
        UiDrawHooks hk;
        if (const UiElement* name = chooser->child(kCityName))
            hk.captions[name] = view_->displayName();
        place(*chooser, 4, 0, hk);
        target(chooser->child(0x2), Action::MainMenu); // "Menu Principal"
    }
    std::optional<std::array<float, 4>> puckRect;
    const UiElement* puckElement = hudRoot.child(kPuck);
    if (const UiElement* puck = puckElement) {
        const auto a = puck->area();
        UiDrawHooks hk;
        if (const UiElement* f = puck->child(kFamiliesButton))
            hk.buttonState[f] = bins_->panel() == HoodBins::Panel::Families ? 2 : 1;
        if (const UiElement* l = puck->child(kLotsButton))
            hk.buttonState[l] = bins_->panel() == HoodBins::Panel::Lots ? 2 : 1;
        place(*puck, 2, float(height - (a[3] - a[1]) - 2), hk);
        if (auto r = rects.find(puck); r != rects.end())
            puckRect = r->second;
        target(puck->child(kFamiliesButton), Action::Families);
        target(puck->child(kLotsButton), Action::Lots);
        // The camera buttons: "Mais Zoom +", "Menos Zoom -" and the two "Rotacionar".
        target(puck->child(0x2BE6FB48), Action::ZoomIn);
        target(puck->child(0x2BE6FB4E), Action::ZoomOut);
        target(puck->child(0x2BE6FB52), Action::RotateLeft);
        target(puck->child(0x2BE6FB57), Action::RotateRight);
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
            const LotValues v = lotValues(lot); // the lot's SIMI
            // The lot's price (GetLotFileInfo: lot, objects and architecture).
            const hood::Lot* hl = view_->neighborhood().lot(lot.id);
            const int64_t price = hl ? hood::lotFileInfo(*hl).price : v.lot;
            if (panel == 0xA000) {
                caption(0xA001, lot.familyName);
                caption(0xA002, lot.name);
                if (v.ok)
                    caption(0xA003, formatMoney(price));
                else
                    hide(0xA003);
                caption(0xA005, sizeText(lot));
                const auto* fam = view_->neighborhood().family(lot.family);
                if (fam && !fam->description.empty())
                    caption(0xA006, fam->description);
                if (const UiElement* e = pe->child(0xA007); e && pic)
                    hk.images[e] = pic;
            } else {
                const uint32_t base = panel; // B000 / C000
                caption(base + 1, lot.name);
                if (v.ok)
                    caption(base + 2, formatMoney(price));
                else
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

    // The Families / Lots & Houses panels over the puck (their question box on top).
    if (puckRect && puckElement) {
        const size_t before = targets_.size();
        bins_->layout(out, *puckRect, *puckElement, rects, width, height);
        (void)before;
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
            if (auto* e = child(0xB008)) { // "Patrimônio": the family's net worth
                if (lotValues(lot).ok)
                    hk.captions[e] = formatMoney(netWorth(lot));
                else
                    hk.hide.insert(e);
            }
            if (auto* e = child(0xB005))
                if (const RgbaImage* snap = familySnapshot(lot.family))
                    hk.images[e] = snap;
        } else if (panel == 0xD000) {
            if (auto* e = child(0xD003))
                hk.captions[e] = lot.name;
            if (auto* e = child(0xD004)) { // an empty lot's value
                if (const hood::Lot* l = view_->neighborhood().lot(lot.id))
                    hk.captions[e] = formatMoney(hood::lotFileInfo(*l).price); // what moving in costs
                else
                    hk.hide.insert(e);
            }
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
    if (dirty_ || bins_->dirty() || laidW_ != cw || laidH_ != ch) {
        layer_ = (layer_ + 1) % layers_.size();
        const UiScreen sc = screen(cw, ch);
        layers_[layer_]->setScreen(sc);
    }
    layers_[layer_]->encode(enc, width, height);
}

} // namespace os2
