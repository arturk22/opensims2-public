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
// (error 0x17); 22/25 ("TutorialNext") are the tutorial's progress window
// (tutorial.hpp): they never wait.

#include "dialog.hpp"

#include "vm.hpp"
#include "hood_bridge.hpp"
#include "tutorial.hpp"

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
// The last object a name token of a dialog string named ($Object,
// $NameLocal, ...): the automatic icon's object (resolveIcon).
thread_local int16_t lastTokenObject = 0;
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

DialogClassInfo dialogClass(int type) {
    // cTSUIDialogManager::GetObjectDialog (BV 0x21c5a8) switches on the type
    // to a class ID; the class of each ID is the one whose code follows its
    // GZCOM_<class>CLSID getter (they sit in front of the class's
    // QueryInterface). The scripts: cTSWinProcPhoneBookServices/Party
    // constructors (0x8C159243/0x8C159242), Neighbors' (0x8C159244),
    // cTSWinProcClothingDialog::Initialize (0x8C159250), the clothing
    // rack's Initialize (0x8C159251, BV 0x40489c), the try-on's (0x8C159255),
    // the vanity's (0x8C159280; 0x8C159286 for plastic surgery), the
    // jewelry rack's (0xACDC3000, not in the base game's ui.package),
    // cTSWinProcNameObjectDialog (0x2D91050A, panels 0xCDC2CAA4-B0).
    using C = DialogClass;
    switch (type) {
    case 5:
    case 15: return {C::PhoneServices, "cTSWinProcPhoneBookServices", 0x8C159243};
    case 6: return {C::PhoneNeighbors, "cTSWinProcPhoneBookNeighbors", 0x8C159244};
    case 7:
    case 0x46: return {C::PhoneParty, "cTSWinProcPhoneBookParty", 0x8C159242};
    case 11:
    case 13:
    case 0x2B:
    case 0x2D: return {C::SelectItems, "cTSWinProcSelectItemsDialog", 0};
    case 12:
    case 0x2C: return {C::JewelryPurchase, "cTSWinProcJewelryPurchaseDialog", 0xACDC3000};
    case 14:
    case 0x1B:
    case 0x1F:
    case 0x2A: return {C::Fill, "cTSWinProcFillDialog", 0};
    case 16:
    case 0x2E: return {C::ClothingPurchase, "cTSWinProcClothingPurchaseDialog", 0x8C159251};
    case 17:
    case 0x31: return {C::Clothing, "cTSWinProcClothingDialog", 0x8C159250};
    case 20: return {C::ClothingTryOn, "cTSWinProcClothingTryOnDialog", 0x8C159255};
    case 21: return {C::Vanity, "cTSWinProcVanityDialog", 0x8C159280};
    case 23: return {C::NameObject, "cTSWinProcNameObjectDialog", 0x2D91050A};
    case 24:
    case 0x22: return {C::SetAspiration, "cTSWinProcSetAspirationDialog", 0};
    case 0x1A:
    case 0x1D:
    case 0x1E:
    case 0x20:
    case 0x21:
    case 0x23:
    case 0x24:
    case 0x25:
    case 0x26:
    case 0x27:
    case 0x28:
    case 0x29:
    case 0x2F:
    case 0x30:
    case 0x32:
    case 0x33:
    case 0x34:
    case 0x35:
    case 0x36:
    case 0x37:
    case 0x38:
    case 0x3A:
    case 0x3B:
    case 0x3C:
    case 0x3D:
    case 0x3E:
    case 0x3F:
    case 0x40:
    case 0x41:
    case 0x42:
    case 0x43:
    case 0x44:
    case 0x45:
    case 0x47:
    case 0x48:
    case 0x49:
    case 0x4A:
    case 0x4C: return {C::Expansion, "(expansion pack class)", 0};
    default: return {};
    }
}

std::function<const StringTable*(uint32_t, uint32_t)>& textTables() {
    static std::function<const StringTable*(uint32_t, uint32_t)> tables;
    return tables;
}

