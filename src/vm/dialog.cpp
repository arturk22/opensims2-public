// Dialog (0x24): the dialogs the trees put up, their `$` tokens and the
// answer going back to the tree (dialog.hpp).
//
// Operands (docs/primitives-mac.md §4 and §5, from the Mac binary:
// TryDialog, BV 0xe2bb64, and cTSObjectDialog::SetParams, BV 0x1d2b20):
//   byte 0 button 3 ("cancel") string, byte 1 icon index, byte 3 button 1
//   ("yes"), byte 4 button 2 ("no"), byte 5 type (STR# 0xD9), byte 6
//   title, byte 7: bit 0 "return" (queued, true at once), bit 7
//   "continue" (the game does not wait; button 1 is a close box,
//   SetupDialog), bits 1-3 icon type (STR# 0xF4), bits 4-6 the temp the
//   choice or the notification ID goes to; byte 8 flags: 0x40 the global
//   strings, else the tree's own (TryDialog's reading of 0x01 is kept
//   below), 0x02 message index in temp 0, 0x10 title index in temp 3,
//   0x04/0x08/0x20 buttons 1/2/3 in temps 1/2/4; byte 10 timeout; u16 13
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
#include "formats/log_once.hpp"

#include "vm.hpp"
#include "hood_bridge.hpp"
#include "tutorial.hpp"

#include "formats/binreader.hpp"
#include "formats/dbpf.hpp"
#include "formats/resindex.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <optional>

