#pragma once

// The Buy-mode catalog: every object the player can buy, from the OBJDs of
// the game's packages, with what Buy mode shows of it (the CTSS catalog name
// and description, the price, the ratings, the thumbnail) and how it sorts
// it (the function and room sort bits and the function subsort).
//
// OBJD fields by their labels in the game's SimAntics editor (STR#
// 0x7FE59FD0/0xCC; field i at file offset 0x40 + 2i, the same order as
// OpenTS2's ObjectDefinitionAsset.FieldNames).

#include "image.hpp"
#include "simantics.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace os2 {

class DbpfPackage;
class ResourceIndex;

namespace objd {
// File offsets of the OBJD fields Buy mode reads.
constexpr size_t WallAdjacentFlags = 0x46;  // "default wall adjacent flags"
constexpr size_t PlacementFlags = 0x48;     // "default placement flags"
constexpr size_t WallPlacementFlags = 0x4A; // "default wall placement flags"
constexpr size_t AllowedHeightFlags = 0x4C; // "default allowed height flags"
constexpr size_t ObjectType = 0x52;
constexpr size_t MasterId = 0x54;
constexpr size_t SubIndex = 0x56;
constexpr size_t CatalogUseFlags = 0x62;
constexpr size_t Price = 0x64;
constexpr size_t SalePrice = 0x84;
constexpr size_t InitialDepreciation = 0x86;
constexpr size_t DailyDepreciation = 0x88;
constexpr size_t SelfDepreciating = 0x8A;
constexpr size_t DepreciationLimit = 0x8C;
constexpr size_t RoomSort = 0x8E;
constexpr size_t FunctionSort = 0x90;
constexpr size_t CatalogStrings = 0x92;   // CTSS instance, same group
constexpr size_t DefaultThumbnail = 0xA4;
constexpr size_t LevelOffset = 0xB0;
constexpr size_t FrontDirection = 0xBA;
constexpr size_t MultiTileLead = 0xBE;
constexpr size_t TileWidth = 0xC6;
constexpr size_t BuildModeType = 0xCA;
constexpr size_t BuildModeSubsort = 0xD4;
constexpr size_t FootprintMask = 0xDA;
constexpr size_t RatingHunger = 0xE4;      // then comfort, hygiene, bladder, energy, fun, room
constexpr size_t RatingSkillFlags = 0xF2;
constexpr size_t FunctionSubSort = 0xFC;
constexpr size_t CommunitySort = 0x108;
} // namespace objd

// Function sorts (OBJD "function sort flags"): bit i is the Buy panel's
// function button 0xA{i+1}00 and its subsort panel 0xB{i+1}00 (script
// 0x49060004): seating, surfaces, appliances, electronics, plumbing,
// decorative, miscellaneous, lighting, hobbies.
constexpr int kFunctionSortCount = 9;
const char* functionSortName(int bit);
// Room sorts (OBJD "room sort flags"): bit i is the room button 0xC{i+1}00:
// kitchen, bedroom, bathroom, living room, outside, dining room,
// miscellaneous, study, kids.
constexpr int kRoomSortCount = 9;
const char* roomSortName(int bit);

struct CatalogEntry {
    uint32_t group = 0;
    uint16_t instance = 0; // of the OBJD (the multi-tile master for multi-tile objects)
    uint32_t guid = 0;
    std::string fileName;    // the OBJD's
    std::string model;       // its entry of the group's "3D Object Names" (STR# 0x85)
    std::string name;        // CTSS string 1 in the preferred language
    std::string description; // CTSS string 2
    int price = 0;
    uint16_t functionSort = 0, functionSubSort = 0, roomSort = 0, communitySort = 0;
    uint16_t buildModeType = 0, buildModeSubsort = 0;
    uint16_t catalogStrings = 0;
    uint16_t objectType = 0;
    bool multiTile = false;
    // Depreciation (OBJD): the price drops by `initialDepreciation` when
    // bought and by `dailyDepreciation` a day, down to `depreciationLimit`.
    int initialDepreciation = 0, dailyDepreciation = 0, depreciationLimit = 0;
    bool selfDepreciating = false;
    // Hunger, comfort, hygiene, bladder, energy, fun, environment.
    std::array<int16_t, 7> ratings{};
    uint16_t skillFlags = 0;
    // Placement flags (OBJD): what the Buy-mode placement checks read.
    uint16_t placementFlags = 0, wallPlacementFlags = 0, wallAdjacentFlags = 0, allowedHeightFlags = 0;

    // What the player gets back selling it now (`value`, the object's
    // current value; the game keeps it in the object's data).
    bool inFunction(int bit) const { return functionSort & (1u << bit); }
    bool inRoom(int bit) const { return roomSort & (1u << bit); }
};

// The key of an object's thumbnail: the scenegraph CRC32 (nameInstanceHiHash)
// of the OBJD's group in decimal followed by its model name (1,201 of the
// game's OBJDs hit the 531 thumbnails this way; found by trying, as SimPE
// computes it).
uint32_t thumbnailKey(uint32_t group, const std::string& model);

class Catalog {
public:
    // Every buyable object of the index: an OBJD with one of the Buy
    // panel's function sorts, single-tile or the master of a multi-tile
    // object (whose parts carry no catalog data), not a Sim, in a game
    // group (not 0xFFFFFFFF: the character files'). Sorted by function,
    // subsort, price and name, as the catalog lists them (inferred).
    static Catalog load(ResourceIndex& index);

    const std::vector<CatalogEntry>& entries() const { return entries_; }
    const CatalogEntry* byGuid(uint32_t guid) const;
    // By GUID ("0x1234ABCD"), OBJD file name or catalog name (case-insensitive).
    const CatalogEntry* find(const std::string& key) const;

    // The entries of a function sort (bit), optionally of one subsort bit
    // (-1: all), in catalog order.
    std::vector<const CatalogEntry*> byFunction(int bit, int subBit = -1) const;
    std::vector<const CatalogEntry*> byRoom(int bit, int functionBit = -1) const;

    // The catalog thumbnail from the thumbnail cache the game ships
    // (TSData/Res/UserData/Thumbnails/ObjectThumbnails.package: 128x128
    // JPEG/PNG, type 0xAC2950C1, group 0x6C2A22C3), null if it has none.
    // The instance is thumbnailKey(group, model).
    const RgbaImage* thumbnail(const CatalogEntry& e);
    void setThumbnailPackage(const std::filesystem::path& path);
    size_t thumbnailCount() const;

private:
    std::vector<CatalogEntry> entries_;
    std::map<uint32_t, size_t> byGuid_;
    std::unique_ptr<DbpfPackage> thumbs_;
    std::map<uint32_t, uint32_t> thumbIndex_; // thumbnail key -> entry index in thumbs_
    std::map<uint32_t, std::shared_ptr<RgbaImage>> thumbCache_;
};

} // namespace os2
