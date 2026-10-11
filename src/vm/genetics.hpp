#pragma once

// Sim genetics (nTSGenetics, docs/cas-mac.md §5): a Sim's DNA, the genes a
// baby gets from two parents (cTSCASSystem::ProcessCreateBabyRequest ->
// DetermineGenetics -> PickGene) and the face it inherits region by region
// (DetermineDescendantFace); the dominant face regions a new Sim draws
// (SetupDominantRegions). Plain data and a random source: no VM, no
// catalog (the catalog's "genetic" values come in through a callback).
//
// The DNA is the SDNA property set (0xEBFEE33F in the neighborhood,
// instance = neighbor ID): key "<t>" = the dominant allele of gene type t,
// key "<0x10000000 + t>" (decimal) = the recessive one. Gene types: 1 hair
// colour (hairtone family), 2 skin tone (skintone family), 3 eye colour
// (the eye overlay's family), 4 the face (not drawn), 5 the dominant face
// regions ("nose,brow,cheek,jaw,"), 6 the skin tone a baby got between its
// parents'; 7-10 and 12-14 are drawn 50/50, 11 is not drawn.

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace os2::genetics {

namespace gene {
constexpr uint32_t Hair = 1, Skin = 2, Eyes = 3, Face = 4, DominantRegions = 5, SkinBetween = 6;
constexpr uint32_t Recessive = 0x10000000;
} // namespace gene

using Allele = std::pair<std::string, std::string>; // dominant, recessive

struct Dna {
    std::map<uint32_t, Allele> alleles;

    static Dna fromProperties(const std::map<std::string, std::string>& sdna);
    std::map<std::string, std::string> toProperties() const;

    bool has(uint32_t type) const { return alleles.count(type) != 0; }
    const std::string& dominant(uint32_t type) const;
    // What shows: the dominant allele (GetExpressedHairColor); for the skin
    // the in-between tone of gene 6 when there is one (inferred: the doc
    // leaves open how gene 6 becomes the shown tone).
    std::string expressed(uint32_t type) const;
    // The dominant regions as a list.
    std::vector<std::string> dominantRegions() const;
};

// A uniform integer in [0, n).
using Random = std::function<uint32_t(uint32_t n)>;
// The catalog's "genetic" value of a family for a gene type (hair tone,
// skin tone or eye overlay); 0 when unknown.
using GeneticValue = std::function<float(uint32_t type, const std::string& family)>;

// A new (non-baby) Sim's DNA from its own colours (SetFromOutfitData with
// flags 0xD: the same allele on both sides) plus its dominant regions.
Dna dnaFromLook(const std::string& hairFamily, const std::string& skinFamily, const std::string& eyeFamily,
                bool female, const Random& random);

// SetupDominantRegions: each of nose, brow, mouth, cheek, cheekbones, eyes,
// jaw, ears is dominant when rand(100) >= its threshold (women / men):
// nose 50/66, brow 50/66, mouth 66/50, cheek 50/50, cheekbones 66/50, eyes
// 66/50, jaw 50/66, ears 50/50. Returns "nose,brow,...," (trailing comma,
// as the saved DNA has it).
std::string setupDominantRegions(bool female, const Random& random);

// PickGene for one gene type. `skinTones`: (family, genetic) of the skin
// tones a baby can get between two tones; for the skin the in-between
// tone goes to `between` (gene 6).
struct SkinTone {
    std::string family;
    float genetic = 0;
};
Allele pickGene(uint32_t type, const Allele& mother, const Allele& father, const GeneticValue& value,
                const Random& random, const std::vector<SkinTone>& skinTones = {}, std::string* between = nullptr);

// DetermineGenetics: every gene type 1-15 either parent has, but 4, 5 and
// 11 (5 comes from the descendant face).
Dna determineGenetics(const Dna& a, const Dna& b, const GeneticValue& value, const Random& random,
                      const std::vector<SkinTone>& skinTones);

// DetermineDescendantFace: for each of nose, brow, mouth, eyes, jaw, ears a
// parent (0 = a, 1 = b): rand(100); a region dominant only in b goes to b
// when r <= 66, otherwise 50/50 (r < 50 -> a; as read, a region dominant
// only in a is 50/50 too). The child's region is dominant when it was in
// the parent chosen. "eyes" brings "cheekbones" along, "mouth" "cheek".
struct DescendantFace {
    std::vector<std::pair<std::string, int>> regions; // region -> parent index
    std::string dominant;                             // the child's gene 5
    std::string script() const;                       // "nose,0;brow,1;..."
};
DescendantFace determineDescendantFace(const Dna& a, const Dna& b, const Random& random);

// The base game catalog's "genetic" values (Skins.package: hair tones,
// skin tones, the eye overlays), for callers without the catalog loaded.
GeneticValue baseGameGeneticValues();
std::vector<SkinTone> baseGameSkinTones();

// A baby's gender: a single parent's (both the same Sim), else a coin flip;
// true = female.
bool babyIsFemale(bool singleParent, bool parentFemale, const Random& random);

} // namespace os2::genetics
