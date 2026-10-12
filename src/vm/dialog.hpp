#pragma once

// What the trees show the player and change on objects that only the host
// can present: dialogs (Dialog, 0x24) and lights (Change Light, 0x6F). The
// structs are plain data, held by VM and Object (vm.hpp); the primitives
// are in dialog.cpp and light.cpp.

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
constexpr int TutorialNext = 22;      // the tutorial's progress window (tutorial.hpp)
constexpr int BabyName = 23;          // "Baby Name": the newborn (stack object) gets the typed first name
constexpr int TutorialNextModal = 25; // the same, modal
constexpr int TnsBirthday = 0x1C;     // a text notification too (docs/primitives-mac.md §4)
} // namespace dialogtype

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
    bool continueOthers = false;
    int icon = 0;      // STR# 0xF4: 0 automatic, 1 none, 2 neighbor, 3 indexed, 4 named
    int timeout = 0;   // operand byte 10: a text notification's lifetime in seconds (0: until closed)
    int priority = 0;  // operand byte 9: a text notification's priority
    // A blocking dialog's time limit (ms; 0: none): it then takes its last
    // button (autoTimeOutDialogs).
    int timeoutMs = 0;
    double opened = -1; // host clock (seconds) when autoTimeOutDialogs first saw it
    int answer = -1;   // set by the host
    // Text entry (3) and Baby Name (23): what the player typed, set by the
    // host with the answer (empty: the default stays).
    std::string text;
    bool female = false; // Baby Name: the newborn is a girl (the name box shows her icon)
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
