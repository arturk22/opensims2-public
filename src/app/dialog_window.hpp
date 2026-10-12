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
//   cTSWinProcClothingPurchaseDialog (16), cTSWinProcClothingTryOnDialog
//     (20) and cTSWinProcVanityDialog (21): scripts 0x8C159251, 0x8C159255
//     and 0x8C159280 with their grids of CAS parts (the host's).
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
#include <tuple>
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
    enum class Hit { Button, Row, Add, Remove, NameBox, Sort, Close, Tab, Prev, Next, Choice, Bar };
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
        std::map<uint32_t, std::array<float, 4>> colors; // window id -> tint (greyed-out items)
        std::vector<uint32_t> buttonIds;              // answer order (hover state)
        std::array<float, 4> textBox{0, 0, 0, 0};     // the caret's box (canvas), if any
        std::string caretText;                        // the text before the caret
        std::string caretFont;
        std::vector<std::string> lines;               // plain-box text
        std::map<uint32_t, int> states;               // window id -> button strip frame (0 off, 1, 2 pressed, 3)
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
    // The clothing rack (16) and try-on (20): the CAS clothing (GZPS) with
    // `partBit` (0x04 tops, 0x10 bottoms, 0x08 whole outfits) of an age and
    // gender in the outfit category, `owned` when it is in the Sim's
    // family's clothing bin and `worn` when the Sim wears it in that
    // category; and the Sim's age and gender bits (the rack's buttons start
    // on them).
    struct ShopItem {
        std::string part; // catalog name (GZPS)
        Tgi key;          // the GZPS (what the family bin keeps)
        const RgbaImage* thumb = nullptr;
        bool owned = false, worn = false;
    };
    std::function<std::vector<ShopItem>(int16_t sim, uint32_t category, uint32_t partBit, uint32_t age, uint32_t gender)>
        shopItems;
    std::function<std::pair<uint32_t, uint32_t>(int16_t sim)> simAgeGender;
    // The vanity (21): the host keeps the look being edited. Items of a
    // group (VanityGroup; a "none" first where the group may be empty, its
    // thumb null), a pick, and the end: OK writes the character file and
    // dresses the Sim again (OnOk), cancel drops the edit.
    enum VanityGroup {
        VanityHair, VanityBlush, VanityEyeliner, VanityEyeShadow, VanityLipstick, VanityGlasses, VanityCostume,
        VanityBrows, VanityStubble, VanityBeards, VanityHairColor, VanityGroups
    };
    struct VanityItem {
        const RgbaImage* thumb = nullptr;
        bool selected = false;
    };
    std::function<std::vector<VanityItem>(int16_t sim, int group)> vanityItems;
    std::function<void(int16_t sim, int group, int index)> vanityPick;
    std::function<void(int16_t sim, bool ok)> vanityDone;
    // The aspiration dialog's "Big Life Moments" catalog: the lifetime goals
    // of an aspiration bit (WantTuning's <lifetimeGoals>, as the want
    // simulator's GetLifetimeGoals gives them), each want's icons (UI image
    // instances), fear and Power level.
    struct LifetimeWant {
        uint32_t icon = 0, modifier = 0;
        bool fear = false, power = false;
        std::string name;
    };
    std::function<std::vector<LifetimeWant>(uint32_t aspiration)> lifetimeWants;
    // Answers go to stdout (the headless runs read them).
    bool log = true;

private:
    void answer(vm::Dialog& d, int index);
    Layout pictureDialog(const vm::Dialog& d, int width, int height);
    Layout nameDialog(const vm::Dialog& d, int width, int height);
    Layout phoneDialog(const vm::Dialog& d, int width, int height);
    Layout plainDialog(const vm::Dialog& d, int width, int height);
    Layout wardrobeDialog(const vm::Dialog& d, int width, int height);
    Layout aspirationDialog(const vm::Dialog& d, int width, int height);
    Layout fillDialog(const vm::Dialog& d, int width, int height);
    void wantsCatalog(Layout& L, UiElement& slide, std::array<float, 2> slideAbs, const std::vector<LifetimeWant>& wants);
    const RgbaImage* maskedIcon(uint32_t icon, uint32_t mask);
    std::map<uint64_t, std::unique_ptr<RgbaImage>> masked_;
    Layout selectItemsDialog(const vm::Dialog& d, int width, int height);
    Layout shopDialog(const vm::Dialog& d, int width, int height); // 16 and 20
    Layout vanityDialog(const vm::Dialog& d, int width, int height);
    // The rack's / try-on's state: the tab (0 tops, 1 bottoms, 2 whole
    // outfits), the age and gender shown, the view (0 rack, 1 cart), the
    // first item of each tab's page, the item picked, the cart's page and
    // pick, the lists by (tab, age, gender) and the try-on's choice per tab.
    struct Shop {
        bool ready = false;
        int tab = 0, view = 0, pick = -1, cartFirst = 0, cartPick = -1;
        uint32_t age = 0, gender = 0;
        std::array<int, 3> first{0, 0, 0};
        std::vector<ShopItem> cart;
        std::map<std::tuple<int, uint32_t, uint32_t>, std::vector<ShopItem>> lists;
        std::array<std::string, 3> chosen; // try-on: top, bottom, whole outfit
    };
    Shop& shop(const vm::Dialog& d);
    const std::vector<ShopItem>& shopList(const vm::Dialog& d, Shop& s, int tab);
    struct Vanity {
        int tab = 0;
        std::map<int, int> first; // group -> first item shown
        std::map<int, std::vector<VanityItem>> items;
    };
    std::vector<VanityItem>& vanityList(const vm::Dialog& d, int group);
    // A grid cell from a thumbnail script (its bg, the picture in window 6,
    // the strip button 5; 7 and 8 the in-cart and owned marks) at (x, y) of
    // `parent`, its ids renumbered; returns the cell's canvas rectangle.
    std::array<float, 4> addCell(Layout& L, UiElement& parent, std::array<float, 2> parentAbs, uint32_t cellScript,
                                 uint32_t serial, float x, float y, const RgbaImage* thumb, int state, bool mark7,
                                 bool mark8);
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
    std::map<uint32_t, int> pages_; // dialog id -> the racks' first item shown
    std::map<uint32_t, Shop> shops_;
    // The phone books' view: the neighbors' mode (SetMode: 1 text by
    // relationship, 2 text by name, 4 slides by relationship, 5 slides by
    // name, the constructor's), the party lists' sorts (0 as given, 1 name,
    // 2 relationship; cTSWinProcPhoneBookParty::PreShow sorts both by name,
    // SetNeighborListBoxSort(1) / SetInviteListBoxSort(1)) and the slides'
    // first item.
    struct Phone {
        int mode = 5, leftSort = 1, rightSort = 1, first = 0;
    };
    std::map<uint32_t, Phone> phones_;
    // A list's rows in the order shown (party: kRowsList the neighbors not
    // invited, kRowsInvite the guests; the others: every entry).
    std::vector<const vm::DialogEntry*> phoneRows(const vm::Dialog& d, int list);
    std::map<uint32_t, Vanity> vanities_;
};

} // namespace os2
