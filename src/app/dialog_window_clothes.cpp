// The Dialog windows of the clothes (dialog_window.hpp): the wardrobe
// (cTSWinProcClothingDialog), the clothing rack and the try-on
// (cTSWinProcClothingPurchaseDialog / ClothingTryOnDialog) with the rack's
// question, and the vanity (cTSWinProcVanityDialog). Numbers marked "binary"
// are from the Mac BV executable (docs/primitives-mac.md "Dialog"); the
// rest is inferred.

#include "app/dialog_window_internal.hpp"

#include "formats/ui_resources.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <tuple>

namespace os2 {

// --- cTSWinProcClothingDialog ------------------------------------------------

DialogWindow::Outfits* DialogWindow::wardrobe(const vm::Dialog& d) {
    auto it = wardrobes_.find(d.id);
    if (it == wardrobes_.end()) {
        std::optional<Outfits> o;
        if (outfits)
            o = outfits(d.outfitSim, d.outfitCategory);
        it = wardrobes_.emplace(d.id, std::move(o)).first;
    }
    return it->second ? &*it->second : nullptr;
}

// Script 0x8C159250: the Sim view (here the chosen part's catalog
// thumbnail, inferred: the game renders the Sim in the outfit), the tabs
// tops / bottoms / whole outfits (0xECC70000-2), previous / next
// (0x8CC731A0/1), OK 0x6CC7318B and cancel 0x6CC73195.
DialogWindow::Layout DialogWindow::wardrobeDialog(const vm::Dialog& d, int width, int height) {
    const UiScript* s = script(0x8C159250);
    if (!s)
        return plainDialog(d, width, height);
    Layout L;
    const UiElement& top = s->roots.front();
    const auto a = top.area();
    const float w = float(a[2] - a[0]), h = float(a[3] - a[1]);
    L.x0 = std::round((float(width) - w) / 2), L.y0 = std::round((float(height) - h) / 2);
    L.x1 = L.x0 + w, L.y1 = L.y0 + h;
    UiElement root = top;
    root.set("area", uiArea(0, 0, int(w), int(h)));
    Outfits* o = wardrobe(d);
    auto rect = [&](uint32_t id) -> std::optional<std::array<float, 4>> {
        if (const UiElement* e = root.child(id)) {
            const auto r = e->area();
            return std::array<float, 4>{L.x0 + float(r[0]), L.y0 + float(r[1]), L.x0 + float(r[2]), L.y0 + float(r[3])};
        }
        return std::nullopt;
    };
    if (o) {
        const auto& list = o->lists[size_t(o->tab)];
        const int cur = o->current[size_t(o->tab)];
        if (cur >= 0 && cur < int(list.size()) && list[size_t(cur)].thumb)
            if (UiElement* view = root.child(0x2CC7315A)) {
                const auto r = view->area();
                const RgbaImage* t = list[size_t(cur)].thumb;
                const float vw = float(r[2] - r[0]), vh = float(r[3] - r[1]);
                const float k = std::min(vw / float(t->width), vh / float(t->height));
                const float tw = std::round(float(t->width) * k), th = std::round(float(t->height) * k);
                UiElement p = element("GZWinBMP", "IGZWinBMP", float(r[0]) + std::round((vw - tw) / 2),
                                      float(r[1]) + std::round((vh - th) / 2), float(r[0]) + std::round((vw - tw) / 2) + tw,
                                      float(r[1]) + std::round((vh - th) / 2) + th);
                p.set("id", hex(0x2CC7315B));
                root.children.insert(root.children.begin() + 2, p);
                L.images[0x2CC7315B] = t;
            }
    }
    for (int t = 0; t < 3; ++t)
        if (auto r = rect(0xECC70000u + uint32_t(t)))
            L.regions.push_back({*r, Hit::Tab, t});
    if (auto r = rect(0x8CC731A0))
        L.regions.push_back({*r, Hit::Prev, 0});
    if (auto r = rect(0x8CC731A1))
        L.regions.push_back({*r, Hit::Next, 0});
    if (auto r = rect(0x6CC7318B)) {
        L.regions.push_back({*r, Hit::Button, 0});
        L.buttonIds.push_back(0x6CC7318B);
    }
    if (auto r = rect(0x6CC73195)) {
        L.regions.push_back({*r, Hit::Button, 1});
        L.buttonIds.push_back(0x6CC73195);
    }
    L.root = std::move(root);
    return L;
}

// --- cTSWinProcClothingPurchaseDialog / ClothingTryOnDialog / VanityDialog ---

namespace {
// Tabs' part bits (0xECC70000-2: tops, bottoms, whole outfits).
constexpr uint32_t kShopPart[3] = {0x04, 0x10, 0x08};
// The rack's age buttons 0x200-0x204 (toddler .. elder) and gender buttons
// 0x400 (female) / 0x401 (male).
constexpr uint32_t kShopAge[5] = {0x01, 0x02, 0x04, 0x08, 0x10};


std::string captionOf(UiResources* res, const UiElement& e) {
    std::string text = e.get("caption");
    if (res)
        if (auto ref = uiStringRef(e.get("captionres")))
            if (auto t = res->string(*ref))
                text = *t;
    return text;
}

std::string printS(std::string fmt, const std::string& value) {
    if (auto p = fmt.find("%s"); p != std::string::npos)
        fmt.replace(p, 2, value);
    return fmt;
}

// Vanity tabs (the buttons of script 0x8C159280 in their order there) and
// what each shows (HandleTabSelect: hair 0x2CEDF326, makeup 0x2CEDF327,
// costume makeup and glasses 0x2CEDF328, eyebrows / stubble / beards the
// facial hair panel 0xAD4F1B1C with its colour row 0x2D4F258C hidden).
constexpr uint32_t kVanityTabs[7] = {0x4CEDF125, 0x4CEDF126, 0x4CEDF128, 0x4CEDF127, 0x4CEDF130, 0x4CEDF131, 0x4CEDF129};
constexpr uint32_t kVanityPanels[7] = {0x2CEDF326, 0x2CEDF327, 0x2CEDF328, 0x2CEDF328, 0xAD4F1B1C, 0xAD4F1B1C, 0xAD4F1B1C};
struct VanityGrid {
    uint32_t component; // the CAS component window holding the grid
    int group;
    uint32_t grid, prev, next, cell;
    int cols, rows;
};
} // namespace

std::array<float, 4> DialogWindow::addCell(Layout& L, UiElement& parent, std::array<float, 2> parentAbs, uint32_t cellScript,
                                           uint32_t serial, float x, float y, const RgbaImage* thumb, int state, bool mark7,
                                           bool mark8) {
    const UiScript* s = script(cellScript);
    UiElement cell;
    float w = 50, h = 50;
    if (s && !s->roots.empty()) {
        cell = s->roots.front();
        const auto a = cell.area();
        w = float(a[2] - a[0]), h = float(a[3] - a[1]);
    } else {
        cell = element("GZWinFlatRect", "IGZWinFlatRect", 0, 0, w, h);
    }
    cell.set("area", uiArea(int(x), int(y), int(x + w), int(y + h)));
    cell.set("id", hex(0x00E00000u | (serial << 8)));
    const uint32_t base = 0x00E00000u | (serial << 8);
    UiElement* pic = nullptr;
    for (auto& c : cell.children) {
        const uint32_t id = c.id();
        if (id == 7)
            c.set("winflag_visible", mark7 ? "yes" : "no");
        if (id == 8)
            c.set("winflag_visible", mark8 ? "yes" : "no");
        if (id == 3) // "backup image": a cell without a picture (the "none" ones)
            c.set("winflag_visible", thumb ? "no" : "yes");
        if (id == 6)
            pic = &c;
        if (id == 5)
            L.states[base | 5] = state;
        c.set("id", hex(base | id));
    }
    if (pic && thumb && thumb->width && thumb->height) {
        // The picture fitted into window 6 (the game draws the part's own
        // rendered thumbnail there; the CASThumbnails ones keep their shape).
        const auto a = pic->area();
        const float pw = float(a[2] - a[0]), ph = float(a[3] - a[1]);
        const float k = std::min(pw / float(thumb->width), ph / float(thumb->height));
        const float tw = std::round(float(thumb->width) * k), th = std::round(float(thumb->height) * k);
        const float px = float(a[0]) + std::round((pw - tw) / 2), py = float(a[1]) + std::round((ph - th) / 2);
        pic->set("area", uiArea(int(px), int(py), int(px + tw), int(py + th)));
        pic->set("image", "");
        L.images[base | 6] = thumb;
    } else if (pic) {
        pic->set("winflag_visible", "no");
    }
    parent.children.push_back(std::move(cell));
    return {parentAbs[0] + x, parentAbs[1] + y, parentAbs[0] + x + w, parentAbs[1] + y + h};
}

DialogWindow::Shop& DialogWindow::shop(const vm::Dialog& d) {
    Shop& s = shops_[d.id];
    if (!s.ready) {
        s.ready = true;
        // The everyday categories open on tops, the others on whole outfits
        // (SetParams' 0x601 test).
        s.tab = d.outfitSeparates ? 0 : 2;
        if (simAgeGender)
            std::tie(s.age, s.gender) = simAgeGender(d.outfitSim);
        if (!s.age)
            s.age = 0x08;
        if (!s.gender)
            s.gender = 0x01;
        // The try-on starts from what the Sim wears in the category (its
        // SetParams copies those parts into the try-on outfit).
        if (d.cls.cls == vm::DialogClass::ClothingTryOn)
            for (int t = 0; t < 3; ++t)
                for (const ShopItem& i : shopList(d, s, t))
                    if (i.worn)
                        s.chosen[size_t(t)] = i.part;
    }
    return s;
}

const std::vector<DialogWindow::ShopItem>& DialogWindow::shopList(const vm::Dialog& d, Shop& s, int tab) {
    const auto key = std::make_tuple(tab, s.age, s.gender);
    auto it = s.lists.find(key);
    if (it == s.lists.end()) {
        std::vector<ShopItem> l;
        if (shopItems)
            l = shopItems(d.outfitSim, d.outfitCategory, kShopPart[tab], s.age, s.gender);
        it = s.lists.emplace(key, std::move(l)).first;
    }
    return it->second;
}

// Scripts 0x8C159251 ("Comprar Roupa") and 0x8C159255 ("Experimentar"):
// the rack window 0xECCDF759 with the tabs (0xECC70000-2), the grid
// 0x1006 and its arrows 0x1004/5; the rack also has the age (0x200-0x204)
// and gender (0x400/1) buttons, "add to cart" 0xECCDF663, the cart window
// 0x2CCDF792 (grid 0x1006, "remove" 0x6CCDF7D7) behind the rack / cart tabs
// 0x8CCD0000/1, the count (0xCCCDF42D) and total (0xACCDF819) and the Sim
// view 0x2CC7315A (here the picked part's picture, else the Sim's head:
// the game renders the Sim wearing it). OK 0x6CC7318B, cancel 0x6CC73195.
DialogWindow::Layout DialogWindow::shopDialog(const vm::Dialog& d, int width, int height) {
    const bool rack = d.cls.cls == vm::DialogClass::ClothingPurchase;
    const UiScript* sc = script(rack ? 0x8C159251 : 0x8C159255);
    if (!sc)
        return plainDialog(d, width, height);
    Layout L;
    const UiElement& top = sc->roots.front();
    const auto a = top.area();
    const float w = float(a[2] - a[0]), h = float(a[3] - a[1]);
    L.x0 = std::round((float(width) - w) / 2), L.y0 = std::round((float(height) - h) / 2);
    L.x1 = L.x0 + w, L.y1 = L.y0 + h;
    UiElement root = top;
    root.set("area", uiArea(0, 0, int(w), int(h)));
    // A picture window without a picture and not transparent (the try-on's
    // backdrop) is painted in its fill colour (inferred from its attributes).
    for (auto& c : root.children)
        if (c.clsid() == "GZWinBMP" && !c.attr("image") && c.get("transparentbkg") == "no") {
            c.set("clsid", "GZWinFlatRect");
            c.set("iid", "IGZWinFlatRect");
        }
    Shop& s = shop(d);
    const bool cartView = rack && s.view == 1;
    std::array<float, 4> r{};
    auto absOf = [&](const std::array<float, 4>& q) { return std::array<float, 4>{L.x0 + q[0], L.y0 + q[1], L.x0 + q[2], L.y0 + q[3]}; };
    auto region = [&](UiElement& under, uint32_t id, Hit kind, int value) {
        std::array<float, 4> q{};
        if (findWin(under, id, 0, 0, &q))
            L.regions.push_back({absOf(q), kind, value});
    };
    UiElement* rackWin = findWin(root, 0xECCDF759, 0, 0, &r);
    const std::array<float, 4> rackRect = r;
    UiElement* cartWin = rack ? findWin(root, 0x2CCDF792, 0, 0, &r) : nullptr;
    const std::array<float, 4> cartRect = r;
    if (rackWin)
        rackWin->set("winflag_visible", cartView ? "no" : "yes");
    if (cartWin)
        cartWin->set("winflag_visible", cartView ? "yes" : "no");
    const auto& list = shopList(d, s, s.tab);
    auto inCart = [&](const ShopItem& i) {
        return std::any_of(s.cart.begin(), s.cart.end(), [&](const ShopItem& c) { return c.key == i.key; });
    };
    uint32_t serial = 0;
    // The grid of the rack (or of the try-on).
    if (rackWin && !cartView) {
        std::array<float, 4> g{};
        if (UiElement* grid = findWin(*rackWin, 0x1006, 0, 0, &g)) {
            const int cols = kShopCols[s.tab], rows = kShopRows[s.tab], per = cols * rows;
            int& first = s.first[size_t(s.tab)];
            first = std::clamp(first, 0, std::max(0, (int(list.size()) - 1) / per * per));
            const UiScript* cs = script(kShopCell[s.tab]);
            float cw = 76, ch = 111;
            if (cs && !cs->roots.empty()) {
                const auto ca = cs->roots.front().area();
                cw = float(ca[2] - ca[0]), ch = float(ca[3] - ca[1]);
            }
            const float gw = g[2] - g[0], gh = g[3] - g[1];
            const float ox = std::round((gw - cw * float(cols)) / 2), oy = std::round((gh - ch * float(rows)) / 2);
            for (int k = 0; k < per && first + k < int(list.size()); ++k) {
                const ShopItem& item = list[size_t(first + k)];
                const bool picked = rack ? s.pick == first + k : s.chosen[size_t(s.tab)] == item.part;
                const auto cell = addCell(L, *grid, {L.x0 + rackRect[0] + g[0], L.y0 + rackRect[1] + g[1]}, kShopCell[s.tab], serial++,
                                          ox + float(k % cols) * cw, oy + float(k / cols) * ch, item.thumb, picked ? 2 : 1,
                                          rack && inCart(item), item.owned);
                L.regions.push_back({cell, Hit::Row, first + k});
            }
        }
        std::array<float, 4> q{};
        for (uint32_t id : {0x1004u, 0x1005u})
            if (findWin(*rackWin, id, rackRect[0], rackRect[1], &q))
                L.regions.push_back({absOf(q), id == 0x1004 ? Hit::Prev : Hit::Next, 0});
        for (int t = 0; t < 3; ++t)
            if (findWin(*rackWin, 0xECC70000u + uint32_t(t), rackRect[0], rackRect[1], &q)) {
                // SetOutfitTabButtonStates: tops and bottoms only for the everyday categories.
                const bool on = t == 2 || d.outfitSeparates;
                L.states[0xECC70000u + uint32_t(t)] = !on ? 0 : t == s.tab ? 2 : 1;
                if (on)
                    L.regions.push_back({absOf(q), Hit::Tab, t});
            }
        if (rack) {
            for (int k = 0; k < 5; ++k)
                if (findWin(*rackWin, 0x200u + uint32_t(k), rackRect[0], rackRect[1], &q)) {
                    L.states[0x200u + uint32_t(k)] = s.age == kShopAge[k] ? 2 : 1;
                    L.regions.push_back({absOf(q), Hit::Choice, 0x100 | int(kShopAge[k])});
                }
            for (int k = 0; k < 2; ++k)
                if (findWin(*rackWin, 0x400u + uint32_t(k), rackRect[0], rackRect[1], &q)) {
                    L.states[0x400u + uint32_t(k)] = s.gender == uint32_t(k + 1) ? 2 : 1;
                    L.regions.push_back({absOf(q), Hit::Choice, 0x200 | (k + 1)});
                }
            if (findWin(*rackWin, 0xECCDF663, rackRect[0], rackRect[1], &q)) {
                const bool can = s.pick >= 0 && s.pick < int(list.size()) && !inCart(list[size_t(s.pick)]);
                L.states[0xECCDF663] = can ? 1 : 0;
                if (can)
                    L.regions.push_back({absOf(q), Hit::Add, 0});
            }
        }
    }
    // The cart.
    if (cartWin && cartView) {
        std::array<float, 4> g{};
        if (UiElement* grid = findWin(*cartWin, 0x1006, 0, 0, &g)) {
            const int cols = 3, rows = 2, per = cols * rows; // kShoppingCartNumColumns / NumRows
            s.cartFirst = std::clamp(s.cartFirst, 0, std::max(0, (int(s.cart.size()) - 1) / per * per));
            for (int k = 0; k < per && s.cartFirst + k < int(s.cart.size()); ++k) {
                const ShopItem& item = s.cart[size_t(s.cartFirst + k)];
                const auto cell = addCell(L, *grid, {L.x0 + cartRect[0] + g[0], L.y0 + cartRect[1] + g[1]}, 0x8C159252, serial++,
                                          float(k % cols) * 101, float(k / cols) * 110, item.thumb,
                                          s.cartPick == s.cartFirst + k ? 2 : 1, false, item.owned);
                L.regions.push_back({cell, Hit::Row, 0x10000 | (s.cartFirst + k)});
            }
        }
        std::array<float, 4> q{};
        for (uint32_t id : {0x1004u, 0x1005u})
            if (findWin(*cartWin, id, cartRect[0], cartRect[1], &q))
                L.regions.push_back({absOf(q), id == 0x1004 ? Hit::Prev : Hit::Next, 1});
        if (findWin(*cartWin, 0x6CCDF7D7, cartRect[0], cartRect[1], &q)) {
            const bool can = s.cartPick >= 0 && s.cartPick < int(s.cart.size());
            L.states[0x6CCDF7D7] = can ? 1 : 0;
            if (can)
                L.regions.push_back({absOf(q), Hit::Remove, 0});
        }
    }
    if (rack) {
        for (int v = 0; v < 2; ++v) {
            region(root, 0x8CCD0000u + uint32_t(v), Hit::Choice, 0x400 | v);
            L.states[0x8CCD0000u + uint32_t(v)] = s.view == v ? 2 : 1;
        }
        if (UiElement* e = findWin(root, 0xCCCDF42D, 0, 0, nullptr))
            L.captions[0xCCCDF42D] = printS(captionOf(res_, *e), std::to_string(s.cart.size()));
        if (UiElement* e = findWin(root, 0xACCDF819, 0, 0, nullptr))
            L.captions[0xACCDF819] =
                printS(captionOf(res_, *e), simoleons(int(s.cart.size()) * d.pricePerItem));
        // The Sim view: the part picked (rack or cart), else the Sim.
        const RgbaImage* view = nullptr;
        if (cartView && s.cartPick >= 0 && s.cartPick < int(s.cart.size()))
            view = s.cart[size_t(s.cartPick)].thumb;
        else if (!cartView && s.pick >= 0 && s.pick < int(list.size()))
            view = list[size_t(s.pick)].thumb;
        if (!view && thumbnail)
            view = thumbnail(d.outfitSim, 0);
        if (view && view->width && view->height)
            if (UiElement* v = findWin(root, 0x2CC7315A, 0, 0, &r)) {
                const float vw = r[2] - r[0], vh = r[3] - r[1];
                const float k = std::min(vw / float(view->width), vh / float(view->height));
                const float tw = std::round(float(view->width) * k), th = std::round(float(view->height) * k);
                UiElement p = element("GZWinBMP", "IGZWinBMP", std::round((vw - tw) / 2), std::round((vh - th) / 2),
                                      std::round((vw - tw) / 2) + tw, std::round((vh - th) / 2) + th);
                p.set("id", hex(0x2CC7315B));
                v->children.push_back(p);
                L.images[0x2CC7315B] = view;
            }
    }
    region(root, 0x6CC7318B, Hit::Button, 0);
    L.buttonIds.push_back(0x6CC7318B);
    region(root, 0x6CC73195, Hit::Button, 1);
    L.buttonIds.push_back(0x6CC73195);
    L.root = std::move(root);
    return L;
}

std::vector<DialogWindow::VanityItem>& DialogWindow::vanityList(const vm::Dialog& d, int group) {
    Vanity& v = vanities_[d.id];
    auto it = v.items.find(group);
    if (it == v.items.end())
        it = v.items.emplace(group, vanityItems ? vanityItems(d.outfitSim, group) : std::vector<VanityItem>{}).first;
    return it->second;
}

// Script 0x8C159280 ("Mudar Aparência"): the tabs (hair, makeup, glasses,
// costume makeup, eyebrows, stubble, beards), each showing its panel and
// CAS components' grids (the components' constants: hair 3 x 4 of
// 0x8C159283 with the colour buttons 0x8800-0x8803, the four makeup rows
// 4 x 1 of 0xCB980E93, the overlays 3 x 5 and the facial hair 3 x 4 of
// 0x8C159283), the Sim view 0xACB91188 (here the Sim's head), OK 0xA000
// and cancel 0xA001.
DialogWindow::Layout DialogWindow::vanityDialog(const vm::Dialog& d, int width, int height) {
    const UiScript* sc = script(0x8C159280);
    if (!sc)
        return plainDialog(d, width, height);
    Layout L;
    const UiElement& top = sc->roots.front();
    const auto a = top.area();
    const float w = float(a[2] - a[0]), h = float(a[3] - a[1]);
    L.x0 = std::round((float(width) - w) / 2), L.y0 = std::round((float(height) - h) / 2);
    L.x1 = L.x0 + w, L.y1 = L.y0 + h;
    UiElement root = top;
    root.set("area", uiArea(0, 0, int(w), int(h)));
    Vanity& v = vanities_[d.id];
    auto absOf = [&](const std::array<float, 4>& q) { return std::array<float, 4>{L.x0 + q[0], L.y0 + q[1], L.x0 + q[2], L.y0 + q[3]}; };
    std::array<float, 4> r{};
    for (int t = 0; t < 7; ++t) {
        if (findWin(root, kVanityTabs[t], 0, 0, &r))
            L.regions.push_back({absOf(r), Hit::Tab, t});
        L.states[kVanityTabs[t]] = t == v.tab ? 2 : 1;
    }
    for (uint32_t p : {0x2CEDF326u, 0x2CEDF327u, 0x2CEDF328u, 0xAD4F1B1Cu})
        if (UiElement* e = findWin(root, p, 0, 0, nullptr))
            e->set("winflag_visible", p == kVanityPanels[v.tab] ? "yes" : "no");
    // HandleTabSelect shows the facial hair colour row 0x2D4F258C with the
    // brows, stubble and beards (FUN_0093fed8(panel, 0x2D4F258C, 1)).
    if (UiElement* e = findWin(root, 0x2D4F258C, 0, 0, nullptr))
        e->set("winflag_visible", v.tab >= 4 ? "yes" : "no");
    std::vector<VanityGrid> grids;
    switch (v.tab) {
    case 0:
        grids = {{0x2CED6A8D, v.custom ? VanityHairCustom : VanityHair, 0x8CB8C2B9, 0x8CB8C2C3, 0xACB8C2CF, 0x8C159283, 3, 4}};
        break;
    case 1:
        grids = {{0x6CA52068, VanityBlush, 0x4011, 0x4008, 0x4009, 0xCB980E93, 4, 1},
                 {0xAD474C78, VanityEyeliner, 0x4011, 0x4008, 0x4009, 0xCB980E93, 4, 1},
                 {0x6CA52069, VanityEyeShadow, 0x4011, 0x4008, 0x4009, 0xCB980E93, 4, 1},
                 {0x6CA52067, VanityLipstick, 0x4011, 0x4008, 0x4009, 0xCB980E93, 4, 1}};
        break;
    case 2: grids = {{0x6CA52066, VanityGlasses, 0x4011, 0x4008, 0x4009, 0x8C159283, 3, 5}}; break;
    case 3: grids = {{0x6CA52066, VanityCostume, 0x4011, 0x4008, 0x4009, 0x8C159283, 3, 5}}; break;
    case 4: grids = {{0x8D4F1FEB, VanityBrows, 0x4011, 0x4008, 0x4009, 0x8C159283, 3, 4}}; break;
    case 5: grids = {{0x8D4F1FEB, VanityStubble, 0x4011, 0x4008, 0x4009, 0x8C159283, 3, 4}}; break;
    case 6: grids = {{0x8D4F1FEB, VanityBeards, 0x4011, 0x4008, 0x4009, 0x8C159283, 3, 4}}; break;
    default: break;
    }
    UiElement* panel = findWin(root, kVanityPanels[v.tab], 0, 0, &r);
    const std::array<float, 4> panelRect = r;
    uint32_t serial = 0;
    for (const VanityGrid& g : grids) {
        if (!panel)
            break;
        std::array<float, 4> cr{};
        UiElement* comp = findWin(*panel, g.component, panelRect[0], panelRect[1], &cr);
        if (!comp)
            continue;
        std::array<float, 4> gr{};
        UiElement* grid = findWin(*comp, g.grid, cr[0], cr[1], &gr);
        auto& items = vanityList(d, g.group);
        const int per = g.cols * g.rows;
        int& first = v.first[g.group];
        first = std::clamp(first, 0, std::max(0, (int(items.size()) - 1) / per * per));
        if (grid) {
            const UiScript* cs = script(g.cell);
            float cw = 50, ch = 50;
            if (cs && !cs->roots.empty()) {
                const auto ca = cs->roots.front().area();
                cw = float(ca[2] - ca[0]), ch = float(ca[3] - ca[1]);
            }
            const float gw = gr[2] - gr[0], gh = gr[3] - gr[1];
            const float ox = std::round((gw - cw * float(g.cols)) / 2), oy = std::round((gh - ch * float(g.rows)) / 2);
            for (int k = 0; k < per && first + k < int(items.size()); ++k) {
                const VanityItem& it = items[size_t(first + k)];
                const auto cell = addCell(L, *grid, {L.x0 + gr[0], L.y0 + gr[1]}, g.cell, serial++, ox + float(k % g.cols) * cw,
                                          oy + float(k / g.cols) * ch, it.thumb, it.selected ? 2 : 1, false, false);
                L.regions.push_back({cell, Hit::Row, (g.group << 16) | (first + k)});
            }
        }
        std::array<float, 4> q{};
        if (findWin(*comp, g.prev, cr[0], cr[1], &q))
            L.regions.push_back({absOf(q), Hit::Prev, g.group});
        if (findWin(*comp, g.next, cr[0], cr[1], &q))
            L.regions.push_back({absOf(q), Hit::Next, g.group});
        if (g.group == VanityHair || g.group == VanityHairCustom) {
            // The colour buttons (black, brown, blond, red; grey is hidden)
            // and "Custom" 0x8804 (cTSUICASComponentHair::HandleTabHairColor:
            // the custom flag on, the grid's filter to the hairs of the
            // other tones, nTSCASCore::IsRegularHairTone = 1..5; a colour
            // turns it off). The colour pressed is the hair's, none in
            // custom mode.
            auto& tones = vanityList(d, VanityHairColor);
            for (int k = 0; k < 4; ++k)
                if (findWin(*comp, 0x8800u + uint32_t(k), cr[0], cr[1], &q)) {
                    L.regions.push_back({absOf(q), Hit::Choice, 0x300 | k});
                    // Several windows share these ids: the state goes by the one found.
                    if (UiElement* b = findWin(*comp, 0x8800u + uint32_t(k), 0, 0, nullptr)) {
                        const uint32_t id = 0x00F00000u | uint32_t(k);
                        b->set("id", hex(id));
                        L.states[id] = !v.custom && k < int(tones.size()) && tones[size_t(k)].selected ? 2 : 1;
                    }
                }
            if (findWin(*comp, 0x8804, cr[0], cr[1], &q)) {
                L.regions.push_back({absOf(q), Hit::Choice, 0x310});
                if (UiElement* b = findWin(*comp, 0x8804, 0, 0, nullptr)) {
                    b->set("id", hex(0x00F00010u));
                    L.states[0x00F00010u] = v.custom ? 2 : 1;
                }
            }
        }
    }
    // The facial hair colour row (onBtnClickedID: a button with an id in
    // 0x8800 of the row 0x2D4F258C sets the GUID of its caption, the hair
    // tone, with cTSUISimEditor::SetHairColor and refreshes the overlays:
    // the brows and beards follow the hair's colour, so it recolours the
    // hair too). Grey 0x8804 is hidden in the script.
    if (v.tab >= 4)
        if (UiElement* rowWin = findWin(root, 0x2D4F258C, 0, 0, &r)) {
            const std::array<float, 4> rowRect = r;
            auto& tones = vanityList(d, VanityHairColor);
            for (int k = 0; k < 4; ++k) {
                std::array<float, 4> q{};
                if (findWin(*rowWin, 0x8800u + uint32_t(k), rowRect[0], rowRect[1], &q)) {
                    L.regions.push_back({absOf(q), Hit::Choice, 0x300 | k});
                    if (UiElement* b = findWin(*rowWin, 0x8800u + uint32_t(k), 0, 0, nullptr)) {
                        const uint32_t id = 0x00F00020u | uint32_t(k);
                        b->set("id", hex(id));
                        L.states[id] = k < int(tones.size()) && tones[size_t(k)].selected ? 2 : 1;
                    }
                }
            }
        }
    // The Sim view: the Sim's head (the game renders the Sim with the edit).
    if (thumbnail)
        if (const RgbaImage* head = thumbnail(d.outfitSim, 0); head && head->width && head->height)
            if (UiElement* view = findWin(root, 0xACB91188, 0, 0, &r)) {
                const float vw = r[2] - r[0], vh = r[3] - r[1];
                const float k = std::min(vw / float(head->width), vh / float(head->height));
                const float tw = std::round(float(head->width) * k), th = std::round(float(head->height) * k);
                UiElement p = element("GZWinBMP", "IGZWinBMP", std::round((vw - tw) / 2), std::round((vh - th) / 2),
                                      std::round((vw - tw) / 2) + tw, std::round((vh - th) / 2) + th);
                p.set("id", hex(0xACB91189));
                view->children.push_back(p);
                L.images[0xACB91189] = head;
            }
    if (findWin(root, 0xA000, 0, 0, &r)) {
        L.regions.push_back({absOf(r), Hit::Button, 0});
        L.buttonIds.push_back(0xA000);
    }
    if (findWin(root, 0xA001, 0, 0, &r)) {
        L.regions.push_back({absOf(r), Hit::Button, 1});
        L.buttonIds.push_back(0xA001);
    }
    L.root = std::move(root);
    return L;
}

// The rack's question (SaveCartStandard): a yes/no cTSObjectDialog with
// UIText STR# 0xCA #7 (the dialog manager's vtable 0x130 with style 3; the
// answer 0x5301814C is "yes"). The base game's UIText has no STR# 0xCA (an
// expansion's): then a text of ours (inferred).
vm::Dialog DialogWindow::cartQuestion(const vm::Dialog& d) {
    vm::Dialog q;
    q.id = d.id ^ 0x40000000u;
    q.type = vm::dialogtype::YesNo;
    std::optional<std::string> text;
    if (res_)
        text = res_->string(UiStringRef{0x7FA7DC7C, 0xCA, 6});
    q.message = text.value_or("Estas roupas n\xC3\xA3o completam nenhum conjunto. Comprar assim mesmo?");
    auto button = [&](uint32_t i, const char* fallback) {
        std::optional<std::string> t;
        if (res_)
            t = res_->string(UiStringRef{0x7FA7DC7C, 0x8E, i}); // STR# 0x8E: 1 OK, 2 Cancel, 3 Yes, 4 No
        return t.value_or(fallback);
    };
    q.buttons = {button(2, "Sim"), button(3, "N\xC3\xA3o")};
    return q;
}

// The question answered: yes completes the purchase (CompletePurchase),
// no closes the rack all the same with nothing bought (OnOk returns true
// either way).
void DialogWindow::confirmCart(vm::Dialog& d, bool yes) {
    Shop& s = shop(d);
    s.asking = false;
    s.confirmed = true;
    if (yes) {
        answer(d, 0);
        return;
    }
    d.cart.clear();
    s.cart.clear();
    d.answer = 0; // OK with an empty cart (past okEnabled: nothing to pay)
    if (log) {
        std::printf("opensims2: dialog \"%s\": nothing bought\n", d.title.c_str());
        std::fflush(stdout);
    }
}

} // namespace os2
