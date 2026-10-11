// Make New Character (0x13), from the Mac binary (BV 1.10, FUN_0065CB14,
// the handler nTSSG::cObject dispatches 0x13 to; Ghidra headless, read only)
// and cTSNeighborManager::AddNewCharacter.
//
// Operands (MakeNewCharacterParam: u16 at 4-5 and 7-8, u32 at 10-13):
// flags byte 9. The new neighbor IDs go into the stack object's object array
// 0, emptied first (no stack object: error 0x15; one without arrays: 0x6C);
// the trees read them from there ("NPC - Make New Character - Specific":
// iterator 0, element → stack object, Generic Sims Call 4 adds it to a
// family).
//
// - Without 0x01, a new Sim from a template: the template GUID is in temps
//   0/1 (0x08) or is the stack object's temporary token's (0x10), else the
//   default person template (TemplatePerson, 0x7FD96B54, as AddNewCharacter
//   takes for -1); a GUID with no OBJD is error 0x7A, a dead one (type 0x11)
//   error 0x94. Age: the local of byte 1 (TS2 age flags: 1 toddler, 2 child,
//   4 teen, 8 adult, 0x10 elder, 0x20 baby), 0 or less = 1 << random(6);
//   gender: the local of byte 2 (1 male, 2 female), 0 or less = a coin flip;
//   the local of byte 0 also goes to the CAS (meaning unknown). With 0x40
//   the outfit comes from an outfit GUID (0x80: temps 2/3, else the u32 at
//   10; 1 = the Downtownie bin, Bon Voyage era); the CAS picks among the
//   outfits of that GUID that fit the age and gender. A local index out of
//   range is error 0x08. True with the new neighbor in the array.
// - With 0x01, babies of two parents: (owner byte 6, data u16 7) and (owner
//   byte 3, data u16 4), neighbor IDs with 0x02 / 0x04, else Sims' object
//   IDs (missing: error 0x37); temp 0 babies (1 to 10, else error 0x76),
//   each from the CAS's genetics (0x18 with both parents' genes). The binary
//   creates them in the background: the first run returns "wait", the
//   reruns poll until every baby is ready (unless byte 13 bit 0, it also
//   posts a message about the birth); here they are ready at once (true).
//   Each baby gets its genes from genetics.hpp as ProcessCreateBabyRequest
//   draws them (PickGene per gene type, the descendant face's dominant
//   regions as gene 5; docs/cas-mac.md §5), from the parents' DNA (the
//   neighborhood's SDNA); gender: a lone parent's (both the same Sim), else
//   a coin flip; person data 58 = 1, fatness (82) 500, body flags (81) 0.
//   The baby looks like every baby (buFace, its skin tone): the face it
//   inherits shows from toddler on, which needs the parents' LxNR (open).
//
// The binary writes byte 14's local as a skin tone (< 4, else error 0x9E),
// but the base game's trees all leave byte 14 at 0, which names the age's
// local (4, 8, 16 from "G - Person Types"): with the Bon Voyage reading
// every NPC would fail. The base game's handler must differ there; the skin
// tone here is a parent's (babies) or random (inferred).
//
// The character itself: the game writes a new character file
// (N00x_User%05d, AddNewCharacter) whose OBJD is the template's with a new
// GUID and person type. Here that OBJD is made in memory in the template's
// group at a new instance (0x7000 on), with the template's OBJf, so
// Create New Object Instance (flag 0x04, the neighbor's GUID) can make the
// Sim. The neighbor ID is the highest known + 1 (inferred; the binary's
// neighbor manager assigns it). Person data: age (58, as Age writes it),
// gender (65: 0 male, 1 female), skin color (60), neighbor id (31); the
// name is the template's title (inferred). Neither the file nor the
// neighborhood entry is saved yet.

#include "characters.hpp"

#include "errors.hpp"
#include "genetics.hpp"
#include "formats/binreader.hpp"

