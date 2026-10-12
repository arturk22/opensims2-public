#include "hood_bins.hpp"

#include "app/ui_panel_util.hpp"
#include "formats/dbpf.hpp"
#include "formats/font.hpp"
#include "formats/hood_moves.hpp"
#include "formats/neighborhood.hpp"
#include "formats/ui_image.hpp"
#include "formats/ui_resources.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace os2 {

namespace {

constexpr uint32_t kFamiliesScript = 0x49001004, kLotsScript = 0x49001003, kItemScript = 0x49001006,
                   kDialogScript = 0x49001035, kCardScript = 0x49001011, kPlaceLotScript = 0x49001002, kOptionsScript = 0x49001028,
                   kDecorScript = 0x4D517180;
constexpr uint32_t kTextGroup = 0x7f3a262a; // Text/Neighborhood.package
constexpr uint32_t kPuckAnchor = 0xAABBCCDD, kPanelAnchor = 0xAAAA;
constexpr uint32_t kBin = 0x0B, kCreateFamily = 0x0A, kMoveFamily = 0x6D13DDA1;
constexpr uint32_t kBulldoze = 0xB000;
constexpr uint32_t kTypeFamilyThumb = 0x8C3CE95A;

std::string area(int x0, int y0, int x1, int y1) { return uiArea(x0, y0, x1, y1); }

// printf with one %s.
std::string fill(const std::string& pattern, const std::string& value) {
    const auto at = pattern.find("%s");
    if (at == std::string::npos)
        return pattern;
    return pattern.substr(0, at) + value + pattern.substr(at + 2);
}

// The panel's top left: its "aabbccdd:tl" window on the puck's 0xAABBCCDD.
simd_float2 anchoredOrigin(const UiElement& panel, const std::array<float, 4>& puckRect, const UiElement& puck) {
    simd_float2 o = simd_make_float2(puckRect[0], puckRect[1]);
    if (const UiElement* a = findWindow(puck, kPuckAnchor)) {
        const auto aa = a->area(); // relative to the puck's top left
        o += simd_make_float2(float(aa[0]), float(aa[1]));
    }
    if (const UiElement* b = findWindow(panel, kPanelAnchor)) {
        const auto ba = b->area();
        o -= simd_make_float2(float(ba[0]), float(ba[1]));
    }
    return o;
}

} // namespace

HoodBins::HoodBins(UiResources& res, FontLibrary& fonts) : res_(&res), fonts_(&fonts) {
    auto load = [&](uint32_t id, UiScript& out) {
        if (auto s = res_->script(id); s && !s->roots.empty()) {
            out = std::move(*s);
            return true;
        }
        return false;
    };
    const bool a = load(kFamiliesScript, families_), b = load(kLotsScript, lots_), c = load(kItemScript, item_),
               d = load(kDialogScript, dialogScript_);
    load(kCardScript, hover_);
    load(kPlaceLotScript, placeScript_);
    load(kOptionsScript, options_);
    load(kDecorScript, decor_);
    ok_ = a && b && c && d;
}

HoodBins::~HoodBins() = default;

std::string HoodBins::text(uint32_t table, uint32_t index, const char* fallback) const {
    try {
        if (auto t = res_->string(UiStringRef{kTextGroup, table, index}); t && !t->empty())
            return *t;
    } catch (const std::exception&) {
    }
    return fallback;
}

void HoodBins::setNeighborhood(const hood::Neighborhood* hood) {
    hood_ = hood;
    snapshots_.clear();
    if (hood_ && selected_ && !hood_->family(selected_))
        selected_ = 0;
    dirty_ = true;
}

void HoodBins::togglePanel(Panel p) {
    panel_ = panel_ == p ? Panel::None : p;
    cancelTool();
    dirty_ = true;
}

void HoodBins::pickDecoration(uint32_t guid) {
    decorGuid_ = guid;
    tool_ = guid ? Tool::Decorate : Tool::None;
    selected_ = 0;
    dirty_ = true;
}

namespace {
// The sort buttons of the Decorations panel.
constexpr std::array<std::pair<uint32_t, const char*>, 5> kDecorSorts = {{{0xA100, "flora"}, {0xA200, "stone"},
                                                                          {0xA300, "landmark"}, {0xA400, "effects"},
                                                                          {0xA500, "misc"}}};
} // namespace

std::vector<uint32_t> HoodBins::visibleDecorations() const {
    std::vector<uint32_t> all;
    for (const auto& p : decorCatalog_)
        if (p.sort == decorSort_)
            all.push_back(p.guid);
    const size_t per = decorExpanded_ ? 4 : 2;
    const size_t pages = std::max<size_t>(1, (all.size() + per - 1) / per);
    const size_t page = std::min(decorPage_, pages - 1);
    std::vector<uint32_t> out;
    for (size_t k = 0; k < per && page * per + k < all.size(); ++k)
        out.push_back(all[page * per + k]);
    return out;
}

void HoodBins::selectFamily(uint32_t family) {
    selected_ = family;
    tool_ = family ? Tool::MoveIn : Tool::None;
    dirty_ = true;
}

void HoodBins::cancelTool() {
    decorGuid_ = 0;
    tool_ = Tool::None;
    selected_ = 0;
    emptySize_ = binLot_ = -1;
    dirty_ = true;
}

void HoodBins::showMessage(const std::string& message) {
    dialog_ = Dialog{message, {}};
    dirty_ = true;
}

void HoodBins::openPlaceLot() {
    placeLot_ = PlaceLot{};
    placeLot_->name = text(0x83, 28, "<Enter Lot Name>");
    dirty_ = true;
}

