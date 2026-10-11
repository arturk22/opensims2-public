// Dialog (0x24): the dialogs the trees put up, their `$` tokens and the
// answer going back to the tree (dialog.hpp).
//
// Operands (docs/primitives-mac.md §4 and §5, from the Mac binary; the
// button bytes from the editor's disassembly and FreeSO's VMDialogOperand,
// which has the same first bytes in The Sims 1):
//   byte 0 button 3 ("cancel") string, byte 1 icon index, byte 3 button 1
//   ("yes"), byte 4 button 2 ("no"), byte 5 type (STR# 0xD9), byte 6
//   title, byte 7: bit 0 "return" and bit 7 "continue" (STR# 0xED dialog
//   behavior; inferred from which dialogs set them: the text
//   notifications have both), bits 1-3 icon type (STR# 0xF4), bits 4-6
//   the temp the choice or the notification ID goes to; byte 8 flags:
//   0x01 private strings, 0x40 global, else semi-global, 0x02 message
//   index in temp 0, 0x10 title index in temp 3; byte 10 timeout; u16 13
//   message (node version 0: byte 2, as in The Sims 1). Strings come from
//   STR# 0x12D of that scope, 1-based (0 = none).
//
// Execution (binary): the first run opens the dialog and waits (Yield)
// while it is open; button 1 -> true, button 2 -> false; with three
// choices the button's index (0/1/2) goes to the temp of byte 7. Types
// 8-10 and 0x1C (text notifications) do not wait and need a stack object
// (error 0x17); 22/25 (tutorial "next") wait only with temp 1 != 0.

#include "dialog.hpp"

#include "vm.hpp"
#include "hood_bridge.hpp"

#include "formats/binreader.hpp"
#include "formats/dbpf.hpp"
#include "formats/resindex.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <map>
#include <memory>

