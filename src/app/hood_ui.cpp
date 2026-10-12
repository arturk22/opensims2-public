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

constexpr uint32_t kPackageScript = 0xEFFF4500, kHudScript = 0x49000000, kHoverScript = 0x49001011, kDialogScript = 0x49001000;
constexpr uint32_t kPuck = 0x4BE6ED7D, kChooser = 0x4BE6ED7E, kCityName = 0x3;
constexpr uint32_t kFamiliesButton = 0x2BE6F563, kLotsButton = 0x4BE6F571, kOptionsButton = 0x2BF08BEC;
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
    load(kPackageScript, packageScript_);
    bins_ = std::make_unique<HoodBins>(*res_, *fonts_);
    bins_->setNeighborhood(&view.neighborhood());
    options_ = std::make_unique<OptionsPanel>(index, resDir, GameSettings{});
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
    if (lot != dialog_)
        advanced_ = false, member_ = 0;
    dialog_ = lot;
    dirty_ = true;
    if (lot && lot->family && renderHead) { // the members' head shots, rendered where the files have none
        const hood::Neighborhood& h = view_->neighborhood();
        renderHeads_ = true;
        if (const hood::Family* f = h.family(lot->family))
            for (uint16_t m : f->members)
                if (const hood::Neighbor* n = h.neighbor(m); n && n->character >= 0)
                    memberThumb(h.characters[size_t(n->character)].path);
        renderHeads_ = false;
    }
}

const RgbaImage* HoodUi::memberThumb(const std::filesystem::path& character) {
    const std::string key = character.string();
    if (auto it = memberThumbs_.find(key); it != memberThumbs_.end())
        return it->second.get();
    std::unique_ptr<RgbaImage> img;
    try {
        // The character file's head shot: the IMG whose instance is an age
        // bit (8 adult, 0x10 elder, ...; the age bit << 8 is the
        // silhouette, inferred from the files).
        if (!character.empty() && std::filesystem::exists(character)) {
            DbpfPackage pkg(character);
            for (const auto& e : pkg.entries())
                if (e.tgi.type == kTypeImg && e.tgi.instance > 0 && e.tgi.instance < 0x100 && !img)
                    if (auto d = decodeUiImage(pkg.read(e)))
                        img = std::make_unique<RgbaImage>(std::move(*d));
        }
        if (!img && renderHeads_ && renderHead && !character.empty())
            if (auto d = renderHead(character))
                img = std::make_unique<RgbaImage>(std::move(*d));
    } catch (const std::exception&) {
    }
    if (!img && !renderHeads_ && renderHead)
        return nullptr; // not rendered yet: do not cache the miss
    return (memberThumbs_[key] = std::move(img)).get();
}

HoodUi::Action HoodUi::press(simd_float2 p) {
    if (laidW_ != canvasW_ || laidH_ != canvasH_ || dirty_ || bins_->dirty())
        screen(canvasW_, canvasH_); // targets for the current canvas
    binRequest_ = {};
    options_->setCanvas(canvasW_, canvasH_);
    if (!bins_->modal() && options_->active() && options_->press(p) != OptionsPanel::Action::None)
        return Action::Consumed;
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
            if (t.action == Action::Info)
                advanced_ = !advanced_, dirty_ = true;
            if (t.action == Action::Package) { // value 0 opens, -1 closes (Aceitar, Cancelar), 1 and 2 pick
                if (t.value == 0)
                    packaging_ = true;
                else if (t.value < 0)
                    packaging_ = false;
                else
                    packagePick_ = t.value;
                dirty_ = true;
                return Action::Consumed;
            }
            if (t.action == Action::Member)
                member_ = size_t(t.value), dirty_ = true;
            if (t.action == Action::Families || t.action == Action::Lots || t.action == Action::Options) {
                bins_->togglePanel(t.action == Action::Families ? HoodBins::Panel::Families
                                   : t.action == Action::Lots   ? HoodBins::Panel::Lots
                                                                : HoodBins::Panel::Options);
                dirty_ = true;
            }
            return t.action;
        }
    if (!dialog_)
        if (auto r = bins_->press(p); r.kind != HoodBins::Request::Kind::None) {
            if (r.kind == HoodBins::Request::Kind::OptionWindow) {
                options_->toggleWindow(int(r.lot));
                dirty_ = true;
                return Action::Consumed;
            }
            binRequest_ = r;
            dirty_ = true;
            return r.kind == HoodBins::Request::Kind::Consumed ? Action::Consumed : Action::Bins;
        }
    for (const auto& r : solid_)
        if (inside(r, p))
            return Action::Consumed;
    return Action::None;
}

