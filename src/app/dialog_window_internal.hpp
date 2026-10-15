#pragma once

// What the Dialog windows' sources share (dialog_window.cpp,
// dialog_window_pickers.cpp, dialog_window_clothes.cpp; nothing else
// includes it): ids and small helpers. Numbers marked "binary" are from the
// Mac BV executable (docs/primitives-mac.md "Dialog"); the rest is inferred.

#include "app/dialog_window.hpp"

#include "app/ui_panel_util.hpp"
#include "vm/vm.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace os2 {

// (Unnamed: each source has its own copy, as when they were one file; the
// functions are inline so that a source that does not use one is not
// warned.)
namespace {

constexpr uint32_t kImageGroup = 0x499DB772;

constexpr int kRowsList = 0, kRowsInvite = 1; // party lists (region value >> 16)

inline UiElement element(const char* clsid, const char* iid, float x0, float y0, float x1, float y1) {
    UiElement e;
    e.tag = "LEGACY";
    e.set("clsid", clsid);
    e.set("iid", iid);
    e.set("area", uiArea(int(std::lround(x0)), int(std::lround(y0)), int(std::lround(x1)), int(std::lround(y1))));
    e.set("winflag_visible", "yes");
    return e;
}

inline std::string hex(uint32_t v) {
    char b[16];
    std::snprintf(b, sizeof b, "0x%08X", v);
    return b;
}

inline bool contains(const std::vector<int16_t>& v, int16_t x) { return std::find(v.begin(), v.end(), x) != v.end(); }

// A window anywhere under `e` by id, and its canvas rectangle given e's
// top-left corner.
inline UiElement* findWin(UiElement& e, uint32_t id, float ox, float oy, std::array<float, 4>* rect) {
    for (auto& c : e.children) {
        const auto a = c.area();
        if (c.id() == id) {
            if (rect)
                *rect = {ox + float(a[0]), oy + float(a[1]), ox + float(a[2]), oy + float(a[3])};
            return &c;
        }
        if (UiElement* f = findWin(c, id, ox + float(a[0]), oy + float(a[1]), rect))
            return f;
    }
    return nullptr;
}

// cTSWinProcFillDialog::CalculateCostOfUnits: 50 units a cost step,
// rounded up, plus the delivery.
inline int fillCost(const vm::Dialog& d, int units) { return (units * d.fillCost + 49) / 50 + d.fillDelivery; }
inline std::string simoleons(int v) { return "\xC2\xA7" + std::to_string(v); }

// The thumbnail scripts and grids of the clothing bin (the scripts'
// constants: kClothingBinTopsThumbnail 0x8C159252 3 x 2, bottoms 0x8C159253
// and whole outfits 0x8C159254 4 x 2; the cart kShoppingCartThumbnail
// 0x8C159252 3 x 2).
constexpr uint32_t kShopCell[3] = {0x8C159252, 0x8C159253, 0x8C159254};
constexpr int kShopCols[3] = {3, 4, 4}, kShopRows[3] = {2, 2, 2};

} // namespace

} // namespace os2
