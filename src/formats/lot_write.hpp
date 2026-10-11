#pragma once

// Writing a lot's build-mode content back: the inverse of Lot::load for
// what Build mode changes (lot_edit.hpp): the wall graphs (WGRA, every
// instance), the wall layer (WLL 0x04: type and coverings of each wall),
// the floors (3ARY 0x00), the room arrays (3ARY 0x09, 0x0A, 0x0B) and the
// pattern string maps (SMAP 0x0D walls, 0x0E floors, with their usage
// counts). An unchanged lot writes back byte for byte what it was read
// from (`s2pkg build-roundtrip`).

#include "lot.hpp"

#include <cstdint>
#include <vector>

namespace os2 {

class DbpfPackage;

struct LotGeometryRecord {
    uint32_t type = 0, group = 0, instance = 0;
    std::vector<uint8_t> bytes;
};

// The records, each keyed (group, file name prefix, version) as the
// source package has it, or as the game writes new ones (group 0xFFFFFFFF,
// zero name). The caller puts them in place of the source's
// (LotObjects::otherRecords).
std::vector<LotGeometryRecord> writeLotGeometry(const Lot& lot, DbpfPackage& source);

} // namespace os2
