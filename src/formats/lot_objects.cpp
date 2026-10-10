#include "lot_objects.hpp"

#include "binreader.hpp"
#include "dbpf.hpp"


namespace os2 {

namespace {

constexpr uint32_t kTypeObjectModule = 0x4F626A4D, kTypeSaveTypes = 0x6F626A74, kTypeObjectState = 0x584F424A,
                   kTypePersonState = 0x50455253;
constexpr size_t kFileNameSize = 64;

// Object data fields saved by each object module version (OpenTS2: 0xAD is
// the base game's, 0xD8 a later expansion's).
size_t dataCount(uint32_t version) {
    switch (version) {
    case 0xAD: return 0x58;
    case 0xD8: return 0x72;
    default: throw FormatError("object state version " + hexString(version) + " not supported");
    }
}

std::vector<int16_t> shorts(BinReader& r, size_t n) {
    std::vector<int16_t> v(n);
    for (auto& x : v)
        x = r.i16();
    return v;
}

} // namespace

LotObjects::State LotObjects::parseState(std::span<const uint8_t> bytes, uint32_t version) {
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
    s.data = shorts(r, dataCount(version));
    // Then the temporary token, object arrays, the tree stack, relationships,
    // slots, effects and timers, which are not read yet.
    return s;
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
            State s = parseState(pkg.read(e), lot.version);
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
