#include "simantics.hpp"

#include <bit>

#include "binreader.hpp"

namespace os2 {

namespace {

constexpr size_t kNameSize = 64;
constexpr uint32_t kMaxCount = 0x10000;

uint32_t count(BinReader& r, const char* what) {
    const uint32_t n = r.u32();
    if (n > kMaxCount)
        throw FormatError(std::string("bad ") + what + " count");
    return n;
}

void expectTag(BinReader& r, const char tag[5], const char* what) {
    const auto at = r.pos();
    const auto b = r.bytes(4);
    if (std::string(reinterpret_cast<const char*>(b.data()), 4) != tag)
        throw FormatError(std::string(what) + ": missing '" + tag + "' at offset " + std::to_string(at));
}

} // namespace

// ---------------------------------------------------------------- OBJD

ObjectDefinition ObjectDefinition::parse(std::span<const uint8_t> data) {
    BinReader r(data);
    ObjectDefinition o;
    o.fileName = r.fixed(kNameSize);
    o.version = r.u32();
    // Fields run up to the title at the end of the resource: u32 length +
    // characters, or u8 length + characters in some objects. Find the split
    // whose length matches exactly and whose title is printable text.
    const size_t start = r.pos();
    auto printable = [&](size_t from) {
        for (size_t i = from; i < data.size(); ++i)
            if (data[i] < 0x20)
                return false;
        return true;
    };
    for (size_t prefix : {4u, 1u})
        for (size_t end = start; end + prefix <= data.size(); end += 2) {
            const size_t len = prefix == 4 ? le32(&data[end]) : data[end];
            if (end + prefix + len != data.size() || !printable(end + prefix))
                continue;
            o.fields.resize((end - start) / 2);
            for (auto& f : o.fields)
                f = r.u16();
            r.skip(prefix);
            o.title = std::string(reinterpret_cast<const char*>(&data[end + prefix]), len);
            return o;
        }
    throw FormatError("OBJD: cannot locate the title");
}

// ---------------------------------------------------------------- OBJf

ObjectFunctions ObjectFunctions::parse(std::span<const uint8_t> data) {
    BinReader r(data);
    ObjectFunctions f;
    f.fileName = r.fixed(kNameSize);
    // Normally two u32 then the tag; a few resources have extra zero padding before it.
    const std::string_view bytes(reinterpret_cast<const char*>(data.data()), data.size());
    const size_t tag = bytes.find("fJBO", kNameSize);
    if (tag == std::string_view::npos || tag < kNameSize + 8)
        throw FormatError("OBJf: missing 'fJBO'");
    r.seek(tag - 8);
    f.unknown1 = r.u32();
    f.unknown2 = r.u32();
    r.skip(4);
    f.functions.resize(count(r, "OBJf function"));
    for (auto& e : f.functions) {
        e.guard = r.u16();
        e.action = r.u16();
    }
    r.expectEnd("OBJf");
    return f;
}

const char* ObjectFunctions::functionName(size_t i) {
    static const char* const names[] = {
        "init",          "main",          "load",          "cleanup",         "queueSkipped",
        "allowIntersect", "wallAdjacency", "roomChanged",  "dynMultiTile",    "placement",
        "pickup",        "userPlacement", "userPickup",    "levelInfo",       "servingSurface",
        "portal",        "gardening",     "washHands",     "prepareFood",     "cookFood",
        "placeOnSurface", "dispose",      "eat",           "pickupFromSlot",  "washDish",
        "eatingSurface", "sit",           "stand",
    };
    return i < std::size(names) ? names[i] : "";
}

// ---------------------------------------------------------------- BCON

Constants Constants::parse(std::span<const uint8_t> data) {
    BinReader r(data);
    Constants c;
    c.fileName = r.fixed(kNameSize);
    // u16 count; bit 15 is a flag.
    const uint16_t header = r.u16();
    c.values.resize(header & 0x7FFF);
    c.flags = uint8_t(header >> 15);
    for (auto& v : c.values)
        v = r.i16();
    r.expectEndOrFill("BCON");
    return c;
}

// ---------------------------------------------------------------- GLOB

SemiGlobal SemiGlobal::parse(std::span<const uint8_t> data) {
    BinReader r(data);
    SemiGlobal g;
    g.fileName = r.fixed(kNameSize);
    // Pascal string, or a C string in some resources.
    if (!r.atEnd() && data[r.pos()] < 0x20)
        g.semiGlobalName = r.str8();
    else
        g.semiGlobalName = r.cstr();
    r.expectEndOrFill("GLOB");
    return g;
}

// ---------------------------------------------------------------- TPRP

BhavLabels BhavLabels::parse(std::span<const uint8_t> data) {
    BinReader r(data);
    BhavLabels t;
    t.fileName = r.fixed(kNameSize);
    expectTag(r, "PRPT", "TPRP");
    t.version = r.u32();
    t.unknown[0] = r.u32();
    t.params.resize(count(r, "TPRP param"));
    t.locals.resize(count(r, "TPRP local"));
    for (auto& p : t.params)
        p = r.str8();
    for (auto& l : t.locals)
        l = r.str8();
    t.unknown[1] = r.u32();
    t.paramFlags.resize(t.params.size());
    for (auto& f : t.paramFlags)
        f = r.u8();
    t.unknown[2] = r.u32();
    r.u32();
    r.expectEnd("TPRP");
    return t;
}

// ---------------------------------------------------------------- SLOT

Slots Slots::parse(std::span<const uint8_t> data) {
    BinReader r(data);
    Slots s;
    s.fileName = r.fixed(kNameSize);
    uint32_t n;
    if (data.size() >= kNameSize + 4 && std::string(reinterpret_cast<const char*>(&data[kNameSize]), 4) == "TOLS") {
        // Sims 2: 'TOLS', version, unused, count.
        s.sims2Layout = true;
        r.skip(4);
        s.version = r.u32();
        r.u32();
        n = count(r, "slot");
    } else {
        // Sims 1: unused, version, 'TOLS', count.
        r.u32();
        s.version = r.u32();
        expectTag(r, "TOLS", "SLOT");
        n = count(r, "slot");
    }
    const uint32_t v = s.sims2Layout ? 10 : s.version; // Sims 2 slots carry every v10 field
    s.slots.resize(n);
    for (auto& sl : s.slots) {
        sl.type = r.u16();
        sl.offset = {r.f32(), r.f32(), r.f32()};
        sl.standing = r.i32();
        sl.sitting = r.i32();
        sl.ground = r.i32();
        sl.flags = r.u32();
        sl.snapTargetSlot = r.i32();
        if (v >= 6) {
            sl.minProximity = r.i32();
            sl.maxProximity = r.i32();
            sl.optimalProximity = r.i32();
            sl.maxSize = r.i32();
            sl.unknown10 = r.i32();
        }
        if (v >= 7)
            sl.gradient = r.f32();
        if (v >= 8)
            sl.height = r.i32();
        if (v >= 9)
            sl.facing = r.i32();
        if (v >= 10)
            sl.resolution = r.i32();
        if (s.sims2Layout)
            sl.ts2Extra = {r.u32(), r.u32()};
        if (s.sims2Layout) {
            sl.minDistance = std::bit_cast<float>(sl.minProximity);
            sl.maxDistance = std::bit_cast<float>(sl.maxProximity);
            sl.optimalDistance = std::bit_cast<float>(sl.optimalProximity);
            const float res = std::bit_cast<float>(sl.resolution);
            sl.resolutionTiles = res > 0.01f && res < 64.0f ? res : float(sl.resolution) / 16.0f;
        } else {
            sl.minDistance = float(sl.minProximity) / 16.0f;
            sl.maxDistance = float(sl.maxProximity) / 16.0f;
            sl.optimalDistance = float(sl.optimalProximity) / 16.0f;
            sl.resolutionTiles = float(sl.resolution) / 16.0f;
        }
    }
    r.expectEnd("SLOT");
    return s;
}

// ---------------------------------------------------------------- TTAB

InteractionTable InteractionTable::parse(std::span<const uint8_t> data) {
    BinReader r(data);
    InteractionTable t;
    t.fileName = r.fixed(kNameSize);
    if (r.atEnd())
        return t; // empty placeholder tables
    if (r.remaining() == 4) {
        // Empty table in the Sims 1 layout: u16 count (0), u16 version.
        if (r.u16() != 0)
            throw FormatError("TTAB: Sims 1 layout tables are not supported");
        t.version = r.u16();
        return t;
    }
    t.typeCode = r.i32();
    t.version = r.u32();
    if (t.version < 0x3F || t.version > 0x4E)
        throw FormatError("TTAB: unsupported version " + hexString(t.version));
    t.unused = r.u32();
    t.interactions.resize(r.u16());
    const uint32_t v = t.version;
    for (auto& it : t.interactions) {
        it.action = r.u16();
        it.guard = r.u16();
        std::vector<uint32_t> counts(v < 0x44 ? 1 : 7);
        for (auto& c : counts)
            c = count(r, "TTAB motive");
        it.flags = r.u16();
        it.flags2 = r.u16();
        it.stringIndex = r.u32();
        it.attenuationCode = r.u32();
        it.attenuationValue = r.f32();
        it.autonomy = r.u32();
        it.joinIndex = r.i32();
        if (v >= 0x45)
            it.uiDisplayType = r.u16();
        if (v >= 0x46)
            it.facialAnimation = r.u32();
        if (v >= 0x4A)
            it.memoryMultiplier = r.f32();
        if (v >= 0x4C) {
            it.objectType = r.u32();
            it.modelTableId = r.u32();
        }
        for (auto c : counts) {
            std::vector<Advertisement> table(c);
            for (auto& a : table) {
                a.minimum = r.i16();
                a.delta = r.i16();
                a.personality = r.u16();
            }
            it.motives.push_back(std::move(table));
        }
    }
    if (!r.atEnd()) {
        // Trailing table name: u8 or u32 length. One table ("Reaction
        // Interaction Table", group 0x7F9FC607, version 0x48) has 96 zero bytes
        // before it: 16 empty advertisements (6 bytes each) beyond what its
        // counts (16, 0, ...) announce, skipped here (docs/data-findings.md).
        for (size_t skip = 0; skip < r.remaining(); skip += 6) {
            const size_t at = r.pos() + skip, rest = r.remaining() - skip;
            if (skip && data[at - 6] | data[at - 5] | data[at - 4] | data[at - 3] | data[at - 2] | data[at - 1])
                break;
            const bool name8 = size_t(data[at]) + 1 == rest;
            const bool name32 = rest >= 4 && size_t(le32(&data[at])) + 4 == rest;
            if (!name8 && !name32)
                continue;
            r.bytes(skip);
            if (name8)
                t.name = r.str8();
            else
                t.name = std::string(reinterpret_cast<const char*>(r.bytes(rest).data() + 4), rest - 4);
            break;
        }
    }
    r.expectEnd("TTAB");
    return t;
}

} // namespace os2