bool HoodUi::covers(simd_float2 p) {
    if (laidW_ != canvasW_ || laidH_ != canvasH_ || dirty_ || bins_->dirty())
        screen(canvasW_, canvasH_);
    for (const auto& r : solid_)
        if (inside(r, p))
            return true;
    return bins_->hits(p) || options_->covers(p);
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
    std::pair<size_t, size_t> puckQuads{0, 0};
    const UiElement* puckElement = hudRoot.child(kPuck);
    if (const UiElement* puck = puckElement) {
        const auto a = puck->area();
        UiDrawHooks hk;
        if (const UiElement* f = puck->child(kFamiliesButton))
            hk.buttonState[f] = bins_->panel() == HoodBins::Panel::Families ? 2 : 1;
        if (const UiElement* l = puck->child(kLotsButton))
            hk.buttonState[l] = bins_->panel() == HoodBins::Panel::Lots ? 2 : 1;
        if (const UiElement* l = puck->child(kOptionsButton))
            hk.buttonState[l] = bins_->panel() == HoodBins::Panel::Options ? 2 : 1;
        puckQuads.first = out.quads.size();
        place(*puck, 2, float(height - (a[3] - a[1]) - 2), hk);
        puckQuads.second = out.quads.size();
        if (auto r = rects.find(puck); r != rects.end())
            puckRect = r->second;
        target(puck->child(kFamiliesButton), Action::Families);
        target(puck->child(kLotsButton), Action::Lots);
        target(puck->child(kOptionsButton), Action::Options);
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

    // The Families / Lots & Houses panels hang from the puck's right edge,
    // under it: their art reaches left under the puck, which is drawn over
    // them as in the game (the puck's quads moved after the panels'), and
    // the question box over both.
    if (puckRect && puckElement) {
        size_t panelsEnd = out.quads.size();
        bins_->layout(out, *puckRect, *puckElement, rects, width, height, &panelsEnd);
        if (bins_->panel() == HoodBins::Panel::Options) {
            const simd_float2 bl = bins_->optionsBottomLeft();
            options_->setPanelBottomLeft(bl.x, bl.y);
        } else if (options_->active()) {
            options_->closeWindow();
        }
        if (puckQuads.second > puckQuads.first && panelsEnd > puckQuads.second && panelsEnd <= out.quads.size())
            std::rotate(out.quads.begin() + std::ptrdiff_t(puckQuads.first),
                        out.quads.begin() + std::ptrdiff_t(puckQuads.second),
                        out.quads.begin() + std::ptrdiff_t(panelsEnd));
    }

    // The Lot Info Dialog, centred.
    if (dialog_) {
        const HoodLot& lot = *dialog_;
        const UiElement& root = dialogScript_.roots.front();
        UiDrawHooks hk;
        const uint32_t panel = lot.type == 1 ? 0xE000 : lot.family ? (advanced_ ? 0xC000 : 0xB000) : 0xD000;
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
        } else if (panel == 0xC000) {
            // "Advanced": the family, the lot, the net worth and a head shot
            // per member (up to 8); the picked member's name and
            // aspiration (person data 46: 2 family, 64 grow up, 4 fortune,
            // 32 knowledge, 16 popularity, 1 romance -> C201-C206).
            if (auto* e = child(0xC006))
                hk.captions[e] = lot.familyName;
            if (auto* e = child(0xC007))
                hk.captions[e] = lot.name;
            if (auto* e = child(0xC008)) {
                if (lotValues(lot).ok)
                    hk.captions[e] = formatMoney(netWorth(lot));
                else
                    hk.hide.insert(e);
            }
            const hood::Neighborhood& h = view_->neighborhood();
            if (const hood::Family* f = h.family(lot.family); f && !f->description.empty() && pe)
                for (const auto& c : pe->children)
                    if (c.get("clsid") == "GZWinTextEdit")
                        hk.captions[&c] = f->description;
            std::vector<const hood::Neighbor*> members;
            if (const hood::Family* f = h.family(lot.family))
                for (uint16_t m : f->members)
                    if (const hood::Neighbor* n = h.neighbor(m); n && members.size() < 8)
                        members.push_back(n);
            member_ = members.empty() ? 0 : std::min(member_, members.size() - 1);
            for (size_t k = 0; k < 8; ++k) {
                const UiElement* win = pe ? findWindow(*pe, 0xC111 + uint32_t(k)) : nullptr;
                const UiElement* btn = pe ? findWindow(*pe, 0xC101 + uint32_t(k)) : nullptr;
                if (k >= members.size()) {
                    if (win)
                        hk.hide.insert(win);
                    if (btn)
                        hk.hide.insert(btn);
                    continue;
                }
                const hood::Neighbor& n = *members[k];
                if (win)
                    if (const UiElement* pic = win->child(1))
                        if (n.character >= 0)
                            if (const RgbaImage* img = memberThumb(h.characters[size_t(n.character)].path))
                                hk.images[pic] = img;
                if (btn)
                    hk.buttonState[btn] = k == member_ ? 2 : 1;
            }
            if (!members.empty()) {
                const hood::Neighbor& n = *members[member_];
                if (auto* e = child(0xC100) ? findWindow(*child(0xC100), 0xC110) : nullptr)
                    hk.captions[e] = n.name();
                if (auto* e = child(0xC100) ? findWindow(*child(0xC100), 0xC120) : nullptr)
                    hk.captions[e] = "";
                const int16_t asp = n.description.personData[hood::sdsc::Aspiration];
                const std::pair<int16_t, uint32_t> icons[] = {{2, 0xC201},  {64, 0xC202}, {4, 0xC203},
                                                              {32, 0xC204}, {16, 0xC205}, {1, 0xC206}};
                for (auto [bit, id] : icons)
                    if (const UiElement* e = pe ? findWindow(*pe, id) : nullptr)
                        (asp == bit ? hk.show : hk.hide).insert(e);
            }
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
        const uint32_t enter = panel == 0xB000 ? 0xB002 : panel == 0xC000 ? 0xC002 : panel == 0xD000 ? 0xD005 : 0xE005;
        const uint32_t close = panel == 0xB000 ? 0xB00A : panel == 0xC000 ? 0xC00A : panel == 0xD000 ? 0xD007 : 0xE007;
        target(child(enter), Action::EnterLot);
        target(child(close), Action::Close);
        if (panel == 0xB000 || panel == 0xC000)
            target(child(panel + 4), Action::Info);
        if (panel == 0xB000 || panel == 0xC000) // "Ver Amigos da Família e Parentes"
            target(child(panel + 3), Action::Friends);
        {
            // "Transferir Lote": C00B, D006, E006.
            const uint32_t pk = panel == 0xC000 ? 0xC00B : panel == 0xD000 ? 0xD006 : panel == 0xE000 ? 0xE006 : 0;
            if (pk)
                if (const UiElement* b = child(pk))
                    if (auto r = rects.find(b); r != rects.end())
                        targets_.push_back({r->second, Action::Package, uiButtonSound(*b), 0});
        }
        if (panel == 0xC000)
            for (size_t k = 0; k < 8; ++k)
                if (const UiElement* b = pe ? findWindow(*pe, 0xC101 + uint32_t(k)) : nullptr)
                    if (auto r = rects.find(b); r != rects.end())
                        targets_.push_back({r->second, Action::Member, uiButtonSound(*b), int(k)});
        // The dialog panel takes the clicks (the root is larger than what is drawn).
        solid_.pop_back();
        if (pe)
            if (auto r = rects.find(pe); r != rects.end())
                solid_.push_back(r->second);
    }
    // The "Transferir Lote" dialog over everything, modal.
    if (packaging_ && !packageScript_.roots.empty()) {
        targets_.clear();
        solid_.clear();
        solid_.push_back({0, 0, float(width), float(height)});
        const UiElement& root = packageScript_.roots.front();
        UiDrawHooks hk;
        for (uint32_t id : {1u, 2u})
            if (const UiElement* b = root.child(id))
                hk.buttonState[b] = packagePick_ == int(id) ? 2 : 1;
        const auto a = root.area();
        place(root, std::round((float(width) - float(a[2] - a[0])) / 2), std::round((float(height) - float(a[3] - a[1])) / 2), hk);
        solid_.erase(solid_.end() - 1); // place() added the dialog's own rectangle
        for (auto [id, v] : {std::pair{1u, 1}, {2u, 2}, {0xA000u, -1}, {0xA001u, -1}})
            if (const UiElement* b = root.child(id))
                if (auto r = rects.find(b); r != rects.end())
                    targets_.push_back({r->second, Action::Package, uiButtonSound(*b), v});
    }
    dirty_ = false;
    return out;
}

void HoodUi::attach(MTL::Device* device, uint32_t colorFormat, uint32_t depthFormat) {
    for (auto& l : layers_)
        l = std::make_unique<UiLayer>(device, colorFormat, depthFormat);
    options_->attach(device, colorFormat, depthFormat);
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
    options_->encode(enc, width, height, scale); // under the puck and its panel, which stay drawn over its tab
    layers_[layer_]->encode(enc, width, height);
}

} // namespace os2
