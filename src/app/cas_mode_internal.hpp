#pragma once

// What the Create-a-Sim sources share (cas_mode.cpp, cas_mode_family.cpp,
// cas_mode_panels.cpp; nothing else includes it): the UI scripts' window
// ids, the cells' colours, small helpers, and what CasMode::screen hands
// its parts (the family view and the step panels).

#include "app/cas_mode.hpp"
#include "formats/font.hpp"

#include <simd/simd.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>

namespace os2 {

// (Unnamed: each source has its own copy, as when they were one file; the
// functions are inline so that a source that does not use one is not
// warned.)
namespace {

constexpr uint32_t kFrame = 0xCB980E50;
constexpr uint32_t kSimBin = 0xCB980E86, kMakeBaby = 0xCB980E87, kFamilyTree = 0x2C054EF0, kTieDialog = 0x2C054EF3,
                   kTreeButton = 0xCCB70001;
constexpr uint32_t kStepScripts[6] = {0xCB980E81, 0xCB980E88, 0xCB980E82, 0xCB980E85, 0xCB980E84, 0xCB980E83};
constexpr uint32_t kSubPanelAnchor = 0x2D050914;
constexpr uint32_t kPanelAnchor = 0x0000AAAA;

// CB980E50 windows.
constexpr uint32_t kControls = 0x8C6B1CA2, kStepButton0 = 0x8C6B0000, kDone = 0xEC6B2109, kCancel = 0xED012A1C,
                   kUndo = 0x8D24D87A, kRedo = 0x8D24D88D, kFirstNameText = 0x2CB7B3D6, kHousehold = 0x0C327D9F,
                   kFamilyNameGen = 0x6C6AE0BD, kRotationWindow = 0x4D097B6D, kPuck = 0x6CB7EA1A,
                   kAccept = 0x122, kExit = 0x105, kZoomIn = 0x2D486AC2, kZoomOut = 0x2D486ACD,
                   kTurnRight = 0x2CAFB772, kTurnLeft = 0x4CAFB7AA, kSimChooser = 0x4BDEFFAB;

// The face panels of CB980E82, in the order of faceTab_.
constexpr uint32_t kFaceTabs[7] = {0x4CB80000, 0x4CB80006, 0x4CB80001, 0x4CB80002, 0x4CB80003, 0x4CB80004, 0x4CB80005};
// The archetype thumbnails' region suffix per face panel (GetCASArchetypeThumbnailContext).
const char* const kRegionThumbs[7] = {"", "", "brow", "eyes", "nose", "mouth", "jaw"};
constexpr uint32_t kFacePanels[7] = {0x6CB8A784, 0xED2A53C1, 0x8CB8A7A5, 0xACB8A7BB, 0x2CB8A844, 0x4CB8A856, 0x4CB8A868};
// CB980E85's tabs in the order of cosmeticTab_ and the overlays component's titles.
constexpr uint32_t kCosmeticTabs[6] = {0x6C6E0002, 0x6C6E0003, 0x6C6E0001, 0x6C6E0000, 0x6C6E0004, 0x6C6E0005};
constexpr uint32_t kCosmeticTitles[6] = {0, 0x0DE60003, 0x0DE60001, 0x0DE60000, 0x0DE60004, 0x0DE60005};
// CB980E84's category tabs (0x02000000 + k, title 0x0DE6000k) and categories.
constexpr uint32_t kClothesTabK[6] = {0, 5, 6, 4, 3, 8};
constexpr uint32_t kClothesCategory[6] = {cas::category::Everyday, cas::category::Formal, cas::category::Underwear,
                                          cas::category::Pajamas,  cas::category::Swimwear, cas::category::Athletic};
constexpr uint32_t kClothesParts[3] = {cas::part::Top, cas::part::Bottom, cas::part::Body};
// CB980E83's trait bars, in Personality order (outgoing, nice, playful, neat, active).
constexpr uint32_t kTraitBars[5] = {0x4BDEFF00, 0x4BDEFF01, 0x4BDEFF02, 0x4BDEFF03, 0x4BDEFF04};

constexpr std::array<float, 4> kCellColor{0.80f, 0.84f, 0.98f, 1}, kCellSelected{1.0f, 0.95f, 0.55f, 1},
    kCellText{0.02f, 0.03f, 0.40f, 1}, kThumbBg{0.16f, 0.22f, 0.45f, 1}, kThumbSelectedBg{0.30f, 0.40f, 0.75f, 1};

inline std::string lower(std::string s) {
    for (auto& c : s)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// "k...=value" constants of the hidden text windows under `e`.
inline void readConstants(const UiElement& e, std::map<std::string, std::string>& out) {
    const std::string caption = e.get("caption");
    if (caption.size() > 1 && caption[0] == 'k')
        if (auto eq = caption.find('='); eq != std::string::npos)
            out.emplace(caption.substr(0, eq), caption.substr(eq + 1));
    for (const auto& c : e.children)
        readConstants(c, out);
}

inline int constantInt(const std::map<std::string, std::string>& c, const std::string& key, int fallback) {
    auto it = c.find(key);
    if (it == c.end())
        return fallback;
    try {
        return std::stoi(it->second, nullptr, 0);
    } catch (const std::logic_error&) {
        return fallback;
    }
}

// The readable part of a catalog name ("amhairshort_brown" -> "short brown").
inline std::string label(const std::string& name) {
    std::string n = name;
    for (const char* p : {"uuface_", "uuface-"})
        if (lower(n).starts_with(p))
            n = n.substr(std::string(p).size());
    if (n.size() > 2 && std::islower(static_cast<unsigned char>(n[0])) && std::islower(static_cast<unsigned char>(n[1])))
        for (const char* kind : {"hair", "body", "top", "bottom", "arch"})
            if (lower(n).substr(2).starts_with(kind)) {
                n = n.substr(2 + std::string(kind).size());
                break;
            }
    std::replace(n.begin(), n.end(), '_', ' ');
    return n;
}

} // namespace

constexpr int kThumbnailSkinTone = 2;

// What CasMode::screen has laid out when it hands over to its parts
// (familyScreen, stepScreen): the screen being made, the windows' canvas
// rectangles, the frame and its origin, the text font and screen's drawing
// helpers (a line of text, a filled rectangle, a window's click target).
struct CasMode::LayoutContext {
    UiScreen& out;
    std::map<const UiElement*, std::array<float, 4>>& rects;
    const UiElement& frame;
    float ox, oy;
    const Font* font;
    float ppem;
    std::function<void(float, float, float, const std::string&, std::array<float, 4>)> text;
    std::function<void(float, float, float, float, std::array<float, 4>)> rect;
    std::function<void(const UiElement*, std::function<void(simd_float2)>)> target;
};

} // namespace os2
