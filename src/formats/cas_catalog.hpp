#pragma once

// Create-a-Sim data (docs/cas-mac.md §2-§4): the skin catalog of
// TSData/Res/Catalog/Skins/Skins.package as nTSCASCore::cTSSkinCatalog reads
// it. Besides the GZPS outfit parts (sim.hpp's SimPart) the package holds
// one XML property set per CAS entry, each with a 3IDR of the same
// group/instance:
//   0x4C158081 skintone        family 0000000N-... = tone sN, genetic
//   0x8C1580B5 hairtone        family 0000000N-..., genetic (1 dark .. 2 light)
//   0x8C93E35C facearchetype   a ready-made head: shapekeyidx -> SHPE (CASFace)
//   0x0C93E3DE facemodifier    a face slider: negativelimit/positivelimit
//                              (±1.5), negativescale/positivescale, groupindex,
//                              compareage/comparegender (the neutral face its
//                              mesh is a delta against), shapekeyidx
//   0x8C93BF6C faceregion      a region mask (nose, brow, mouth, eyes, ...)
//   0x6C93B566 faceneutral     the neutral face of an age/gender (afFace, ...)
//   0x2C1FD8A1 textureoverlay  a texture over the face: subtype 0 facial hair
//                              (stubble/beard textures), 1 brows, 2 lipstick,
//                              3 eyes, 4 costume makeup/marks, 5 mask, 6
//                              blush, 7 eye shadow/liner; layer, bin,
//                              materialkeyidx -> TXMT, hairtone
//   0x0C1FE246 meshoverlay     a mesh over the face (glasses, beards with
//                              their own mesh): resourcekeyidx -> CRES
// Every entry has name, age and gender bits, family (GUID text; the same
// family across ages and hair colours), flags, genetic, species.
// Ages: 0x20 baby, 0x01 toddler, 0x02 child, 0x04 teen, 0x08 adult, 0x10
// elder (0x40 young adult is Bon Voyage era); gender 1 female, 2 male.

#include "sim.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace os2 {

class ResourceIndex;

namespace cas {

namespace type {
constexpr uint32_t SkinTone = 0x4C158081;
constexpr uint32_t HairTone = 0x8C1580B5;
constexpr uint32_t FaceArchetype = 0x8C93E35C;
constexpr uint32_t FaceModifier = 0x0C93E3DE;
constexpr uint32_t FaceRegion = 0x8C93BF6C;
constexpr uint32_t FaceNeutral = 0x6C93B566;
constexpr uint32_t TextureOverlay = 0x2C1FD8A1;
constexpr uint32_t MeshOverlay = 0x0C1FE246;
} // namespace type

namespace age {
constexpr uint32_t Toddler = 0x01, Child = 0x02, Teen = 0x04, Adult = 0x08, Elder = 0x10, Baby = 0x20,
                   YoungAdult = 0x40;
}
namespace gender {
constexpr uint32_t Female = 1, Male = 2;
}

// GZPS "outfit" part bits and the AGED/character outfit categories.
namespace part {
constexpr uint32_t Hair = 0x01, Face = 0x02, Top = 0x04, Body = 0x08, Bottom = 0x10;
}
namespace category {
constexpr uint32_t Everyday = 0x001, Swimwear = 0x008, Pajamas = 0x010, Formal = 0x020, Underwear = 0x040,
                   Naked = 0x080, Maternity = 0x100, Athletic = 0x200, TryOn = 0x400;
}

// textureoverlay subtypes.
namespace overlay {
constexpr uint32_t FacialHair = 0, Brows = 1, Lipstick = 2, Eyes = 3, Costume = 4, Mask = 5, Blush = 6,
                   EyeShadow = 7;
}

// "fa", "mu", ... prefixes of the catalog names: age letter (b, p, c, t, a,
// e) and gender letter (f, m, or u for unisex babies, toddlers, children).
std::string agePrefix(uint32_t ageBit, uint32_t genderBit, bool unisexYoung = true);
const char* ageName(uint32_t ageBit); // "adult", ...

// An XML entry or a GZPS part (both SimPart: properties + 3IDR).
std::string entryType(const SimPart& e); // "type" ("skintone", "facemodifier", "skin", ...)
std::string family(const SimPart& e);    // lowercase GUID text
float genetic(const SimPart& e);
bool fits(const SimPart& e, uint32_t ageBit, uint32_t genderBit); // its age and gender bits include them
float floatProperty(const SimPart& e, const std::string& key, float fallback = 0);

struct CasCatalog {
    std::vector<SimPart> skinTones, hairTones, archetypes, modifiers, regions, neutrals, textureOverlays,
        meshOverlays;
    std::vector<SimPart> parts; // every GZPS with a 3IDR (loadSimParts)

