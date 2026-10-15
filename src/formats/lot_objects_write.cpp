// Writing the object module back (lot_objects.hpp): the mirror of the
// reader, which follows the Mac binary's readers; cTSObject::Write,
// cEdithMTObject::Write and cTSPerson::Write write the same fields in the
// same order (docs/save-mac.md §2-§4).

#include "lot_objects.hpp"

#include "binreader.hpp"
#include "binwriter.hpp"
#include "dbpf.hpp"

#include <algorithm>
#include <map>
#include <tuple>

namespace os2 {

namespace {

constexpr uint32_t kTypeObjectModule = 0x4F626A4D, kTypeSaveTypes = 0x6F626A74, kTypeObjectState = 0x584F424A,
                   kTypeMultiTileState = 0x584D544F, kTypePersonState = 0x50455253, kTypeSimulator = 0x53494D49;

void writeInteraction(BinWriter& w, const LotObjects::SavedInteraction& x, uint32_t v) {
    w.u32(x.u08);
    w.u32(x.uid);
    w.i16(x.u0C);
    w.i16(x.stackObject);
    w.i16(x.target);
    if (v > 0xA5)
        w.i16(x.initiator);
    w.u32(x.ttabEntry);
    w.shorts(x.params);
    w.u32(x.priority);
    w.i16(x.u24);
    w.f32(x.u28);
    w.u32(x.u54);
    if (v > 0x6E)
        w.i16(x.u30);
    if (v >= 0x72) {
        w.i16(x.u2C);
        w.i16(x.u2E);
    }
    if (v >= 0x7C) {
        w.u16(x.u40);
        w.u16(x.u42);
    }
    if (v > 0x96) {
        w.str7(x.name);
        w.u32(x.u38);
        w.u32(x.u3C);
    }
    if (v > 0xBA)
        w.i16(x.u32);
}

void writeInteractions(BinWriter& w, const std::vector<LotObjects::SavedInteraction>& v, uint32_t version) {
    w.u32(uint32_t(v.size()));
    for (const auto& x : v)
        writeInteraction(w, x, version);
}

void writePerson(BinWriter& w, const LotObjects::State& s, uint32_t v) {
    w.u32(s.personTag);
    w.u32(s.personVersion);
    for (float m : s.motives)
        w.f32(m);
    for (float m : s.motives2)
        w.f32(m);
    std::vector<int16_t> data = s.personData;
    data.resize(lotPersonDataCount(v), 0);
    w.shorts(data);
    w.u32(s.personU0C);
    writeInteraction(w, s.slotA, v);
    writeInteraction(w, s.slotB, v);
    writeInteractions(w, s.queue, v);
    w.u32(uint32_t(s.objectRecords.size()));
    for (const auto& o : s.objectRecords) {
        w.i16(o.object);
        w.u32(o.value);
        w.u8(o.flag);
    }
    w.u32(uint32_t(s.motiveIncs.size()));
    for (const auto& m : s.motiveIncs) {
        w.u32(m.motive);
        w.f32(m.a);
        w.f32(m.b);
    }
    if (v > 0x9A)
        w.u16(s.personU644);
    if (v >= 0x9E)
        w.u16(s.personU646);
    if (v > 0x71) {
        w.u32(s.pending ? 1 : 0);
        if (s.pending)
            writeInteraction(w, *s.pending, v);
        writeInteractions(w, s.list5AC, v);
    }
    if (v >= 0x9D) {
        w.str7(s.personName);
        if (v > 0xA8) {
            for (uint32_t x : s.personU90C)
                w.u32(x);
            w.shorts(s.personU918);
        }
    }
    if (v > 0xB3)
        w.u32(s.personU930);
    if (v > 0xB4)
        writeInteractions(w, s.list5E0, v);
    if (v > 0xCC) {
        w.u32(s.personU938);
        w.u32(s.personU93C);
    }
}

} // namespace

std::vector<uint8_t> LotObjects::relationshipTableBytes(const std::vector<uint32_t>& keys) {
    BinWriter w;
    w.i32(3);
    w.u32(uint32_t(keys.size()));
    for (uint32_t k : keys)
        w.u32(k);
    return std::move(w.bytes());
}

std::vector<uint8_t> LotObjects::writeState(const State& s, uint32_t v) {
    BinWriter w;
    w.raw(s.fileName);
    for (float f : s.headerFloats)
        w.f32(f);
    w.f32(s.y);
    w.f32(s.x);
    w.i32(s.level);
    w.u16(s.headerOne);
    w.f32(s.elevation);
    w.i32(s.multiTileGroup);
    w.i16(s.header298);
    if (v > 0xD6)
        w.f32(s.headerD6);
    w.i16(int16_t(s.attributes.size()));
    w.shorts(s.attributes);
    w.i16(int16_t(s.semiAttributes.size()));
    w.shorts(s.semiAttributes);
    w.shorts(s.temps);
    std::vector<int16_t> data = s.data;
    data.resize(lotObjectDataCount(v), 0);
    w.shorts(data);
    if (!s.tailError.empty()) {
        w.raw(s.unparsedTail);
        return std::move(w.bytes());
    }
    w.shorts(s.tempTokenFields);
    w.u32(s.token.guid);
    w.u16(s.token.flags);
    if (v > 0xCA)
        w.raw(s.token.extra);
    w.u32(uint32_t(s.token.properties.size()));
    w.shorts(s.token.properties);
    w.i16(int16_t(s.arrays.size()));
    for (const auto& a : s.arrays) {
        w.i16(int16_t(a.size()));
        w.shorts(a);
    }
    w.i16(int16_t(s.shortList.size()));
    w.shorts(s.shortList);
    if (v == 0x8E || v == 0x8F)
        w.u32(s.extra8E);
    if (v >= 0x8F)
        w.i32(s.ownership);
    w.i16(int16_t(s.materialSubstitutes.size()));
    for (const auto& m : s.materialSubstitutes)
        w.str7(m);
    w.u16(s.persistentFlag);
    w.i32(int32_t(s.stack.size()));
    w.u32(s.stackUnknown);
    for (const auto& f : s.stack) {
        w.u16(f.objectId);
        w.u16(f.treeId);
        w.u16(f.node);
        w.u8(uint8_t(f.locals.size()));
        w.u8(uint8_t(f.params.size()));
        if (v >= 0xA5) {
            w.u16(f.runningObject);
            w.u16(f.runningOn);
        }
        w.shorts(f.params);
        w.shorts(f.locals);
        w.i32(f.primitiveState);
        w.u16(f.behaviorSaveType);
    }
    w.raw(s.relationshipTable);
    // Slots: the raw encoding, with the contents written over it when the
    // count is unchanged (each content is the last u16 of its entry).
    std::vector<uint8_t> slots = s.slotsRaw;
    if (!slots.empty()) {
        BinReader r(slots);
        std::vector<size_t> at;
        const int16_t n = r.i16();
        if (n < 0) {
            if (n == -100)
                r.u16();
            for (auto m = r.i16(); m > 0; --m)
                at.push_back(r.pos()), r.u16();
        } else {
            for (int i = 0; i < n; ++i)
                r.u16(), at.push_back(r.pos()), r.u16();
        }
        if (at.size() == s.slots.size())
            for (size_t i = 0; i < at.size(); ++i) {
                slots[at[i]] = uint8_t(s.slots[i]);
                slots[at[i] + 1] = uint8_t(uint16_t(s.slots[i]) >> 8);
            }
    }
    w.raw(slots);
    w.raw(s.effectsRaw);
    w.raw(s.overridesRaw);
    if (v > 0x95) {
        w.u32(uint32_t(s.timers.size()));
        for (const auto& t : s.timers) {
            w.u32(t.id);
            w.u32(t.tree);
            w.u16(t.object);
            w.u32(t.flags);
            w.i32(t.period);
            w.i32(t.ticksLeft);
            w.shorts(t.params);
        }
    }
    if (s.kind == Kind::MultiTile) {
        for (uint32_t x : s.multiTile)
            w.u32(x);
        w.i16(s.leadObject);
    } else if (s.kind == Kind::Person) {
        writePerson(w, s, v);
    }
    w.raw(s.rest);
    return std::move(w.bytes());
}

std::vector<uint8_t> LotObjects::writeModule() const {
    BinWriter w;
    w.raw(moduleFileName);
    w.u32(moduleZero);
    w.u32(version);
    w.u32(kTypeObjectModule);
    w.u32(uint32_t(pairs.size()));
    for (const auto& [id, saveType] : pairs) {
        w.u32(id);
        w.i32(saveType);
    }
    w.u32(uint32_t(postTable.size()));
    for (uint32_t x : postTable)
        w.u32(x);
    w.u32(postCount);
    w.u32(uint32_t(postList.size()));
    for (uint32_t x : postList)
        w.u32(x);
    w.u32(postZero);
    w.str7(postName);
    w.raw(postRest);
    return std::move(w.bytes());
}

std::vector<uint8_t> LotObjects::writeGroups() const {
    BinWriter w;
    w.raw(groupsFileName);
    w.u32(groupsVersion);
    w.u32(uint32_t(groups.size()));
    for (const auto& g : groups) {
        w.u32(g.key);
        w.u32(uint32_t(g.objects.size()));
        w.shorts(g.objects);
    }
    w.raw(groupsRest);
    return std::move(w.bytes());
}

std::vector<uint8_t> LotObjects::writeSimulator() const {
    // cTSSimulator::Write's order, with the fields the version holds.
    const Simulator& m = simulator;
    if (!m.read)
        return simulatorRaw;
    const uint32_t v = m.version;
    BinWriter w;
    w.raw(m.fileName);
    w.u32(kTypeSimulator);
    w.u32(v);
    w.u32(m.zero);
    std::vector<int16_t> g = globals;
    g.resize(simulatorGlobalCount(v), 0);
    w.shorts(g);
    w.u32(m.fA0);
    w.u16(m.f11C);
    if (v > 1)
        w.u32(m.f98);
    if (v > 0xD) {
        w.u32(m.fB4);
        w.u32(m.service);
    }
    if (v >= 0xF)
        w.u32(m.f9C);
    if (v >= 0x25) {
        w.u32(m.fA4);
        w.u32(m.fAC);
        if (v >= 0xAD)
            w.u32(m.fA8);
        w.u32(m.fB0);
    }
    if (v > 0x25)
        for (const auto& x : m.reports) {
            w.u32(x.kind);
            for (uint32_t y : x.values)
                w.u32(y);
        }
    w.raw(m.rest);
    return std::move(w.bytes());
}

std::vector<uint8_t> LotObjects::writeSaveTypes() const {
    BinWriter w;
    w.raw(saveTypesHeader);
    for (int16_t id : saveTypeOrder) {
        const SaveType& t = saveTypes.at(id);
        w.u32(t.guid);
        w.u32(t.initTreeVersion);
        w.u32(t.mainTreeVersion);
        w.u32(t.field3A);
        w.u32(t.field3B);
        w.u32(t.stringCount);
        w.i16(id);
        w.i16(t.objectType);
        w.str7(t.catalogName);
        w.u32(t.flags);
        w.u32(t.guid2);
    }
    w.u32(0);
    return std::move(w.bytes());
}

void LotObjects::save(DbpfPackage& source, const std::filesystem::path& out) const {
    if (std::filesystem::exists(out) && std::filesystem::equivalent(out, source.path()))
        throw FormatError("refusing to write a lot over its source package");
    std::map<std::pair<uint32_t, uint32_t>, const DbpfEntry*> states;
    DbpfWriter w;
    auto add = [&](const DbpfEntry& e, std::vector<uint8_t> bytes) { w.add(e.tgi, std::move(bytes), e.compressed()); };
    std::map<std::tuple<uint32_t, uint32_t, uint32_t>, const Record*> others;
    for (const auto& r : otherRecords)
        others[{r.type, r.group, r.instance}] = &r;
    for (const auto& e : source.entries()) {
        const uint32_t t = e.tgi.type;
        if (t == type::DIR)
            continue;
        if (std::any_of(deletedRecords.begin(), deletedRecords.end(), [&](const Record& r) {
                return r.type == t && r.group == e.tgi.group && r.instance == e.tgi.instance;
            }))
            continue;
        if (auto it = others.find({t, e.tgi.group, e.tgi.instance}); it != others.end()) {
            add(e, it->second->bytes);
            others.erase(it);
            continue;
        }
        if (t == kTypeObjectState || t == kTypeMultiTileState || t == kTypePersonState) {
            states[{t, e.tgi.instance & 0xFFFF}] = &e;
            continue; // written below, orphans dropped
        }
        if (t == kTypeObjectModule && e.tgi.instance == 1)
            add(e, writeModule());
        else if (t == kTypeObjectModule && e.tgi.instance == 2 && haveGroups)
            add(e, writeGroups());
        else if (t == kTypeSimulator && e.tgi.instance == 1 && !simulatorRaw.empty())
            add(e, writeSimulator());
        else if (t == kTypeSaveTypes && !saveTypesHeader.empty())
            add(e, writeSaveTypes());
        else
            w.addStored(e.tgi, source.readRaw(e), e.rawSize);
    }
    for (const auto& s : objects) {
        // The key and compression of the state it was read from; new ones
        // get the game's group (0xFFFFFFFF) and are compressed.
        Tgi tgi{resourceType(s.kind), 0xFFFFFFFF, uint32_t(uint16_t(s.id)), 0};
        bool compress = true;
        if (auto it = states.find({tgi.type, tgi.instance}); it != states.end()) {
            tgi = it->second->tgi;
            compress = it->second->compressed();
        }
        w.add(tgi, writeState(s, version), compress);
    }
    for (const auto& [key, r] : others)
        w.add(Tgi{r->type, r->group, r->instance, 0}, r->bytes, true);
    source.closeFile();
    w.save(out);
}

} // namespace os2
