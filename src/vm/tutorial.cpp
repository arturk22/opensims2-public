// The tutorial's primitives and controllers: tutorial.hpp, docs/tutorial-mac.md.

#include "tutorial.hpp"

#include "engine.hpp"
#include "errors.hpp"
#include "placement.hpp"
#include "vm.hpp"

#include "formats/binreader.hpp"
#include "formats/strings.hpp"

#include <cstdlib>
#include <exception>

namespace os2::vm {

namespace {

constexpr uint16_t kButtonIds = 0x96;     // STR# "Button GUIDs"
constexpr uint16_t kDialogStrings = 0x12D;
constexpr size_t kMaxCommands = 512;

// The STR# of a scope: 0 the tree's own group (else the code owner's), 1
// its semi-global, 2 the global one.
const StringTable* tableOf(Context& c, int scope, uint16_t table) {
    CodeLibrary& code = c.vm.code();
    const Object* owner = c.vm.object(c.frame.codeOwner);
    switch (scope) {
    case 0:
        if (const StringTable* t = code.strings(c.frame.group, table))
            return t;
        return owner ? code.strings(owner->group, table) : nullptr;
    case 1: {
        if (owner && owner->semiGroup)
            if (const StringTable* t = code.strings(owner->semiGroup, table))
                return t;
        if (const uint32_t semi = code.semiGlobalFor(c.frame.group))
            if (const StringTable* t = code.strings(semi, table))
                return t;
        return code.strings(c.frame.group, table);
    }
    default: return code.strings(kGlobalGroup, table);
    }
}

// Entry `index` (0-based) as a number ("0x480e8304"); the strings of these
// tables are the same in every language, so English first.
uint32_t idAt(const StringTable* t, int index) {
    if (!t || index < 0)
        return 0;
    std::string s = t->get(size_t(index), lang::EnglishUS);
    if (s.empty())
        s = t->get(size_t(index));
    return uint32_t(std::strtoul(s.c_str(), nullptr, 0));
}

// UI Effect (cTSObject::TryUIEffect, BV 0xe2b310). Operands: mode s8 0,
// window index u16 1, control index u16 3, flags byte 5 (0x02 the window's
// table is the semi-global one, 0x01 the global one, else the tree's own;
// 0x08 / 0x04 the same for the control's; docs/tutorial-mac.md §2), effect
// u16 6, event tree u16 8.
// Mode 8 (an event tree on a text notification) takes the notification from
// the variable (owner 13, data u16 11). Always true (the binary's errors for
// a window that is not up are left to the host).
Exit uiEffect(Context& c) {
    const auto& o = c.instruction.operands;
    // The engine primitives' recorder still sees it (engine.hpp uiEvents).
    {
        auto& events = uiEvents();
        if (events.size() >= 256)
            events.pop_front();
        events.push_back({uint8_t(c.instruction.opcode), c.thread.me, c.frame.stackObject, c.instruction.operands});
    }
    const int mode = int8_t(o[0]);
    const uint8_t flags = o[5];
    TutorialUiCommand cmd;
    cmd.mode = mode;
    cmd.effect = le16(&o[6]);
    cmd.tree = le16(&o[8]);
    cmd.object = c.thread.me;
    cmd.tick = c.vm.ticks;
    if (mode != uieffect::TnsEventTree) {
        const int windowScope = flags & 0x01 ? 2 : flags & 0x02 ? 1 : 0;
        const int controlScope = flags & 0x04 ? 2 : flags & 0x08 ? 1 : 0;
        cmd.window = idAt(tableOf(c, windowScope, kButtonIds), le16(&o[1]));
        cmd.control = idAt(tableOf(c, controlScope, kButtonIds), le16(&o[3]));
    }
    auto& q = c.vm.tutorial.commands;
    if (q.size() >= kMaxCommands)
        q.pop_front();
    q.push_back(cmd);
    return Exit::True;
}

std::string text(const StringTable* t, int oneBased) {
    if (!t || oneBased <= 0)
        return {};
    return t->get(size_t(oneBased - 1));
}

} // namespace

int tutorialNextDialog(Context& c, int type, uint32_t stringsGroup) {
    VM& vm = c.vm;
    const auto& o = c.instruction.operands;
    const auto& temps = c.thread.temps;
    TutorialProgress& p = vm.tutorial.progress;
    // TryDialog (BV 0xe2bb64): temp 1 = 0 hides the progress window
    // (cTSTutorialManager::HideProgressDisplay).
    if (temps[1] == 0) {
        p.visible = false;
        ++p.serial;
        return int(Exit::True);
    }
    const StringTable* t = vm.code().strings(stringsGroup, kDialogStrings);
    const uint8_t flags = o[8];
    // Title: byte 6 (temp 3 with flag 0x10); message: u16 13 (temp 0 with
    // 0x02, as is); picture: the string at byte 0 | byte 2 << 8 (temp 4 with 0x20), an
    // image ID ("2d8fac55"; English only: the other languages leave it empty).
    const int title = flags & 0x10 ? temps[3] : o[6];
    const int message = flags & 0x02 ? temps[0] : le16(&o[13]);
    const int picture = flags & 0x20 ? temps[4] : (o[0] | o[2] << 8);
    p.title = expandDialogTokens(c, text(t, title));
    p.message = expandDialogTokens(c, text(t, message));
    p.image = 0;
    if (t && picture > 0) {
        std::string s = t->get(size_t(picture - 1), lang::EnglishUS);
        if (s.empty())
            s = t->get(size_t(picture - 1));
        p.image = uint32_t(std::strtoul(s.c_str(), nullptr, 16));
    }
    p.step = temps[1];
    p.total = temps[2];
    p.modal = type == 25;
    p.owner = c.thread.me;
    p.visible = true;
    ++p.serial;
    TutorialUiCommand shown;
    shown.mode = uieffect::ProgressShown;
    shown.window = kProgressWindow;
    shown.object = c.thread.me;
    shown.tick = vm.ticks;
    vm.tutorial.commands.push_back(shown);
    return int(Exit::True);
}

void registerTutorialPrimitives(VM& vm) {
    vm.setPrimitive(0x22, uiEffect);
}

int ensureTutorialControllers(VM& vm) {
    int created = 0;
    for (uint32_t guid : {kTutorialControllerGuid, kTutorialDeathCheckerGuid}) {
        bool present = false;
        for (const Object* o : vm.objectTable())
            if (o && o->definition && o->definition->guid() == guid) {
                present = true;
                break;
            }
        if (present || !vm.code().definitionByGuid(guid))
            continue;
        std::vector<ObjectId> before;
        for (const auto& [id, o] : vm.objects())
            before.push_back(id);
        try {
            Object* o = vm.instantiateGuid(guid);
            if (!o)
                continue;
            o->position = Position{};
            o->data[objdata::ContainerId] = 0;
            o->data[objdata::SlotNumber] = -1;
            vm.initialize(o->id, before);
            ++created;
        } catch (const std::exception&) {
            // A controller that does not start leaves the lot as it was.
        }
    }
    return created;
}

bool runTutorialEventTree(VM& vm, ObjectId object, uint16_t tree) {
    if (!vm.object(object) || !tree)
        return false;
    std::array<int16_t, kTempCount> temps{};
    try {
        runOneTickTree(vm, temps, object, object, 0, tree, {});
    } catch (const std::exception&) {
        return false;
    }
    return true;
}

void TutorialUi::reset() {
    callbacks_.clear();
    enabled_.clear();
    hidden_.clear();
    highlight_ = {0, 0};
    presses_.clear();
}

void TutorialUi::update(VM& vm) {
    auto& q = vm.tutorial.commands;
    while (!q.empty()) {
        const TutorialUiCommand c = q.front();
        q.pop_front();
        const Key k{c.window, c.control};
        switch (c.mode) {
        case uieffect::ProgressShown:
            // A new window: what was set on the old one's controls is gone.
            for (auto it = callbacks_.begin(); it != callbacks_.end();)
                it = it->first.first == kProgressWindow ? callbacks_.erase(it) : std::next(it);
            for (auto it = enabled_.begin(); it != enabled_.end();)
                it = it->first.first == kProgressWindow ? enabled_.erase(it) : std::next(it);
            for (auto it = hidden_.begin(); it != hidden_.end();)
                it = it->first == kProgressWindow ? hidden_.erase(it) : std::next(it);
            if (highlight_.first == kProgressWindow)
                highlight_ = {0, 0};
            break;
        case uieffect::Press: presses_.push_back(k); break;
        case uieffect::Disable: enabled_[k] = false; break;
        case uieffect::Enable: enabled_[k] = true; break;
        case uieffect::Highlight: highlight_ = c.control ? k : Key{0, 0}; break;
        case uieffect::EventTree:
            if (c.control)
                callbacks_[k] = {c.object, c.tree};
            break;
        case uieffect::Restore: enabled_.erase(k); break;
        case uieffect::ResetTrees: callbacks_.clear(); break;
        case uieffect::Show: hidden_.erase(k); break;
        case uieffect::Hide:
            if (c.control)
                hidden_.insert(k);
            break;
        default: break;
        }
    }
}

bool TutorialUi::enabled(uint32_t window, uint32_t control) const {
    auto it = enabled_.find({window, control});
    return it == enabled_.end() || it->second;
}

bool TutorialUi::visible(uint32_t window, uint32_t control) const {
    return !hidden_.count({window, control});
}

bool TutorialUi::hasEventTree(uint32_t window, uint32_t control) const {
    return callbacks_.count({window, control}) != 0;
}

bool TutorialUi::click(VM& vm, uint32_t window, uint32_t control) {
    update(vm);
    if (!enabled(window, control) || !visible(window, control))
        return false;
    TutorialProgress& p = vm.tutorial.progress;
    if (window == kProgressWindow && control == kProgressNext && p.modal && p.visible) {
        p.visible = false; // the modal window ends with its button
        ++p.serial;
    }
    if (auto it = callbacks_.find({window, control}); it != callbacks_.end()) {
        const Callback cb = it->second;
        runTutorialEventTree(vm, cb.object, cb.tree);
        update(vm);
    }
    return true;
}

} // namespace os2::vm
