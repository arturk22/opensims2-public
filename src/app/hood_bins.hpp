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
//               bulldoze tool), move/rotate lot 0xA000, move to the bin
//               0xE000, and the panels the binary's SetPanel switches:
//               0xC100 the lots bin (0xC000: items in 0xC300's 0x1006 /
//               0x2006, the lot's picture IMG 0x35CA0000), 0xD100 the empty
//               lots (0xD000: sizes 0xD001-0xD00A = cTSUserToolPlaceNewLot
//               sizes 0-9, cTSWinProcNeighborhoodUCPSubPanelTools::
//               onBtnClickedID)
//   0x49001028  "Opções" (0x2BF08BEC, F5): Main Menu 0xA221 and Quit 0xA220;
//               the option windows 0xA241-0xA246 are Request::OptionWindow
//               (app/options_panel.hpp draws them)
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

#include "formats/hood_lots.hpp"
#include <functional>
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
struct BinLot;
}

class HoodBins {
public:
    HoodBins(UiResources& res, FontLibrary& fonts);
    ~HoodBins();

    enum class Panel { None, Families, Lots, Options, Decorations };
    enum class Tool { None, MoveIn, Evict, Bulldoze, PlaceEmpty, PlaceBin, MoveLot, ToBin, Decorate, DecorHand, DecorPick };
    // What a click asked for: the neighborhood mode carries it out.
    struct Request {
        enum class Kind { None, Consumed, CreateFamily, MoveIn, MoveOut, Bulldoze, ToBin, PlaceLot, DeleteBinLot, MainMenu, Quit, OptionWindow, DecorUndo, DecorRedo } kind = Kind::None;
        uint32_t family = 0, lot = 0; // DeleteBinLot: lot = the bin index; OptionWindow: lot = 1 audio, 2 game, 3 graphics, 4 camera, 5 credits
    };
    // The Lots & Houses bin's lots (formats/hood_lots.hpp) and the tools'
    // picks: the empty lot size (0-9, buttons 0xD001-0xD00A), the bin lot.
    void setLotBin(std::vector<hood::BinLot> lots);
    const std::vector<hood::BinLot>& lotBin() const { return binLots_; }
    int emptySize() const { return emptySize_; }
    int binLot() const { return binLot_; }
    void pickEmptyLot(int size);  // the place-empty-lot tool
    void pickBinLot(size_t index); // the place-from-bin tool
    void setLotsPanel(int which) { lotsPanel_ = which, dirty_ = true; } // 0 bin, 1 empty lots

    // The Decorations panel (F3, ui.package 0x4D517180): the catalog's
    // products by sort (the five buttons A100 flora, A200 stone, A300
    // landmark, A400 effects, A500 misc), the Hand tool (A5: pick a placed
    // decoration up), the Eyedropper (A4: take a placed decoration's product)
    // and Undo / Redo (A1, A2), over the same bin as the families' (collapsed
    // 0x1000, expanded 0x2000). Choosing a product is Tool::Decorate: the
    // neighborhood mode puts it where the mouse clicks.
    void setDecorCatalog(std::vector<hood::DecorProduct> products) { decorCatalog_ = std::move(products), dirty_ = true; }
    const std::vector<hood::DecorProduct>& decorCatalog() const { return decorCatalog_; }
    uint32_t decorGuid() const { return decorGuid_; }
    void pickDecoration(uint32_t guid); // Tool::Decorate with this product (0: none)
    const std::string& decorSort() const { return decorSort_; }
    void setDecorSort(const std::string& sort) { decorSort_ = sort, decorPage_ = 0, dirty_ = true; }
    // The products the bin shows now (sort, page and size).
    std::vector<uint32_t> visibleDecorations() const;
    // A product's picture (null: none yet): set by the app, which renders them.
    std::function<const RgbaImage*(uint32_t guid)> decorThumb;
    // Where the Options panel was laid out last (its bottom left, canvas pixels).
    simd_float2 optionsBottomLeft() const { return optionsBL_; }
    Panel panel() const { return panel_; }
    Tool tool() const { return tool_; }
    uint32_t selectedFamily() const { return selected_; }
    bool modal() const { return dialog_.has_value() || placeLot_.has_value(); }
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