int autoTimeOutDialogs(VM& vm, double now) {
    if (!vm.dialogTimeouts)
        return 0;
    int answered = 0;
    // The dialogs nobody waits for ("return" ones) go once answered.
    std::erase_if(vm.dialogs, [](const auto& kv) { return kv.second.detached && kv.second.answer >= 0; });
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
    // The tutorial's modal progress window (type 25) pauses the game until
    // its button is pressed (cTSWinProcTutorialProgressDialog::Show).
    if (vm.tutorial.progress.visible && vm.tutorial.progress.modal)
        return true;
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
        } else if (name == "object") {
            value = stack ? displayName(vm, *stack) : "";
            if (stack)
                lastTokenObject = stack->id;
        }
        else if (name == "neighbor") // the neighbor whose ID is in the stack object
            value = neighborName(vm, c.frame.stackObject);
        else if (name == "local" && !args.empty())
            value = std::to_string(local(arg(0)));
        else if (name == "attrib" && !args.empty() && stack) // the stack object's attribute (as FreeSO)
            value = std::to_string(size_t(arg(0)) < stack->attributes.size() ? stack->attributes[size_t(arg(0))] : 0);
        else if (name == "namelocal" && !args.empty()) {
            value = objectName(local(arg(0)));
            if (vm.object(local(arg(0))))
                lastTokenObject = local(arg(0));
        }
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
        } else if (name == "aspirationstring" && args.size() >= 2) {
            // <table local>:<index local>: Live.package's aspiration level
            // names, as global "Aspiration - Get Level" leaves them in temps
            // 1 and 2 (the age's table 0x9A-0x9D and 1 + 13 x aspiration + 2
            // x level + 1 for a female: one-based like the Dialog's own,
            // "Don Juan" 11, "Femme Fatale" 12; read from the data).
            const auto& tables = textTables();
            const StringTable* t = tables ? tables(kLiveTextGroup, uint32_t(uint16_t(local(arg(0))))) : nullptr;
            value = stringAt(t, local(arg(1)));
            known = t != nullptr;
        } else if ((name == "privatestring" || name == "semiglobalstring" || name == "globalstring") && args.size() >= 2 &&
                   depth < 4) {
            // <table local>:<index local>, from the code owner's scope; the
            // index is one-based like the Dialog's own (Controller - Age's
            // "has grown into $PrivateString:4:5" with local 5 = 18 for
            // entry 18, "an adult"; inferred from the data).
            const Object* owner = vm.object(c.frame.codeOwner);
            const int scope = name == "privatestring" ? 0 : name == "semiglobalstring" ? 1 : 2;
            const StringTable* t =
                owner ? scopeStrings(vm, *owner, c.frame.group, scope, uint16_t(local(arg(0)))) : nullptr;
            value = expandDialogTokens(c, stringAt(t, local(arg(1))), depth + 1);
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

// UIText (Text/UIText.package, group 0x7FA7DC7C) entry, one-based as the
// binary asks for it; `fallback` without the table.
std::string uiText(uint32_t table, int oneBased, const char* fallback) {
    const auto& tables = textTables();
    const StringTable* t = tables ? tables(0x7FA7DC7C, table) : nullptr;
    std::string s = stringAt(t, oneBased);
    return s.empty() ? std::string(fallback) : s;
}

std::string trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos)
        return {};
    return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}

// The neighbor whose person data the dialogs read: the Sim on the lot's
// own, else the neighborhood's copy.
const std::array<int16_t, kPersonDataCount>* personDataOf(VM& vm, int16_t neighbor) {
    if (const Object* sim = vm.neighborSim(neighbor); sim && sim->person)
        return &sim->person->data;
    if (const Neighbor* n = vm.neighbor(neighbor))
        return &n->personData;
    return nullptr;
}

std::string neighborFirstName(VM& vm, int16_t id) { return neighborName(vm, id); }