HoodBins::Request HoodBins::acceptPlaceLot() {
    if (!placeLot_)
        return {Request::Kind::Consumed};
    const std::string& name = placeLot_->name;
    if (name.find_first_of("\\/:*?<>|;") != std::string::npos) {
        showMessage(text(0x99, 8, "Your lot name cannot include any of the following characters:") + " " +
                    "\\ / : * ? < > | ;");
        return {Request::Kind::Consumed};
    }
    if (hood_)
        for (const auto& l : hood_->lots)
            if ((l.name.empty() ? l.info.name : l.name) == name) {
                showMessage(text(0x82, 7, "There can only be one lot with a particular name in the neighborhood. "
                                          "Please try again!"));
                return {Request::Kind::Consumed};
            }
    placedName_ = name;
    placedZone_ = placeLot_->community ? 1 : 0;
    placeLot_.reset();
    dirty_ = true;
    return {Request::Kind::PlaceLot};
}

HoodBins::Request HoodBins::typeKey(int keyCode, const std::string& typed) {
    if (!textEntry())
        return {};
    dirty_ = true;
    constexpr int kReturn = 36, kEnter = 76, kEscape = 53, kBackspace = 51;
    if (keyCode == kReturn || keyCode == kEnter)
        return acceptPlaceLot();
    if (keyCode == kEscape) {
        placeLot_.reset();
        return {Request::Kind::Consumed};
    }
    std::string& name = placeLot_->name;
    if (!placeLot_->edited) // the default text is selected: a key replaces it
        name.clear(), placeLot_->edited = true;
    if (keyCode == kBackspace) {
        while (!name.empty() && (uint8_t(name.back()) & 0xC0) == 0x80)
            name.pop_back();
        if (!name.empty())
            name.pop_back();
        return {Request::Kind::Consumed};
    }
    for (char c : typed)
        if (uint8_t(c) >= 0x20 && c != 0x7F && name.size() < 64)
            name += c;
    return {Request::Kind::Consumed};
}

void HoodBins::setLotBin(std::vector<hood::BinLot> lots) {
    binLots_ = std::move(lots);
    binPictures_.clear();
    if (binLot_ >= int(binLots_.size()))
        binLot_ = -1;
    dirty_ = true;
}

void HoodBins::pickEmptyLot(int size) {
    selected_ = 0;
    emptySize_ = size;
    binLot_ = -1;
    tool_ = size >= 0 ? Tool::PlaceEmpty : Tool::None;
    dirty_ = true;
}

void HoodBins::pickBinLot(size_t index) {
    selected_ = 0;
    emptySize_ = -1;
    binLot_ = index < binLots_.size() ? int(index) : -1;
    tool_ = binLot_ >= 0 ? Tool::PlaceBin : Tool::None;
    dirty_ = true;
}

const RgbaImage* HoodBins::binPicture(size_t index) {
    if (auto it = binPictures_.find(index); it != binPictures_.end())
        return it->second.get();
    std::unique_ptr<RgbaImage> img;
    if (index < binLots_.size())
        try {
            DbpfPackage pkg(binLots_[index].file);
            for (const auto& e : pkg.entries())
                if (e.tgi.type == 0x856DDBAC && e.tgi.instance == 0x35CA0000)
                    if (auto d = decodeUiImage(pkg.read(e)))
                        img = std::make_unique<RgbaImage>(std::move(*d));
        } catch (const std::exception&) {
        }
    return (binPictures_[index] = std::move(img)).get();
}

void HoodBins::openQuestion(const std::string& message, Request onAccept) {
    dialog_ = Dialog{message, onAccept};
    dirty_ = true;
}

void HoodBins::lotClicked(uint32_t lotId) {
    if (!hood_)
        return;
    const hood::Lot* lot = hood_->lot(lotId);
    if (!lot)
        return;
    const std::string lotName = lot->name.empty() ? lot->info.name : lot->name;
    switch (tool_) {
    case Tool::None: return;
    case Tool::MoveIn: {
        const hood::Family* fam = hood_->family(selected_);
        if (!fam)
            return;
        switch (hood::canMoveIn(*hood_, selected_, lotId)) {
        case hood::MoveInCheck::Ok: {
            // ConfirmAndMoveInFamily: "Move %s family into this lot?" and,
            // unless the lot is a dorm (type 2), "Cost to move in: " + the price.
            std::string m = fill(text(0x82, 5, "Move %s family into this lot?"), fam->name);
            if (lot->info.type != 2)
                m += "\n" + text(0x82, 6, "Cost to move in: ") + formatMoney(hood::lotFileInfo(*lot).price);
            openQuestion(m, {Request::Kind::MoveIn, selected_, lotId});
            break;
        }
        case hood::MoveInCheck::CantAfford:
            showMessage(fill(text(0x82, 2, "The %s family cannot afford this lot."), fam->name));
            break;
        case hood::MoveInCheck::Community:
            showMessage(text(0x82, 3, "Families cannot move into community lots."));
            break;
        case hood::MoveInCheck::Occupied: showMessage(text(0x82, 4, "This lot is already occupied.")); break;
        default: break;
        }
        return;
    }
    case Tool::Evict: {
        const uint32_t f = hood::familyOnLot(*hood_, lotId);
        if (!f)
            return; // nothing to move out: the tool ignores empty lots
        openQuestion(fill(text(0x83, 18,
                               "Move %s family to the Family Bin? (Personal belongings will be sold for simoleons, "
                               "and the family will lose any Rewards they may have earned.)"),
                          hood_->family(f)->name),
                     {Request::Kind::MoveOut, f, lotId});
        return;
    }
    case Tool::PlaceEmpty:
    case Tool::PlaceBin:
    case Tool::MoveLot: return; // the neighborhood mode places
    case Tool::ToBin: {
        // cTSUserToolMoveLotToCatalog: a lot without a house cannot go
        // (STR# 0x83 #26); a lot with one asks #28.
        if (!hood::lotFileInfo(*lot).hasHouse) {
            showMessage(text(0x83, 25, "Cannot move empty lots to the Lots & Houses Bin."));
            return;
        }
        // #28, and with a family living there #31 after it: the family goes
        // with the lot (BV 0x37f554).
        std::string q = fill(text(0x83, 27, "Are you sure you want to move the lot %s to the Lots & Houses Bin?"),
                             lotName);
        if (const uint32_t f = hood::familyOnLot(*hood_, lotId)) {
            std::string more = fill(text(0x83, 30,
                                         "The %s family living in the lot will be moved with the lot, and will lose "
                                         "all of their relationships with their neighbors."),
                                    hood_->family(f)->name);
            while (!more.empty() && (more.front() == '\n' || more.front() == ' '))
                more.erase(more.begin());
            q += " " + more;
        }
        openQuestion(q, {Request::Kind::ToBin, 0, lotId});
        return;
    }
    case Tool::Bulldoze: {
        if (const uint32_t f = hood::familyOnLot(*hood_, lotId)) {
            showMessage(fill(text(0x83, 29,
                                  "You must first move the %s family from this lot into the Family Bin before it can "
                                  "be bulldozed."),
                             hood_->family(f)->name));
            return;
        }
        openQuestion(fill(text(0x83, 26, "Are you sure you want to bulldoze the lot: %s?"), lotName),
                     {Request::Kind::Bulldoze, 0, lotId});
        return;
    }
    }
}

