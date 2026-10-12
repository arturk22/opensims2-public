#pragma once

// Helpers shared by the in-game panels drawn from ui.package scripts (the
// live panel and its subpanels; other modes' panels can use them too). Plain
// C++ over the formats layer, so the tests link them.

#include "formats/image.hpp"
#include "formats/ui_script.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace os2 {

// Depth-first search for a window by id under `e` (e itself included).
const UiElement* findWindow(const UiElement& e, uint32_t id);
UiElement* findWindow(UiElement& e, uint32_t id);

// "(x0,y0,x1,y1)", the value of an area attribute.
std::string uiArea(int x0, int y0, int x1, int y1);

// Canvas rectangle (x0, y0, x1, y1) test.
inline bool insideRect(const std::array<float, 4>& r, float x, float y) {
    return x >= r[0] && x < r[2] && y >= r[1] && y < r[3];
}

// Nearest-neighbour resize.
RgbaImage scaledImage(const RgbaImage& src, uint32_t width, uint32_t height);

// Money as the Brazilian Portuguese locale writes it: "§511.632" (the game
// asks the locale's format service, docs/ui-live-mac.md §2).
std::string formatMoney(int64_t value);

// The instance of an object's picture in UserData/Thumbnails/
// ObjectThumbnails.package (type 0xAC2950C1, group 0x6C2A22C3), as
// cTSUIThumbnailService::GetObjectThumbnailGUIDAndModelName (BV 0x2a0ba8)
// keys it: CRC32 (poly 0x04C11DB7, MSB first, init ~0, no final xor; the
// game's FUN_00120ff8) of the lower-cased "<OBJD group as a signed
// decimal><model name>", the model name being the OBJD's entry (model index)
// of its group's STR# 0x85. 531 of the 532 shipped thumbnails match.
uint32_t objectThumbnailInstance(uint32_t group, const std::string& modelName);

// The colour of a gradient fill bar (cTSWinProcGradientFillBar,
// docs/ui-live-mac.md §5): the mask's size, each row (horizontal) or column
// (vertical) a copy of the gradient's row/column i = round(N (v - min) /
// (max - min)), limited to N - 1, alpha from the mask.
RgbaImage gradientFill(const RgbaImage& gradient, const RgbaImage& mask, float value, float min, float max,
                       bool vertical);

// A stepped bar (the skills' and personality's fill bars: range 0..1000 in
// 10 divisions): the "full" image cut after floor(divisions * v / max) of
// its divisions (equal widths), the rest transparent.
RgbaImage segmentFill(const RgbaImage& full, float value, float max, int divisions);

// The UI sound a button's script names: btnclicksnd={0,0xFFxxxxxx} keeps
// the CRC24 of the name (as the resource index keys sounds); the name from
// the executable's ui_* names (docs/ui-live-mac.md §7.7). Empty if none.
std::string uiButtonSound(const UiElement& button, std::string_view attr = "btnclicksnd");

// The action queue's icon positions (cTSUIActionQueue::OnTick, BV 0x1e44b0):
// top-level icons side by side from (x0, y0), `margin` apart; the
// sub-interactions stacked down at x0 under them, the first moved by
// `subOffset` (the nested window's top minus the top-level's bottom in
// 0x0609A1C0: -4). When both kinds are shown the first top-level icon and
// every sub-interaction after the first show the connector
// (SetHasConnectedIcon).
struct QueueSlot {
    float x = 0, y = 0;
    bool connected = false;
};
struct QueueMetrics {
    float x0 = 50, y0 = 10, margin = 4;
    float topWidth = 51, subHeight = 52, subOffset = -4;
};
std::vector<QueueSlot> layoutActionQueue(const std::vector<bool>& nested, const QueueMetrics& m);

// The windows a panel's screen drew, by id, to find a control by the game's
// window ids (the tutorial's "Button GUIDs": a window and a control in it;
// 0x480E8304, the main window, holds them all). A window holds a control
// when its rectangle holds the control's (inferred stand-in for the window
// tree); a window not drawn itself holds any control with the id.
struct DrawnWindows {
    std::vector<std::pair<uint32_t, std::array<float, 4>>> list;
    void capture(const std::map<const UiElement*, std::array<float, 4>>& rects);
    std::optional<std::array<float, 4>> find(uint32_t window, uint32_t control) const;
};

} // namespace os2