// cTSWinProcNameObjectDialog::SetParams (BV 0x370a50): the neighbors in
// temps 0-3 (temp 0 needed), each box starting with the Sim's first name;
// person data 65 = 1 (female) picks the girl icon, and one Sim older than a
// baby (person data 58 "Person Age" > 1) is a rename; onAttach (0x371cfc)
// then shows panel 0xCDC2CAA4 (rename), else 0xCDC2CAA5 (one baby) or
// 0xCDC2CAA6 (twins: temp 1 set); a servo gets 0xCDC2CAA7 (pets, the
// expansion's panels 0xCDC2CAA8-B0, are not in the base game).
void setupNames(Context& c, Dialog& d) {
    VM& vm = c.vm;
    bool rename = true;
    for (size_t i = 0; i < 4; ++i) {
        const int16_t id = c.thread.temps[i];
        if (!id)
            continue;
        // The base game's panels have two boxes at most (0xCDC2CAA6, twins);
        // the birth tree leaves the second baby in temp 2 too, which showed
        // it twice in the list (round 18). One box per Sim (inferred: the
        // panel shows no third box for what SetParams reads there).
        if (std::any_of(d.names.begin(), d.names.end(), [&](const DialogName& x) { return x.neighbor == id; }))
            continue;
        const auto* data = personDataOf(vm, id);
        if (!data)
            continue;
        DialogName n;
        n.neighbor = id;
        n.text = neighborFirstName(vm, id);
        n.female = (*data)[65] == 1;
        if (i == 0)
            rename = (*data)[persondata::PersonAge] > 1;
        else
            rename = false;
        d.names.push_back(std::move(n));
    }
    if (d.names.empty()) // the newborn as the stack object (older trees and tests)
        if (const Object* baby = vm.object(c.frame.stackObject); baby && baby->person) {
            DialogName n;
            n.neighbor = baby->person->data[persondata::NeighborId];
            n.text = displayName(vm, *baby);
            n.female = baby->person->data[65] == 1;
            rename = baby->person->data[persondata::PersonAge] > 1;
            d.names.push_back(std::move(n));
        }
    d.namePanel = rename ? 0xCDC2CAA4 : d.names.size() > 1 ? 0xCDC2CAA6 : 0xCDC2CAA5;
    if (!d.names.empty())
        d.female = d.names[0].female, d.text = d.names[0].text;
}

// A Change Outfit literal (temp 0 of the clothing dialogs) as the bit of
// the outfit category (the same switch in cTSWinProcClothingDialog,
// cTSWinProcClothingPurchaseDialog and cTSWinProcClothingTryOnDialog's
// SetParams): 0 and the others everyday.
uint32_t outfitCategoryOfTemp(int16_t t) {
    switch (t) {
    case 1: return 0x080;
    case 2: return 0x008;
    case 3: return 0x200;
    case 4: return 0x020;
    case 5: return 0x010;
    case 6: return 0x400;
    case 8: return 0x100;
    case 9: return 0x002;
    case 10: return 0x004;
    case 11: return 0x040;
    case 13: return 0x1000;
    default: return 0x001;
    }
}