const RgbaImage* HoodBins::familySnapshot(uint32_t family) {
    if (auto it = snapshots_.find(family); it != snapshots_.end())
        return it->second.get();
    std::unique_ptr<RgbaImage> img;
    if (hood_)
        try {
            const auto path = hood_->package.parent_path() / "Thumbnails" / (hood_->prefix + "_FamilyThumbnails.package");
            if (std::filesystem::exists(path)) {
                DbpfPackage pkg(path);
                for (const auto& e : pkg.entries())
                    if (e.tgi.type == kTypeFamilyThumb && e.tgi.instance == family)
                        if (auto d = decodeUiImage(pkg.read(e)))
                            img = std::make_unique<RgbaImage>(std::move(*d));
            }
        } catch (const std::exception&) {
        }
    return (snapshots_[family] = std::move(img)).get();
}

void HoodBins::layout(UiScreen& out, const std::array<float, 4>& puckRect, const UiElement& puck,
                      std::map<const UiElement*, std::array<float, 4>>& puckRects, int width, int height,
                      size_t* panelsEnd) {
    targets_.clear();
    if (panelsEnd)
        *panelsEnd = out.quads.size();
    if (!ok_)
        return;
    std::map<const UiElement*, std::array<float, 4>> rects;
    auto rectOf = [&](const UiElement* e) -> std::optional<std::array<float, 4>> {
        if (!e)
            return std::nullopt;
        auto it = rects.find(e);
        return it == rects.end() ? std::nullopt : std::optional(it->second);
    };
    auto target = [&](const UiElement* e, Hit h, uint32_t v = 0) {
        if (auto r = rectOf(e))
            targets_.push_back({*r, h, v});
    };
    auto textAt = [&](const std::string& s, float x, float y, float w, float h, const char* align,
                      const char* font = "LiveModePanelPuck", const char* color = "(255,255,255)") {
        UiElement t;
        t.tag = "LEGACY";
        t.attrs = {{"clsid", "GZWinText"}, {"caption", s},   {"font", font},       {"forecolor", color},
                   {"align", align},       {"wrapped", "yes"}, {"winflag_visible", "yes"},
                   {"area", area(0, 0, int(w), int(h))}};
        layoutUiElement(t, x, y, *res_, *fonts_, out, {});
    };
    std::vector<Target> solids;
    // The puck's family/lots buttons show which panel is open.
    (void)puckRects;

    if (panel_ == Panel::Families) {
        const UiElement& root = families_.roots.front();
        const simd_float2 o = anchoredOrigin(root, puckRect, puck);
        const auto ra = root.area();
        UiDrawHooks hk;
        hk.rects = &rects;
        const UiElement* bin = root.child(kBin);
        const UiElement* collapsed = bin ? bin->child(0x1000) : nullptr;
        const UiElement* expanded = bin ? bin->child(0x2000) : nullptr;
        if (collapsed && expanded)
            (expanded_ ? hk.show : hk.hide).insert(expanded), (expanded_ ? hk.hide : hk.show).insert(collapsed);
        if (const UiElement* mf = root.child(kMoveFamily))
            hk.buttonState[mf] = tool_ == Tool::Evict ? 2 : 1;
        layoutUiElement(root, o.x - float(ra[0]), o.y - float(ra[1]), *res_, *fonts_, out, hk);
        // The whole panel takes clicks (after its buttons).
        for (const auto& [e, r] : rects)
            if (e->get("clsid") != "GZWinFlatRect")
                solids.push_back({r, Hit::Solid});
        target(root.child(kCreateFamily), Hit::Create);
        target(root.child(kMoveFamily), Hit::Evict);
        const UiElement* view = expanded_ ? expanded : collapsed;
        const uint32_t base = expanded_ ? 0x2000 : 0x1000;
        if (view) {
            target(view->child(base + 4), Hit::Prev);
            target(view->child(base + 5), Hit::Next);
            target(view->child(base + 9), Hit::Expand);
        }
        // The bin's items in its hidden rectangle 0x?006.
        const UiElement* slot = view ? view->child(base + 6) : nullptr;
        if (auto vr = rectOf(view); vr && slot && hood_) {
            const auto sa = slot->area();
            const float x0 = (*vr)[0] + float(sa[0]), y0 = (*vr)[1] + float(sa[1]);
            const float w = float(sa[2] - sa[0]), h = float(sa[3] - sa[1]);
            const int cols = 2, rows = expanded_ ? 2 : 1;
            const float s = std::floor(std::min(w / float(cols), h / float(rows)));
            const auto families = hood::homelessFamilies(*hood_);
            const size_t per = size_t(cols * rows);
            const size_t pages = std::max<size_t>(1, (families.size() + per - 1) / per);
            page_ = std::min(page_, pages - 1);
            const UiElement& item = item_.roots.front();
            for (size_t k = 0; k < per && page_ * per + k < families.size(); ++k) {
                const uint32_t fid = families[page_ * per + k];
                const float ix = x0 + float(k % cols) * (w / float(cols)) + (w / float(cols) - s) / 2;
                const float iy = y0 + float(k / cols) * (h / float(rows)) + (h / float(rows) - s) / 2;
                // The 94 x 93 item scaled to s: snapshot under the overlay button.
                UiElement it = item;
                const float k94 = s / 94.0f;
                auto scale = [&](UiElement& e) {
                    const auto a = e.area();
                    e.set("area", area(int(std::lround(a[0] * k94)), int(std::lround(a[1] * k94)),
                                       int(std::lround(a[2] * k94)), int(std::lround(a[3] * k94))));
                };
                scale(it);
                it.set("area", area(0, 0, int(s), int(s)));
                for (auto& c : it.children)
                    scale(c);
                UiDrawHooks ih;
                std::map<const UiElement*, std::array<float, 4>> irects;
                ih.rects = &irects;
                if (UiElement* bg = it.child(1))
                    if (const RgbaImage* snap = familySnapshot(fid))
                        ih.images[bg] = snap;
                if (UiElement* alpha = it.child(2))
                    ih.hide.insert(alpha);
                if (UiElement* btn = it.child(3))
                    ih.buttonState[btn] = fid == selected_ ? 2 : 1;
                layoutUiElement(it, ix, iy, *res_, *fonts_, out, ih);
                targets_.push_back({{ix, iy, ix + s, iy + s}, Hit::Family, fid});
            }
        }
        // The selected family: name, funds and members, above the panel, in
        // the frame of the "occupied lot catalog description" popup
        // (0x49001011 panel 0xA000, without the lot's picture).
        if (const hood::Family* fam = selected_ && hood_ ? hood_->family(selected_) : nullptr) {
            if (const UiElement* card = hover_.roots.empty() ? nullptr : hover_.roots.front().child(0xA000)) {
                UiElement b = *card;
                std::string members;
                for (uint16_t m : fam->members)
                    if (const hood::Neighbor* n = hood_->neighbor(m))
                        members += (members.empty() ? "" : "\n") + n->name();
                UiDrawHooks bh;
                for (auto& c : b.children) {
                    const uint32_t id = c.id();
                    if (id == 0xA007 || id == 0xA003 || id == 0xA005)
                        c.set("winflag_visible", "no");
                    if (id == 0xA001)
                        c.set("caption", fam->name);
                    if (id == 0xA002)
                        c.set("caption", formatMoney(fam->record.funds));
                    if (id == 0xA006) {
                        c.set("initvalue", members); // what a text edit shows
                        c.children.clear(); // no scroll bar
                    }
                }
                // The frame starts at (10, 82) of the popup.
                const float bx = o.x + 112, by = std::max(4.0f, o.y + 24 - 196);
                b.set("area", area(0, 0, 296, 196));
                for (auto& c : b.children) {
                    const auto ca = c.area();
                    c.set("area", area(ca[0] - 10, ca[1] - 82, ca[2] - 10, ca[3] - 82));
                }
                layoutUiElement(b, bx, by, *res_, *fonts_, out, bh);
            }
        }
    } else if (panel_ == Panel::Decorations && !decor_.roots.empty()) {
        const UiElement& root = decor_.roots.front();
        const simd_float2 o = anchoredOrigin(root, puckRect, puck);
        const auto ra = root.area();
        UiDrawHooks hk;
        hk.rects = &rects;
        const UiElement* bin = root.child(0x2D51756D);
        const UiElement* collapsed = bin ? bin->child(0x1000) : nullptr;
        const UiElement* expanded = bin ? bin->child(0x2000) : nullptr;
        if (collapsed && expanded)
            (decorExpanded_ ? hk.show : hk.hide).insert(expanded), (decorExpanded_ ? hk.hide : hk.show).insert(collapsed);
        hk.hide.insert(root.child(0xB5)); // the end cap (hidden in the script)
        for (const auto& [id, sort] : kDecorSorts)
            if (const UiElement* e = root.child(0xA000) ? root.child(0xA000)->child(id) : nullptr)
                hk.buttonState[e] = decorSort_ == sort ? 2 : 1;
        if (const UiElement* e = root.child(0xA5))
            hk.buttonState[e] = tool_ == Tool::DecorHand ? 2 : 1;
        if (const UiElement* e = root.child(0xA4))
            hk.buttonState[e] = tool_ == Tool::DecorPick ? 2 : 1;
        layoutUiElement(root, o.x - float(ra[0]), o.y - float(ra[1]), *res_, *fonts_, out, hk);
        for (const auto& [e, r] : rects)
            if (e->get("clsid") != "GZWinFlatRect")
                solids.push_back({r, Hit::Solid});
        for (size_t i = 0; i < kDecorSorts.size(); ++i)
            target(root.child(0xA000) ? root.child(0xA000)->child(kDecorSorts[i].first) : nullptr, Hit::DecorSort,
                   uint32_t(i));
        target(root.child(0xA5), Hit::DecorTool, 1);
        target(root.child(0xA4), Hit::DecorTool, 2);
        target(root.child(0xA1), Hit::DecorUndo);
        target(root.child(0xA2), Hit::DecorRedo);
        const UiElement* view = decorExpanded_ ? expanded : collapsed;
        const uint32_t base = decorExpanded_ ? 0x2000 : 0x1000;
        if (view) {
            target(view->child(base + 4), Hit::DecorPrev);
            target(view->child(base + 5), Hit::DecorNext);
            target(view->child(base + 9), Hit::DecorExpand);
        }
        const UiElement* slot = view ? view->child(base + 6) : nullptr;
        if (auto vr = rectOf(view); vr && slot) {
            const auto sa = slot->area();
            const float x0 = (*vr)[0] + float(sa[0]), y0 = (*vr)[1] + float(sa[1]);
            const float w = float(sa[2] - sa[0]), h = float(sa[3] - sa[1]);
            const int cols = 2, rows = decorExpanded_ ? 2 : 1;
            const float s = std::floor(std::min(w / float(cols), h / float(rows)));
            const auto shown = visibleDecorations();
            const UiElement& item = item_.roots.front();
            for (size_t k = 0; k < shown.size(); ++k) {
                const uint32_t guid = shown[k];
                const float ix = x0 + float(k % size_t(cols)) * (w / float(cols)) + (w / float(cols) - s) / 2;
                const float iy = y0 + float(k / size_t(cols)) * (h / float(rows)) + (h / float(rows) - s) / 2;
                UiElement it = item;
                const float k94 = s / 94.0f;
                auto scale = [&](UiElement& e) {
                    const auto a = e.area();
                    e.set("area", area(int(std::lround(a[0] * k94)), int(std::lround(a[1] * k94)),
                                       int(std::lround(a[2] * k94)), int(std::lround(a[3] * k94))));
                };
                scale(it);
                it.set("area", area(0, 0, int(s), int(s)));
                for (auto& c : it.children)
                    scale(c);
                UiDrawHooks ih;
                std::map<const UiElement*, std::array<float, 4>> irects;
                ih.rects = &irects;
                if (UiElement* bg = it.child(1))
                    if (decorThumb)
                        if (const RgbaImage* img = decorThumb(guid))
                            ih.images[bg] = img;
                if (UiElement* alpha = it.child(2))
                    ih.hide.insert(alpha);
                if (UiElement* btn = it.child(3))
                    ih.buttonState[btn] = guid == decorGuid_ ? 2 : 1;
                layoutUiElement(it, ix, iy, *res_, *fonts_, out, ih);
                targets_.push_back({{ix, iy, ix + s, iy + s}, Hit::DecorItem, guid});
            }
        }
    } else if (panel_ == Panel::Options && !options_.roots.empty()) {
        // The neighborhood's options panel, hung from the puck like the others.
        const UiElement& root = options_.roots.front();
        const UiElement* box = root.child(0xA200);
        const UiElement* anchor = box ? box->child(kPanelAnchor) : nullptr;
        simd_float2 o = simd_make_float2(puckRect[0], puckRect[1]);
        if (const UiElement* a = findWindow(puck, kPuckAnchor))
            o += simd_make_float2(float(a->area()[0]), float(a->area()[1]));
        if (box && anchor)
            o -= simd_make_float2(float(box->area()[0] + anchor->area()[0]), float(box->area()[1] + anchor->area()[1]));
        const auto ra = root.area();
        UiDrawHooks hk;
        hk.rects = &rects;
        if (box)
            if (const UiElement* back = box->child(0xA250))
                hk.hide.insert(back);
        layoutUiElement(root, o.x - float(ra[0]), o.y - float(ra[1]), *res_, *fonts_, out, hk);
        for (const auto& [e, r] : rects)
            if (e->get("clsid") != "GZWinFlatRect")
                solids.push_back({r, Hit::Solid});
        if (box) {
            target(box->child(0xA221), Hit::OptMainMenu);
            target(box->child(0xA220), Hit::OptQuit);
            // The option windows: A241 graphics, A242 audio, A243 game,
            // A244 house (hidden), A245 camera, A246 credits.
            for (auto [id, w] : {std::pair{0xA241u, 3u}, {0xA242u, 1u}, {0xA243u, 2u}, {0xA245u, 4u}, {0xA246u, 5u}})
                target(box->child(0xA240) ? box->child(0xA240)->child(id) : nullptr, Hit::OptWindow, w);
        }
        if (auto r = rects.find(&root); r != rects.end())
            optionsBL_ = simd_make_float2(r->second[0], r->second[1]); // the panel's top left: the window rises from it
    } else if (panel_ == Panel::Lots) {
        const UiElement& root = lots_.roots.front();
        const simd_float2 o = anchoredOrigin(root, puckRect, puck);
        const auto ra = root.area();
        UiDrawHooks hk;
        hk.rects = &rects;
        // SetPanel: the bin (0xC000, its picture 0xC200) or the empty lots
        // (0xD000, 0xD200).
        const UiElement* binPanel = root.child(0xC000);
        const UiElement* emptyPanel = root.child(0xD000);
        for (auto [id, show] : {std::pair{0xC000u, lotsPanel_ == 0}, {0xC200u, lotsPanel_ == 0},
                                {0xD000u, lotsPanel_ == 1}, {0xD200u, lotsPanel_ == 1}})
            if (const UiElement* e = root.child(id))
                (show ? hk.show : hk.hide).insert(e);
        if (const UiElement* e = root.child(0xC100))
            hk.buttonState[e] = lotsPanel_ == 0 ? 2 : 1;
        if (binPanel)
            for (auto [id, f] : {std::pair{0xC400u, 1}, {0xC500u, 2}, {0xC600u, 0}})
                if (const UiElement* e = binPanel->child(id))
                    hk.buttonState[e] = binFilter_ == f ? 2 : 1;
        if (const UiElement* e = root.child(0xD100))
            hk.buttonState[e] = lotsPanel_ == 1 ? 2 : 1;
        if (const UiElement* e = root.child(0xA000))
            hk.buttonState[e] = tool_ == Tool::MoveLot ? 2 : 1;
        if (const UiElement* e = root.child(0xE000))
            hk.buttonState[e] = tool_ == Tool::ToBin ? 2 : 1;
        if (const UiElement* bd = root.child(kBulldoze))
            hk.buttonState[bd] = tool_ == Tool::Bulldoze ? 2 : 1;
        if (emptyPanel)
            for (uint32_t k = 0; k < uint32_t(hood::kEmptyLotSizes); ++k)
                if (const UiElement* b = emptyPanel->child(0xD001 + k))
                    hk.buttonState[b] = tool_ == Tool::PlaceEmpty && emptySize_ == int(k) ? 2 : 1;
        const UiElement* binView = nullptr;
        if (binPanel)
            if (const UiElement* c3 = binPanel->child(0xC300)) {
                const UiElement* collapsed = c3->child(0x1000);
                const UiElement* expanded = c3->child(0x2000);
                if (collapsed && expanded)
                    (binExpanded_ ? hk.show : hk.hide).insert(expanded),
                        (binExpanded_ ? hk.hide : hk.show).insert(collapsed);
                binView = binExpanded_ ? expanded : collapsed;
            }
        layoutUiElement(root, o.x - float(ra[0]), o.y - float(ra[1]), *res_, *fonts_, out, hk);
        for (const auto& [e, r] : rects)
            if (e->get("clsid") != "GZWinFlatRect")
                solids.push_back({r, Hit::Solid});
        target(root.child(kBulldoze), Hit::Bulldoze);
        target(root.child(0xA000), Hit::MoveLotTool);
        target(root.child(0xE000), Hit::ToBinTool);
        target(root.child(0xC100), Hit::LotsPanel, 0);
        target(root.child(0xD100), Hit::LotsPanel, 1);
        if (lotsPanel_ == 1 && emptyPanel)
            for (uint32_t k = 0; k < uint32_t(hood::kEmptyLotSizes); ++k)
                target(emptyPanel->child(0xD001 + k), Hit::EmptySize, k);
        if (lotsPanel_ == 0 && binPanel) {
            target(binPanel->child(0xC400), Hit::BinFilter, 1);
            target(binPanel->child(0xC500), Hit::BinFilter, 2);
            target(binPanel->child(0xC600), Hit::BinFilter, 0);
        }
        if (lotsPanel_ == 0 && binView) {
            const uint32_t base = binExpanded_ ? 0x2000 : 0x1000;
            if (const UiElement* holder = binView->child(base + 0xA))
                target(holder->child(0xC), Hit::BinDelete);
            target(binView->child(base + 4), Hit::BinPrev);
            target(binView->child(base + 5), Hit::BinNext);
            target(binView->child(base + 9), Hit::BinExpand);
            // The bin's lots in the hidden rectangle 0x?006, the lot's picture
            // (4:3) under the item's button overlay.
            const UiElement* slot = binView->child(base + 6);
            if (auto vr = rectOf(binView); vr && slot) {
                const auto sa = slot->area();
                const float x0 = (*vr)[0] + float(sa[0]), y0 = (*vr)[1] + float(sa[1]);
                const float w = float(sa[2] - sa[0]), h = float(sa[3] - sa[1]);
                const int cols = 2, rows = binExpanded_ ? 2 : 1;
                const float cw = w / float(cols), ch = h / float(rows);
                const float iw = std::floor(std::min(cw - 4, (ch - 4) * 4 / 3)), ih = std::floor(iw * 3 / 4);
                const size_t per = size_t(cols * rows);
                // LoadLots(filter): 0 all, 1 occupied, 2 unoccupied.
                std::vector<size_t> shown;
                for (size_t i = 0; i < binLots_.size(); ++i)
                    if (binFilter_ == 0 || (binFilter_ == 1) == !binLots_[i].familyName.empty())
                        shown.push_back(i);
                const size_t pages = std::max<size_t>(1, (shown.size() + per - 1) / per);
                binPage_ = std::min(binPage_, pages - 1);
                for (size_t k = 0; k < per && binPage_ * per + k < shown.size(); ++k) {
                    const size_t idx = shown[binPage_ * per + k];
                    const float ix = x0 + float(k % cols) * cw + (cw - iw) / 2;
                    const float iy = y0 + float(k / cols) * ch + (ch - ih) / 2;
                    UiElement pic;
                    pic.tag = "LEGACY";
                    pic.attrs = {{"clsid", "GZWinBMP"}, {"winflag_visible", "yes"},
                                 {"area", area(0, 0, int(iw), int(ih))}};
                    UiDrawHooks ph;
                    if (const RgbaImage* img = binPicture(idx))
                        ph.images[&pic] = img;
                    layoutUiElement(pic, ix, iy, *res_, *fonts_, out, ph);
                    if (int(idx) == binLot_) {
                        // The pick: a frame round it.
                        for (auto [fx, fy, fw, fh] : {std::array<float, 4>{ix - 2, iy - 2, iw + 4, 2},
                                                      {ix - 2, iy + ih, iw + 4, 2},
                                                      {ix - 2, iy, 2, ih},
                                                      {ix + iw, iy, 2, ih}}) {
                            UiElement f;
                            f.tag = "LEGACY";
                            f.attrs = {{"clsid", "GZWinFlatRect"}, {"winflag_visible", "yes"},
                                       {"fillcolor", "(255,255,255)"},
                                       {"area", area(0, 0, int(fw), int(fh))}};
                            layoutUiElement(f, fx, fy, *res_, *fonts_, out, {});
                        }
                    }
                    targets_.push_back({{ix, iy, ix + iw, iy + ih}, Hit::BinItem, uint32_t(idx)});
                }
            }
        }
    }
    targets_.insert(targets_.end(), solids.begin(), solids.end());
    if (panelsEnd)
        *panelsEnd = out.quads.size();

    // The place-lot dialog, centred (Show centres it on its parent).
    if (placeLot_ && !placeScript_.roots.empty()) {
        const UiElement& root = placeScript_.roots.front();
        UiDrawHooks ph;
        std::map<const UiElement*, std::array<float, 4>> prects;
        ph.rects = &prects;
        UiElement r = root;
        if (const UiElement* b = r.child(1))
            ph.buttonState[b] = placeLot_->community ? 1 : 2;
        if (const UiElement* b = r.child(2))
            ph.buttonState[b] = placeLot_->community ? 2 : 1;
        if (UiElement* edit = r.child(3)) {
            edit->set("initvalue", placeLot_->name + (placeLot_->edited ? "_" : ""));
            edit->set("captionres", "");
            edit->set("caption", placeLot_->name);
        }
        const auto a = r.area();
        const float w = float(a[2] - a[0]), h = float(a[3] - a[1]);
        const float x = std::round((float(width) - w) / 2), y = std::round((float(height) - h) / 2);
        layoutUiElement(r, x - float(a[0]), y - float(a[1]), *res_, *fonts_, out, ph);
        std::vector<Target> modal;
        for (const auto& c : r.children) {
            auto it = prects.find(&c);
            if (it == prects.end())
                continue;
            const uint32_t id = c.id();
            const Hit hit = id == 1 ? Hit::PlaceResidential : id == 2 ? Hit::PlaceCommunity
                          : id == 0xA000 ? Hit::PlaceAccept : id == 0xA001 ? Hit::PlaceCancel : Hit::Solid;
            if (hit != Hit::Solid) {
                modal.push_back({it->second, hit});
                if (!dialog_ && !placeLot_->announced)
                    std::printf("opensims2: place-lot dialog %s at (%.0f, %.0f)\n",
                                id == 1 ? "Residential" : id == 2 ? "Community" : id == 0xA000 ? "Accept" : "Cancel",
                                (it->second[0] + it->second[2]) / 2, (it->second[1] + it->second[3]) / 2);
            }
        }
        if (!dialog_)
            placeLot_->announced = true;
        std::fflush(stdout);
        modal.push_back({{0, 0, float(width), float(height)}, Hit::Solid});
        targets_ = std::move(modal);
    }

    // The question / message, centred, over everything.
    if (dialog_) {
        UiElement d = dialogScript_.roots.front();
        UiDrawHooks dh;
        std::map<const UiElement*, std::array<float, 4>> drects;
        dh.rects = &drects;
        const bool question = dialog_->onAccept.kind != Request::Kind::None;
        // Sized for the text (inferred: about 7 px a character, 18 px a
        // line); the text-entry box and its frame go.
        const std::string& msg = dialog_->message;
        const int wide = msg.size() > 70 ? 140 : 0;
        const int perLine = std::max(1, (190 + wide) / 8);
        int lines = 0;
        for (size_t at = 0; at <= msg.size();) {
            const size_t nl = std::min(msg.find('\n', at), msg.size());
            lines += std::max<int>(1, int((nl - at + size_t(perLine) - 1) / size_t(perLine)));
            at = nl + 1;
        }
        const int extra = std::max(30, 18 * lines - 40);
        const auto a = d.area();
        d.set("area", area(0, 0, a[2] - a[0] + wide, a[3] - a[1] + extra));
        for (auto& c : d.children) {
            const auto ca = c.area();
            if (c.get("clsid") == "GZWinTextEdit" || (c.get("clsid") == "GZWinBMP" && ca[1] >= 60))
                c.set("winflag_visible", "no");
            else if (c.get("clsid") == "GZWinText") {
                c.set("area", area(ca[0], ca[1], ca[2] + 8 + wide, ca[3] + 40 + extra));
                c.set("caption", dialog_->message);
                c.set("captionres", "");
                c.set("wrapped", "yes");
            } else if (c.get("clsid") == "GZWinBtn") {
                int x0 = ca[0] + wide / 2;
                if (!question && c.id() == 0xA000)
                    x0 = ((a[2] - a[0] + wide) - (ca[2] - ca[0])) / 2;
                if (!question && c.id() == 0xA001)
                    c.set("winflag_visible", "no");
                c.set("area", area(x0, ca[1] + extra, x0 + (ca[2] - ca[0]), ca[3] + extra));
            }
        }
        const float w = float(a[2] - a[0] + wide), h = float(a[3] - a[1] + extra);
        const float x = std::round((float(width) - w) / 2), y = std::round((float(height) - h) / 2);
        layoutUiElement(d, x, y, *res_, *fonts_, out, dh);
        // Modal: the dialog's buttons first, everything else swallowed.
        std::vector<Target> modal;
        for (const auto& c : d.children)
            if (auto it = drects.find(&c); it != drects.end() && c.get("clsid") == "GZWinBtn") {
                modal.push_back({it->second, c.id() == 0xA000 ? Hit::Accept : Hit::Cancel});
                if (!dialog_->announced) // where to click, for scripted runs
                    std::printf("opensims2: dialog %s at (%.0f, %.0f)\n", c.id() == 0xA000 ? "Accept" : "Cancel",
                                (it->second[0] + it->second[2]) / 2, (it->second[1] + it->second[3]) / 2);
            }
        dialog_->announced = true;
        std::fflush(stdout);
        modal.push_back({{0, 0, float(width), float(height)}, Hit::Solid});
        targets_ = std::move(modal);
    }
    dirty_ = false;
}