    static CasCatalog load(ResourceIndex& index);

    // The entries that fit an age and gender, sorted by name.
    std::vector<const SimPart*> archetypesFor(uint32_t ageBit, uint32_t genderBit) const;
    std::vector<const SimPart*> modifiersFor(uint32_t ageBit, uint32_t genderBit) const;
    const SimPart* neutralFor(uint32_t ageBit, uint32_t genderBit) const;
    std::vector<const SimPart*> overlaysFor(uint32_t subtype, uint32_t ageBit, uint32_t genderBit) const;
    std::vector<const SimPart*> meshOverlaysFor(uint32_t ageBit, uint32_t genderBit) const;
    // The overlay of the same family in another hair colour (brows, facial
    // hair follow the hair), or `e` when there is none.
    const SimPart* recolor(const SimPart& e, std::string_view hairToneFamily) const;
    // Outfit parts (GZPS type "skin") with `partBit` in "outfit", the age,
    // gender and category bit, normal fitness unless `fat`; the Body Shop
    // editor's own copies ("CASIE_" names) and parts without a mesh are
    // left out, and with `casOnly` those with flag 0x01 (career and NPC
    // uniforms, the alien and mannequin skins and the per-category copies
    // of the hair: what CAS does not offer; read from the data, every such
    // part has it). Sorted by name.
    std::vector<const SimPart*> outfitParts(uint32_t partBit, uint32_t ageBit, uint32_t genderBit,
                                            uint32_t categoryBit = category::Everyday, bool fat = false,
                                            bool casOnly = true) const;
    // Hair styles (one part per style family) and the colour variants of one.
    std::vector<const SimPart*> hairStyles(uint32_t ageBit, uint32_t genderBit, std::string_view hairToneFamily) const;
    const SimPart* hairInTone(const SimPart& hair, std::string_view hairToneFamily) const;

    const SimPart* byFamily(const std::vector<SimPart>& list, std::string_view family) const;
    const SimPart* byName(const std::vector<SimPart>& list, std::string_view name) const; // case-insensitive
    const SimPart* skinTone(int tone) const; // 1..4 (family 0000000N)
    const SimPart* hairTone(int tone) const; // 1..5
};

// "0000000N-0000-0000-0000-000000000000"
std::string toneFamily(int n);
int toneNumber(std::string_view family); // 0 if not of that form
// The "s<N>" of a skin tone family's textures: 1-4 for 0000000N, 5 for the
// Alien family (6baf064a-...: its GZPS amtopnaked_alien and the other
// "*_alien" skin parts use the *_nude_s5 materials); 0 for others (the
// mannequins).
constexpr std::string_view kAlienSkinFamily = "6baf064a-85ad-4e37-8d81-a987e9f8da46";
int skinToneNumber(std::string_view family);

// The texture an overlay entry draws: its TXMT's stdMatBaseTextureName,
// else its name with '_' after the prefix turned into '-'.
std::string overlayTexture(ResourceIndex& index, const SimPart& overlay);

// The face skin texture of an age/gender prefix ("tf") and skin tone (1-4):
// the one the game's "<prefix>face_s<n>_txmt" names, else "<prefix>face-s<n>".
// Teen girls have no texture of their own: tfface_s4 uses "afface-s4".
std::string faceSkinTexture(ResourceIndex& index, const std::string& prefix, int skinTone);

} // namespace cas
} // namespace os2
