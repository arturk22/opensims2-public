#pragma once

// What the trees show the player and change on objects that only the host
// can present: dialogs (Dialog, 0x24) and lights (Change Light, 0x6F). The
// structs are plain data, held by VM and Object (vm.hpp); the primitives
// are in dialog.cpp and light.cpp.

#include "formats/dbpf.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace os2 {
struct Behavior;
struct StringTable;
}

namespace os2::vm {

class VM;
struct Object;
struct Context;

// Dialog types (operand byte 5), labelled in the editor's STR# 0xD9
// "Dialog Primtive Types" (descriptions in 0xDA).
namespace dialogtype {
constexpr int Message = 0;     // a message and a "yes" (OK) button
constexpr int YesNo = 1;       // true on yes, false on no
constexpr int YesNoCancel = 2; // the choice (0/1/2) into a temp
constexpr int TextEntry = 3;
constexpr int Tutorial = 4;
constexpr int TextNotification = 8; // on the side of the screen; does not wait
constexpr int StopTextNotification = 9;
constexpr int ModifyTextNotification = 10;
constexpr int PhoneServices = 5;      // "Phone Book - Services": objects of the stack object's array 0
constexpr int PhoneInvite = 6;        // "Phone Book - Invite": one neighbor of array 0
constexpr int PhoneParty = 7;         // "Phone Book - Party": the guests among array 0
constexpr int SpecialPhoneServices = 15;
constexpr int JewelryPurchase = 12;  // an expansion pack's jewelry rack (no base game object)
constexpr int ClothingPurchase = 16;  // the clothing rack's "Buy": the cart into the family's clothing bin
constexpr int ClothingSelection = 17; // the dresser's "Plan Outfit"
constexpr int ClothingTryOn = 20;     // the rack's "Try On": the try-on outfit
constexpr int Vanity = 21;            // the mirror's "Change Appearance": hair, makeup, glasses, facial hair
constexpr int TutorialNext = 22;      // the tutorial's progress window (tutorial.hpp)
constexpr int BabyName = 23;          // "Baby Name": the newborns (neighbors in temps 0-3) get the typed first names
constexpr int SetAspiration = 24;
constexpr int TutorialNextModal = 25; // the same, modal
constexpr int TnsBirthday = 0x1C;     // a text notification too (docs/primitives-mac.md §4)
} // namespace dialogtype

// The window class the game's factory makes for a Dialog type
// (cTSUIDialogManager::GetObjectDialog, BV 0x21c5a8: a switch from the type
// to a class ID, docs/primitives-mac.md "Dialog"), and the UI script that
// class loads (its constructor or Initialize; 0 = none: cTSObjectDialog
// builds its window in code, cTSWinPictureDialog).
enum class DialogClass {
    Object,          // cTSObjectDialog (every type the switch does not name)
    PhoneServices,   // cTSWinProcPhoneBookServices (5, 15)
    PhoneNeighbors,  // cTSWinProcPhoneBookNeighbors (6)
    PhoneParty,      // cTSWinProcPhoneBookParty (7, 0x46)
    SelectItems,     // cTSWinProcSelectItemsDialog (11, 13, 0x2B, 0x2D)
    JewelryPurchase, // cTSWinProcJewelryPurchaseDialog (12, 0x2C)
    Fill,            // cTSWinProcFillDialog (14, 0x1B, 0x1F, 0x2A)
    ClothingPurchase,// cTSWinProcClothingPurchaseDialog (16, 0x2E)
    Clothing,        // cTSWinProcClothingDialog (17, 0x31)
    ClothingTryOn,   // cTSWinProcClothingTryOnDialog (20)
    Vanity,          // cTSWinProcVanityDialog (21)
    NameObject,      // cTSWinProcNameObjectDialog (23)
    SetAspiration,   // cTSWinProcSetAspirationDialog (24, 0x22)
    Expansion,       // the classes of the expansion packs' types (0x1A on)
};
struct DialogClassInfo {
    DialogClass cls = DialogClass::Object;
    const char* name = "cTSObjectDialog";
    uint32_t script = 0; // ui.package instance (group 0xA99D8A11)
};
DialogClassInfo dialogClass(int type);

// A row of a special dialog's list: a phone book's service (an object) or
// neighbor, with what the row shows.
struct DialogEntry {
    int16_t id = 0;       // object ID (services) or neighbor ID
    uint32_t guid = 0;    // the object's or the Sim's OBJD GUID (thumbnail)
    std::string name;
    std::string detail;   // the neighbor's lot name (CreateNeighborCallList: the lot its family lives on)
    int relationship = 0; // daily + lifetime of the caller to it (CreateNeighborCallList)
    int flags = 0;        // the caller's relationship flags to it (variable 1: crush, love, ..., friend, enemy)
    int price = 0;        // a rack's item: the object's catalog price (OBJD)
    std::string first, last; // a neighbor's names (CompareByName: last, then first)
};

// A name box of the name dialog (cTSWinProcNameObjectDialog): the neighbor
// in temp 0-3 and its text, the first name it has at first.
struct DialogName {
    int16_t neighbor = 0;
    std::string text;
    bool female = false;
};

// A dialog a tree put up. Blocking ones keep the tree's instruction
// waiting (Yield) until the host answers (`answer` = the button's index,
// 0 for the first); the others (text notifications) are only shown.
struct Dialog {
    uint32_t id = 0;   // serial, per VM
    int16_t me = 0;    // whose thread waits
    int16_t stackObject = 0;
    int type = 0;      // dialogtype
    std::string typeName; // STR# 0xD9 label
    std::string title, message;
    // Button labels in the order of the answer indices: button 1 ("yes"),
    // button 2 ("no"), button 3 ("cancel"); as many as the type has.
    std::vector<std::string> buttons;
    bool blocking = true;
    // Operand byte 7 bit 7: "continue" the other Sims while it is up
    // (STR# 0xED "dialog behavior"; inferred); without it the game pauses.
    // cTSObjectDialog::SetupDialog then gives the window a close box
    // (cTSWinPictureDialog::AddCloseBox, button 1) instead of button 1.
    bool continueOthers = false;
    // Operand byte 7 bit 0 ("return"): TryDialog queues the dialog and goes
    // on at once (true); the window stays until the player closes it, and
    // nothing waits for its answer.
    bool detached = false;
    int icon = 0;      // STR# 0xF4: 0 automatic, 1 none, 2 neighbor, 3 indexed, 4 named
    // What the icon shows (cTSObjectDialog::SetParams): an object (its
    // catalog thumbnail; a Sim: the head), a neighbor's head, or a UI image
    // ("gz <instance>" of a named icon); 0 = none.
    int16_t iconObject = 0;
    int16_t iconNeighbor = 0;
    uint32_t iconImage = 0;
    DialogClassInfo cls;
    // Special dialogs: the rows of a phone book (the stack object's array 0,
    // as the class lists it), the rows chosen (a party's guests; services and
    // invite take selection[0]) and how many guests a party may have (temp
    // 5; 0 = no limit). OK writes the choice to the stack object's array 0.
    std::vector<DialogEntry> entries;
    std::vector<int16_t> selection;
    int maxSelection = 0;
    // The party's "at most N guests" line (0xC000; PreShow: button 1's
    // string printed with temp 5; empty: hidden) and window title (button
    // 2's string; empty: UIText's). The party shows the title and message
    // over its two lists.
    std::string caption, caption2;
    // The name dialog's boxes (one per newborn, or the Sim to rename), the
    // panel of the script (0xCDC2CAA4 rename, A5 one baby, A6 twins, A7 a
    // servo) and the box with the caret.
    std::vector<DialogName> names;
    uint32_t namePanel = 0;
    int nameFocus = 0;
    // The wardrobe (cTSWinProcClothingDialog): whose clothes (the stack
    // object if it is a Sim, else the Sim running the tree) and which
    // outfit category, as the bit of the character file's outfit lists
    // (SetParams maps temp 0, a Change Outfit literal: 0 everyday 0x001,
    // 1 naked 0x080, 2 swimwear 0x008, 3 athletic 0x200, 4 formal 0x020,
    // 5 pajamas 0x010, 6 try-on 0x400, 8 maternity 0x100, 9/10 the other
    // casuals 0x002/0x004, 11 underwear 0x040, 13 0x1000).
    int16_t outfitSim = 0;
    uint32_t outfitCategory = 0;
    // The clothing rack (cTSWinProcClothingPurchaseDialog, 16) and the
    // try-on (cTSWinProcClothingTryOnDialog, 20) map temp 0 the same way;
    // for the everyday categories (Change Outfit 0, 9, 10) they offer tops
    // and bottoms besides whole outfits and open on tops (SetParams' 0x601
    // test), else whole outfits only. The rack charges temp 1 per item
    // (cTSShoppingCartBase::SetPricePerItem) and OK puts the cart (the GZPS
    // keys) in the family's clothing bin (SaveCartStandard ->
    // cTSShoppingCart::CompletePurchase: cTSFamily::AddClothing for each,
    // then the total off the family's funds). `fillFunds` holds the funds.
    bool outfitSeparates = false;
    int pricePerItem = 0;
    std::vector<Tgi> cart;
    // The vanity (cTSWinProcVanityDialog, 21): temp 0 = 1 is the expansion
    // packs' plastic surgery panel (script 0x8C159286, not in the base
    // game); the Sim is the stack object.
    bool vanitySurgery = false;
    // The aspiration dialog (cTSWinProcSetAspirationDialog, type 24): the
    // neighbor (ID in the stack object) and the aspiration picked, as person
    // data 46's bits (1 romance, 2 family, 4 fortune, 0x10 popularity, 0x20
    // knowledge; 0 none yet: OK is off, Validate).
    int16_t aspirationNeighbor = 0;
    int16_t aspiration = 0;
    // The food dialog (cTSWinProcFillDialog, type 14): from the tree's
    // locals at operand byte 11 (SetParams), the units it has, the cost of
    // 50 units, the delivery cost and the most units it takes; the fill
    // bar's value (units after buying) and the family's funds (the bar
    // stops where the total passes them). OK puts value - units in temp 0.
    int fillUnits = 0, fillCost = 0, fillDelivery = 0, fillMax = 0, fillValue = 0, fillFunds = 0x7FFFFFFF;
    // The racks (cTSWinProcSelectItemsDialog, types 11-13): several items
    // may be picked (temp 0 = 1) and only what the family can pay (temp 1
    // = 1; funds in fillFunds). The items are `entries`, the picks
    // `selection`; OK writes -1 over the items of array 0 not picked.
    bool multiSelect = false, obeyPrice = false;
    int timeout = 0;   // operand byte 10: a text notification's lifetime in seconds (0: until closed)
    int priority = 0;  // operand byte 9: a text notification's priority
    // A blocking dialog's time limit (ms; 0: none): it then takes its last
    // button (autoTimeOutDialogs).
    int timeoutMs = 0;
    double opened = -1; // host clock (seconds) when autoTimeOutDialogs first saw it
    int answer = -1;   // set by the host
    // Text entry (3): what the player typed, set by the host with the
    // answer. (The name dialog keeps its texts in `names`; `text` and
    // `female` mirror its first box.)
    std::string text;
    bool female = false;
    // The instruction waiting for it (the thread's stack depth, tree, node).
    size_t depth = 0;
    const Behavior* tree = nullptr;
    uint16_t node = 0;
};
constexpr size_t kMaxNotifications = 64;

// Whether a blocking dialog without "continue" is waiting for the player:
// the game pauses every Sim until it is answered (STR# 0xED "engage and
// block sim"). Hosts stop ticking meanwhile.
bool dialogPausesGame(const VM& vm);

// Dialogs nobody answers (docs/primitives-mac.md, Dialog): the game's
// message, yes/no, yes/no/cancel and text entry dialogs (types 0-3, not
// the tutorial) click their LAST button by themselves 4000 + 40 x (length
// of the message) ms after they open (cTSWinPictureDialog::SetTimeout, a
// timer of real time), and a text notification goes away byte 10 seconds
// after it appeared. `now` is the host's clock in seconds: the app passes
// real time; headless runs (VM::tick with `vm.dialogClockIsTicks`) pass
// game time at speed 1, ticks / ticksPerSecond (30 ticks = 1 s). Returns
// how many dialogs were answered.
int autoTimeOutDialogs(VM& vm, double now);

// The language's text formatter as ParseUIString uses it (cRZLanguageUtility,
// docs/primitives-mac.md "Dialog", round n45), for a STR# language code:
// MakeNumberString (digits in threes: "1.200" in Brazilian Portuguese,
// "1,200" in US English) and MakeTimeString with minutes and no seconds
// ("17:00"; "5:00 pm" in US English).
std::string languageNumber(int64_t value, uint8_t language);
std::string languageTime(int hour, int minute, uint8_t language);

// Expands the `$` tokens of a dialog string (`$Me`, `$Object`, `$Local:n`,
// `$NameLocal:n`, `$TimeLocal:n`, ...; see dialog.cpp) for the instruction
// running in `c`.
std::string expandDialogTokens(Context& c, const std::string& text, int depth = 0);

// The STR# tables of the game's text packages (Text/*.package, stored in the
// local group and named by the package's group hash, as UiResources reads
// them), which the host provides: the VM's code library has the objects'
// tables only. Null (unset or missing) leaves the tokens that need them
// unexpanded. Live.package (0x7F96C284) has the aspiration level names
// (0x9A adult, 0x9B child, 0x9C teen, 0x9D elder) and the baby names (0x95).
std::function<const StringTable*(uint32_t group, uint32_t instance)>& textTables();
constexpr uint32_t kLiveTextGroup = 0x7F96C284;

// The name a dialog shows for an object: a Sim's first name (the
// neighborhood's, else the one in its OBJD file name), else the object's
// catalog name (CTSS), else its OBJD file name.
std::string displayName(VM& vm, const Object& o);

// A light's state, as Change Light leaves it: the intensity in percent it
// goes to, from what, from which VM tick and over how many ticks (the
// game's change is instant in the VM; the fade is the renderer's).
struct LightState {
    int from = 100, to = 100;
    uint64_t since = 0;
    int ticks = 0;
    int level(uint64_t now) const {
        if (ticks <= 0 || now >= since + uint64_t(ticks))
            return to;
        return from + int((to - from) * double(now - since) / ticks);
    }
};
// Lights of an object by name ("" = every light of its model).
using ObjectLights = std::map<std::string, LightState>;

// For the renderer: an object's light level now (0-100) for a light name
// (the named light, else the object's "all lights" state), or -1 when
// Change Light has not touched it since the lot loaded (a resumed lot does
// not rerun the Inits that set it: the lamps keep their on/off state in
// their own attributes, e.g. the lamp semi-global's semi-attribute 2).
int lightLevel(const VM& vm, const Object& o, const std::string& name = {});

void registerDialogPrimitives(VM& vm); // Dialog (0x24)
void registerAppearancePrimitives(VM& vm); // Change Light (0x6F), Change Outfit (0x79): appearance.cpp

} // namespace os2::vm
