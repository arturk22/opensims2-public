#pragma once

// Sound primitives: Play / Stop Sound Event (0x17) and Stop ALL Sounds
// (0x30). The VM does not play anything: it resolves the event to its name
// (STR# 0x4132 "Sound ID Table") and leaves a SoundRequest for the host
// (VM::soundRequests), as it does with engine requests.

#include <array>
#include <cstdint>
#include <string>

namespace os2::vm {

class VM;
struct Object;

// STR# of sound event names in an object's, semi-global or the global group.
constexpr uint16_t kSoundIdTable = 0x4132;
// STR# "soundinfo" of an object: 0 the surface it is (or is put on: "wood",
// "cloth", "marble"; "dest"/"destination" = none), 1 the surface it gives a
// held object ("plate"; "source" = none), 2 its quality ("exp", "chp",
// "wdglass", ...), 3 its ambience partition ("flora", "object:tombstone").
constexpr uint16_t kSoundInfoTable = 0x94;

// The four soundinfo strings of an object (empty where it has none, and for
// the "dest"/"destination"/"source" placeholders).
std::array<std::string, 4> soundInfo(VM& vm, const Object& o);

// The event name of sound ID `id` for code owned by `owner`, running in
// `group`: IDs below 10000 index the owner's own table, 10000-19999 the
// global group's, 20000 and up the owner's semi-global table (inferred from
// which table holds the entry in 312 of the 318 uses; the running tree's
// group is tried when the expected table lacks it). Empty if none.
std::string soundEventName(VM& vm, const Object* owner, uint32_t group, int id);

// Registers 0x17 and 0x30.
void registerSoundPrimitives(VM& vm);

} // namespace os2::vm
