#pragma once

// Writing a new character file (Neighborhoods/<N>/Characters/<N>_User#####
// .package) from Create-a-Sim choices, as cTSCASSystem::ProcessSaveSimRequest
// lays it out (docs/cas-mac.md §7) and as the base game's files hold it
// (N001_User00001):
// - AGED (0xAC598EAC, instance 1, XML property set) with 3IDR 1: age,
//   gender, species, fitness, skincolor/haircolor (tone families),
//   eyecolor (the eye overlay's family), skeletonkeyidx, the outfit lists
//   (listcnt; lk<i> = category << 16 | part, ls<i> = 1, le<i>_0 = 3IDR
//   index of the part's GZPS), validparts, stretch, activecat, creatorid;
// - GZPS 1 (sharing 3IDR 1 with the AGED) and GZPS 2 (with 3IDR 2): the face
//   of the current age and of the next one (type "skin", outfit 2, flags 1,
//   hairtone, skintone, resourcekeyidx = the face CRES, shapekeyidx a null
//   reference as in the game's files);
// - per face the scenegraph chain copied from the neutral face of that age
//   (<p>Face_cres -> SHPE -> GMND -> GMDC) under names of the file's own
//   ("#0x7fe09489!0x<id>_age<n>_0_cres", group 0xFFFFFFFF, instance and
//   high instance = the name hashes without the prefix), the GMDC baked by
//   cas_face, and a SimSkin TXMT "ss_0x<id>_cmpm" listing what the game
//   composites over the face skin (baseTexture0 = <p>face-s<tone>, then
//   the eyes and the other overlays by layer; cafSkinTone, cafHairTone,
//   cafNumOverlays, cafOverlay<i> = the overlays' families);
// - the Sim's object: the template person's resources (TemplatePerson,
//   OBJD GUID 0x7FD96B54 in objects.package: OBJD, OBJf, GLOB, SLOT, NREF,
//   STR#, BHAV) moved to group 0xFFFFFFFF, the OBJD with a new GUID, the
//   person object type and "<file> - <first name>" as its name; CTSS 0x7D0
//   with the first name, description and last name; the 0xEBFEE342 record
//   ("The Sims 2", the version that wrote it).
// Not written (yet): the LxNR (its format is not decoded; the game reads it
// back to edit the face again), the thumbnails (IMG 0x856DDBAC) and mesh
// overlays (glasses, beards with a mesh).

#include "cas_catalog.hpp"
#include "cas_face.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace os2 {

class ResourceIndex;

namespace cas {

struct CharacterSpec {
    uint32_t age = age::Adult, gender = gender::Female;
    int skinTone = 1; // 1..4 (Light, Medium, Medium-Dark, Dark)
    int hairTone = 2; // 1 black, 2 brown, 3 blond, 4 red, 5 grey
    int fitness = 0;  // AGED "fitness": 0 normal, 2 fat (the fat outfit versions)
    std::string eyes; // textureoverlay family (subtype 3)
    // Other texture overlays by family (brows, lipstick, blush, eye shadow,
    // facial hair, costume makeup); brows and facial hair follow the hair tone.
    std::vector<std::string> overlays;
    FaceSpec face;
    // Catalog part names per outfit category, by part bit (hair, top,
    // bottom, body). Missing categories are filled by completeOutfits.
    std::map<uint32_t, std::map<uint32_t, std::string>> outfits;
    std::string firstName, lastName, description;
    uint32_t guid = 0; // the Sim's OBJD GUID; 0 = a new one
};

// The outfit categories a new Sim of that age and gender gets (everyday,
// formal, underwear, pajamas, swimwear, athletic, naked, maternity for
// adult women, try-on), in the order the AGED lists them.
std::vector<uint32_t> outfitCategories(uint32_t ageBit, uint32_t genderBit);

// Fills what the spec leaves empty the way a new Sim gets it: hair (the
// first style in the hair tone), eyes (dark brown), brows (the first in
// the hair tone), and per category the first CAS part of each needed kind
// (everyday: top and bottom; the others: a whole outfit; naked: the naked
// top and bottom of the skin tone). `seed` != 0 picks at random instead.
void completeSpec(const CasCatalog& catalog, CharacterSpec& spec, uint32_t seed = 0);

struct CharacterWriteResult {
    uint32_t guid = 0;
    size_t resources = 0;
    std::vector<std::string> warnings;
};

// Writes the character package to `out`. `objectName` names the file's
// object ("N001_User00012"); the OBJD is called "<objectName> - <first>".
CharacterWriteResult writeCharacter(ResourceIndex& index, const CasCatalog& catalog, FaceBaker& baker,
                                    const CharacterSpec& spec, const std::string& objectName,
                                    const std::filesystem::path& out);

// The next life stage's age bit (baby -> toddler -> child -> teen -> adult
// -> elder), 0 for elders.
uint32_t nextAge(uint32_t ageBit);
// The editor's age index (1 -> 0, 2 -> 1, 4 -> 2, 8 -> 3, 0x10 -> 4, 0x20 -> 5).
int editorAgeIndex(uint32_t ageBit);

// cGZPropertySetString as the game writes it: keys sorted, AnyUint32 /
// AnyString / AnyFloat32 by the value's kind.
struct PropertyValue {
    enum Kind { Uint32, String, Float32 } kind = String;
    std::string text;
};
std::vector<uint8_t> writePropertySetXml(const std::map<std::string, PropertyValue>& properties);

} // namespace cas
} // namespace os2