    // The place-lot dialog (ui.package 0x49001002, cTSWinProcPlaceLotDialog)
    // a new empty lot asks before it is placed: "Residential" 1 /
    // "Community" 2 (one on, residential at first), the name in text edit 3
    // starting as "<Enter Lot Name>" (STR# 0x83 #29, all selected: typing
    // replaces it), Accept 0xA000 / Cancel 0xA001. Accept checks the name as
    // onBtnClickedID does: none of \ / : * ? < > | ; (nTSUI::
    // ValidateUIString; UIText STR# 0x99 #9-#11) and not another lot's name
    // (STR# 0x82 #8/#9); then Request::Kind::PlaceLot with placeLotName()
    // and placeLotZone() (0 residential, 1 community).
    void openPlaceLot();
    bool textEntry() const { return placeLot_.has_value() && !dialog_; }
    // A key while the dialog is up: Return accepts, Esc cancels, Delete
    // erases, `typed` (UTF-8) is added. Returns the request Return made.
    Request typeKey(int keyCode, const std::string& typed);
    void setPlaceLotName(const std::string& name) {
        if (placeLot_)
            placeLot_->name = name, placeLot_->edited = true, dirty_ = true;
    }
    const std::string& placeLotName() const { return placedName_; }
    uint8_t placeLotZone() const { return placedZone_; }

    // Lays the panels out (the puck at `puck`, its canvas rectangle), adds
    // their click targets; then `press` answers a click.
    // `panelsEnd` (optional) gets where the panels' quads end in `out`, before
    // the question box: the puck is drawn between them.
    void layout(UiScreen& out, const std::array<float, 4>& puckRect, const UiElement& puck,
                std::map<const UiElement*, std::array<float, 4>>& puckRects, int width, int height,
                size_t* panelsEnd = nullptr);
    Request press(simd_float2 p);
    // Whether a click at `p` would land on one of the panels' buttons.
    bool hits(simd_float2 p) const;
    bool dirty() const { return dirty_; }
    void clean() { dirty_ = false; }

private:
    const RgbaImage* familySnapshot(uint32_t family);
    std::string text(uint32_t table, uint32_t index, const char* fallback) const;
    void openQuestion(const std::string& message, Request onAccept);

    UiResources* res_;
    FontLibrary* fonts_;
    UiScript families_, lots_, item_, dialogScript_, hover_, options_, decor_;
    std::vector<hood::DecorProduct> decorCatalog_;
    std::string decorSort_ = "flora";
    uint32_t decorGuid_ = 0;
    size_t decorPage_ = 0;
    bool decorExpanded_ = false;
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
        bool announced = false;
    };
    std::optional<Dialog> dialog_;
    struct PlaceLot {
        std::string name;
        bool edited = false, community = false, announced = false;
    };
    std::optional<PlaceLot> placeLot_;
    std::string placedName_;
    uint8_t placedZone_ = 0;
    UiScript placeScript_;
    Request acceptPlaceLot();

    enum class Hit { Request, Family, Expand, Prev, Next, Create, Evict, Bulldoze, Accept, Cancel, Solid, LotsPanel,
                     EmptySize, BinItem, BinExpand, BinPrev, BinNext, MoveLotTool, ToBinTool,
                     PlaceResidential, PlaceCommunity, PlaceAccept, PlaceCancel, BinFilter, BinDelete,
                     OptMainMenu, OptQuit, OptWindow, DecorItem, DecorSort, DecorTool, DecorUndo, DecorRedo, DecorPrev, DecorNext, DecorExpand };
    const RgbaImage* binPicture(size_t index);
    std::vector<hood::BinLot> binLots_;
    std::map<size_t, std::unique_ptr<RgbaImage>> binPictures_;
    int lotsPanel_ = 0, emptySize_ = -1, binLot_ = -1, binFilter_ = 0;
    size_t binPage_ = 0;
    bool binExpanded_ = false;
    struct Target {
        std::array<float, 4> rect;
        Hit hit;
        uint32_t value = 0;
    };
    std::vector<Target> targets_;
    simd_float2 optionsBL_{0, 0};
    bool dirty_ = true;
};

} // namespace os2
