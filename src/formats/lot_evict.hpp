#pragma once

// The cleanup after an eviction (cTSLoadLotController::
// CleanupThingsRelatedToEvictedFamily, BV 0x87e7b0; docs/neighborhood-mac.md
// §11.2): when a lot whose lot info has flag 8 (its family was evicted)
// loads, every object that is not in a container, or is a person, goes if it
// is a person (or of category 0x67), a product with bit 0 of OBJD field 0x2A
// (TV channels, controllers), or cTSObject::IsDeletedByEvict picks it (OBJD
// type 2, or a Buy-mode object, build mode type 0, without bit 0x10 "stays
// after evict" of its movement flags, object data 43) on a residential lot;
// the PlumbBob (GUID 0xF37E5AE0) stays, and a multi-tile object goes by its
// lead part, with its other parts. What the objects that go hold goes with
// them. Done here on the saved object module before the VM creates the
// objects (the game deletes them right after the load). The family's objects
// value (SIMI +0xAC) becomes 0, as cTSLot::ComputeAndStoreLotData recounts it
// (all that IsDeletedByEvict picks is gone), and globals 48/49 follow
// (UpdateTotalObjectsValue). Plain C++.

#include "lot_objects.hpp"

#include <cstdint>
#include <functional>
#include <optional>

namespace os2 {

// The OBJD fields the cleanup reads, by the object's GUID.
struct EvictObjd {
    uint16_t type = 0;          // field 9 (file offset 0x52)
    uint16_t buildModeType = 0; // field 0x45 (0xCA)
    uint16_t field2A = 0;       // field 0x2A (0x94)
};
using EvictObjdLookup = std::function<std::optional<EvictObjd>(uint32_t guid)>;

struct EvictCleanup {
    size_t sims = 0, objects = 0; // removed
};

// `lotType` is the lot info's (types 2 and 3, the expansions' dorms and
// businesses, keep what IsDeletedByEvict alone would take).
EvictCleanup cleanupEvictedFamily(LotObjects& saved, const EvictObjdLookup& objd, int lotType = 0);

} // namespace os2
