#pragma once

// What Build mode sells besides objects: the floor and wall coverings
// (cTSCatalog::LoadFloorPatterns / LoadWallPatterns: the catalog's
// property sets, type 0xCCA8E925 in Catalog/Patterns/catpatterns.bundle.package,
// with "type" floor or wall, "cost", "subsort", "material", "guid", the
// name and description in the STR# "stringsetid" of group
// "stringsetgroupid"), and the walls of walls.txt (catalog text "build"
// 129). Doors, windows and the other build objects are OBJDs with a "build
// mode type" (Catalog::load(index, true), catalog.hpp).

#include <array>
#include <cstdint>
#include <set>
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
    bool walkOver = false;                // "maywalkover": Sims step over it
};

// The GUIDs of every fence whose "maywalkover" is set (cTSFenceEntry::
// MayWalkOver: 13 of the base game's 23 low fences), shown in the catalog or
// not; routing leaves them out (inferred: MayWalkOver as the partition's
// BlocksLocomotion, while it still blocks placement).
std::set<uint32_t> walkOverFenceGuids(ResourceIndex& index);

// A terrain paint (catalog property sets of type 0x4DCADB7E: "texturetname",
// "brushwidth", free).
struct BuildTerrainPaint {
    uint32_t guid = 0;
    std::string name, description, texture;
};

// A roof pattern (catalog property sets of type 0xACA8EA06; "texturetop"
// is its picture; free). The game draws every roof of a lot with one.
struct BuildRoofPattern {
    uint32_t guid = 0;
    std::string name, description, texture;
};

// A modular staircase (modularstairs.txt "modularStairs N", cTSProductModularStairs):
// its five objects by selector GUID, in the order of the product's object
// slots (cITSProductModularStairs::tObjectType): 0 landing, 1 placeholder
// (keeps the space over the steps), 2 step, 3 top step, 4 bottom step (0:
// the step). Name, description and price (per step: CalculatePreviewCost
// multiplies the steps by it) are the catalog text "build" 139 entries 3i+1,
// 3i+2 and 3i of its catalogTextIndex i.
struct BuildStairs {
    int id = 0;
    std::array<uint32_t, 5> objects{};
    std::string name, description;
    int price = 0;
};

// A roof product of roofs.txt (cTSProductRoof): "roof 0-5" are the roof types
// (Lot::RoofType 1-6), "roof 6-11" the dormers (cTSWinProcCatalog::LoadDormers:
// one, two and three tiles wide, gable then hip), priced unlike the roofs.
// Name, description and price from catalog text "build" 140 at the
// catalogTextIndex.
struct BuildRoofProduct {
    int id = 0;
    std::string defaultPattern; // "roof-ui-hipped", "dormerpreview-2wide", ...
    std::string name, description;
    int price = 0;
    uint32_t thumbnail = 0; // thumbnailResource instance (group 0x499DB772), 0 if none
};

// Build-mode prices that are not in the data (docs/buy-build-mac.md §7).
// A wall costs §70 a section (the game's catalog text; the binary's wall
// cost path is not traced: inferred), and selling back what was built in
// an earlier session pays RefundRate (0.8) of it (cTSCatalog, BV).
constexpr int kWallSectionPrice = 70;
// A foundation tile: string 0 of the catalog text "build" 138 (Text/Build.package).
constexpr int kFoundationTilePrice = 4;
// Decks (deckfoundations.txt "deck 1", lattice skirt, wall style 16, and
// "deck 2", columns at the corners, style 4): §3 and §2 a tile (catalog
// text "build" 138).
constexpr int kLatticeDeckTilePrice = 3, kColumnDeckTilePrice = 2;
// A pool tile: catalog text "build" 138, deckfoundations.txt "pool 3".
constexpr int kPoolTilePrice = 75;
// What a foundation raises the ground floor by (the saved lots: level 1 at
// the terrain + 0.75 over foundations).
constexpr float kFoundationHeight = 0.75f;
constexpr float kBuildRefundRate = 0.8f;

class BuildCatalog {
public:
    // The patterns shown in the catalog (showincatalog 1, not deprecated),
    // by price then name, as the game sorts its products.
    // (Kept for the next load on the same index.)
    static BuildCatalog load(ResourceIndex& index);

    const std::vector<BuildPattern>& floors() const { return floors_; }
    const std::vector<BuildFence>& fences() const { return fences_; }
    const std::vector<BuildTerrainPaint>& terrainPaints() const { return paints_; }
    const std::vector<BuildRoofPattern>& roofPatterns() const { return roofs_; }
    const BuildRoofPattern* findRoofPattern(const std::string& key) const;
    const BuildTerrainPaint* findTerrainPaint(const std::string& key) const;
    // The terrain brushes' profiles (terrainSculptBrushes.txt
    // profileTextureID: 32x32 grey TGAs in terrain.package, group
    // 0x4B72DA28): raise (0x6B96E0A2) and lower (0x8B96E0CF); 32 x 32
    // values, empty when missing.
    const std::vector<uint8_t>& raiseProfile() const { return raise_; }
    const std::vector<uint8_t>& lowerProfile() const { return lower_; }
    // The water tool's (cTSUserToolWater::spWaterBrushProfile, instance 0x8B731811).
    const std::vector<uint8_t>& waterProfile() const { return water_.empty() ? lower_ : water_; }
    // A fence by GUID ("0x...") or (part of) its name.
    const BuildFence* findFence(const std::string& key) const;
    const BuildFence* fenceByGuid(uint32_t guid) const;
    const std::vector<BuildPattern>& walls() const { return walls_; }
    // modularstairs.txt's products in catalog order (empty when the script is missing).
    const std::vector<BuildStairs>& stairs() const { return stairs_; }
    const BuildStairs* stairsById(int id) const;
    // roofs.txt's products (roof 0-11).
    const std::vector<BuildRoofProduct>& roofProducts() const { return roofProducts_; }
    const BuildRoofProduct* roofProduct(int id) const;
    // A pattern by GUID ("0x...") or (part of) its name or material.
    const BuildPattern* find(const std::string& key, bool floor) const;
    const BuildPattern* byGuid(uint32_t guid) const;
    // The patterns of a subsort ("" for all).
    std::vector<const BuildPattern*> bySubsort(bool floor, const std::string& subsort) const;

private:
    static BuildCatalog loadUncached(ResourceIndex& index);
    std::vector<BuildPattern> floors_, walls_;
    std::vector<BuildFence> fences_;
    std::vector<BuildTerrainPaint> paints_;
    std::vector<BuildRoofPattern> roofs_;
    std::vector<uint8_t> raise_, lower_, water_;
    std::vector<BuildStairs> stairs_;
    std::vector<BuildRoofProduct> roofProducts_;
};

} // namespace os2
