// Writing neighborhood records back (neighborhood.hpp): the inverse of the
// parsers, for the base game's versions (NGBH 0x6F, SDSC 0x20, FAMI 0x4E).

#include "neighborhood.hpp"

#include "binwriter.hpp"

namespace os2::hood {

namespace {

void str32(BinWriter& w, const std::string& s) {
    w.u32(uint32_t(s.size()));
    w.raw(std::span(reinterpret_cast<const uint8_t*>(s.data()), s.size()));
}

void writeTokens(BinWriter& w, const std::vector<InventoryToken>& tokens) {
    w.u32(uint32_t(tokens.size()));
    for (const auto& t : tokens) {
        w.u32(t.guid);
        w.u16(t.flags);
        w.u32(uint32_t(t.data.size()));
        for (uint16_t v : t.data)
            w.u16(v);
    }
}

void writeInventory(BinWriter& w, const Inventory& inv) {
    writeTokens(w, inv.counted);
    writeTokens(w, inv.singular);
}

void writeInventories(BinWriter& w, const std::vector<Inventory>& list) {
    w.u32(uint32_t(list.size()));
    for (const auto& inv : list) {
        w.u32(inv.id);
        writeInventory(w, inv);
    }
}

} // namespace

std::vector<uint8_t> NeighborhoodInventories::write() const {
    BinWriter w;
    w.u32(hoodtype::NGBH);
    w.u32(version);
    for (uint32_t h : header)
        w.u32(h);
    str32(w, regionType);
    // The second region type in a 32-byte block, zero-padded.
    const size_t block = w.size();
    str32(w, secondRegionType);
    while (w.size() < block + 32)
        w.u8(0);
    writeInventory(w, global);
    writeInventories(w, lots);
    writeInventories(w, families);
    writeInventories(w, neighbors);
    w.u8(trailer);
    return std::move(w.bytes());
}

std::vector<uint8_t> SimDescription::write() const {
    BinWriter w;
    w.u32(unknown0);
    w.u32(version);
    w.u32(unknown2);
    w.shorts(personData);
    w.u16(neighborId);
    w.u32(guid);
    w.u32(unknown3);
    w.u32(uint32_t(relations.size()));
    for (uint32_t r : relations)
        w.u32(r);
    w.u8(trailer);
    return std::move(w.bytes());
}

std::vector<uint8_t> FamilyRecord::write() const {
    BinWriter w;
    w.u32(hoodtype::FAMI);
    w.u32(version);
    w.u32(unknown);
    w.u32(lot);
    w.u32(simGroupId);
    w.i32(funds);
    w.u32(friends);
    w.u32(flags);
    w.u32(uint32_t(members.size()));
    for (uint32_t g : members)
        w.u32(g);
    w.u32(albumGuid);
    return std::move(w.bytes());
}

std::vector<uint8_t> Relationship::write() const {
    BinWriter w;
    w.u32(version);
    w.u32(uint32_t(values.size()));
    for (int32_t v : values)
        w.i32(v);
    return std::move(w.bytes());
}

std::vector<uint8_t> LotInfo::writeBase(bool str7Names) const {
    BinWriter w;
    w.u16(baseVersion);
    w.u32(width);
    w.u32(depth);
    w.u8(type);
    w.u8(roads);
    w.u8(creationFrontEdge);
    if (baseVersion >= 5)
        w.u32(flags);
    if (baseVersion > 5) {
        if (str7Names) {
            w.str7(name);
            w.str7(description);
        } else {
            str32(w, name);
            str32(w, description);
        }
    }
    if (baseVersion > 3) {
        w.u32(uint32_t(heights.size()));
        for (float h : heights)
            w.f32(h);
    }
    if (baseVersion > 6)
        w.f32(roadHeight);
    if (baseVersion > 7)
        w.u32(baseUnknown);
    return std::move(w.bytes());
}

std::vector<uint8_t> LotInfo::write() const {
    BinWriter w;
    w.u16(version);
    w.raw(writeBase());
    w.u32(x);
    w.u32(y);
    w.f32(heightOffset);
    w.i32(lotId);
    w.u8(frontEdge);
    str32(w, skirtTexture);
    w.raw(rest);
    return std::move(w.bytes());
}

} // namespace os2::hood
