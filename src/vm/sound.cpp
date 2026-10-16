// Play / Stop Sound Event (0x17) and Stop ALL Sounds (0x30).
//
// Operands of 0x17 as the Mac binary reads them (cTSObject::TryPlaySound, BV
// 0xe41844; docs/primitives-mac.md §4j): event ID s16 at 0, flags u8 at 4,
// and with a negative event ID a data owner u8 at 6 and value u16 at 7. The
// sample rate (u16 at 2) and volume (u8 at 5) of The Sims 1's layout
// (FreeSO VMPlaySound) are not read (always 0 in the game's trees). Flags:
// bit 6 (0x40) stops the event (the message the binary sends is the one
// cTSAudioEventHandler::DoEvent, BV 0x6108a4, stops with), bit 1 (0x02)
// takes the stack object as the source when the byte is not 0xFF. |n| picks
// the code owner's table: 20000+ the semi-global one (n - 20000), 10000+ the
// global one (n - 10000), else its own; with a negative n the index in that
// table is the value of (owner, data): -1 = temp 0 in "Vox - Choose and
// Play", -20000 + the station in the stereo's attribute 11. Always true.
// Looping is the track's, not the primitive's (inferred here: flag bit 0,
// which only the smoke alarm sets and the binary does not read, or a track
// name ending in "_lp").
#include "sound.hpp"

#include "vm.hpp"

#include "formats/binreader.hpp"

namespace os2::vm {

std::string voxSuffix(const Object& sim) {
    if (!sim.person)
        return {};
    const auto& d = sim.person->data;
    const int age = d[58];
    char letter = 'a';
    if (age < 2)
        letter = 'b';
    else if (age == 2)
        letter = 'p';
    else if (age < 16)
        letter = 'c';
    else if (age < 19)
        letter = 't';
    else if (age > 50)
        letter = 'e';
    // Voices per age (thresholds 1, 2, 15, 18, 50, max; counts 1, 1, 1, 1, 2, 1).
    char voice = 'g';
    if ((d[68] & 1) == 0) {
        const int voices = age > 18 && age <= 50 ? 2 : 1;
        voice = char('a' + uint16_t(d[88]) % voices);
    }
    std::string out(1, letter);
    if (age < 3)
        return out;
    if (age >= 16)
        out += d[65] == 1 ? 'f' : 'm';
    out += voice;
    return out;
}

namespace {

constexpr uint8_t kFlagLoop = 0x01, kFlagStackObjectSource = 0x02, kFlagStop = 0x40;

std::string tableEntry(VM& vm, uint32_t group, size_t index) {
    if (!group)
        return {};
    const StringTable* t = vm.code().strings(group, kSoundIdTable);
    if (!t || index >= t->count(lang::EnglishUS))
        return {};
    return t->get(index, lang::EnglishUS);
}

Exit playStopSoundEvent(Context& c) {
    const auto& o = c.instruction.operands;
    int id = int16_t(le16(&o[0]));
    const uint8_t flags = o[4];
    if (id < 0) { // the value is the index in the table |n| picks
        const int value = c.get(o[6], int16_t(le16(&o[7])));
        const int n = -id;
        id = value >= 0 && value < 10000 ? (n >= 20000 ? 20000 : n >= 10000 ? 10000 : 0) + value : -1;
    }
    Object* source = &c.me();
    if ((flags & kFlagStackObjectSource) && flags != 0xFF)
        if (Object* s = c.vm.object(c.frame.stackObject))
            source = s;
    const std::string name = soundEventName(c.vm, c.vm.object(c.frame.codeOwner ? c.frame.codeOwner : c.thread.me),
                                            c.frame.group, id);
    if (name.empty()) {
        // The game shows a "missing sound" headline (uiHeadlineGroup_MissingSound)
        // and goes on; so does this.
        ++c.vm.missingSounds[id];
        return Exit::True;
    }
    auto& loops = c.vm.loopingSounds[source->id];
    SoundRequest r;
    r.source = source->id;
    r.event = name;
    r.flags = flags;
    r.volume = 1.0f; // the volume byte is not read
    if (flags & kFlagStop) {
        r.kind = SoundRequest::Kind::Stop;
        loops.erase(name);
        c.vm.requestSound(std::move(r));
        return Exit::True;
    }
    r.loop = (flags & kFlagLoop) || name.ends_with("_lp");
    if (r.loop && !loops.insert(name).second)
        return Exit::True; // already looping on this object
    c.vm.requestSound(std::move(r));
    return Exit::True;
}

// Stop ALL Sounds (0x30): u16 at 0, 0 my sounds, anything else the stack
// object's (Mac binary).
Exit stopAllSounds(Context& c) {
    Object* source = &c.me();
    if (le16(&c.instruction.operands[0]) != 0)
        if (Object* s = c.vm.object(c.frame.stackObject))
            source = s;
    c.vm.loopingSounds.erase(source->id);
    SoundRequest r;
    r.kind = SoundRequest::Kind::StopAll;
    r.source = source->id;
    c.vm.requestSound(std::move(r));
    return Exit::True;
}

} // namespace

std::array<std::string, 4> soundInfo(VM& vm, const Object& o) {
    std::array<std::string, 4> out;
    const StringTable* t = vm.code().strings(o.group, kSoundInfoTable);
    if (!t)
        return out;
    for (size_t i = 0; i < out.size() && i < t->count(lang::EnglishUS); ++i) {
        std::string s = t->get(i, lang::EnglishUS);
        if (s != "dest" && s != "destination" && s != "source")
            out[i] = std::move(s);
    }
    return out;
}

std::string soundEventName(VM& vm, const Object* owner, uint32_t group, int id) {
    if (id < 0)
        return {};
    const size_t index = size_t(id % 10000);
    uint32_t expected = 0;
    if (id >= 20000)
        expected = owner ? owner->semiGroup : 0;
    else if (id >= 10000)
        expected = kGlobalGroup;
    else
        expected = owner ? owner->group : 0;
    if (std::string s = tableEntry(vm, expected, index); !s.empty())
        return s;
    return tableEntry(vm, group, index);
}

void registerSoundPrimitives(VM& vm) {
    vm.setPrimitive(0x17, playStopSoundEvent);
    vm.setPrimitive(0x30, stopAllSounds);
}

} // namespace os2::vm
