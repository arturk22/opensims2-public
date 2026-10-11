#include "lot_objects.hpp"

#include "binreader.hpp"
#include "dbpf.hpp"
#include "neighborhood.hpp"

#include <algorithm>
#include <cstring>

namespace os2 {

namespace {

constexpr uint32_t kTypeObjectModule = 0x4F626A4D, kTypeSaveTypes = 0x6F626A74, kTypeObjectState = 0x584F424A,
                   kTypeMultiTileState = 0x584D544F, kTypePersonState = 0x50455253;
constexpr size_t kFileNameSize = 64;
constexpr uint32_t kTypeSimulator = 0x53494D49;
constexpr size_t kGlobalCount = 54;
constexpr uint32_t kAnimatorTag = 0xDEADBEEF;
constexpr int16_t kPersonObjectType = 2;

std::vector<int16_t> shorts(BinReader& r, size_t n) {
    std::vector<int16_t> v(n);
    for (auto& x : v)
        x = r.i16();
    return v;
}

std::vector<uint8_t> copyOf(std::span<const uint8_t> b) { return {b.begin(), b.end()}; }

// Bytes the reader moved over since `from`.
std::vector<uint8_t> since(const BinReader& r, size_t from) { return copyOf(r.data().subspan(from, r.pos() - from)); }

template <size_t N>
void fixedBytes(BinReader& r, std::array<uint8_t, N>& out) {
    const auto b = r.bytes(N);
    std::copy(b.begin(), b.end(), out.begin());
}

// cTSInteraction::Read.
LotObjects::SavedInteraction readInteraction(BinReader& r, uint32_t v) {
    LotObjects::SavedInteraction x;
    x.u08 = r.u32();
    x.uid = r.u32();
    x.u0C = r.i16();
    x.stackObject = r.i16();
    x.target = r.i16();
    if (v > 0xA5)
        x.initiator = r.i16();
    x.ttabEntry = r.u32();
    for (auto& p : x.params)
        p = r.i16();
    x.priority = r.u32();
    x.u24 = r.i16();
    x.u28 = r.f32();
    if (v < 0x3E)
        throw FormatError("interaction of version " + hexString(v) + " not supported");
    x.u54 = r.u32();
    if (v > 0x6E)
        x.u30 = r.i16();
    if (v >= 0x72) {
        x.u2C = r.i16();
        x.u2E = r.i16();
    }
    if (v >= 0x7C) {
        x.u40 = r.u16();
        x.u42 = r.u16();
    }
    if (v > 0x96) {
        x.name = r.str7();
        x.u38 = r.u32();
        x.u3C = r.u32();
    }
    if (v > 0xBA)
        x.u32 = r.i16();
    return x;
}

std::vector<LotObjects::SavedInteraction> readInteractions(BinReader& r, uint32_t v) {
    const uint32_t n = r.u32();
    if (n > 256)
        throw FormatError(std::to_string(n) + " interactions");
    std::vector<LotObjects::SavedInteraction> out;
    for (uint32_t i = 0; i < n; ++i)
        out.push_back(readInteraction(r, v));
    return out;
}

// cTSPerson::Read after the cTSObject.
void parsePerson(BinReader& r, uint32_t v, LotObjects::State& s) {
    if (v < 0x3E)
        throw FormatError("person of version " + hexString(v) + " not supported");
    // ReadSAnimator: from 0x3E a tag and a version; version 2 and later
    // saves nothing else (older animators are not read here).
    s.personTag = r.u32();
    if (s.personTag != kAnimatorTag)
        throw FormatError("animator without its tag");
    s.personVersion = r.u32();
    if (s.personVersion <= 1)
        throw FormatError("animator version " + std::to_string(s.personVersion) + " not supported");
    for (auto& m : s.motives)
        m = r.f32();
    for (auto& m : s.motives2)
        m = r.f32();
    s.personData = shorts(r, lotPersonDataCount(v));
    s.personRead = true;
    s.personU0C = r.u32();
    s.slotA = readInteraction(r, v);
    s.slotB = readInteraction(r, v);
    s.queue = readInteractions(r, v);
    for (uint32_t n = r.u32(); n; --n) {
        LotObjects::State::ObjectRecord o;
        o.object = r.i16();
        o.value = r.u32();
        o.flag = r.u8();
        s.objectRecords.push_back(o);
    }
    for (uint32_t n = r.u32(); n; --n) {
        LotObjects::State::MotiveInc m;
        m.motive = r.u32();
        m.a = r.f32();
        m.b = r.f32();
        s.motiveIncs.push_back(m);
    }
    if (v > 0x9A)
        s.personU644 = r.u16();
    if (v >= 0x9E)
        s.personU646 = r.u16();
    if (v > 0x71) {
        if (r.u32())
            s.pending = readInteraction(r, v);
        s.list5AC = readInteractions(r, v);
    }
    if (v >= 0x9D) {
        s.personName = r.str7();
        if (v > 0xA8) {
            for (auto& x : s.personU90C)
                x = r.u32();
            for (auto& x : s.personU918)
                x = r.i16();
        }
    }
    if (v > 0xB3)
        s.personU930 = r.u32();
    if (v > 0xB4)
        s.list5E0 = readInteractions(r, v);
    if (v > 0xCC) {
        s.personU938 = r.u32();
        s.personU93C = r.u32();
    }
}

} // namespace

size_t lotObjectDataCount(uint32_t v) {
    // cTSObject::Read: each step holds while the version is past it.
    size_t n = 72;
    if (v <= 0x7C) return n;
    n = 78;
    if (v == 0x7D) return n;
    n = 79;
    if (v <= 0x85) return n;
    n = 80;
    if (v <= 0x88) return n;
    n = 83;
    if (v <= 0x90) return n;
    n = 84;
    if (v <= 0x93) return n;
    n = 85;
    if (v <= 0x97) return n;
    n = 86;
    if (v == 0x98) return n;
    n = 87;
    if (v <= 0xA2) return n;
    n = 88;
    if (v <= 0xB1) return n;
    n = 98;
    if (v == 0xB2) return n;
    n = 99;
    if (v <= 0xB5) return n;
    n = 101;
    if (v <= 0xBA) return n;
    n = 102;
    if (v == 0xBB) return n;
    n = 103;
    if (v <= 0xBE) return n;
    n = 104;
    if (v == 0xBF) return n;
    n = 108;
    if (v <= 0xC5) return n;
    n = 111;
    if (v <= 0xC7) return n;
    return v == 0xC8 ? 112 : 113;
}

size_t lotPersonDataCount(uint32_t v) {
    // cTSPerson::Read.
    if (v >= 0xCE) return 0xCC;
    if (v >= 0xCC) return 0xC9;
    if (v >= 0xC7) return 200;
    if (v >= 0xC3) return 0xC7;
    if (v >= 0xBD) return 0xC3;
    if (v == 0xBC) return 0xC2;
    if (v >= 0xBA) return 0xC1;
    if (v == 0xB9) return 0xC0;
    if (v >= 0xB8) return 0xBD;
    if (v == 0xB7) return 0xBA;
    if (v >= 0xB3) return 0xB4;
    if (v >= 0xB0) return 0xB3;
    if (v >= 0xAE) return 0xB2;
    if (v >= 0xA8) return 0xAA;
    if (v >= 0xA2) return 0xA7;
    if (v >= 0xA0) return 0xA6;
    if (v == 0x9F) return 0xA5;
    if (v >= 0x9C) return 0xA3;
    if (v >= 0x9A) return 0xA2;
    if (v == 0x99) return 0xA1;
    if (v >= 0x97) return 0x9E;
    if (v >= 0x8B) return 0x9D;
    if (v == 0x8A) return 0x90;
    if (v >= 0x88) return 0x7C;
    if (v >= 0x85) return 0x7B;
    if (v >= 0x83) return 0x7A;
    if (v == 0x82) return 0x79;
    if (v >= 0x80) return 0x77;
    if (v == 0x7F) return 0x76;
    if (v >= 0x7D) return 0x70;
    if (v == 0x7C) return 0x6F;
    if (v == 0x7B) return 0x6B;
    if (v >= 0x78) return 0x68;
    if (v == 0x77) return 0x66;
    if (v >= 0x75) return 0x5C;
    if (v >= 0x73) return 0x5B;
    if (v >= 0x6E) return 0x59;
    if (v == 0x6D) return 0x58;
    if (v >= 0x6C) return 0x53;
    if (v >= 0x6A) return 0x51;
    if (v >= 0x29) return 0x50;
    return 0x40;
}

uint32_t LotObjects::resourceType(Kind k) {
    switch (k) {
    case Kind::MultiTile: return kTypeMultiTileState;
    case Kind::Person: return kTypePersonState;
    default: return kTypeObjectState;
    }
}

LotObjects::State LotObjects::parseState(std::span<const uint8_t> bytes, uint32_t v, Kind kind) {
    BinReader r(bytes);
    State s;
    s.kind = kind;
    s.person = kind == Kind::Person;
    if (v < 0x94)
        throw FormatError("object state version " + hexString(v) + " not supported");
    fixedBytes(r, s.fileName);
    // cTSObject::ReadHeader (>= 0x86).
    for (auto& f : s.headerFloats)
        f = r.f32();
    s.y = r.f32();
    s.x = r.f32();
    s.level = r.i32();
    s.headerOne = r.u16();
    s.elevation = r.f32();
    s.multiTileGroup = r.i32();
    s.header298 = r.i16();
    if (v > 0xD6)
        s.headerD6 = r.f32();
    s.attributes = shorts(r, uint16_t(r.i16()));
    s.semiAttributes = shorts(r, uint16_t(r.i16()));
    for (auto& t : s.temps)
        t = r.i16();
    s.data = shorts(r, lotObjectDataCount(v));
    const size_t tailStart = r.pos();
    try {
        for (auto& t : s.tempTokenFields)
            t = r.i16();
        s.token.guid = r.u32();
        s.token.flags = r.u16();
        if (v > 0xCA)
            fixedBytes(r, s.token.extra);
        s.token.properties = shorts(r, r.u32());
        for (auto n = r.i16(); n > 0; --n)
            s.arrays.push_back(shorts(r, uint16_t(r.i16())));
        s.shortList = shorts(r, uint16_t(r.i16()));
        if (v == 0x8E || v == 0x8F)
            s.extra8E = r.u32();
        if (v >= 0x8F)
            s.ownership = r.i32();
        for (auto n = r.i16(); n > 0; --n)
            s.materialSubstitutes.push_back(r.str7());
        s.persistentFlag = r.u16();
        // cTSTreeStack::Read.
        const int32_t frames = r.i32();
        if (frames < 0 || frames > 1024)
            throw FormatError("tree stack of " + std::to_string(frames) + " frames");
        s.stackUnknown = r.u32();
        for (int32_t i = 0; i < frames; ++i) {
            State::Frame f;
            f.objectId = r.u16();
            f.treeId = r.u16();
            f.node = r.u16();
            const uint8_t locals = r.u8(), params = r.u8();
            if (v >= 0xA5) {
                f.runningObject = r.u16();
                f.runningOn = r.u16();
            }
            f.params = shorts(r, params);
            f.locals = shorts(r, locals);
            f.primitiveState = r.i32();
            f.behaviorSaveType = r.u16();
            s.stack.push_back(std::move(f));
        }
        // Relationship table (OpenTS2's layout).
        size_t from = r.pos();
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
        s.relationshipTable = since(r, from);
        // cTSObject::ReadSlots.
        from = r.pos();
        const int16_t slots = r.i16();
        if (slots < 0) {
            if (slots == -100)
                r.u16();
            for (auto n = r.i16(); n > 0; --n)
                s.slots.push_back(r.i16());
        } else {
            if (slots > 0) {
                r.u16();
                s.slots.push_back(r.i16());
            }
            for (int i = 0; i < slots - 1; ++i) {
                r.u16();
                s.slots.push_back(r.i16());
            }
        }
        s.slotsRaw = since(r, from);
        // cTSObject::ReadEffects (>= 0x93).
        from = r.pos();
        if (r.i16() != 0 && r.u16() == 1)
            for (auto n = r.u32(); n; --n) {
                r.str7();
                r.str7();
                r.skip(8);
            }
        s.effectsRaw = since(r, from);
        from = r.pos();
        if (v > 0x94)
            for (auto n = r.i16(); n > 0; --n) {
                r.str7();
                r.str7();
                r.str7();
            }
        s.overridesRaw = since(r, from);
        if (v > 0x95) {
            const uint32_t timers = r.u32();
            if (timers > 1024)
                throw FormatError(std::to_string(timers) + " timers");
            for (uint32_t i = 0; i < timers; ++i) {
                State::Timer t;
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
        }
        if (kind == Kind::MultiTile) {
            for (auto& x : s.multiTile)
                x = r.u32();
            s.leadObject = r.i16();
        } else if (kind == Kind::Person) {
            parsePerson(r, v, s);
        }
        if (!r.atEnd())
            s.tailError = std::to_string(r.remaining()) + " bytes after the state";
    } catch (const FormatError& e) {
        s.tailError = e.what();
    }
    s.rest = copyOf(r.data().subspan(r.pos()));
    if (!s.tailError.empty())
        s.unparsedTail = copyOf(r.data().subspan(tailStart));
    return s;
}

uint32_t LotObjects::guidOf(int16_t id) const {
    const SaveType* t = saveTypeOfObject(id);
    return t ? t->guid : 0;
}

const LotObjects::SaveType* LotObjects::saveTypeOfObject(int16_t id) const {
    auto st = saveTypeOf.find(id);
    if (st == saveTypeOf.end())
        return nullptr;
    auto t = saveTypes.find(st->second);
    return t == saveTypes.end() ? nullptr : &t->second;
}

const LotObjects::State* LotObjects::state(int16_t id) const {
    auto it = std::lower_bound(objects.begin(), objects.end(), id,
                               [](const State& s, int16_t i) { return s.id < i; });
    return it != objects.end() && it->id == id ? &*it : nullptr;
}

std::map<uint32_t, LotObjects::SaveType> LotObjects::loadSaveTypes(DbpfPackage& pkg) {
    std::map<uint32_t, SaveType> out;
    for (const auto& e : pkg.entries()) {
        if (e.tgi.type != kTypeSaveTypes)
            continue;
        const auto bytes = pkg.read(e);
        BinReader r(bytes);
        r.skip(kFileNameSize);
        if (r.u32() != kTypeSaveTypes || r.u32() < 0x49)
            continue;
        r.u32();
        try {
            for (;;) {
                SaveType t;
                t.guid = r.u32();
                if (!t.guid)
                    break;
                fixedBytes(r, t.unknown);
                r.i16(); // the save type: this package's number
                t.objectType = r.i16();
                t.catalogName = r.str7();
                t.flags = r.u32();
                t.guid2 = r.u32();
                out.emplace(t.guid, std::move(t));
            }
        } catch (const FormatError&) {
        }
    }
    return out;
}

LotObjects LotObjects::load(DbpfPackage& pkg) {
    LotObjects lot;
    bool haveModule = false;
    for (const auto& e : pkg.entries()) {
        if (e.tgi.type == kTypeObjectModule && e.tgi.instance == 1) {
            const auto bytes = pkg.read(e);
            BinReader r(bytes);
            fixedBytes(r, lot.moduleFileName);
            lot.moduleZero = r.u32();
            lot.version = r.u32();
            if (r.u32() != kTypeObjectModule)
                throw FormatError("object module without its \"ObjM\" tag");
            for (uint32_t n = r.u32(); n; --n) {
                const uint32_t id = r.u32();
                const int32_t saveType = r.i32();
                lot.pairs.emplace_back(id, saveType);
                lot.saveTypeOf[int16_t(id)] = int16_t(saveType);
            }
            // read_post_object.
            try {
                const uint32_t n = r.u32();
                for (uint32_t i = 0; i < n; ++i)
                    lot.postTable.push_back(r.u32());
                lot.postCount = r.u32();
                for (uint32_t m = r.u32(); m; --m)
                    lot.postList.push_back(r.u32());
                lot.postZero = r.u32();
                lot.postName = r.str7();
            } catch (const FormatError& ex) {
                lot.warnings.push_back(std::string("object module, after the pairs: ") + ex.what());
            }
            lot.postRest = copyOf(r.data().subspan(r.pos()));
            haveModule = true;
        } else if (e.tgi.type == kTypeObjectModule && e.tgi.instance == 2) {
            const auto bytes = pkg.read(e);
            BinReader r(bytes);
            try {
                fixedBytes(r, lot.groupsFileName);
                lot.groupsVersion = r.u32();
                for (uint32_t n = r.u32(); n; --n) {
                    Group g;
                    g.key = r.u32();
                    g.objects = shorts(r, r.u32());
                    lot.groups.push_back(std::move(g));
                }
                lot.haveGroups = true;
            } catch (const FormatError& ex) {
                lot.warnings.push_back(std::string("object groups: ") + ex.what());
            }
            lot.groupsRest = copyOf(r.data().subspan(r.pos()));
        } else if (e.tgi.type == kTypeSimulator && e.tgi.instance == 1) {
            const auto bytes = pkg.read(e);
            lot.simulatorRaw = bytes;
            BinReader r(bytes);
            r.skip(kFileNameSize);
            if (r.u32() != kTypeSimulator)
                throw FormatError("simulator state without its \"SIMI\" tag");
            r.u32(); // version (the object module's)
            r.u32();
            lot.globals = shorts(r, kGlobalCount);
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
            lot.saveTypesHeader = copyOf(r.data().subspan(0, r.pos()));
            for (;;) {
                SaveType t;
                t.guid = r.u32();
                if (!t.guid)
                    break;
                fixedBytes(r, t.unknown);
                const int16_t saveType = r.i16();
                t.objectType = r.i16();
                t.catalogName = r.str7();
                t.flags = r.u32();
                t.guid2 = r.u32();
                lot.saveTypeOrder.push_back(saveType);
                lot.saveTypes[saveType] = std::move(t);
            }
        }
    }
    if (!haveModule)
        return lot; // an empty lot
    // The state of each object of the module, under its class's type.
    std::map<int16_t, State> states;
    std::map<std::pair<uint32_t, uint32_t>, const DbpfEntry*> byKey;
    for (const auto& e : pkg.entries())
        if (e.tgi.type == kTypeObjectState || e.tgi.type == kTypeMultiTileState || e.tgi.type == kTypePersonState)
            byKey[{e.tgi.type, e.tgi.instance & 0xFFFF}] = &e;
    std::map<std::pair<uint32_t, uint32_t>, bool> used;
    for (const auto& [rawId, saveType] : lot.pairs) {
        const auto id = int16_t(rawId);
        const SaveType* t = lot.saveTypeOfObject(id);
        Kind kind = Kind::Object;
        if (t && t->objectType == kPersonObjectType)
            kind = Kind::Person;
        else if (t && (t->flags & 1))
            kind = Kind::MultiTile;
        auto it = byKey.find({resourceType(kind), uint16_t(id)});
        if (it == byKey.end()) {
            lot.warnings.push_back("object " + std::to_string(id) + ": no " +
                                   (kind == Kind::Person ? "PERS" : kind == Kind::MultiTile ? "XMTO" : "XOBJ") +
                                   " state");
            continue;
        }
        used[it->first] = true;
        try {
            State s = parseState(pkg.read(*it->second), lot.version, kind);
            if (s.data.size() <= 11 || s.data[11] != id)
                lot.warnings.push_back("object " + std::to_string(id) + ": object ID field " +
                                       std::to_string(s.data.size() > 11 ? s.data[11] : 0) + " is not the instance");
            s.id = id;
            states[id] = std::move(s);
        } catch (const std::exception& ex) {
            lot.warnings.push_back("object " + std::to_string(id) + ": " + ex.what());
        }
    }
    for (const auto& [key, _] : byKey)
        if (!used.count(key))
            ++lot.orphans[key.first];
    for (auto& [_, s] : states)
        lot.objects.push_back(std::move(s));
    for (const auto& e : pkg.entries()) {
        if (e.tgi.type != hoodtype::SREL)
            continue;
        try {
            const auto rel = hood::Relationship::parse(pkg.read(e), e.tgi.instance);
            lot.relationships.push_back({int16_t(rel.from), int16_t(rel.to), rel.values});
        } catch (const std::exception& ex) {
            lot.warnings.push_back("relationship " + hexString(e.tgi.instance) + ": " + ex.what());
        }
    }
    return lot;
}

} // namespace os2
