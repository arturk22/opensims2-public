#pragma once

#include "dbpf.hpp"
#include "skinning.hpp"

#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace os2 {

class ResourceIndex;

// 3IDR (0xAC506764): the resources a property set refers to by index.
// u32 0xDEADBEEF, u32 version, u32 count, then type, group, instance and (in
// version 2) instanceHi per entry.
std::vector<Tgi> parseReferenceList(std::span<const uint8_t> data);

// One Sim part from the catalog: a GZPS property set (Catalog/Skins) plus the
// 3IDR with the same group and instance. Keys used here:
//   name, age (bits: 1 toddler, 2 child, 4 teen, 8 adult, 16 elder,
//   32 baby, 64 young adult), gender (1 female, 2 male), outfit (1 hair,
//   2 face, 4 top, 8 body, 16 bottom, ...), type ("skin" for meshes),
//   resourcekeyidx (CRES), shapekeyidx (SHPE), numoverrides and
//   override<N>subset / override<N>resourcekeyidx (TXMT per GMDC subset).
struct SimPart {
    Tgi tgi;
    std::map<std::string, std::string> properties;
    std::vector<Tgi> references;

    std::string property(const std::string& key) const;
    uint32_t number(const std::string& key) const; // 0 if absent
    std::string name() const { return property("name"); }
    // The reference a "...idx" property points at.
    std::optional<Tgi> reference(const std::string& indexKey) const;
};

// Every GZPS with its 3IDR (parts without one are skipped).
std::vector<SimPart> loadSimParts(ResourceIndex& index);
const SimPart* findSimPart(const std::vector<SimPart>& parts, std::string_view name); // case-insensitive

// Resource name stored in a scenegraph resource (CRES, TXMT, ...) found by TGI;
// empty if missing.
std::string scenegraphName(ResourceIndex& index, const Tgi& tgi);

// The meshes of `part` (its CRES, with the TXMT overrides per subset) as a
// model ready for SkinnedModel::add.
ObjectModel loadSimPartModel(ResourceIndex& index, const SimPart& part);

// What a Sim wears and looks like. Names are catalog part names (GZPS
// "name"); the face, eyes and brows are fixed textures until the face
// templates and CAS compositing are decoded.
struct SimLook {
    std::string prefix = "am"; // age and gender: "am" adult male, "af" adult female
    std::string body = "ambodyoverhangtshirt_blue";
    std::string hair = "amhairshort_brown";
    int skinTone = 1; // 1-4 (s1 lightest .. s4 darkest)
    std::string eyes = "uuface-eye-dkblue";     // TXTR over the face
    std::string brows = "uuface-browbushy-brown"; // TXTR over the face
};

// Assembles an adult Sim on the "auskel" skeleton: body outfit (skin texture
// under the clothes), face (skin tone texture with eyes and brows) and hair.
SkinnedModel loadSim(ResourceIndex& index, const std::vector<SimPart>& parts, const SimLook& look);

} // namespace os2
