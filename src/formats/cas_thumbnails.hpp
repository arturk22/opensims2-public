#pragma once

// The Create-a-Sim thumbnail cache the game ships
// (TSData/Res/UserData/Thumbnails/CASThumbnails.package: 256x256 JPEGs with
// an ALFA alpha segment, type 0x0C7E9A76, group 0xEC7E9A54 =
// cTSUIThumbnailService +0xAC). The service looks a thumbnail up by an
// "instance only" key (FixInstanceOnlyCASKey fills type and group); the
// instance is a hash the CAS UI builds per kind of entry (Mac BV binary):
//
//   CreateCASFileName(age, gender, species)  -> "<age><gender>[<species>]"
//       age "toddler" "child" "teen" "adult" "elder" ("youngadult"; nothing
//       for babies), gender "female"/"male", species nothing for humans
//   CreateClothingThumbnailInstanceId  (BV 0x983ea0, GZPS without hairtone)
//       crc32("<file>_<group>-<type>-<instance>")   (the GZPS key, %d)
//   CreateHairThumbnailInstanceId      (BV 0x983d30, GZPS with a hairtone)
//       crc32("<file>_<group>-<type>-<instance>_<hairtone GUID>")
//   CreateOverlayThumbnailInstanceId   (BV 0x983be0, texture and mesh overlays)
//       crc32("<file>_<family GUID>_<hairtone GUID>")
//   GetCASArchetypeThumbnailContext    (BV 0x988f30, the head/region bins)
//       crc32("<file><family GUID><region>") (no '_'), region "" (whole face),
//       "brow", "eyes", "nose", "mouth", "jaw"
// crc32 = the MSB-first CRC-32 of the lowercased string (nameInstanceHiHash);
// GUIDs as "%08x-%04x-%04x-%04x-%04x%08x". Checked against the package: the
// hashes find 2803 GZPS, 450 overlay and 1224 archetype thumbnails (4477 of
// 5623; the rest are premade Sims, body options, ... not decoded).

#include "image.hpp"
#include "sim.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

namespace os2 {

class DbpfPackage;
class ResourceIndex;

namespace cas {

constexpr uint32_t kThumbnailType = 0x0C7E9A76, kThumbnailGroup = 0xEC7E9A54;

// "adultfemale", "childmale", ... (empty for babies).
std::string casFileName(uint32_t ageBit, uint32_t genderBit);
uint32_t clothingThumbnailId(uint32_t ageBit, uint32_t genderBit, const Tgi& gzps);
uint32_t hairThumbnailId(uint32_t ageBit, uint32_t genderBit, const Tgi& gzps, const std::string& hairTone);
uint32_t overlayThumbnailId(uint32_t ageBit, uint32_t genderBit, const std::string& family, const std::string& hairTone);
uint32_t archetypeThumbnailId(uint32_t ageBit, uint32_t genderBit, const std::string& family, const std::string& region);
// The id of a catalog entry as the CAS grids show it at an age and gender:
// GZPS parts (hair when they name a hairtone), texture/mesh overlays.
uint32_t entryThumbnailId(const SimPart& e, uint32_t ageBit, uint32_t genderBit);

// The overlays the cache has no picture of (eyes, lipstick, blush, eye
// shadow and liner: the game renders those on demand, GetSkinOverlayThumbnail
// -> the CAS thumbnail factory): the overlay's texture over the face skin
// texture of `faceTexture` ("afface-s2"), cut to the part it paints (the
// face textures share one layout: the eye at the top left, the eyes, the
// mouth), 96x96. Null image when a texture is missing.
std::optional<RgbaImage> overlayPreview(ResourceIndex& index, const std::string& faceTexture,
                                        const std::string& overlayTexture, uint32_t subtype);
// The largest level of the TXTR "<name>_txtr", if any.
std::optional<RgbaImage> loadNamedTexture(ResourceIndex& index, const std::string& name);

class CasThumbnails {
public:
    explicit CasThumbnails(const std::filesystem::path& package);
    ~CasThumbnails();
    bool loaded() const { return pkg_ != nullptr; }
    size_t size() const { return byInstance_.size(); }
    bool has(uint32_t instance) const { return byInstance_.count(instance) != 0; }
    // The decoded image (cached), null if the package has none.
    const RgbaImage* image(uint32_t instance);

private:
    std::unique_ptr<DbpfPackage> pkg_;
    std::unordered_map<uint32_t, size_t> byInstance_;
    std::map<uint32_t, std::unique_ptr<RgbaImage>> cache_;
};

} // namespace cas
} // namespace os2
