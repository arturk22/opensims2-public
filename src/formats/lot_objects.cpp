#include "lot_objects.hpp"

#include "binreader.hpp"
#include "dbpf.hpp"


namespace os2 {

namespace {

constexpr uint32_t kTypeObjectModule = 0x4F626A4D, kTypeSaveTypes = 0x6F626A74, kTypeObjectState = 0x584F424A,
                   kTypePersonState = 0x50455253;
constexpr size_t kFileNameSize = 64;

// Object data fields saved by each object module version (OpenTS2: 0xAD is
// the base game's, 0xD8 a later expansion's). The game's older lots (0xA4 in
// CAS!, 0xAC in the tutorial's) have the same layout up to the object data;
// 88 fields are read from them too (their object ID field is the instance in
// every state), though how many they save is not known.
size_t dataCount(uint32_t version) {
    if (version >= 0xA4 && version <= 0xAD)
        return 0x58;
    if (version == 0xD8)
        return 0x72;
    throw FormatError("object state version " + hexString(version) + " not supported");
}

std::vector<int16_t> shorts(BinReader& r, size_t n) {
    std::vector<int16_t> v(n);
    for (auto& x : v)
        x = r.i16();
    return v;
}

// After the object data (OpenTS2's SimsObjectCodec, checked on the game's
// lots: every XOBJ ends right after its timers).
// `runningFields`: the frames carry the objects they run as and on (the
// current layout); stale states written by an older build lack them.
void parseTail(BinReader& r, uint32_t version, LotObjects::State& s, bool runningFields) {
    for (auto& t : s.tempTokenFields)
        t = r.i16();
    s.token.guid = r.u32();
    s.token.flags = r.u16();
    if (version > 0xCA)
        r.skip(2 + 4 + 2); // stack ID, personal inventory instance, category
    s.token.properties = shorts(r, r.u32());
    for (auto n = r.i16(); n > 0; --n)
        s.arrays.push_back(shorts(r, uint16_t(r.i16())));
    shorts(r, uint16_t(r.i16())); // not understood
    s.ownership = r.i32();
    for (auto n = r.i16(); n > 0; --n)
        s.materialSubstitutes.push_back(r.str7());
    r.u16(); // persistent flag
    const int32_t frames = r.i32();
    if (frames < 0 || frames > 1024)
        throw FormatError("tree stack of " + std::to_string(frames) + " frames");
    r.u32();
    for (int32_t i = 0; i < frames; ++i) {
        LotObjects::State::Frame f;
        f.objectId = r.u16();
        f.treeId = r.u16();
        f.node = r.u16();
        const uint8_t locals = r.u8(), params = r.u8();
        if (runningFields) {
            f.runningObject = r.u16();
            f.runningOn = r.u16();
        }
        f.params = shorts(r, params);
        f.locals = shorts(r, locals);
        f.primitiveState = r.i32();
        f.behaviorSaveType = r.u16();
        s.stack.push_back(std::move(f));
    }
    // Relationship table.
    const int32_t relFlag = r.i32();
    if (relFlag < 0) {
        for (auto n = r.i32(); n > 0; --n) {
            if (!r.i32())
                continue;
            r.u32();
            r.skip(4 * size_t(uint32_t(r.i32())));
        }
    } else if (relFlag == 2 || relFlag == 3) {
        r.skip(4 * size_t(uint32_t(r.i32())));
    }
    // Slots.
    const int16_t slots = r.i16();
    if (slots < 0) {
        if (slots == -100)
            r.u16();
        r.skip(2 * size_t(uint16_t(r.i16())));
    } else {
        if (slots > 0)
            r.skip(4);
        for (int i = 0; i < slots - 1; ++i)
            r.skip(4);
    }
    // Effects.
    if (r.i16() != 0 && r.u16() == 1)
        for (auto n = r.u32(); n; --n) {
            r.str7();
            r.str7();
            r.skip(8);
        }
    for (auto n = r.i16(); n > 0; --n) { // overrides
        r.str7();
        r.str7();
        r.str7();
    }
    const uint32_t timers = r.u32();
    if (timers > 1024)
        throw FormatError(std::to_string(timers) + " timers");
    for (uint32_t i = 0; i < timers; ++i) {
        LotObjects::State::Timer t;
        t.a = r.u32();
        t.b = r.u32();
        t.c = r.u16();
        t.d = r.u32();
        t.e = r.i32();
        t.f = r.i32();
        for (auto& p : t.params)
            p = r.i16();
        s.timers.push_back(t);
    }
    const auto rest = r.bytes(r.remaining());
    s.rest.assign(rest.begin(), rest.end());
}

} // namespace

LotObjects::State LotObjects::parseState(std::span<const uint8_t> bytes, uint32_t version, bool person) {
    BinReader r(bytes);
    r.skip(kFileNameSize);
    r.skip(16); // four floats, zero in the game's lots
    State s;
    s.y = r.f32();
    s.x = r.f32();
    s.level = r.i32();
    r.u16(); // 1
    s.elevation = r.f32();
    s.multiTileGroup = r.i32();
    r.i16();
    if (version > 0xD6)
        r.f32();
    const auto attributes = uint16_t(r.i16());
    s.attributes = shorts(r, attributes);
    const auto semiAttributes = uint16_t(r.i16());
    s.semiAttributes = shorts(r, semiAttributes);
    for (auto& t : s.temps)
        t = r.i16();
    // Two layouts after the attributes, told apart by where they end: the
    // current one (the object data's 88 fields, frames with the objects they
    // run as/on) and an older one (87 fields, frames without them) that some
    // states keep. In N001_Lot11, 566 of 745 XOBJs end exactly with the first
    // layout and 85 with the second; most of the older ones belong to objects
    // the object module does not list (stale states). A PERS goes on after its
    // timers, so the first layout that reads through is taken.
    const size_t dataStart = r.pos();
    const size_t count = dataCount(version);
    std::string firstError;
    State best;
    bool found = false;
    for (int layout = 0; layout < 2 && !found; ++layout) {
        State t = s;
        BinReader q(bytes);
        q.seek(dataStart);
        t.data = shorts(q, count - size_t(layout));
        if (layout)
            t.data.push_back(0);
        try {
            parseTail(q, version, t, layout == 0);
            if (t.rest.empty() || person) {
                best = std::move(t);
                found = true;
            } else if (firstError.empty()) {
                firstError = std::to_string(t.rest.size()) + " bytes after the timers";
                best = std::move(t);
            }
        } catch (const FormatError& e) {
            if (firstError.empty()) {
                firstError = e.what();
                best = std::move(t);
            }
        }
    }
    if (!found)
        best.tailError = firstError;
    return best;
}

uint32_t LotObjects::guidOf(int16_t id) const {
    auto st = saveTypeOf.find(id);
    if (st == saveTypeOf.end())
        return 0;
    auto t = saveTypes.find(st->second);
    return t == saveTypes.end() ? 0 : t->second.guid;
}

LotObjects LotObjects::load(DbpfPackage& pkg) {
    LotObjects lot;
    bool haveModule = false;
    for (const auto& e : pkg.entries()) {
        if (e.tgi.type == kTypeObjectModule && e.tgi.instance == 1) {
            const auto bytes = pkg.read(e);
            BinReader r(bytes);
            r.skip(kFileNameSize + 4);
            lot.version = r.u32();
            if (r.u32() != kTypeObjectModule)
                throw FormatError("object module without its \"ObjM\" tag");
            for (uint32_t n = r.u32(); n; --n) {
                const auto id = int16_t(r.i32());
                lot.saveTypeOf[id] = int16_t(r.i32());
            }
            haveModule = true;
        } else if (e.tgi.type == kTypeSaveTypes) {
            const auto bytes = pkg.read(e);
            BinReader r(bytes);
            r.skip(kFileNameSize);
            if (r.u32() != kTypeSaveTypes)
                throw FormatError("save type table without its \"objt\" tag");
            const uint32_t version = r.u32();
            if (version < 0x49)
                throw FormatError("save type table version " + hexString(version) + " not supported");
            r.u32();
            for (;;) {
                SaveType t;
                t.guid = r.u32();
                if (!t.guid)
                    break;
                r.skip(20); // init/main tree versions, attribute and array counts, a string set count
                const int16_t saveType = r.i16();
                r.i16(); // interaction table ID
                t.catalogName = r.str7();
                t.flags = r.u32();
                r.u32();
                lot.saveTypes[saveType] = std::move(t);
            }
        }
    }
    if (!haveModule)
        return lot; // an empty lot
    // A Sim has both: its cTSObject (XOBJ) and its cTSPerson (PERS), which is kept.
    std::map<int16_t, State> states;
    for (const auto& e : pkg.entries()) {
        if (e.tgi.type != kTypeObjectState && e.tgi.type != kTypePersonState)
            continue;
        const auto id = int16_t(e.tgi.instance);
        const bool person = e.tgi.type == kTypePersonState;
        if (auto it = states.find(id); it != states.end() && it->second.person)
            continue;
        try {
            State s = parseState(pkg.read(e), lot.version, person);
            if (s.data.size() <= 11 || s.data[11] != id)
                throw FormatError("object ID field " + std::to_string(s.data.size() > 11 ? s.data[11] : 0) +
                                  " is not the instance");
            s.id = id;
            s.person = person;
            states[id] = std::move(s);
        } catch (const std::exception& ex) {
            lot.warnings.push_back(std::string(person ? "person" : "object") + " state " + hexString(e.tgi.instance) +
                                   ": " + ex.what());
        }
    }
    for (auto& [_, s] : states)
        lot.objects.push_back(std::move(s));
    return lot;
}

} // namespace os2