// The phone books (cTSWinProcPhoneBookDialog::SetParams, BV 0xa7af30, and
// the classes' PreShow): title byte 6, message u16 13, buttons bytes 3/4
// from STR# 0x12D; the rows are the stack object's array 0: objects for
// the services (AddAvailableServices: the catalog name), neighbors for the
// others (CreateNeighborCallList: name, family lot, daily + lifetime
// relationship of the caller to them). Services and invite start with the
// first row chosen (PreShow selects row 0); a party starts with no guests
// and takes at most temp 5 of them.
void setupPhone(Context& c, Dialog& d, Object* stack) {
    VM& vm = c.vm;
    if (!stack || stack->arrays.empty())
        return;
    const auto values = stack->array(0).values;
    for (int16_t v : values) {
        DialogEntry e;
        e.id = v;
        if (d.cls.cls == DialogClass::PhoneServices) {
            const Object* o = vm.object(v);
            if (!o)
                continue;
            e.guid = o->guid();
            e.name = displayName(vm, *o);
        } else {
            const Neighbor* n = vm.neighbor(v);
            if (!n)
                continue;
            e.guid = n->guid;
            e.first = neighborName(vm, v);
            e.last = n->lastName;
            e.name = n->lastName.empty() ? e.first : e.first + " " + n->lastName;
            // CreateNeighborCallList: the name of the lot whose residents are
            // the neighbor's family (SetLotName).
            if (auto f = vm.families.find(n->personData[persondata::FamilyNumber]); f != vm.families.end() && f->second.house)
                if (auto l = vm.lots.find(f->second.house); l != vm.lots.end())
                    e.detail = l->second.name;
            if (e.detail.empty() && n->house)
                if (auto l = vm.lots.find(n->house); l != vm.lots.end())
                    e.detail = l->second.name;
            const auto key = std::make_pair(vm.relationshipKey(c.thread.me), VM::RelationshipKey{1, v});
            if (auto r = vm.relationships.find(key); r != vm.relationships.end()) {
                const auto& vars = r->second;
                e.relationship = (vars.size() > 0 ? vars[0] : 0) + (vars.size() > 2 ? vars[2] : 0);
                e.flags = vars.size() > 1 ? (vars[1] & 0xFFFF) : 0;
            }
        }
        d.entries.push_back(std::move(e));
    }
    // The neighbors' book opens in mode 5 (its constructor: slides sorted
    // by name, SortNeighborCallListByName: last names, then first names) and
    // PreShow's SetSelectedSim(0) picks the first of that order.
    if (d.cls.cls == DialogClass::PhoneNeighbors) {
        auto key = [](const std::string& t) {
            std::string k = t;
            for (char& ch : k)
                ch = char(std::tolower(static_cast<unsigned char>(ch)));
            return k;
        };
        std::stable_sort(d.entries.begin(), d.entries.end(), [&](const DialogEntry& a, const DialogEntry& b) {
            return std::make_pair(key(a.last), key(a.first)) < std::make_pair(key(b.last), key(b.first));
        });
    }
    if (d.cls.cls == DialogClass::PhoneParty)
        d.maxSelection = c.thread.temps[5];
    if (d.cls.cls == DialogClass::PhoneParty && !d.buttons.empty()) {
        d.caption = d.buttons[0];
        for (const char* f : {"%d", "%i", "%u"})
            if (auto p = d.caption.find(f); p != std::string::npos)
                d.caption.replace(p, 2, std::to_string(d.maxSelection));
        d.caption2 = d.buttons.size() > 1 ? d.buttons[1] : std::string();
    }
    else if (!d.entries.empty())
        d.selection = {d.entries[0].id};
}

// What the dialog's OK does before the tree goes on (the classes' OnOk and
// TryDialog's state 2).
void acceptDialog(Context& c, Dialog& d) {
    VM& vm = c.vm;
    switch (d.cls.cls) {
    case DialogClass::Object:
        if (d.type == dialogtype::TextEntry) // TryDialog: GetEnteredText to the simulator
            vm.enteredText = d.text;
        break;
    case DialogClass::NameObject:
        // ProceedOkButton: each box's trimmed text is the Sim's first name.
        for (const auto& n : d.names) {
            const std::string name = trim(n.text);
            if (name.empty())
                continue;
            if (Neighbor* nb = vm.neighbor(n.neighbor))
                nb->firstName = name;
        }
        break;
    case DialogClass::SelectItems:
        // OnOk: -1 over every item of array 0 that is not picked.
        if (Object* stack = vm.object(d.stackObject); stack && !stack->arrays.empty())
            for (int16_t& v : stack->array(0).values)
                if (std::find(d.selection.begin(), d.selection.end(), v) == d.selection.end())
                    v = -1;
        break;
    case DialogClass::Fill:
        // OnOk: temp 0 = the units bought.
        c.thread.temps[0] = int16_t(d.fillValue - d.fillUnits);
        break;
    case DialogClass::ClothingPurchase: {
        // SaveCartStandard -> cTSShoppingCart::CompletePurchase: each item
        // of the cart into the family's bin (cTSFamily::AddClothing; one
        // already there is not added again: CanBeAdded), then the total
        // (items x the price per item) off the family's funds (vtable 0xB0,
        // expense 6). The family is the Sim's (person data 61).
        const Object* sim = vm.object(d.outfitSim);
        const int16_t family = sim && sim->person ? sim->person->data[persondata::FamilyNumber] : 0;
        auto f = vm.families.find(family);
        if (f == vm.families.end() || d.cart.empty())
            break;
        for (const Tgi& k : d.cart)
            if (std::find(f->second.wardrobe.begin(), f->second.wardrobe.end(), k) == f->second.wardrobe.end())
                f->second.wardrobe.push_back(k);
        setFamilyFunds(vm, family, familyFunds(vm, family) - int32_t(d.cart.size()) * d.pricePerItem);
        break;
    }
    case DialogClass::SetAspiration:
        // The button sets the neighbor's person data 46 (vtable 0x44) and
        // OnOk the Sim's on the lot, if it is there.
        if (d.aspiration) {
            if (Neighbor* n = vm.neighbor(d.aspirationNeighbor))
                n->personData[46] = d.aspiration;
            if (Object* sim = vm.neighborSim(d.aspirationNeighbor); sim && sim->person)
                sim->person->data[46] = d.aspiration;
        }
        break;
    case DialogClass::PhoneServices:
    case DialogClass::PhoneNeighbors:
    case DialogClass::PhoneParty:
        // OnOk: array 0 of the stack object = the choice (services: the
        // object ID, invite: the neighbor ID, 0 for none; party: the guests).
        if (Object* stack = vm.object(d.stackObject); stack && !stack->arrays.empty()) {
            auto& a = stack->array(0).values;
            a.clear();
            if (d.cls.cls == DialogClass::PhoneParty)
                a = d.selection;
            else
                a.push_back(d.selection.empty() ? int16_t(0) : d.selection[0]);
        }
        break;
    default: break;
    }
}