namespace os2::vm {

namespace {

constexpr uint16_t kDialogStrings = 0x12D;
constexpr uint16_t kJobStrings = 0x93; // a job object's titles and descriptions
constexpr uint32_t kTypeCtss = 0x43545353;
constexpr uint32_t kEditorLabelGroup = 0x7FE59FD0;
constexpr uint16_t kDialogTypeLabels = 0xD9;
// OBJD "catalog strings id" (FreeSO's OBJD field list, as for the fields
// vm.hpp already uses: 0x44 + 2 * 39).
constexpr size_t kObjdCatalogStrings = 0x92;

std::string lower(std::string s) {
    for (char& ch : s)
        ch = char(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

// CTSS of an object, read through the resource index (the code library
// does not keep them), cached per index.
const StringTable* catalogStrings(VM& vm, const Object& o) {
    static std::map<std::pair<const ResourceIndex*, uint64_t>, std::unique_ptr<StringTable>> cache;
    ResourceIndex* index = vm.code().index();
    if (!index || !o.definition)
        return nullptr;
    const uint16_t instance = o.definition->field(kObjdCatalogStrings);
    if (!instance)
        return nullptr;
    const auto key = std::make_pair(static_cast<const ResourceIndex*>(index), (uint64_t(o.group) << 16) | instance);
    auto it = cache.find(key);
    if (it == cache.end()) {
        std::unique_ptr<StringTable> t;
        try {
            if (auto data = index->read(Tgi{kTypeCtss, o.group, instance, 0}))
                t = std::make_unique<StringTable>(StringTable::parse(*data));
        } catch (const std::exception&) {
        }
        it = cache.emplace(key, std::move(t)).first;
    }
    return it->second.get();
}

const Neighbor* neighborOf(VM& vm, const Object& o) {
    return o.person ? vm.neighbor(o.person->data[persondata::NeighborId]) : nullptr;
}

// "N001_User00000 - Mortimer" -> "Mortimer".
std::string nameFromFileName(const std::string& fileName) {
    const auto dash = fileName.find(" - ");
    return dash == std::string::npos ? fileName : fileName.substr(dash + 3);
}

std::string fullName(VM& vm, const Object& o) {
    if (const Neighbor* n = neighborOf(vm, o); n && !n->firstName.empty())
        return n->lastName.empty() ? n->firstName : n->firstName + " " + n->lastName;
    return displayName(vm, o);
}

std::string neighborName(VM& vm, int16_t id) {
    if (const Object* sim = vm.neighborSim(id))
        return displayName(vm, *sim);
    if (const Neighbor* n = vm.neighbor(id)) {
        if (!n->firstName.empty())
            return n->firstName;
        if (auto def = vm.code().definitionByGuid(n->guid))
            if (const ObjectDefinition* d = vm.code().definition(def->first, def->second))
                return nameFromFileName(d->fileName);
    }
    return {};
}

// "17:00" (24-hour clock, as Brazilian Portuguese writes it; inferred).
std::string clockTime(int hour, int minute) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%d:%02d", ((hour % 24) + 24) % 24, std::clamp(minute, 0, 59));
    return buf;
}

// The work days of a job (the "days" bits of the career tables): bit 0
// Monday ... bit 6 Sunday (inferred: the jobs of the game have 0x1F,
// Monday to Friday, or bits past 4 for the weekend ones).
std::string dayFlags(int bits) {
    static const char* names[] = {"Seg", "Ter", "Qua", "Qui", "Sex", "Sáb", "Dom"};
    std::string out;
    for (int d = 0; d < 7; ++d)
        if (bits & (1 << d)) {
            if (!out.empty())
                out += ", ";
            out += names[d];
        }
    return out;
}

const StringTable* scopeStrings(VM& vm, const Object& owner, uint32_t frameGroup, int scope, uint16_t table) {
    CodeLibrary& code = vm.code();
    switch (scope) {
    case 0: // private: the group the tree is in (a semi-global tree's own
        // strings: "CT - Offer Job" of the job semi-global, run on a job
        // object, finds its offer in the semi-global's STR# 0x12D, not in
        // the job object's, which holds its chance cards), else the code owner's
        if (const StringTable* t = code.strings(frameGroup, table))
            return t;
        return code.strings(owner.group, table);
    case 1:
        if (owner.semiGroup)
            if (const StringTable* t = code.strings(owner.semiGroup, table))
                return t;
        return code.strings(frameGroup, table);
    default: return code.strings(kGlobalGroup, table);
    }
}

std::string stringAt(const StringTable* t, int oneBased) {
    if (!t || oneBased <= 0)
        return {};
    return t->get(size_t(oneBased - 1));
}

} // namespace

int autoTimeOutDialogs(VM& vm, double now) {
    if (!vm.dialogTimeouts)
        return 0;
    int answered = 0;
    for (auto& [who, d] : vm.dialogs) {
        if (!d.blocking || d.answer >= 0 || d.timeoutMs <= 0 || d.buttons.empty())
            continue;
        if (d.opened < 0)
            d.opened = now;
        else if ((now - d.opened) * 1000 >= d.timeoutMs) {
            d.answer = int(d.buttons.size()) - 1;
            ++answered;
        }
    }
    for (auto it = vm.notifications.begin(); it != vm.notifications.end();) {
        if (it->opened < 0)
            it->opened = now;
        if (it->timeout > 0 && now - it->opened >= it->timeout)
            it = vm.notifications.erase(it);
        else
            ++it;
    }
    return answered;
}

bool dialogPausesGame(const VM& vm) {
    for (const auto& [who, d] : vm.dialogs)
        if (d.blocking && d.answer < 0 && !d.continueOthers)
            return true;
    return false;
}

std::string displayName(VM& vm, const Object& o) {
    if (o.person) {
        if (const Neighbor* n = neighborOf(vm, o); n && !n->firstName.empty())
            return n->firstName;
        return o.definition ? nameFromFileName(o.definition->fileName) : std::string();
    }
    if (const StringTable* t = catalogStrings(vm, o)) {
        std::string s = t->get(0);
        if (!s.empty())
            return s;
    }
    return o.definition ? o.definition->fileName : std::string();
}

// The tokens, as Sims2.exe names them ("$Me", "$Object", "$Local", ...; the
// list is in the exe next to the dialog code). Their meaning is inferred
// from the strings that use them (FreeSO's VMDialogHandler has The Sims 1's
// versions): numbers after ':' are local variables of the tree that puts
// the dialog up. Tokens not understood stay as they are.
std::string expandDialogTokens(Context& c, const std::string& text, int depth) {
    VM& vm = c.vm;
    std::string out;
    out.reserve(text.size());
    auto local = [&](int i) -> int16_t {
        return i >= 0 && size_t(i) < c.frame.locals.size() ? c.frame.locals[size_t(i)] : int16_t(0);
    };
    auto objectName = [&](int16_t id) {
        const Object* o = vm.object(id);
        return o ? displayName(vm, *o) : std::string();
    };
    for (size_t i = 0; i < text.size();) {
        if (text[i] != '$') {
            out += text[i++];
            continue;
        }
        if (i + 1 < text.size() && text[i + 1] == '$') { // "$$Local:2": a dollar sign, then the token
            out += '$';
            ++i;
            continue;
        }
        size_t j = i + 1;
        while (j < text.size() && std::isalpha(static_cast<unsigned char>(text[j])))
            ++j;
        const std::string name = lower(text.substr(i + 1, j - i - 1));
        std::vector<int> args;
        size_t k = j;
        while (k + 1 < text.size() && text[k] == ':' && std::isdigit(static_cast<unsigned char>(text[k + 1]))) {
            size_t e = k + 1;
            int v = 0;
            while (e < text.size() && std::isdigit(static_cast<unsigned char>(text[e])))
                v = v * 10 + (text[e++] - '0');
            args.push_back(v);
            k = e;
        }
        const std::string raw = text.substr(i, k - i);
        auto arg = [&](size_t n) { return n < args.size() ? args[n] : 0; };
        std::string value;
        bool known = true;
        const Object* me = vm.object(c.thread.me);
        const Object* stack = vm.object(c.frame.stackObject);
        if (name == "me")
            value = me ? displayName(vm, *me) : "";
        else if (name == "fullname")
            value = me ? fullName(vm, *me) : "";
        else if (name == "lastname") {
            const Neighbor* n = me ? neighborOf(vm, *me) : nullptr;
            value = n ? n->lastName : "";
        } else if (name == "object")
            value = stack ? displayName(vm, *stack) : "";
        else if (name == "neighbor") // the neighbor whose ID is in the stack object
            value = neighborName(vm, c.frame.stackObject);
        else if (name == "local" && !args.empty())
            value = std::to_string(local(arg(0)));
        else if (name == "attrib" && !args.empty() && stack) // the stack object's attribute (as FreeSO)
            value = std::to_string(size_t(arg(0)) < stack->attributes.size() ? stack->attributes[size_t(arg(0))] : 0);
        else if (name == "namelocal" && !args.empty())
            value = objectName(local(arg(0)));
        else if (name == "fullnamelocal" && !args.empty()) {
            const Object* o = vm.object(local(arg(0)));
            value = o ? fullName(vm, *o) : "";
        } else if (name == "neighborlocal" && !args.empty())
            value = neighborName(vm, local(arg(0)));
        else if (name == "cataloglocal" && !args.empty())
            value = objectName(local(arg(0)));
        else if (name == "timelocal" && !args.empty())
            value = clockTime(local(arg(0)), args.size() > 1 ? local(arg(1)) : 0);
        else if (name == "dayflags" && !args.empty())
            value = dayFlags(local(arg(0)));
        else if (name == "cash" || name == "familyassets") {
            const int16_t family = me && me->person ? me->person->data[persondata::FamilyNumber] : 0;
            auto f = vm.families.find(family);
            value = std::to_string(f == vm.families.end() ? 0 : f->second.funds);
        } else if (name == "jobtitle" && args.size() >= 2) {
            // $JobTitle:<title index local>:<job object local>: the entry
            // (0-based) of the job object's STR# 0x93 of titles and
            // descriptions, as "Function - Get Job Title Index for Level"
            // computes it (title, description per level and gender; inferred).
            const Object* job = vm.object(local(arg(1)));
            const StringTable* t = job ? vm.code().strings(job->group, kJobStrings) : nullptr;
            value = t ? t->get(size_t(std::max<int>(local(arg(0)), 0))) : std::string();
            known = !value.empty();
        } else if ((name == "privatestring" || name == "semiglobalstring" || name == "globalstring") && args.size() >= 2 &&
                   depth < 4) {
            // <table local>:<index local> (inferred), from the code owner's scope.
            const Object* owner = vm.object(c.frame.codeOwner);
            const int scope = name == "privatestring" ? 0 : name == "semiglobalstring" ? 1 : 2;
            const StringTable* t =
                owner ? scopeStrings(vm, *owner, c.frame.group, scope, uint16_t(local(arg(0)))) : nullptr;
            value = expandDialogTokens(c, stringAt(t, local(arg(1)) + 1), depth + 1);
            known = t != nullptr;
        } else
            known = false;
        out += known ? value : raw;
        i = k;
    }
    return out;
}

namespace {

std::string typeLabel(VM& vm, int type) {
    if (const StringTable* t = vm.code().strings(kEditorLabelGroup, kDialogTypeLabels))
        return t->get(size_t(type), lang::EnglishUS);
    return {};
}

Exit dialog(Context& c) {
    VM& vm = c.vm;
    const auto& o = c.instruction.operands;
    Object& me = c.me();
    const size_t depth = c.thread.stack.size();
    const int type = int8_t(o[5]);
    const int resultTemp = (o[7] >> 4) & 7;

    // An answer to the dialog this instruction put up.
    if (auto it = vm.dialogs.find(me.id); it != vm.dialogs.end()) {
        Dialog& d = it->second;
        if (d.depth == depth && d.tree == c.frame.behavior && d.node == c.frame.pc) {
            if (d.answer < 0 && vm.autoDialogAnswer >= 0)
                d.answer = std::min<int>(vm.autoDialogAnswer, int(d.buttons.size()) - 1);
            if (d.answer < 0)
                return Exit::Yield;
            const int answer = std::max(d.answer, 0);
            vm.dialogs.erase(it);
            if (type == dialogtype::YesNoCancel)
                c.thread.temps[size_t(resultTemp)] = int16_t(answer);
            return answer == 0 ? Exit::True : Exit::False;
        }
        vm.dialogs.erase(it); // left behind by a tree that was reset
    }

    const uint8_t flags = o[8];
    const int scope = flags & 0x01 ? 0 : flags & 0x40 ? 2 : 1;
    const Object* owner = vm.object(c.frame.codeOwner);
    if (!owner)
        owner = &me;
    const StringTable* strings = scopeStrings(vm, *owner, c.frame.group, scope, kDialogStrings);
    const int messageIndex = flags & 0x02 ? c.thread.temps[0] : c.instruction.nodeVersion == 0 ? o[2] : le16(&o[13]);
    const int titleIndex = flags & 0x10 ? c.thread.temps[3] : o[6];

    Dialog d;
    d.id = vm.nextDialogId++;
    d.me = me.id;
    d.stackObject = c.frame.stackObject;
    d.type = type;
    d.typeName = typeLabel(vm, type);
    d.message = expandDialogTokens(c, stringAt(strings, messageIndex));
    d.title = expandDialogTokens(c, stringAt(strings, titleIndex));
    d.icon = (o[7] >> 1) & 7;
    d.timeout = o[10];
    d.priority = o[9];
    d.continueOthers = o[7] & 0x80;
    d.depth = depth, d.tree = c.frame.behavior, d.node = c.frame.pc;
    auto button = [&](int index, const char* fallback) {
        const std::string s = expandDialogTokens(c, stringAt(strings, index));
        return s.empty() ? std::string(fallback) : s;
    };

    switch (type) {
    case dialogtype::TextNotification:
    case dialogtype::StopTextNotification:
    case dialogtype::ModifyTextNotification:
    case dialogtype::TnsBirthday:
        if (!vm.object(c.frame.stackObject))
            throw VMError("Could not find object reference (text notification without a stack object)");
        d.blocking = false;
        if (type == dialogtype::TextNotification || type == dialogtype::TnsBirthday)
            c.thread.temps[size_t(resultTemp)] = int16_t(d.id & 0x7FFF);
        if (vm.notifications.size() >= kMaxNotifications)
            vm.notifications.pop_front();
        vm.notifications.push_back(std::move(d));
        return Exit::True;
    case dialogtype::TutorialNext:
    case dialogtype::TutorialNextModal:
        if (!c.thread.temps[1]) {
            d.blocking = false;
            if (vm.notifications.size() >= kMaxNotifications)
                vm.notifications.pop_front();
            vm.notifications.push_back(std::move(d));
            return Exit::True;
        }
        d.buttons = {button(o[3], "OK")};
        break;
    case dialogtype::YesNo: d.buttons = {button(o[3], "Sim"), button(o[4], "Não")}; break;
    case dialogtype::YesNoCancel:
        d.buttons = {button(o[3], "Sim"), button(o[4], "Não"), button(o[0], "Cancelar")};
        break;
    case dialogtype::Message:
    case dialogtype::TextEntry:
    case dialogtype::Tutorial: d.buttons = {button(o[3], "OK")}; break;
    default: // the special dialogs (phone book, racks, clothing, ...): OK or cancel
        d.buttons = {button(o[3], "OK"), button(o[0], "Cancelar")};
        break;
    }
    // cTSObjectDialog's time limit (docs/primitives-mac.md, Dialog): not
    // for the tutorial nor the special dialogs (their own classes).
    if (type >= dialogtype::Message && type <= dialogtype::TextEntry && type != dialogtype::Tutorial) {
        size_t chars = 0; // the message's length in characters (UTF-8)
        for (unsigned char ch : d.message)
            chars += (ch & 0xC0) != 0x80;
        d.timeoutMs = 4000 + 40 * int(chars);
    }
    if (vm.autoDialogAnswer >= 0)
        d.answer = std::min<int>(vm.autoDialogAnswer, int(d.buttons.size()) - 1);
    vm.dialogs[me.id] = std::move(d);
    return Exit::Yield;
}

} // namespace

void registerDialogPrimitives(VM& vm) { vm.setPrimitive(0x24, dialog); }

} // namespace os2::vm
