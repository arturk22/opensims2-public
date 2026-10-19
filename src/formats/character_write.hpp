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
// - the LxNR (0xCCCEF852, instance 1), the face structure the game reads
//   back to edit the face and to grow the Sim up (formats/lxnr.hpp).
// The thumbnails (IMG 0x856DDBAC) are written from CharacterSpec::portraitPng when given.
// Not written (yet): the faces' low level of detail chain (LOD15).

#include "cas_catalog.hpp"
#include "cas_face.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <random>
#include <string>
#include <vector>

namespace os2 {

class ResourceIndex;

namespace cas {

struct CharacterSpec {
    uint32_t age = age::Adult, gender = gender::Female;
    int skinTone = 1; // 1..4 (Light, Medium, Medium-Dark, Dark); the textures' "s<N>" (5: alien)
    // A skin tone family that is not "0000000N-..." (the alien skin of
    // Strangetown's Sims), kept for the AGED and the face TXMT; empty =
    // the family of skinTone.
    std::string skinFamily;
    int hairTone = 2; // 1 black, 2 brown, 3 blond, 4 red, 5 grey
    int fitness = 0;  // AGED "fitness": 0 normal, 2 fat (the fat outfit versions)
    std::string eyes; // textureoverlay family (subtype 3)
    // Other texture overlays by family (brows, lipstick, blush, eye shadow,
    // facial hair, costume makeup); brows and facial hair follow the hair tone.
    std::vector<std::string> overlays;
    // Mesh overlays by family (glasses, beards with a mesh; a beard follows
    // the hair tone).
    std::vector<std::string> meshOverlays;
    FaceSpec face;
    // Catalog part names per outfit category, by part bit (hair, top,
    // bottom, body). Missing categories are filled by completeOutfits.
    std::map<uint32_t, std::map<uint32_t, std::string>> outfits;
    std::string firstName, lastName, description;
    uint32_t guid = 0; // the Sim's OBJD GUID; 0 = a new one
    // The OBJD GUID of the object the file's Sim object is copied from:
    // 0 = TemplatePerson (0x7FD96B54); an NPC made by Make New Character
    // from "NPC - Template - Maid" gets that template's OBJD and resources.
    uint32_t templateGuid = 0;
    // The Sim's head shot as PNG bytes: written as the thumbnails the game keeps in the file (IMG
    // 0x856DDBAC, group 0xFFFFFFFF, instance = the age bit for the small one and the age bit << 8 for the
    // large one: 0x08 and 0x800 for an adult). Empty = none (the family panels then show no portrait).
    std::vector<uint8_t> portraitPng;
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

// The skin catalog's category flags (cTSSkinCatalog::CheckInit, Mac BV
// 0x7e4c34; GetCategoryFlags 0x7e9058): 1 for swimwear, pajamas, underwear,
// naked and athletic (no hat: a hat hair is swapped for its hat-less one), 2
// for try-on (no clothes drawn), 3 for 0x800; 0 for the others.
uint32_t categoryFlags(uint32_t categoryBit);

// The CAS's number generator: RandomizeForHumans' function-local one (Mac BV
// 0x2c3629c, seeded once with clock() x 1000 / 100, FUN_000fcb70) and the
// genetics manager's (+0x08) are the same class (FUN_00171508 seeds,
// FUN_00171674 draws; docs/cas-mac.md, round n51): the state goes to
// s x 1103515245 + 12345 (mod 2^32) and a draw below n is n x bits 16..47 of
// that 64-bit sum, over 2^32 (not a modulo). Seed 0xFFFFFFFF: time ^ clock.
class CasRandom {
public:
    explicit CasRandom(uint32_t seed) : state_(seed) {}
    uint32_t below(uint32_t n);
    uint32_t state() const { return state_; }

private:
    uint32_t state_;
};

// The hair and clothes of a human as the CAS randomize button draws them
// (SetDefaultClothing(true, ...) -> cTSSimOutfitData::Randomize(4, 0) then
// Randomize(-1, 9) -> RandomizeForHumans, Mac BV 0x1b63ac / 0x818734):
// candidates are the parts that are neither hidden nor "no_random" (flags
// 1|8), else the "default" ones (flag 4). Hair: one everyday style of the hair
// tone for every category, the hat-less one of the same family where the
// category has flag 1. Clothes, for each category but naked and try-on: 49 %
// a whole outfit first, else (or without one) a top and a bottom, a whole
// outfit when there is no bottom. With `hair` false only the clothes
// (MakeNewAge). Naked, try-on and a baby's hair are left to completeSpec.
void randomizeOutfits(const CasCatalog& catalog, CharacterSpec& spec, CasRandom& rng, bool hair = true);

// A human's look from the CAS randomize button (HandleButtonGlobalRandomize,
// Mac BV 0x1bc14c, FUN_001b7f08 with every bit): normal fitness
// (SetFitnessLevel(500)), skin and hair tones, the face (an archetype, eyes
// and brows drawn among the parts that are neither hidden nor "no_random":
// the data's beards, glasses and alien eyes all are), hair and clothes
// (randomizeOutfits), the rest by completeSpec. Age and gender stay, and so
// do the personality and the name (the button does not touch them).
void randomizeLook(const CasCatalog& catalog, CharacterSpec& spec, CasRandom& rng);

struct CharacterWriteResult {
    uint32_t guid = 0;
    size_t resources = 0;
    std::vector<std::string> warnings;
};

// Writes the character package to `out`. `objectName` names the file's
// object ("N001_User00012"); the OBJD is called "<objectName> - <first>".
// `faceStructure`: also the LxNR (formats/lxnr.hpp: the face at every age,
// both genders and levels of detail; a second or so of baking, which a
// preview can skip).
CharacterWriteResult writeCharacter(ResourceIndex& index, const CasCatalog& catalog, FaceBaker& baker,
                                    const CharacterSpec& spec, const std::string& objectName,
                                    const std::filesystem::path& out, bool faceStructure = true);

// The other way: a saved Sim as CAS choices, to edit it again or to start
// from a premade one. `group` is the Sim's group in `package`: 0xFFFFFFFF
// in a character file, the Sim's own (0x7F...) in the premade Sim bin
// (Catalog/Bins/globalcatbin.bundle.package: per Sim an AGED, its 3IDR, a
// face GZPS with its mesh chain, the face TXMT and the LxNR in one group,
// listed by a BINX whose objectidx names the AGED). Reads the AGED (age,
// gender, fitness, skin, hair and eye colours, the outfit lists, resolved
// to catalog part names), the face TXMT's cafOverlay* (the overlays), the
// LxNR (face.structure) and the CTSS names. Throws without an AGED.
// `agedInstance`: the AGED (and its 3IDR) of that instance; the outfit
// objects ("Outfit - NPC - Maid", OBJD group 7FE81E93) keep one AGED per
// age and gender at instances 1, 2, ...
CharacterSpec readCharacterSpec(class DbpfPackage& package, uint32_t group, const CasCatalog& catalog,
                                uint32_t agedInstance = 1);

// A Sim grown into the age `newAge`, as cTSSimOutfitData::MakeNewAge
// does it for the Age primitive (docs/cas-mac.md §17.2): the same Sim
// (tones, eyes, overlays, the LxNR face, which gives the new age's face)
// with, per category, the hair of the same family in the new age (else one
// in the hair tone; elders grey), and new clothes drawn at random (`seed`).
void ageSpec(const CasCatalog& catalog, CharacterSpec& spec, uint32_t newAge, uint32_t seed);

// Reads the character file `file` (its AGED, LxNR, names and its OBJD's
// GUID), grows it into `newAge` (ageSpec) and writes the new character
// file to `out` under the same object name and GUID, with the LxNR (the
// game rewrites the character on aging: TryAge's UpdateFaces, §17.1).
CharacterWriteResult writeAgedCharacter(ResourceIndex& index, const CasCatalog& catalog, FaceBaker& baker,
                                        const std::filesystem::path& file, uint32_t newAge,
                                        const std::filesystem::path& out, uint32_t seed = 1);

// A Sim whose character the game keeps in its object's group instead of a
// character file: the "NPC - Unique" objects of objects.package (the Grim
// Reaper, OBJD 0x84EC24A8, group 7F404283: AGED + 3IDR over catalog parts;
// the Pollination Technician 0x2E17B9FC: a whole character, its face
// GZPS/CRES/SHPE/GMND/GMDC, TXMT and LxNR) are neighbors with no file
// (`s2pkg hood`: "no character file"). Writes that group as a character
// file (group 0xFFFFFFFF, the 3IDRs' references to the group made local;
// the scenegraph links keep the group, found in the game's data). False
// when the OBJD is not found or its group has no AGED.
bool writeEmbeddedCharacter(ResourceIndex& index, uint32_t objdGuid, const std::filesystem::path& out);

// A character file's names (its CTSS: string 0 the first name, 2 the
// family name; the description kept) set to these, as the game's character
// takes the name the player gives (the newborn's name dialog). The file is
// rewritten in place; false when it has no CTSS or already had them.
bool setCharacterNames(const std::filesystem::path& file, const std::string& firstName, const std::string& lastName);

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
// A 3IDR (0xDEADBEEF, version 2, type/group/instance/instanceHi each).
std::vector<uint8_t> writeReferenceList(const std::vector<Tgi>& refs);
std::vector<uint8_t> writePropertySetXml(const std::map<std::string, PropertyValue>& properties);

// The clothes a Sim brings to its family's clothing bin
// (cTSFamily::AddMemberClothing -> AddClothingFromOutfitData, BV
// 0x47ec68/0x47e5ac): the GZPS of the tops (4), bottoms (0x10) and whole
// outfits (8) of every outfit category but 0x002, 0x004, 0x080 (naked),
// 0x400 (try-on) and 0x800 (mask 0xC86), once each. (The binary takes the
// accessories, part 0x20, through the bin service; not here.)
std::vector<Tgi> familyBinClothing(const CasCatalog& catalog, const CharacterSpec& spec);

} // namespace cas
} // namespace os2
