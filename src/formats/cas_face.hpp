#pragma once

// Baking a Sim's face from Create-a-Sim choices (docs/cas-mac.md §3): an
// archetype head plus weighted face modifiers, laid over the neutral face of
// the Sim's age and gender (the mesh with the bones and the "faceblends"
// morphs the animations use), as the game exports it into the character
// file (cTSCASSystem::ExportFace).
//
// What the data shows (CASFace.package, Skins.package):
// - every face mesh (neutral faces, archetypes, modifiers, masks) is the same
//   head of ~521 vertices; groups and vertex order differ between files, so
//   vertices are matched by texture coordinate (and, where a coordinate is
//   shared by several vertices, by the nearest position);
// - an archetype is an absolute head for its age and gender (amArchBase is
//   amFace vertex for vertex); the archetypes of one "family" GUID are the
//   same head at each age;
// - a modifier is a whole head too, unisex ("uuMod..."), whose difference
//   from the neutral face of its compareage/comparegender (afFace for all
//   of the base game's) is the slider's direction; the weight runs over
//   negativelimit..positivelimit (±1.5) and is scaled by negativescale or
//   positivescale on that side (inferred: the scales are < 1 mostly on one
//   side, e.g. "Ear Size" 0.5/0.25, as if to keep the extremes sane).
// The baked face = the neutral face's vertices moved to the archetype's,
// plus sum(weight * scale * (modifier - its neutral)); vertices no head
// covers (the neutral's eyelashes) move with the nearest covered vertex.
// Normals follow the change of the surface normals.

#include "cas_catalog.hpp"
#include "gmdc.hpp"
#include "lxnr.hpp"

#include <array>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace os2 {

class ResourceIndex;

namespace cas {

struct FaceSpec {
    std::string archetype;                  // facearchetype family (lowercase GUID); empty = the neutral face
    std::map<std::string, float> modifiers; // facemodifier family -> slider weight
    // A region taken from another archetype (the "Change Eyes", "Change
    // Nose", ... bins): the bin's region mask list ("eyes;cheekbones", the
    // panel's kCAFArchetypeRegionMask) -> archetype family. The faceregion
    // masks are partial heads whose vertex colours weigh the blend (inferred:
    // the largest colour channel / 255).
    std::map<std::string, std::string> regions;
    // A saved face (the character file's LxNR): its deltas for the age and
    // gender replace the archetype head (regions and modifiers still apply
    // on top). Picking an archetype in CAS drops it.
    std::shared_ptr<const FaceStructure> structure;
};

class FaceBaker {
public:
    FaceBaker(ResourceIndex& index, const CasCatalog& catalog);
    ~FaceBaker();

    struct Baked {
        std::string neutral;       // neutral face name ("afFace")
        std::vector<uint8_t> gmdc; // the neutral's GMDC resource with positions and normals replaced
        size_t vertices = 0, moved = 0, matched = 0;
        // Per vertex of the neutral mesh: (baked - neutral position, uv),
        // what the LxNR keeps for this age, gender and detail.
        std::vector<FaceStructure::Vertex> structure;
        std::vector<std::string> warnings;
    };
    // The face for `spec` at an age and gender (its archetype family's head
    // of that age, else the neutral face).
    // `lowDetail`: the neutral face's low level of detail (its SHPE's other
    // LOD), for the LxNR's 0xF0000 keys.
    Baked bake(const FaceSpec& spec, uint32_t ageBit, uint32_t genderBit, bool lowDetail = false);
    // The LxNR of a face: every age the archetype (or the structure) has a
    // head for, both genders' keys as the game writes them, both levels of
    // detail.
    FaceStructure structureOf(const FaceSpec& spec, uint32_t genderBit);

    // The archetype of a family at an age and gender, if the catalog has one.
    const SimPart* archetypeFor(const std::string& family, uint32_t ageBit, uint32_t genderBit) const;

private:
    struct Mesh;
    const Mesh& meshByName(const std::string& gmdcName);
    const Mesh* meshOfEntry(const SimPart& entry, bool lowDetail = false); // its SHPE's LOD 0 (or other) GMDC

    ResourceIndex& index_;
    const CasCatalog& catalog_;
    std::map<std::string, std::unique_ptr<Mesh>> meshes_;
};

} // namespace cas
} // namespace os2
