#pragma once

// What the environment score reads of the floor and wall coverings
// (cTSEnvironmentScoreManager's UpdateFloorScores / UpdateWallScores through
// the catalog's products, docs/motives-mac.md §8.3): each pattern's price,
// NicenessMultiplier and CrapScore.
//
// - The catalog's property sets (type 0xCCA8E925 in
//   Catalog/Patterns/catpatterns.bundle.package, "type" floor or wall) give
//   "cost", "nicenessmultiplier" and "crapscore"; the base game's loader
//   (Mac base 0x1e87a8) puts 1 and 0 when the last two are missing. Every
//   product counts, shown in the catalog or not, deprecated or not (a lot
//   may still hold one).
// - The old patterns of Catalog/Scripts/floorPatterns.txt and
//   wallPatterns.txt ("blank", the default wall: crapScore 0.1;
//   "floor_plywoodFoundation", the default floor: crapScore 1.0) have their
//   "crapScore" (the base binary's cCrapScoreCommand); their catalog text
//   lines are commented out in the base game, so their price is 0, and the
//   scripts have no niceness command (1, inferred: the product's default).
//
// Keyed by the name a lot keeps the pattern by (Lot::floorPatterns,
// wallPatterns): the GUID in lowercase hex for the catalog's, the script's
// name lowercased for the others.

#include <map>
#include <string>

namespace os2 {

class ResourceIndex;

struct PatternScore {
    int price = 0;      // the product's Price (vtable 0x58 of the base's cTSProduct)
    float niceness = 1; // NicenessMultiplier (0x5c)
    float crap = 0;     // CrapScore (0x60)
};

struct PatternScores {
    std::map<std::string, PatternScore> floors, walls;
    const PatternScore* floor(const std::string& lotName) const;
    const PatternScore* wall(const std::string& lotName) const;
};

// Read once per index (later calls with the same index return the same tables).
const PatternScores& patternScores(ResourceIndex& index);

} // namespace os2
