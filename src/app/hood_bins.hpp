#pragma once

// The neighborhood puck's Families and Lots & Houses panels and the tools
// they start, from ui.package, drawn by the neighborhood UI (app/hood_ui.hpp):
//
//   0x49001004  "Families" (puck button 0x2BE6F563, F1): the family bin
//               (window 0x0B: collapsed 0x1000 / expanded 0x2000, items in
//               the hidden rectangles 0x1006 / 0x2006, Previous/Next
//               0x1004-5 / 0x2004-5, Expand/Collapse 0x1009 / 0x2009), one
//               0x49001006 item per family (the family's snapshot under the
//               button overlay), "Create New Family" 0x0A and "Move Family"
//               0x6D13DDA1 (the evict tool)
//   0x49001003  "Lots & Houses" (0x4BE6F571, F2): "Bulldoze" 0xB000 (the
//               bulldoze tool); move/rotate lot 0xA000, move to the bin
//               0xE000, the lots bin 0xC100 and the empty lots 0xD100 are
//               shown disabled (placing lots is not done)
//   0x49001035  the small "Rename this lot?" dialog, reused for the
//               questions and errors (its text, Accept 0xA000, Cancel 0xA001)
//
// Each panel hangs from the puck as its hidden window captioned
// "aabbccdd:tl" says: that window's top left on the puck's 0xAABBCCDD.
// Selecting a family in the bin starts moving it in: a click on a lot asks
// "Move %s family into this lot? Cost to move in: §n" (Neighborhood.package
// STR# 0x82 #5/#6) or says why not (#2 cannot afford, #3 community lot, #4
// occupied), as cTSUserToolNeighborhoodMoveIn::ConfirmAndMoveInFamily and
// NormalNeighborhoodErrorText do. The evict tool asks STR# 0x83 #18, the
// bulldoze tool #26 (or #29 when a family lives there). The rules are
// formats/hood_moves.hpp's; this only asks. Which panel layout details the
// window procedures add (cTSWinProcNeighborhoodUCPSubPanelFamilies) is
// inferred: the bin's items fill its rectangle, two per row.

#include "formats/image.hpp"
#include "formats/ui_screen.hpp"
#include "formats/ui_script.hpp"

#include <simd/simd.h>

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace os2 {

class FontLibrary;
class UiResources;
namespace hood {
struct Neighborhood;
}

class HoodBins {
public:
    HoodBins(UiResources& res, FontLibrary& fonts);
    ~HoodBins();

    enum class Panel { None, Families, Lots };
    enum class Tool { None, MoveIn, Evict, Bulldoze };
    // What a click asked for: the neighborhood mode carries it out.
    struct Request {
        enum class Kind { None, Consumed, CreateFamily, MoveIn, MoveOut, Bulldoze } kind = Kind::None;
        uint32_t family = 0, lot = 0;
    };

    Panel panel() const { return panel_; }
    Tool tool() const { return tool_; }
    uint32_t selectedFamily() const { return selected_; }
    bool modal() const { return dialog_.has_value(); }
    void togglePanel(Panel p);
    void selectFamily(uint32_t family); // starts moving it in (0: none)
    void cancelTool();
    void setTool(Tool t) { tool_ = t, selected_ = t == Tool::MoveIn ? selected_ : 0, dirty_ = true; }

    // The neighborhood the panels show (reread after every change).
    void setNeighborhood(const hood::Neighborhood* hood);
    // A lot clicked while a tool is on: opens the question or the error.
    void lotClicked(uint32_t lot);
    // A message box with only Accept (the result of a change that failed).
    void showMessage(const std::string& text);

    // Lays the panels out (the puck at `puck`, its canvas rectangle), adds
    // their click targets; then `press` answers a click.
    void layout(UiScreen& out, const std::array<float, 4>& puckRect, const UiElement& puck,
                std::map<const UiElement*, std::array<float, 4>>& puckRects, int width, int height);
    Request press(simd_float2 p);
    bool dirty() const { return dirty_; }
    void clean() { dirty_ = false; }

private:
    const RgbaImage* familySnapshot(uint32_t family);
    std::string text(uint32_t table, uint32_t index, const char* fallback) const;
    void openQuestion(const std::string& message, Request onAccept);

    UiResources* res_;
    FontLibrary* fonts_;
    UiScript families_, lots_, item_, dialogScript_, hover_;
    bool ok_ = false;
    const hood::Neighborhood* hood_ = nullptr;
    std::map<uint32_t, std::unique_ptr<RgbaImage>> snapshots_;

    Panel panel_ = Panel::None;
    Tool tool_ = Tool::None;
    uint32_t selected_ = 0;
    size_t page_ = 0;
    bool expanded_ = false;
    struct Dialog {
        std::string message;
        Request onAccept; // Kind::None: a message (Accept only closes it)
    };
    std::optional<Dialog> dialog_;

    enum class Hit { Request, Family, Expand, Prev, Next, Create, Evict, Bulldoze, Accept, Cancel, Solid };
    struct Target {
        std::array<float, 4> rect;
        Hit hit;
        uint32_t value = 0;
    };
    std::vector<Target> targets_;
    bool dirty_ = true;
};

} // namespace os2