// cTSObjectDialog::SetParams' icon (BV 0x1d2b20; byte 7 bits 1-3, STR#
// 0xF4): 0 automatic, the object a token of the text named (inferred: the
// token expansion hands one back) else the stack object; 2 the neighbor
// whose ID is in the stack object (error 0x37 in TryDialog's notifications
// if none); 3 "indexed", the tree's object (inferred: the binary asks the
// object definition of the tree's file); 4 "named", a string of STR# 0x12D
// at byte 1: "gz <instance>"/"gzi <instance>" a UI image, "guid <n>" an
// object; 1 none.
void resolveIcon(Context& c, Dialog& d, const StringTable* strings, int16_t tokenObject) {
    VM& vm = c.vm;
    const auto& o = c.instruction.operands;
    switch (d.icon) {
    case 0: d.iconObject = tokenObject ? tokenObject : c.frame.stackObject; break;
    case 2:
        if (vm.neighbor(c.frame.stackObject))
            d.iconNeighbor = c.frame.stackObject;
        break;
    case 3: d.iconObject = c.frame.codeOwner; break;
    case 4: {
        const std::string named = stringAt(strings, o[1]);
        const auto sp = named.find(' ');
        const std::string head = lower(named.substr(0, sp));
        const long n = sp == std::string::npos ? 0 : std::strtol(named.c_str() + sp + 1, nullptr, 0);
        if ((head == "gz" || head == "gzi") && n)
            d.iconImage = uint32_t(n);
        break;
    }
    default: break;
    }
    if (d.iconObject && !vm.object(d.iconObject))
        d.iconObject = 0;
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
            if (answer == 0)
                acceptDialog(c, d);
            vm.dialogs.erase(it);
            // TryDialog: state 3 (button 2, "no") writes 0 and state 4
            // (button 3, "cancel") 1 to the temp; "yes" leaves it (STR# 0xDA).
            if (type == dialogtype::YesNoCancel && answer > 0)
                c.thread.temps[size_t(resultTemp)] = int16_t(answer - 1);
            return answer == 0 ? Exit::True : Exit::False;
        }
        vm.dialogs.erase(it); // left behind by a tree that was reset
    }

    // TryDialog (BV 0xe2bb64): byte 8 bit 0x01 takes the STR# 0x12D from
    // the tree's own resource (vtable 0x118), 0x40 the global one, neither
    // from the tree's file (vtable 0x48 -> 0x3c): the group the tree is in,
    // so a controller's private tree ("Age Transition" of Controller - Age,
    // run on the Sim by Run Tree by Name) finds its own strings and not
    // its OBJD's semi-global's (PhoneCallGlobals). 0x02 takes the message
    // from temp 0 + 1.
    const uint8_t flags = o[8];
    const int scope = flags & 0x40 ? 2 : 0;
    const Object* owner = vm.object(c.frame.codeOwner);
    if (!owner)
        owner = &me;
    const StringTable* strings = scopeStrings(vm, *owner, c.frame.group, scope, kDialogStrings);
    const int messageIndex = flags & 0x02 ? c.thread.temps[0] + 1 : c.instruction.nodeVersion == 0 ? o[2] : le16(&o[13]);
    const int titleIndex = flags & 0x10 ? c.thread.temps[3] : o[6];

    Dialog d;
    d.id = vm.nextDialogId++;
    d.me = me.id;
    d.stackObject = c.frame.stackObject;
    d.type = type;
    d.typeName = typeLabel(vm, type);
    d.cls = dialogClass(type);
    lastTokenObject = 0;
    d.message = expandDialogTokens(c, stringAt(strings, messageIndex));
    d.title = expandDialogTokens(c, stringAt(strings, titleIndex));
    d.icon = (o[7] >> 1) & 7;
    d.timeout = o[10];
    d.priority = o[9];
    d.continueOthers = o[7] & 0x80;
    d.detached = o[7] & 0x01;
    d.depth = depth, d.tree = c.frame.behavior, d.node = c.frame.pc;
    const int16_t tokenObject = lastTokenObject;
    // A button's string (STR# 0x12D), else cTSObjectDialog's default from
    // UIText STR# 0x8E (FUN_001d0a38: 1 OK, 2 Cancel, 3 Yes, 4 No).
    auto button = [&](int index, int fallback) {
        const std::string s = expandDialogTokens(c, stringAt(strings, index));
        if (!s.empty())
            return s;
        static const char* pt[] = {"", "OK", "Cancelar", "Sim", "Não"};
        return uiText(0x8E, fallback, pt[fallback]);
    };

    switch (type) {
    case dialogtype::TextNotification:
    case dialogtype::StopTextNotification:
    case dialogtype::ModifyTextNotification:
    case dialogtype::TnsBirthday:
        // TryDialog takes the text's object from the stack element's
        // tree-owning object (vtable 0x0C: TryKillObject's log calls it "the
        // tree owning object"), the stack object only when that is gone;
        // error 0x17 only without both. The stack object just gives the icon,
        // so a notification about an object that has left (Circe's phone
        // call, N002_Lot3) shows without one instead of killing the Main.
        if (!owner && !vm.object(c.frame.stackObject))
            throw VMError("Could not find object reference (text notification without an object)");
        d.blocking = false;
        d.detached = false;
        resolveIcon(c, d, strings, tokenObject);
        if (type == dialogtype::TextNotification || type == dialogtype::TnsBirthday)
            c.thread.temps[size_t(resultTemp)] = int16_t(d.id & 0x7FFF);
        if (vm.notifications.size() >= kMaxNotifications)
            vm.notifications.pop_front();
        vm.notifications.push_back(std::move(d));
        return Exit::True;
    case dialogtype::TutorialNext:
    case dialogtype::TutorialNextModal: {
        // Not a dialog that waits: the tutorial's progress window
        // (tutorial.hpp), shown or hidden at once.
        --vm.nextDialogId;
        // The group scopeStrings took the STR# 0x12D from.
        const uint32_t group = scope == 2                                           ? kGlobalGroup
                               : vm.code().strings(c.frame.group, kDialogStrings) ? c.frame.group
                                                                                  : owner->group;
        return Exit(tutorialNextDialog(c, type, group));
    }
    default: break;
    }

    // TryDialog: before queueing, the types 5-7, 11-13, 15, 0x21, 0x33,
    // 0x34 and 0x46 need a stack object that has an object array (error
    // 0x6C "Bad Object Array"); without a stack object they go on.
    if (type == 18 && !d.detached) // "Tutorial Tasks": TryDialog goes on without a window in BV
        return --vm.nextDialogId, Exit::True;
    if (!d.detached && ((type >= 5 && type <= 7) || (type >= 11 && type <= 13) || type == 15 || type == 0x21 || type == 0x33 ||
         type == 0x34 || type == 0x46))
        if (const Object* stack = vm.object(c.frame.stackObject); stack && stack->arrays.empty())
            throw VMError("Bad Object Array (dialog type " + std::to_string(type) + ")");

    Object* stack = vm.object(c.frame.stackObject);
    switch (d.cls.cls) {
    case DialogClass::Object:
        // cTSObjectDialog::SetParams / SetupDialog (BV 0x1d2b20, 0x1d0da8):
        // button 1 (or a close box with "continue"), 2 and 3 by the type.
        switch (type) {
        case dialogtype::YesNo: d.buttons = {button(o[3], 3), button(o[4], 4)}; break;
        case dialogtype::YesNoCancel: d.buttons = {button(o[3], 3), button(o[4], 4), button(o[0], 2)}; break;
        default: d.buttons = {button(o[3], 1)}; break;
        }
        resolveIcon(c, d, strings, tokenObject);
        break;
    case DialogClass::NameObject:
        // OK (0xA000) and, in the panels that have one, cancel (0xA001).
        d.buttons = {button(o[3], 1), button(o[4], 2)};
        setupNames(c, d);
        break;
    case DialogClass::PhoneServices:
    case DialogClass::PhoneNeighbors:
    case DialogClass::PhoneParty:
        // The buttons are pictures (OK 0xAAAA, cancel 0xBBBB); the party
        // prints bytes 3/4's strings elsewhere (setupPhone).
        d.buttons = {expandDialogTokens(c, stringAt(strings, o[3])), expandDialogTokens(c, stringAt(strings, o[4]))};
        setupPhone(c, d, stack);
        d.buttons = {uiText(0x8E, 1, "OK"), uiText(0x8E, 2, "Cancelar")};
        break;
    case DialogClass::Clothing: {
        // cTSWinProcClothingDialog::SetParams (BV 0x330088).
        d.buttons = {uiText(0x8E, 1, "OK"), uiText(0x8E, 2, "Cancelar")};
        d.outfitSim = stack && stack->person ? stack->id : me.id;
        d.outfitCategory = outfitCategoryOfTemp(c.thread.temps[0]);
        break;
    }
    case DialogClass::ClothingPurchase:
    case DialogClass::ClothingTryOn: {
        // SetParams (BV 0x404ac8 / 0x3bed44): the Sim is the stack object
        // (the trees put the Sim running them there); temp 0 the outfit
        // type, mapped as the wardrobe's; the everyday ones (0, 9, 10) offer
        // tops and bottoms and open on tops. The rack: temp 1 the price per
        // item (byte 9 = '.' is the expansions' owned shop, which reads the
        // tree's locals instead: not in the base game).
        d.buttons = {uiText(0x8E, 1, "OK"), uiText(0x8E, 2, "Cancelar")};
        d.outfitSim = stack && stack->person ? stack->id : me.id;
        // Both take 6 (try-on) as everyday (0x001), the rack testing it as
        // 0 (separates) and the try-on as 6 (whole outfits only).
        int16_t t = c.thread.temps[0];
        d.outfitCategory = t == 6 ? 0x001 : outfitCategoryOfTemp(t);
        if (t == 6 && d.cls.cls == DialogClass::ClothingPurchase)
            t = 0;
        d.outfitSeparates = t == 0 || t == 9 || t == 10;
        if (d.cls.cls == DialogClass::ClothingPurchase) {
            d.pricePerItem = c.thread.temps[1];
            const Object* sim = vm.object(d.outfitSim);
            const int16_t family = sim && sim->person ? sim->person->data[persondata::FamilyNumber] : 0;
            if (vm.families.count(family))
                d.fillFunds = familyFunds(vm, family);
        }
        break;
    }
    case DialogClass::Vanity:
        // SetParams (BV 0x2effa4): the Sim is the stack object; temp 0 = 1
        // the plastic surgery panel (Initialize: script 0x8C159286 instead
        // of 0x8C159280).
        d.buttons = {uiText(0x8E, 1, "OK"), uiText(0x8E, 2, "Cancelar")};
        d.outfitSim = stack && stack->person ? stack->id : me.id;
        d.vanitySurgery = c.thread.temps[0] == 1;
        break;
    case DialogClass::JewelryPurchase:
        // cTSWinProcJewelryPurchaseDialog (script 0xACDC3000) is an
        // expansion pack's: the script is not in the base game's ui.package
        // and no base game object opens it ("Interaction - Jewelry Rack -
        // Browse" of the shopping rack globals has no rack). OK or cancel.
        d.buttons = {uiText(0x8E, 1, "OK"), uiText(0x8E, 2, "Cancelar")};
        d.outfitSim = stack && stack->person ? stack->id : me.id;
        break;
    case DialogClass::SelectItems: {
        // SetParams (BV 0x38a69c): temps 0/1 = multiple picks / obey the
        // price (temps 2/3, 100 by default, were not followed).
        d.buttons = {uiText(0x8E, 1, "OK"), uiText(0x8E, 2, "Cancelar")};
        d.multiSelect = c.thread.temps[0] == 1;
        d.obeyPrice = c.thread.temps[1] == 1;
        const int16_t family = me.person ? me.person->data[persondata::FamilyNumber] : 0;
        if (auto f = vm.families.find(family); f != vm.families.end())
            d.fillFunds = f->second.funds;
        if (stack && !stack->arrays.empty())
            for (int16_t v : stack->array(0).values)
                if (const Object* item = vm.object(v)) {
                    DialogEntry e;
                    e.id = v, e.guid = item->guid(), e.name = displayName(vm, *item);
                    e.price = item->definition ? item->definition->price() : 0;
                    d.entries.push_back(std::move(e));
                }
        break;
    }
    case DialogClass::Fill: {
        // SetParams (BV 0x294ca4): locals i..i+3 (i = byte 11); 50 units per
        // cost step (type 14). The binary throws 0x7C when vtable 0x6C (a
        // count the frame gives) is under i + 4; "Sub - Food Dialog" asks
        // for locals 4-7 of a tree with 6, so what that count is was not
        // settled: missing locals read 0 here (inferred).
        d.buttons = {uiText(0x8E, 1, "OK"), uiText(0x8E, 2, "Cancelar")};
        const size_t i = o[11];
        auto local = [&](size_t k) { return k < c.frame.locals.size() ? int(c.frame.locals[k]) : 0; };
        d.fillUnits = local(i), d.fillCost = local(i + 1);
        d.fillDelivery = local(i + 2), d.fillMax = local(i + 3);
        d.fillValue = d.fillUnits;
        const int16_t family = me.person ? me.person->data[persondata::FamilyNumber] : 0;
        if (auto f = vm.families.find(family); f != vm.families.end())
            d.fillFunds = f->second.funds;
        break;
    }
    case DialogClass::SetAspiration:
        // SetParams (BV 0x3cdb08): the neighbor whose ID is the stack object;
        // no aspiration picked yet.
        d.buttons = {uiText(0x8E, 1, "OK")};
        d.aspirationNeighbor = c.frame.stackObject;
        d.text = neighborName(vm, d.aspirationNeighbor); // the "%s" of the question
        if (const auto* data = personDataOf(vm, d.aspirationNeighbor))
            d.female = (*data)[65] == 1;
        break;
    default: // the other special dialogs: OK or cancel for now
        d.buttons = {button(o[3], 1), button(o[0], 2)};
        break;
    }
    // cTSObjectDialog's time limit (docs/primitives-mac.md, Dialog): not
    // for the tutorial nor the special dialogs (their own classes).
    if (d.cls.cls == DialogClass::Object && type != dialogtype::Tutorial) {
        size_t chars = 0; // the message's length in characters (UTF-8)
        for (unsigned char ch : d.message)
            chars += (ch & 0xC0) != 0x80;
        d.timeoutMs = 4000 + 40 * int(chars);
    }
    if (d.detached) {
        // "return": queued, and the tree goes on at once; kept under a key of
        // its own (negative, as the host's own dialogs) until answered.
        d.answer = vm.autoDialogAnswer >= 0 ? 0 : -1;
        const ObjectId key = ObjectId(-0x1000 - int(d.id & 0x3FFF));
        vm.dialogs[key] = std::move(d);
        return Exit::True;
    }
    if (vm.autoDialogAnswer >= 0)
        d.answer = std::min<int>(vm.autoDialogAnswer, int(d.buttons.size()) - 1);
    vm.dialogs[me.id] = std::move(d);
    return Exit::Yield;
}

} // namespace

void registerDialogPrimitives(VM& vm) { vm.setPrimitive(0x24, dialog); }

} // namespace os2::vm