namespace os2::vm {

namespace {

constexpr uint32_t kTemplatePerson = 0x7FD96B54;
constexpr int16_t kSkinColor = 60;
constexpr int16_t kGender = 65;
constexpr int16_t kBodyFlags = 81, kFatness = 82;

int16_t ageData(int16_t ageFlag) {
    switch (ageFlag) {
    case 0x01: return 0x02; // toddler
    case 0x02: return 0x03; // child
    case 0x04: return 0x10; // teen
    case 0x10: return 0x33; // elder
    case 0x20: return 0x00; // baby
    default: return 0x13;   // adult (and young adult)
    }
}

int16_t local(Context& c, uint8_t index) {
    if (index >= c.frame.locals.size())
        throw gameError(err::StackNumberOutOfRange, "Make New Character: local " + std::to_string(index));
    return c.frame.locals[index];
}

uint16_t nextCharacterInstance = 0x7000;

// A neighbor with an in-memory character made from the template OBJD.
int16_t makeCharacter(VM& vm, uint32_t templateGuid, int16_t ageFlag, int16_t gender, int16_t skin) {
    CodeLibrary& code = vm.code();
    const auto at = code.definitionByGuid(templateGuid);
    const ObjectDefinition* t = at ? code.definition(at->first, at->second) : nullptr;
    if (!t)
        throw gameError(0x7A, hexString(templateGuid));
    if (t->objectType() == 0x11)
        throw gameError(0x94, hexString(templateGuid));

    // A GUID nothing else has.
    uint32_t guid = 0x0FD00000u | (uint32_t(vm.random()) & 0xFFFFF);
    while (code.definitionByGuid(guid))
        ++guid;
    ObjectDefinition d = *t;
    const auto set = [&](size_t offset, uint16_t v) {
        const size_t i = (offset - 0x44) / 2;
        if (d.fields.size() <= i)
            d.fields.resize(i + 1);
        d.fields[i] = v;
    };
    set(0x52, kObjectTypePerson);
    set(0x5C, uint16_t(guid));
    set(0x5E, uint16_t(guid >> 16));
    // In the template's group, so its private trees (Init 0x1001, Main
    // 0x1000 of the NPC templates) are the new Sim's, at a new instance.
    const uint32_t group = at->first;
    uint16_t instance = nextCharacterInstance;
    while (code.definition(group, instance))
        ++instance;
    nextCharacterInstance = uint16_t(instance + 1);
    if (const ObjectFunctions* f = code.functions(at->first, at->second))
        code.add(group, instance, ObjectFunctions(*f));
    code.add(group, instance, std::move(d));

    int16_t id = 0;
    for (const auto& [n, _] : vm.neighbors)
        id = std::max(id, n);
    ++id;
    Neighbor& n = vm.neighbors[id];
    n.guid = guid;
    n.personData[persondata::NeighborId] = id;
    n.personData[persondata::PersonAge] = ageData(ageFlag);
    n.personData[kGender] = gender == 1 ? 0 : 1;
    n.personData[kSkinColor] = skin;
    std::string name = t->title.empty() ? t->fileName : t->title;
    if (const auto dash = name.rfind(" - "); t->title.empty() && dash != std::string::npos)
        name = name.substr(dash + 3);
    n.firstName = name;
    return id;
}

int16_t parentNeighbor(Context& c, uint8_t owner, int16_t data, bool isNeighbor) {
    const int16_t v = c.get(owner, data);
    if (isNeighbor)
        return c.vm.neighbor(v) ? v : int16_t(0);
    const Object* o = c.vm.object(v);
    if (!o || !o->person)
        return 0;
    const int16_t id = o->person->data[persondata::NeighborId];
    return c.vm.neighbor(id) ? id : int16_t(0);
}

Exit makeNewCharacter(Context& c) {
    const auto& o = c.instruction.operands;
    const uint8_t flags = o[9];
    VM& vm = c.vm;
    Object* holder = vm.object(c.frame.stackObject);
    if (!holder)
        throw gameError(err::NoStackObject, "Make New Character");
    auto& list = holder->array(0);
    list.values.clear();
    list.iterator = 0;
    auto rand = [&](int n) { return int(vm.random() % uint32_t(n)); };

    uint32_t templateGuid = kTemplatePerson;
    if (flags & 0x08)
        templateGuid = uint16_t(c.thread.temps[0]) | uint32_t(uint16_t(c.thread.temps[1])) << 16;
    else if (flags & 0x10)
        templateGuid = holder->tempToken.loaded ? holder->tempToken.token.guid : 0;

    if (!(flags & 0x01)) {
        int16_t age = local(c, o[1]);
        if (age <= 0)
            age = int16_t(1 << rand(6));
        int16_t gender = local(c, o[2]);
        if (gender <= 0)
            gender = rand(100) < 50 ? 2 : 1;
        else if (gender != 1)
            gender = 2;
        (void)local(c, o[0]);
        list.values.push_back(makeCharacter(vm, templateGuid, age, gender, int16_t(rand(4))));
        return Exit::True;
    }

    const int16_t a = parentNeighbor(c, o[6], int16_t(le16(&o[7])), flags & 0x02);
    const int16_t b = parentNeighbor(c, o[3], int16_t(le16(&o[4])), flags & 0x04);
    if (!a || !b)
        throw gameError(err::MissingNeighbor, "Make New Character: parent");
    const int16_t count = c.thread.temps[0];
    if (count < 1 || count > 10)
        throw gameError(0x76, "Make New Character: " + std::to_string(count) + " babies");
    if (!(flags & 0x08) && !(flags & 0x10))
        if (vm.code().definitionByGuid(vm.neighbor(a)->guid))
            templateGuid = vm.neighbor(a)->guid; // the CAS starts from a parent (inferred)
    // The CAS's baby request (genetics.hpp): the genes of both parents, the
    // gender (a lone parent's, else a coin flip), the face region by region.
    const genetics::Random random = [&](uint32_t n) { return n ? vm.random() % n : 0u; };
    const genetics::GeneticValue value = genetics::baseGameGeneticValues();
    const auto skinTones = genetics::baseGameSkinTones();
    for (int i = 0; i < count; ++i) {
        const Neighbor& ma = *vm.neighbor(a);
        const Neighbor& pa = *vm.neighbor(b);
        const genetics::Dna dnaA = genetics::Dna::fromProperties(ma.dna), dnaB = genetics::Dna::fromProperties(pa.dna);
        genetics::Dna dna = genetics::determineGenetics(dnaA, dnaB, value, random, skinTones);
        const genetics::DescendantFace face = genetics::determineDescendantFace(dnaA, dnaB, random);
        dna.alleles[genetics::gene::DominantRegions] = {face.dominant, ""};
        const bool female = genetics::babyIsFemale(a == b, ma.personData[kGender] == 1, random);
        const int16_t skin = ma.personData[kSkinColor];
        const int16_t familyNumber = ma.personData[persondata::FamilyNumber];
        const std::string lastName = ma.lastName;
        const int16_t id = makeCharacter(vm, templateGuid, 0x20, female ? 2 : 1, skin);
        Neighbor& baby = vm.neighbors[id];
        baby.lastName = lastName;
        baby.firstName.clear();
        baby.dna = dna.toProperties();
        baby.personData[persondata::PersonAge] = 1; // baby (ProcessCreateBabyRequest: data 58 = 1)
        baby.personData[kFatness] = 500;
        baby.personData[kBodyFlags] = 0;
        baby.personData[persondata::FamilyNumber] = familyNumber;
        list.values.push_back(id);
    }
    return Exit::True;
}

} // namespace

void registerCharacterPrimitives(VM& vm) { vm.setPrimitive(0x13, makeNewCharacter); }

} // namespace os2::vm
