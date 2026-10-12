#pragma once

// The windows of the Dialog primitive's dialogs (vm/dialog.hpp), as the
// game's classes build them (docs/primitives-mac.md "Dialog"):
//
//   cTSObjectDialog (types 0-4 and the ones the factory does not name): a
//     cTSWinPictureDialog built in code, laid out as its DoLayout does
//     (frame 0xA9500615 in nine parts, title GenHeader, text GenDialogBody,
//     buttons GenButton on 0xA9500630, the icon on 0xE9000005-7);
//   cTSWinProcNameObjectDialog (23): the panels of script 0x2D91050A (one
//     baby, twins, rename);
//   cTSWinProcPhoneBookServices/Neighbors/Party (5/15, 6, 7): scripts
//     0x8C159243, 0x8C159244 and 0x8C159242 with their list boxes.
//
// The other special dialogs still show as an OK/cancel box.
//
// Plain C++ over the formats layer (the tests link it); the pie menu host
// draws it over the lot and feeds it the mouse and the keys.

#include "formats/image.hpp"
#include "formats/ui_screen.hpp"
#include "formats/ui_script.hpp"
#include "vm/dialog.hpp"

#include <array>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace os2 {

class FontLibrary;
class UiResources;
namespace vm {
class VM;
}

class DialogWindow {
public:
    // Without resources (tests) the windows are laid out with plain boxes
    // and fixed text widths.
    DialogWindow(vm::VM& vm, UiResources* res, FontLibrary* fonts);
    ~DialogWindow();

    // A region of the window and what a click there does.
    enum class Hit { Button, Row, Add, Remove, NameBox, Sort, Close, Tab, Prev, Next };
    struct Region {
        std::array<float, 4> rect{0, 0, 0, 0}; // canvas x0, y0, x1, y1
        Hit kind = Hit::Button;
        int value = 0; // button: answer index; row: index (list << 16 | row); name box: index
    };
    struct Layout {
        float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        std::vector<Region> regions;
        UiElement root; // drawn at (x0, y0); empty tag: drawn with plain boxes
        std::map<uint32_t, std::string> captions;     // window id -> text
        std::map<uint32_t, const RgbaImage*> images;   // window id -> image
        std::vector<uint32_t> buttonIds;              // answer order (hover state)
        std::array<float, 4> textBox{0, 0, 0, 0};     // the caret's box (canvas), if any
        std::string caretText;                        // the text before the caret
        std::string caretFont;
        std::vector<std::string> lines;               // plain-box text
    };
    Layout layout(const vm::Dialog& d, int width, int height);
    void draw(UiScreen& out, const vm::Dialog& d, int width, int height);
    // A click at canvas p on the open dialog; true when the dialog took it
    // (it is modal: clicks never go through).
    bool click(vm::Dialog& d, float x, float y, int width, int height);
    void hover(const vm::Dialog& d, float x, float y, int width, int height);
    // Mac key code and the characters typed. Return/Enter = button 1, Esc =
    // the last button, space = button 1 (cTSObjectDialog::SetupDialog's
    // SetKey; not in yes/no and yes/no/cancel); text boxes take the rest.
    bool key(vm::Dialog& d, int keyCode, const std::string& typed);
    // The dialog has a box that takes typing.
    static bool hasTextBox(const vm::Dialog& d);
    // The text the caret is in (text entry: the dialog's text; names: the
    // focused box).
    static std::string& editText(vm::Dialog& d);
    // Whether OK may be pressed now (the name dialog's EnableOkButton: no
    // empty box).
    static bool okEnabled(const vm::Dialog& d);

    // A 40x40 picture for the icon and the phone books' thumbnails: an
    // object's catalog thumbnail or a Sim's head, by the object's ID or a
    // neighbor's ID (one of the two is 0). Null: none.
    std::function<const RgbaImage*(int16_t object, int16_t neighbor)> thumbnail;
    // The wardrobe's choices (cTSWinProcClothingDialog): per tab (0 tops,
    // 1 bottoms, 2 whole outfits) the catalog parts of the Sim's age and
    // gender in the category, the one it wears, and the tab it opens on
    // (whole outfits when it wears one, else tops: SetParams).
    struct OutfitItem {
        std::string part; // catalog name (GZPS)
        const RgbaImage* thumb = nullptr;
    };
    struct Outfits {
        std::array<std::vector<OutfitItem>, 3> lists;
        std::array<int, 3> current{0, 0, 0};
        int tab = 2;
    };
    std::function<std::optional<Outfits>(int16_t sim, uint32_t category)> outfits;
    // OK in the wardrobe: dress the Sim (its character file) in the parts
    // chosen: a whole outfit, or a top and a bottom (empty strings for the
    // others; OnOk copies the try-on outfit into the category).
    std::function<void(int16_t sim, uint32_t category, const std::string& top, const std::string& bottom,
                       const std::string& body)>
        applyOutfit;
    // Answers go to stdout (the headless runs read them).
    bool log = true;

private:
    void answer(vm::Dialog& d, int index);
    Layout pictureDialog(const vm::Dialog& d, int width, int height);
    Layout nameDialog(const vm::Dialog& d, int width, int height);
    Layout phoneDialog(const vm::Dialog& d, int width, int height);
    Layout plainDialog(const vm::Dialog& d, int width, int height);
    Layout wardrobeDialog(const vm::Dialog& d, int width, int height);
    Outfits* wardrobe(const vm::Dialog& d);
    const UiScript* script(uint32_t id);
    const RgbaImage* icon(const vm::Dialog& d);
    const RgbaImage* frame0(uint32_t image); // a four-state strip's first frame
    float textWidth(const std::string& style, const std::string& s) const;
    float lineHeight(const std::string& style) const;
    std::vector<std::string> wrap(const std::string& style, const std::string& text, float width) const;

    UiResources* res_;
    FontLibrary* fonts_;
    std::map<uint32_t, std::optional<UiScript>> scripts_;
    std::map<uint32_t, std::unique_ptr<RgbaImage>> icons_; // by dialog id
    std::map<uint32_t, std::unique_ptr<RgbaImage>> frames_; // by image instance
    int hover_ = -1;          // region index
    uint32_t edited_ = 0;     // the dialog whose default text a key replaced
    int editedBox_ = -1;
    std::map<uint32_t, int> partyFocus_; // dialog id -> focused list row (list << 16 | row)
    std::map<uint32_t, std::optional<Outfits>> wardrobes_; // dialog id -> the wardrobe's lists and choices
};

} // namespace os2
