#pragma once

// What the trees show the player and change on objects that only the host
// can present: dialogs (Dialog, 0x24) and lights (Change Light, 0x6F). The
// structs are plain data, held by VM and Object (vm.hpp); the primitives
// are in dialog.cpp and light.cpp.

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace os2 {
struct Behavior;
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
constexpr int TutorialNext = 22;      // waits only with temp 1 != 0
constexpr int TutorialNextModal = 25; // ditto
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
    int timeout = 0;   // operand byte 10, as is (the text notifications' time on screen)
    double opened = -1; // host clock (seconds) when autoTimeOutDialogs first saw it
    int answer = -1;   // set by the host
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

// The binary's "autoTimeoutDialogs" (a debug property of the game's
// config, off in the shipping game): a dialog the player has not answered
// closes by itself some time after it opened. Every tree dialog still
// waiting `vm.dialogTimeoutSeconds` (0: never, as the game) after the host
// clock `now` first saw it gets its first button (inferred: the default
// button). The game's blocking dialogs do not read operand byte 10
// (docs/primitives-mac.md, Dialog): only the text notifications hand it
// to the notification window. Returns how many were answered.
int autoTimeOutDialogs(VM& vm, double now);

// Expands the `$` tokens of a dialog string (`$Me`, `$Object`, `$Local:n`,
// `$NameLocal:n`, `$TimeLocal:n`, ...; see dialog.cpp) for the instruction
// running in `c`.
std::string expandDialogTokens(Context& c, const std::string& text, int depth = 0);

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