bool HoodBins::hits(simd_float2 p) const {
    for (const auto& t : targets_)
        if (insideRect(t.rect, p.x, p.y))
            return true;
    return false;
}

HoodBins::Request HoodBins::press(simd_float2 p) {
    for (const auto& t : targets_) {
        if (!insideRect(t.rect, p.x, p.y))
            continue;
        dirty_ = true;
        switch (t.hit) {
        case Hit::Accept: {
            const Request r = dialog_ ? dialog_->onAccept : Request{};
            dialog_.reset();
            if (r.kind == Request::Kind::MoveIn || r.kind == Request::Kind::MoveOut ||
                r.kind == Request::Kind::Bulldoze || r.kind == Request::Kind::ToBin ||
                r.kind == Request::Kind::DeleteBinLot)
                tool_ = Tool::None, selected_ = 0;
            return r.kind == Request::Kind::None ? Request{Request::Kind::Consumed} : r;
        }
        case Hit::Cancel: dialog_.reset(); return {Request::Kind::Consumed};
        case Hit::Family: selectFamily(t.value == selected_ ? 0 : t.value); return {Request::Kind::Consumed};
        case Hit::Expand: expanded_ = !expanded_; return {Request::Kind::Consumed};
        case Hit::Prev:
            if (page_ > 0)
                --page_;
            return {Request::Kind::Consumed};
        case Hit::Next: ++page_; return {Request::Kind::Consumed};
        case Hit::Create: cancelTool(); return {Request::Kind::CreateFamily};
        case Hit::Evict:
            selected_ = 0;
            tool_ = tool_ == Tool::Evict ? Tool::None : Tool::Evict;
            return {Request::Kind::Consumed};
        case Hit::Bulldoze:
            selected_ = 0;
            tool_ = tool_ == Tool::Bulldoze ? Tool::None : Tool::Bulldoze;
            return {Request::Kind::Consumed};
        case Hit::LotsPanel:
            lotsPanel_ = int(t.value);
            return {Request::Kind::Consumed};
        case Hit::EmptySize:
            pickEmptyLot(tool_ == Tool::PlaceEmpty && emptySize_ == int(t.value) ? -1 : int(t.value));
            return {Request::Kind::Consumed};
        case Hit::BinItem:
            if (tool_ == Tool::PlaceBin && binLot_ == int(t.value))
                cancelTool();
            else
                pickBinLot(t.value);
            return {Request::Kind::Consumed};
        case Hit::BinExpand: binExpanded_ = !binExpanded_; return {Request::Kind::Consumed};
        case Hit::BinFilter:
            binFilter_ = int(t.value), binPage_ = 0;
            return {Request::Kind::Consumed};
        case Hit::BinDelete: {
            // cTSWinProcLotCatalog::onBtnClickedID 0xC: the picked lot,
            // asked with STR# 0x83 #15 (#24 with its family).
            if (binLot_ < 0 || binLot_ >= int(binLots_.size()))
                return {Request::Kind::Consumed};
            const hood::BinLot& b = binLots_[size_t(binLot_)];
            const std::string q = b.familyName.empty()
                                      ? text(0x83, 14, "Are you sure you want to delete this lot?")
                                      : fill(text(0x83, 23, "Delete the %s family and its lot?"), b.familyName);
            Request r{Request::Kind::DeleteBinLot};
            r.lot = uint32_t(binLot_);
            openQuestion(q, r);
            return {Request::Kind::Consumed};
        }
        case Hit::BinPrev:
            if (binPage_ > 0)
                --binPage_;
            return {Request::Kind::Consumed};
        case Hit::BinNext: ++binPage_; return {Request::Kind::Consumed};
        case Hit::MoveLotTool: {
            const bool on = tool_ == Tool::MoveLot;
            cancelTool();
            tool_ = on ? Tool::None : Tool::MoveLot;
            return {Request::Kind::Consumed};
        }
        case Hit::ToBinTool: {
            const bool on = tool_ == Tool::ToBin;
            cancelTool();
            tool_ = on ? Tool::None : Tool::ToBin;
            return {Request::Kind::Consumed};
        }
        case Hit::PlaceResidential:
        case Hit::PlaceCommunity:
            if (placeLot_)
                placeLot_->community = t.hit == Hit::PlaceCommunity;
            return {Request::Kind::Consumed};
        case Hit::PlaceAccept: return acceptPlaceLot();
        case Hit::OptMainMenu: panel_ = Panel::None; return {Request::Kind::MainMenu};
        case Hit::OptQuit: panel_ = Panel::None; return {Request::Kind::Quit};
        case Hit::OptWindow: return {Request::Kind::OptionWindow, 0, t.value};
        case Hit::DecorItem: pickDecoration(t.value == decorGuid_ ? 0 : t.value); return {Request::Kind::Consumed};
        case Hit::DecorSort: setDecorSort(kDecorSorts[std::min<size_t>(t.value, 4)].second); return {Request::Kind::Consumed};
        case Hit::DecorTool: {
            const Tool want = t.value == 1 ? Tool::DecorHand : Tool::DecorPick;
            const bool on = tool_ == want;
            cancelTool();
            tool_ = on ? Tool::None : want;
            return {Request::Kind::Consumed};
        }
        case Hit::DecorUndo: return {Request::Kind::DecorUndo};
        case Hit::DecorRedo: return {Request::Kind::DecorRedo};
        case Hit::DecorPrev:
            if (decorPage_ > 0)
                --decorPage_;
            return {Request::Kind::Consumed};
        case Hit::DecorNext: {
            size_t n = 0;
            for (const auto& p : decorCatalog_)
                n += p.sort == decorSort_;
            const size_t per = decorExpanded_ ? 4 : 2;
            decorPage_ = std::min(decorPage_ + 1, std::max<size_t>(1, (n + per - 1) / per) - 1);
            return {Request::Kind::Consumed};
        }
        case Hit::DecorExpand: decorExpanded_ = !decorExpanded_, decorPage_ = 0; return {Request::Kind::Consumed};
        case Hit::PlaceCancel: placeLot_.reset(); return {Request::Kind::Consumed};
        case Hit::Solid:
        case Hit::Request: return {Request::Kind::Consumed};
        }
    }
    return {};
}

} // namespace os2
