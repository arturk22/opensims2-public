// Play / Stop Sound Event (0x17) and Stop ALL Sounds (0x30).
//
// Operands of 0x17, from the 318 uses in the game's trees (none of the
// editor's labels name them; the disassembly strings in Sims2.exe are "Play
// Sound " / "Stop Sound ", ", volume %d", ", sampled at %f", ", auto vary",
// ", with stack obj as source"), laid out as The Sims 1's (FreeSO
// VMPlaySound): event ID s16 at 0, sample rate u16 at 2 (always 0), flags u8
// at 4, volume u8 at 5 (always 0 = full). Flags: bit 6 (0x40) stops the event
// instead of playing it ("Alarm Ring" 0x02 / "Alarm Shutoff" 0x42, "Start
// Drive Sound" 0x02 / "Stop Drive Sound" 0x42), bit 1 (0x02) takes the stack
// object as the source (The Sims 1's "StackObjAsSource"), bit 0 loops. Bytes
// 6-7 are a data owner and value: my object ID (3, 11) on the fireplace,
// literal 0 mostly; with a negative event ID the event comes from it: -1 =
// that value (temp 0 in "Vox - Choose and Play"), -N = N + the value (the
// stereo: -20000 + the station in its attribute 11) — inferred. Events whose
// track name ends in "_lp" loop (the game's naming; whether the track settings
// say so is not decoded).
#include "sound.hpp"

#include "vm.hpp"

#include "formats/binreader.hpp"

namespace os2::vm {

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
    const uint8_t flags = o[4], volume = o[5];
    if (id < 0) {
        const int value = c.get(o[6], int16_t(o[7]));
        id = id == -1 ? value : -id + value;
    }
    Object* source = &c.me();
    if (flags & kFlagStackObjectSource)
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
    r.volume = volume ? volume / 255.0f : 1.0f;
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

// Stop ALL Sounds (0x30): byte 0 is 0 or 1 (14 uses each); taken as the
// source, me (0) or the stack object (1) — inferred.
Exit stopAllSounds(Context& c) {
    Object* source = &c.me();
    if (c.instruction.operands[0] == 1)
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