namespace os2::vm {

namespace {

constexpr uint16_t kDialogStrings = 0x12D;
// The object or neighbor the last icon-giving token of a dialog string
// named (ParseUIString's selector out-parameter: $Neighbor and $FullName
// the neighbor in the stack object, $Object the stack object): the
// automatic icon (resolveIcon).
struct TokenIcon {
    int16_t object = 0, neighbor = 0;
};
thread_local TokenIcon lastTokenIcon;
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
        } catch (const std::exception& err) {
            os2::logOnce("catalog strings", std::to_string(o.group), err);
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


// Live.package's STR# 0x99, one-based: 1 Sunday ... 7 Saturday.
std::string dayName(int oneBased) {
    static const char* fallback[] = {"Domingo", "Segunda-feira", "Terça-feira", "Quarta-feira", "Quinta-feira", "Sexta-feira", "Sábado"};
    const auto& tables = textTables();
    const StringTable* t = tables ? tables(kLiveTextGroup, 0x99) : nullptr;
    if (t && oneBased > 0 && size_t(oneBased) <= t->entries.size()) {
        std::string s = t->get(size_t(oneBased - 1));
        if (!s.empty())
            return s;
    }
    return oneBased >= 1 && oneBased <= 7 ? fallback[oneBased - 1] : std::string();
}

// $DayFlags (ParseUIString): bits 0-6 of a job's days are STR# 0x99 entries
// 2-7 and 1 (Monday ... Saturday, Sunday), joined by ", ".
std::string dayFlags(int bits) {
    std::string out;
    for (int d = 0; d < 7; ++d)
        if (bits & (1 << d)) {
            if (!out.empty())
                out += ", ";
            out += dayName(d + 2 == 8 ? 1 : d + 2);
        }
    return out;
}

// The group a dialog's strings come from (cTSObjectDialog::SetParams BV
// 0x1d2b20 and TryDialog BV 0xe2bb64; docs/primitives-mac.md "Dialog",
// round n45): the frame's cTSBehavior (+0x20) is the one of the object whose
// code runs (the code owner), and its private group (vtable 0x28) is the
// default, not the group the tree is in: the career's chance cards ("Sub -
// Chance Card - Dialog" of JobDataGlobals, byte 8 0x0E) run on the job
// object read its STR# 0x12D, which has the cards, not the semi-global's,
// which has the job offers. Byte 8 bit 0x01 takes the semi-global of the
// owner's definition (vtable 0x118; none: no strings), else 0x40 the global
// one; the semi-global's own trees that want its strings set bit 0 ("CT -
// Offer Job").
uint32_t scopeGroup(const Object& owner, int scope) {
    switch (scope) {
    case 0: return owner.group;
    case 1: return owner.semiGroup;
    default: return kGlobalGroup;
    }
}

const StringTable* scopeStrings(VM& vm, const Object& owner, int scope, uint16_t table) {
    const uint32_t group = scopeGroup(owner, scope);
    return group ? vm.code().strings(group, table) : nullptr;
}

std::string stringAt(const StringTable* t, int oneBased) {
    if (!t || oneBased <= 0)
        return {};
    return t->get(size_t(oneBased - 1));
}

} // namespace

// The language's formatter (cRZLanguageUtility, the one RZ::FS()'s
// GetLanguageUtility(0) gives, BV 0x12b5a8; docs/primitives-mac.md "Dialog",
// round n45). GetThousandSeparator (BV 0x136dd4) by the STR# language code.
static std::string thousandSeparator(uint8_t language) {
    switch (language) {
    case 4: case 5: case 6: case 7: case 8: case 0x0E: case 0x23: return ".";
    case 3: case 9: case 10: case 0x0B: case 0x0D: case 0x10: case 0x1A: return "\xC2\xA0"; // no-break space (+0x18)
    default: return ","; // 1, 2, 0x0C, 0x0F, 0x11-0x14, 0x1C and the others
    }
}

// MakeNumberString (BV 0x138448, base 1.0 the same): "%lld" of the
// magnitude, its digits in threes from the right joined by the thousand
// separator, "-" in front of a negative one.
std::string languageNumber(int64_t value, uint8_t language) {
    const bool negative = value < 0;
    const std::string digits = std::to_string(negative ? uint64_t(0) - uint64_t(value) : uint64_t(value));
    const std::string sep = thousandSeparator(language);
    std::string out;
    size_t first = digits.size() % 3;
    if (first == 0)
        first = 3;
    out = digits.substr(0, std::min(first, digits.size()));
    for (size_t i = first; i < digits.size(); i += 3)
        out += sep + digits.substr(i, 3);
    return negative ? "-" + out : out;
}

// MakeTimeString(hour, minute, -1) (BV 0x137700): US English (1) and 0x2A
// in 12 hours with am / pm (0 is 12), else the language's 24-hour form;
// Brazilian Portuguese (0x23) is "%u:%.2u". (The BV's force12HrTime
// property, not in base 1.0, is not read.)
std::string languageTime(int hour, int minute, uint8_t language) {
    hour = ((hour % 24) + 24) % 24;
    minute = ((minute % 60) + 60) % 60;
    char buf[24];
    switch (language) {
    case 1:
    case 0x2A:
        std::snprintf(buf, sizeof buf, "%d:%02d %s", hour == 0 ? 12 : hour > 12 ? hour - 12 : hour, minute,
                      hour < 12 ? "am" : "pm");
        break;
    case 2: case 3: case 4: case 5: case 0x10: case 0x1D:
        std::snprintf(buf, sizeof buf, "%02d:%02d", hour, minute);
        break;
    case 8: case 0x24: case 0x27: case 0x28:
        std::snprintf(buf, sizeof buf, "%02d.%02d", hour, minute);
        break;
    case 0x0B: case 0x1C: case 0x1A:
        std::snprintf(buf, sizeof buf, "%d.%02d", hour, minute);
        break;
    default:
        std::snprintf(buf, sizeof buf, "%d:%02d", hour, minute);
        break;
    }
    return buf;
}

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

namespace {

// GetPropertyValueAsString (BV 0xb05f38): `token` in `text` from `from`,
// without case and as a prefix ("$Mes" holds "$Me"), then up to two
// ":<digits>" arguments (-1 when absent); `len` covers all of it.
struct TokenMatch {
    size_t pos = std::string::npos, len = 0;
    int a = -1, b = -1;
};

TokenMatch findToken(const std::string& text, const std::string& lowerToken, size_t from) {
    TokenMatch m;
    if (from > text.size())
        return m;
    const std::string low = lower(text);
    const size_t pos = low.find(lowerToken, from);
    if (pos == std::string::npos)
        return m;
    m.pos = pos;
    size_t p = pos + lowerToken.size();
    auto digits = [&](int& out) {
        std::string d;
        while (p < text.size() && std::isdigit(static_cast<unsigned char>(text[p])))
            d += text[p++];
        if (!d.empty())
            out = std::atoi(d.c_str());
    };
    if (p < text.size() && text[p] == ':') {
        ++p;
        digits(m.a);
        if (p < text.size() && text[p] == ':') {
            ++p;
            digits(m.b);
        }
    }
    m.len = p - pos;
    return m;
}

} // namespace

// cTSObject::ParseUIString (vtable 0x408; base 1.0 FUN_00b36ed0, BV
// 0xb0eeb4; docs/primitives-mac.md "Dialog", round n27): the base game's 35
// tokens, each a pass over the whole text in the game's order; a token
// whose arguments do not hold (a local past the tree's locals, no such
// neighbor or object) stays as it is and the pass goes on one letter
// later. "local n" is the tree's local variable n; the stack object, when
// a token reads a neighbor, is a neighbor ID. `depth` is not used (the
// game inserts a string token's text and goes on with the tokens after it
// in the order); it stays for the callers.
std::string expandDialogTokens(Context& c, const std::string& text, int depth) {
    (void)depth;
    VM& vm = c.vm;
    std::string out = text;
    const int locals = int(c.frame.locals.size());
    auto validLocal = [&](int i) { return i >= 0 && i < locals; };
    auto local = [&](int i) -> int16_t { return validLocal(i) ? c.frame.locals[size_t(i)] : int16_t(0); };
    const int16_t stackId = c.frame.stackObject;
    const Object* me = vm.object(c.thread.me);
    // An object's name as its selector gives it (a multi-tile part: its
    // master's).
    auto objectName = [&](int16_t id) -> std::optional<std::string> {
        const Object* o = vm.object(id);
        if (!o)
            return std::nullopt;
        if (const Object* lead = vm.object(multiTileGroup(*o)))
            o = lead;
        return displayName(vm, *o);
    };
    auto neighborFirst = [&](int16_t id) -> std::optional<std::string> {
        if (!vm.neighbor(id))
            return std::nullopt;
        return neighborName(vm, id);
    };
    auto neighborFull = [&](int16_t id) -> std::optional<std::string> {
        const Neighbor* n = vm.neighbor(id);
        if (!n)
            return std::nullopt;
        const std::string first = neighborName(vm, id), last = n->lastName;
        if (!first.empty() && !last.empty())
            return first + " " + last;
        return last.empty() ? first : last;
    };
    auto familyOf = [&](int16_t neighborId) -> const VM::Family* {
        const Neighbor* n = vm.neighbor(neighborId);
        if (!n)
            return nullptr;
        const Object* sim = vm.neighborSim(neighborId);
        const int16_t family = sim && sim->person ? sim->person->data[persondata::FamilyNumber]
                                                  : n->personData[persondata::FamilyNumber];
        auto f = vm.families.find(family);
        return f == vm.families.end() ? nullptr : &f->second;
    };
    // The language's MakeNumberString (ParseUIString: utility vtable 0x48).
    auto number = [](int64_t v) { return languageNumber(v, lang::preferred()); };
    auto table = [&](uint32_t group, int t, int index) -> std::optional<std::string> {
        const StringTable* st = group ? vm.code().strings(group, uint16_t(t)) : nullptr;
        if (!st)
            return std::string(); // FUN_00212618 gives an empty string
        return stringAt(st, index);
    };

    // Each entry: the token and its value at a match (none: stays).
    using Value = std::function<std::optional<std::string>(const TokenMatch&)>;
    auto twoLocals = [&](const TokenMatch& m) { return validLocal(m.a) && validLocal(m.b); };
    const std::pair<const char*, Value> tokens[] = {
        // 1-8: the inventory tokens ($TokenPersonalCountedLocalName ...):
        // not done (no base-game string uses them); they stay.
        {"$familyassets", [&](const TokenMatch& m) -> std::optional<std::string> {
             if (!validLocal(m.a))
                 return std::nullopt;
             auto f = vm.families.find(local(m.a));
             if (f == vm.families.end())
                 return std::nullopt;
             return number(f->second.funds); // family vtable 0xa4 (inferred: the funds)
         }},
        {"$family", [&](const TokenMatch&) -> std::optional<std::string> {
             const VM::Family* f = familyOf(stackId);
             if (!f)
                 return std::nullopt;
             return f->name;
         }},
        {"$lastname", [&](const TokenMatch&) -> std::optional<std::string> {
             const Neighbor* n = vm.neighbor(stackId);
             if (!n)
                 return std::nullopt;
             return n->lastName;
         }},
        {"$neighborlocal", [&](const TokenMatch& m) -> std::optional<std::string> {
             return validLocal(m.a) ? neighborFirst(local(m.a)) : std::nullopt;
         }},
        {"$neighbor", [&](const TokenMatch&) -> std::optional<std::string> {
             auto v = neighborFirst(stackId);
             if (v)
                 lastTokenIcon = {0, stackId};
             return v;
         }},
        {"$me", [&](const TokenMatch&) -> std::optional<std::string> {
             if (!me)
                 return std::nullopt;
             return displayName(vm, *me);
         }},
        {"$object", [&](const TokenMatch&) -> std::optional<std::string> {
             auto v = objectName(stackId);
             if (v)
                 lastTokenIcon = {stackId, 0};
             return v;
         }},
        {"$local", [&](const TokenMatch& m) -> std::optional<std::string> {
             return validLocal(m.a) ? std::optional<std::string>(number(local(m.a))) : std::nullopt;
         }},
        {"$money", [&](const TokenMatch& m) -> std::optional<std::string> {
             if (!validLocal(m.a) || !validLocal(m.a + 1))
                 return std::nullopt;
             const int64_t x = local(m.a), y = local(m.a + 1);
             return number(y ? x * y : x);
         }},
        {"$cash", [&](const TokenMatch& m) -> std::optional<std::string> {
             if (!validLocal(m.a) || !validLocal(m.a + 1))
                 return std::nullopt;
             return number(int64_t(local(m.a)) + int64_t(local(m.a + 1)) * 10000);
         }},
        {"$timelocal", [&](const TokenMatch& m) -> std::optional<std::string> {
             if (!validLocal(m.a) || local(m.a) < 0 || local(m.a) >= 24)
                 return std::nullopt;
             return languageTime(local(m.a), 0, lang::preferred()); // utility vtable 0x24(hour, 0, -1)
         }},
        {"$attrib", [&](const TokenMatch& m) -> std::optional<std::string> {
             const Object* s = vm.object(stackId);
             if (!s || m.a < 0 || size_t(m.a) >= s->attributes.size())
                 return std::nullopt;
             return number(s->attributes[size_t(m.a)]);
         }},
        {"$nameattrib", [&](const TokenMatch& m) -> std::optional<std::string> {
             const Object* s = vm.object(stackId);
             if (!s || m.a < 0 || size_t(m.a) >= s->attributes.size())
                 return std::nullopt;
             return objectName(s->attributes[size_t(m.a)]);
         }},
        {"$namelocal", [&](const TokenMatch& m) -> std::optional<std::string> {
             return validLocal(m.a) ? objectName(local(m.a)) : std::nullopt;
         }},
        {"$fullnamelocal", [&](const TokenMatch& m) -> std::optional<std::string> {
             if (!validLocal(m.a))
                 return std::nullopt;
             if (auto v = neighborFull(local(m.a)))
                 return v;
             return neighborFull(stackId);
         }},
        {"$fullname", [&](const TokenMatch&) -> std::optional<std::string> {
             auto v = neighborFull(stackId);
             if (v)
                 lastTokenIcon = {0, stackId};
             return v;
         }},
        {"$lotnamelocal", [&](const TokenMatch& m) -> std::optional<std::string> {
             if (!validLocal(m.a))
                 return std::nullopt;
             const VM::Family* f = familyOf(local(m.a));
             if (!f)
                 return std::nullopt;
             auto l = vm.lots.find(f->house);
             if (l == vm.lots.end())
                 return std::nullopt;
             return l->second.name;
         }},
        {"$cataloglocal", [&](const TokenMatch& m) -> std::optional<std::string> {
             return validLocal(m.a) ? objectName(local(m.a)) : std::nullopt;
         }},
        {"$daylocal", [&](const TokenMatch& m) -> std::optional<std::string> {
             return validLocal(m.a) ? std::optional<std::string>(dayName(local(m.a) + 1)) : std::nullopt;
         }},
        {"$dayflags", [&](const TokenMatch& m) -> std::optional<std::string> {
             return validLocal(m.a) ? std::optional<std::string>(dayFlags(local(m.a))) : std::nullopt;
         }},
        {"$jobtitle", [&](const TokenMatch& m) -> std::optional<std::string> {
             // STR# 0x93 of the object in local b's group, entry local a + 1.
             if (!twoLocals(m))
                 return std::nullopt;
             const Object* job = vm.object(local(m.b));
             if (!job)
                 return std::nullopt;
             return table(job->group, kJobStrings, local(m.a) + 1);
         }},
        {"$privatestring", [&](const TokenMatch& m) -> std::optional<std::string> {
             // STR# local a of the stack object's own file, entry local b.
             if (!twoLocals(m))
                 return std::nullopt;
             const Object* s = vm.object(stackId);
             if (!s)
                 return std::nullopt;
             return table(s->group, local(m.a), local(m.b));
         }},
        {"$behaviorstring", [&](const TokenMatch& m) -> std::optional<std::string> {
             if (!twoLocals(m))
                 return std::nullopt;
             return table(kEditorLabelGroup, local(m.a), local(m.b));
         }},
        {"$aspirationstring", [&](const TokenMatch& m) -> std::optional<std::string> {
             // Live.package's tables, as "Aspiration - Get Level" leaves them
             // in temps 1 and 2 (the age's table 0x9A-0x9D, entry 1 + 13 x
             // aspiration + 2 x level + 1 for a female).
             if (!twoLocals(m))
                 return std::nullopt;
             const auto& tables = textTables();
             const StringTable* t = tables ? tables(kLiveTextGroup, uint32_t(uint16_t(local(m.a)))) : nullptr;
             return stringAt(t, local(m.b));
         }},
        {"$semiglobalstring", [&](const TokenMatch& m) -> std::optional<std::string> {
             if (!twoLocals(m))
                 return std::nullopt;
             const Object* s = vm.object(stackId);
             if (!s)
                 return std::nullopt;
             return table(s->semiGroup, local(m.a), local(m.b));
         }},
        {"$globalstring", [&](const TokenMatch& m) -> std::optional<std::string> {
             if (!twoLocals(m))
                 return std::nullopt;
             return table(kGlobalGroup, local(m.a), local(m.b));
         }},
        // 35: $TokenNameLocal (the personal inventory of the Sim): not done.
    };
    for (const auto& [token, value] : tokens) {
        const std::string t = token;
        for (size_t from = 0;;) {
            const TokenMatch m = findToken(out, t, from);
            if (m.pos == std::string::npos)
                break;
            if (auto v = value(m))
                out.replace(m.pos, m.len, *v);
            from = m.pos + 1;
        }
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
            // GetFullName: the language utility's MakeFullName (BV 0x137c04),
            // "first last", the one there is when the other is empty.
            e.name = n->lastName.empty() ? e.first : e.first.empty() ? n->lastName : e.first + " " + n->lastName;
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
    // by name) and PreShow's SetSelectedSim(0) picks the first of that
    // order: SortNeighborCallListByName (BV 0xa7c664) compares the full
    // names with the language utility's CompareStrings, case-blind:
    // _CompareStringA -> FUN_0003d7dc lowers each byte sign-extended, so the
    // UTF-8 bytes of accented letters come before every ASCII letter.
    if (d.cls.cls == DialogClass::PhoneNeighbors) {
        auto less = [](const std::string& a, const std::string& b) {
            for (size_t i = 0;; ++i) {
                const int x = i < a.size() ? int(static_cast<signed char>(a[i])) : 0;
                const int y = i < b.size() ? int(static_cast<signed char>(b[i])) : 0;
                const int lx = x >= 0 ? std::tolower(x) : x, ly = y >= 0 ? std::tolower(y) : y;
                if (lx != ly)
                    return lx < ly;
                if (lx == 0)
                    return false;
            }
        };
        std::stable_sort(d.entries.begin(), d.entries.end(),
                         [&](const DialogEntry& a, const DialogEntry& b) { return less(a.name, b.name); });
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
// 0xF4): 0 automatic, the selector the token expansion hands back (the
// last $Neighbor/$FullName neighbor or $Object object) else the stack
// object; 2 the neighbor
// whose ID is in the stack object (error 0x37 in TryDialog's notifications
// if none); 3 "indexed", the tree's object (inferred: the binary asks the
// object definition of the tree's file); 4 "named", a string of STR# 0x12D
// at byte 1: "gz <instance>"/"gzi <instance>" a UI image, "guid <n>" an
// object; 1 none.
void resolveIcon(Context& c, Dialog& d, const StringTable* strings, TokenIcon token) {
    VM& vm = c.vm;
    const auto& o = c.instruction.operands;
    switch (d.icon) {
    case 0:
        if (token.neighbor)
            d.iconNeighbor = token.neighbor;
        else
            d.iconObject = token.object ? token.object : c.frame.stackObject;
        break;
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

    // TryDialog (BV 0xe2bb64) / SetParams: the STR# 0x12D of the code
    // owner's private group (a controller's private tree, "Age Transition"
    // of Controller - Age, run by Run Tree by Name with the controller as
    // the code owner, finds its own strings and not those of its OBJD's
    // semi-global, PhoneCallGlobals), of its semi-global with byte 8 bit
    // 0x01, of the global one with 0x40 (scopeGroup). 0x02 takes the message
    // from temp 0: + 1 for the text notifications (TryDialog reads it
    // itself), as it is for the dialogs cTSObjectDialog::SetParams (BV
    // 0x1d2b20) reads, which also take button 1 from temp 1 with 0x04,
    // button 2 from temp 2 with 0x08 and button 3 from temp 4 with 0x20 (the
    // career's chance cards: "Sub - Chance Card - Dialog", byte 8 0x0E).
    const uint8_t flags = o[8];
    const int scope = flags & 0x01 ? 1 : flags & 0x40 ? 2 : 0; // bit 0 before 0x40, as SetParams tests them
    const Object* owner = vm.object(c.frame.codeOwner);
    if (!owner)
        owner = &me;
    const StringTable* strings = scopeStrings(vm, *owner, scope, kDialogStrings);
    const bool notification = type == dialogtype::TextNotification || type == dialogtype::ModifyTextNotification ||
                              type == dialogtype::TnsBirthday;
    const int messageIndex = flags & 0x02 ? c.thread.temps[0] + (notification ? 1 : 0)
                             : c.instruction.nodeVersion == 0 ? o[2]
                                                              : le16(&o[13]);
    const int titleIndex = flags & 0x10 ? c.thread.temps[3] : o[6];
    const int button1 = flags & 0x04 ? c.thread.temps[1] : o[3];
    const int button2 = flags & 0x08 ? c.thread.temps[2] : o[4];
    const int button3 = flags & 0x20 ? c.thread.temps[4] : o[0];

    Dialog d;
    d.id = vm.nextDialogId++;
    d.me = me.id;
    d.stackObject = c.frame.stackObject;
    d.type = type;
    d.typeName = typeLabel(vm, type);
    d.cls = dialogClass(type);
    // SetParams expands the title before the message: the message's
    // icon-giving tokens are the last ones.
    lastTokenIcon = {};
    d.title = expandDialogTokens(c, stringAt(strings, titleIndex));
    d.message = expandDialogTokens(c, stringAt(strings, messageIndex));
    d.icon = (o[7] >> 1) & 7;
    d.timeout = o[10];
    d.priority = o[9];
    d.continueOthers = o[7] & 0x80;
    d.detached = o[7] & 0x01;
    d.depth = depth, d.tree = c.frame.behavior, d.node = c.frame.pc;
    const TokenIcon tokenIcon = lastTokenIcon;
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
        resolveIcon(c, d, strings, tokenIcon);
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
        const uint32_t group = scopeGroup(*owner, scope);
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
        case dialogtype::YesNo: d.buttons = {button(button1, 3), button(button2, 4)}; break;
        case dialogtype::YesNoCancel: d.buttons = {button(button1, 3), button(button2, 4), button(button3, 2)}; break;
        default: d.buttons = {button(button1, 1)}; break;
        }
        resolveIcon(c, d, strings, tokenIcon);
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
