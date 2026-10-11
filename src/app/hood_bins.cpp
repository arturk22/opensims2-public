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
                   kDialogScript = 0x49001035, kCardScript = 0x49001011;
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

void HoodBins::selectFamily(uint32_t family) {
    selected_ = family;
    tool_ = family ? Tool::MoveIn : Tool::None;
    dirty_ = true;
}

void HoodBins::cancelTool() {
    tool_ = Tool::None;
    selected_ = 0;
    dirty_ = true;
}

void HoodBins::showMessage(const std::string& message) {
    dialog_ = Dialog{message, {}};
    dirty_ = true;
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
                      std::map<const UiElement*, std::array<float, 4>>& puckRects, int width, int height) {
    targets_.clear();
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
    } else if (panel_ == Panel::Lots) {
        const UiElement& root = lots_.roots.front();
        const simd_float2 o = anchoredOrigin(root, puckRect, puck);
        const auto ra = root.area();
        UiDrawHooks hk;
        hk.rects = &rects;
        // The lots bin (catalog) and empty lots: placing lots is not done.
        if (const UiElement* c = root.child(0xC000))
            hk.hide.insert(c);
        for (uint32_t id : {0xA000u, 0xE000u, 0xC100u, 0xD100u})
            if (const UiElement* e = root.child(id))
                hk.buttonState[e] = 0;
        if (const UiElement* bd = root.child(kBulldoze))
            hk.buttonState[bd] = tool_ == Tool::Bulldoze ? 2 : 1;
        layoutUiElement(root, o.x - float(ra[0]), o.y - float(ra[1]), *res_, *fonts_, out, hk);
        for (const auto& [e, r] : rects)
            if (e->get("clsid") != "GZWinFlatRect")
                solids.push_back({r, Hit::Solid});
        target(root.child(kBulldoze), Hit::Bulldoze);
    }
    targets_.insert(targets_.end(), solids.begin(), solids.end());

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
                r.kind == Request::Kind::Bulldoze)
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
        case Hit::Solid:
        case Hit::Request: return {Request::Kind::Consumed};
        }
    }
    return {};
}

} // namespace os2
