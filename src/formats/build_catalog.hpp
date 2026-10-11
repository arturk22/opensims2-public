#pragma once

// What Build mode sells besides objects: the floor and wall coverings
// (cTSCatalog::LoadFloorPatterns / LoadWallPatterns: the catalog's
// property sets, type 0xCCA8E925 in Catalog/Patterns/catpatterns.bundle.package,
// with "type" floor or wall, "cost", "subsort", "material", "guid", the
// name and description in the STR# "stringsetid" of group
// "stringsetgroupid"), and the walls of walls.txt (catalog text "build"
// 129). Doors, windows and the other build objects are OBJDs with a "build
// mode type" (Catalog::load(index, true), catalog.hpp).

#include <cstdint>
#include <string>
#include <vector>

namespace os2 {

class ResourceIndex;

struct BuildPattern {
    uint32_t guid = 0;
    bool floor = false; // else a wall covering
    std::string name, description;
    std::string material; // TXMT name without "_txmt"
    std::string subsort;  // floors: stone brick wood carpet tile lino poured other; walls:
                          // brick masonry siding poured paint wallpaper tile paneling other
    int price = 0;        // per tile (floors) or per wall side (walls)
    // The name a lot's string map keeps it by: the GUID in lowercase hex
    // ("cc13f31").
    std::string lotName() const;
};

// A fence (cTSCatalog::LoadFences: the catalog's property sets of type
// 0x2CB230B8 in catpatterns.bundle.package, "cost" a section, "rail",
// "diagrail" and "post" models, the name and description in the STR#
// "stringsetid" of group "stringsetgroupid"). A lot keeps a fence as a
// wall whose type is the fence's GUID (Lot::isFence).
struct BuildFence {
    uint32_t guid = 0;
    std::string name, description;
    std::string rail, diagonalRail, post; // CRES names
    int price = 0;                        // per section
};

// A terrain paint (catalog property sets of type 0x4DCADB7E: "texturetname",
// "brushwidth", free).
struct BuildTerrainPaint {
    uint32_t guid = 0;
    std::string name, description, texture;
};

// Build-mode prices that are not in the data (docs/buy-build-mac.md §7).
// A wall costs §70 a section (the game's catalog text; the binary's wall
// cost path is not traced: inferred), and selling back what was built in
// an earlier session pays RefundRate (0.8) of it (cTSCatalog, BV).
constexpr int kWallSectionPrice = 70;
// A foundation tile: string 0 of the catalog text "build" 138 (Text/Build.package).
constexpr int kFoundationTilePrice = 4;
// What a foundation raises the ground floor by (the saved lots: level 1 at
// the terrain + 0.75 over foundations).
constexpr float kFoundationHeight = 0.75f;
constexpr float kBuildRefundRate = 0.8f;

class BuildCatalog {
public:
    // The patterns shown in the catalog (showincatalog 1, not deprecated),
    // by price then name, as the game sorts its products.
    static BuildCatalog load(ResourceIndex& index);

    const std::vector<BuildPattern>& floors() const { return floors_; }
    const std::vector<BuildFence>& fences() const { return fences_; }
    const std::vector<BuildTerrainPaint>& terrainPaints() const { return paints_; }
    const BuildTerrainPaint* findTerrainPaint(const std::string& key) const;
    // The terrain brushes' profiles (terrainSculptBrushes.txt
    // profileTextureID: 32x32 grey TGAs in terrain.package, group
    // 0x4B72DA28): raise (0x6B96E0A2) and lower (0x8B96E0CF); 32 x 32
    // values, empty when missing.
    const std::vector<uint8_t>& raiseProfile() const { return raise_; }
    const std::vector<uint8_t>& lowerProfile() const { return lower_; }
    // A fence by GUID ("0x...") or (part of) its name.
    const BuildFence* findFence(const std::string& key) const;
    const BuildFence* fenceByGuid(uint32_t guid) const;
    const std::vector<BuildPattern>& walls() const { return walls_; }
    // A pattern by GUID ("0x...") or (part of) its name or material.
    const BuildPattern* find(const std::string& key, bool floor) const;
    const BuildPattern* byGuid(uint32_t guid) const;
    // The patterns of a subsort ("" for all).
    std::vector<const BuildPattern*> bySubsort(bool floor, const std::string& subsort) const;

private:
    std::vector<BuildPattern> floors_, walls_;
    std::vector<BuildFence> fences_;
    std::vector<BuildTerrainPaint> paints_;
    std::vector<uint8_t> raise_, lower_;
};

} // namespace os2
