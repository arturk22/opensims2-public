// Change Light (0x6F) and Change Outfit (0x79): what an object or Sim
// looks like, for the host to show (dialog.hpp).

#include "dialog.hpp"

#include "vm.hpp"

#include "formats/binreader.hpp"

#include <algorithm>

namespace os2::vm {

namespace {

// STR# of an object group naming the lights of its model (Change Light).
constexpr uint16_t kLightNameStrings = 0x8E;

// Person data (STR# 0xC8).
constexpr int16_t kCurrentOutfit = 8;
constexpr int16_t kOutfitSourceGuid1 = 119, kOutfitSourceGuid2 = 120;

// Change Light (docs/primitives-mac.md §4, binary): the object is (owner
// byte 2, data u16 3); the intensity is byte 7 in percent (temp 0 with
// byte 1 bit 1), over 100 becomes 50; the duration is u16 5 (temp 1 with
// byte 1 bit 0); light byte 8 = 0xFF every light of the model, else the
// light named by that entry of the stack object's STR# 0x8E. The game's
// change is instant (the renderer fades); here the duration is kept with
// the level so the renderer can fade over it, taken as ticks (inferred:
// the uses have 0, 5, 10, 15 and 40). Always true.
Exit changeLight(Context& c) {
    const auto& o = c.instruction.operands;
    VM& vm = c.vm;
    Object* target = vm.object(c.get(o[2], int16_t(le16(&o[3]))));
    if (!target)
        throw VMError("Could not find object reference");
    int intensity = o[1] & 0x02 ? c.thread.temps[0] : o[7];
    if (intensity > 100)
        intensity = 50;
    intensity = std::max(intensity, 0);
    const int duration = o[1] & 0x01 ? c.thread.temps[1] : le16(&o[5]);
    std::string name;
    if (o[8] != 0xFF) {
        const Object& stack = c.stackObject();
        const StringTable* t = vm.code().strings(stack.group, kLightNameStrings);
        name = t ? t->get(o[8], lang::EnglishUS) : std::string();
        if (name.empty())
            throw VMError("Change Light: no light name " + std::to_string(o[8]) + " in STR# 0x8E");
    }
    auto set = [&](LightState& s) {
        s.from = s.level(vm.ticks);
        s.to = intensity;
        s.since = vm.ticks;
        s.ticks = std::max(duration, 0);
    };
    if (name.empty()) {
        // Every light: the named ones follow too.
        for (auto& [n, s] : target->lights)
            set(s);
        if (!target->lights.count(""))
            set(target->lights[""]);
    } else {
        set(target->lights[name]);
    }
    return Exit::True;
}

// Change Outfit (docs/primitives-mac.md §4, binary): the Sim is (owner
// byte 9, data u16 10); the category is byte 8 (STR# 0xFA "Person Outfit
// Categories") or the variable (owner byte 1, data u16 2) with byte 0 bit
// 0x04; 0x10 rebuilds the current outfit; the source of the clothes is the
// stack object's definition (0x01), the GUID u32 4 (0x02) or the GUID in
// temps 0/1 (0x40), else the Sim's own, and goes to person data 119/120
// "Outfit Source GUID"; 0x20 clears those; 0x08 writes to the file (not
// done). The host re-dresses the Sim when person data 8 "current outfit"
// changes (app/lot_vm). Always true.
Exit changeOutfit(Context& c) {
    const auto& o = c.instruction.operands;
    VM& vm = c.vm;
    Object* sim = vm.object(c.get(o[9], int16_t(le16(&o[10]))));
    if (!sim)
        throw VMError("Change Outfit: the Sim does not exist");
    if (!sim->person)
        throw VMError("Change Outfit: not a person");
    auto& data = sim->person->data;
    const uint8_t flags = o[0];
    uint32_t source = 0;
    bool hasSource = false;
    if (flags & 0x01) {
        if (const Object* stack = vm.object(c.frame.stackObject))
            source = stack->guid(), hasSource = true;
    } else if (flags & 0x02) {
        source = le32(&o[4]), hasSource = true;
    } else if (flags & 0x40) {
        source = uint32_t(uint16_t(c.thread.temps[0])) | uint32_t(uint16_t(c.thread.temps[1])) << 16;
        hasSource = true;
    }
    if (flags & 0x20)
        data[kOutfitSourceGuid1] = data[kOutfitSourceGuid2] = 0;
    if (hasSource) {
        data[kOutfitSourceGuid1] = int16_t(source & 0xFFFF);
        data[kOutfitSourceGuid2] = int16_t(source >> 16);
    }
    if (flags & 0x10)
        return Exit::True; // the current outfit, rebuilt: nothing changes here
    const int16_t category = flags & 0x04 ? c.get(o[1], int16_t(le16(&o[2]))) : int16_t(o[8]);
    data[kCurrentOutfit] = category;
    return Exit::True;
}

// Want Satisfy (0x7C; operands from the disassembler, docs/primitives-mac.md
// §5): the Sim (owner byte 0, data u16 1), the want's GUID u32 3, its type
// (0 value, 1 person, 2/3 object, 4 skill, 5 career) with (owner 7, data
// u16 8) and a level (owner 10, data u16 11). Wants and aspiration points
// are not simulated yet, so it only checks the Sim and returns true (the
// job offer, "CT - Offer Job", satisfies "get a job" this way).
Exit wantSatisfy(Context& c) {
    const auto& o = c.instruction.operands;
    const Object* sim = c.vm.object(c.get(o[0], int16_t(le16(&o[1]))));
    if (!sim || !sim->person)
        return Exit::False;
    return Exit::True;
}

} // namespace

int lightLevel(const VM& vm, const Object& o, const std::string& name) {
    if (!name.empty())
        if (auto it = o.lights.find(name); it != o.lights.end())
            return it->second.level(vm.ticks);
    if (auto it = o.lights.find(""); it != o.lights.end())
        return it->second.level(vm.ticks);
    return -1;
}

void registerAppearancePrimitives(VM& vm) {
    vm.setPrimitive(0x6F, changeLight);
    vm.setPrimitive(0x79, changeOutfit);
    vm.setPrimitive(0x7C, wantSatisfy);
}

} // namespace os2::vm
