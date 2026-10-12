#pragma once

// A family made in Create-a-Sim, added to a neighborhood the way the game
// keeps one (docs/cas-mac.md §7, formats/neighborhood.hpp): a character file
// per Sim in Characters/<P>_User<NNNNN>.package (the next free numbers), and
// in <P>_Neighborhood.package an SDSC per Sim (instance = a new neighbor ID,
// version 0x20 with the base game's header words 0x70/0x20/3 and trailer 1
// for family members), its SDNA, a FAMI (a new family number, no lot,
// kDefaultFamilyFunds = 20000 from CB980E50's constants, flags 1, the
// members' GUIDs), the family's STR# (name, description) and an empty
// inventory for the family and each Sim in the NGBH. Everything else of the
// package is copied byte for byte.
//
// Person data written (the rest 0), as the shipped families have them:
// traits 2/3/5/6/7 (0..1000) and their copies 47-51 (neat, nice, active,
// outgoing, playful: equal in every shipped Sim), aspiration 46, age 58,
// gender 65 (1 female), family 61, zodiac 70, body flags 81, fatness 82,
// days left in the age 91 and 156 (equal in the shipped Sims; 25 here,
// inferred), 118 = 3100, 158 = 1 (constant in the shipped Sims), the 17
// interests 124-140 drawn 0..1000 (inferred: the shipped values look random).

#include "formats/cas_personality.hpp"
#include "formats/dbpf.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace os2::vm {

struct NewSim {
    std::string firstName, lastName;
    uint32_t ageBit = 8, genderBit = 2;
    int fitness = 0; // 0 normal, 2 fat
    cas::Personality personality;
    uint32_t aspiration = 0;
    int zodiac = 1;
    std::map<std::string, std::string> dna; // SDNA
    // The family tree from this Sim's side: (index of the other Sim in the
    // family, tie type as formats/neighborhood.hpp's TieType: 0/1 my parent
    // is, 2 I'm married to, 3 my sibling is, 4 my child is). Written into
    // the neighborhood's family ties (0x8C870743).
    std::vector<std::pair<size_t, uint32_t>> ties;
    // The Sim's clothes for the family's clothing bin (GZPS keys), as
    // cTSFamily::AddClothingFromOutfitData takes them from the Sim's outfits:
    // the tops (4), bottoms (0x10) and whole outfits (8) of every category
    // but 0x002, 0x004, 0x080 (naked), 0x400 (try-on) and 0x800.
    std::vector<Tgi> clothing;
    // Writes the Sim's character file to `out` with the object name given
    // ("N001_User00012"); returns the OBJD GUID.
    std::function<uint32_t(const std::string& objectName, const std::filesystem::path& out)> writeCharacter;
};

// A family's STR# (name, description), in US English and Brazilian
// Portuguese, as the CAS families have it.
std::vector<uint8_t> writeFamilyStrings(const std::vector<std::string>& values);

struct FamilyWriteResult {
    std::filesystem::path neighborhood; // the folder written
    uint32_t family = 0;
    std::vector<uint16_t> neighbors;
    std::vector<std::filesystem::path> characters;
    std::vector<std::string> warnings;
};

// Adds the family to the neighborhood folder `target`, which is first made a
// copy of `source` when it does not exist yet (target == source is
// refused: the shipped neighborhoods are never written). `seed` draws the
// interests.
// `inPlace` lets target == source (the user's own copy of a neighborhood,
// never the shipped data: the caller checks).
FamilyWriteResult writeNewFamily(const std::filesystem::path& source, const std::filesystem::path& target,
                                 const std::string& familyName, const std::string& description,
                                 std::vector<NewSim> sims, uint32_t seed = 1, bool inPlace = false);

// Adds the family's photo (a PNG) to <P>_FamilyThumbnails.package under
// `neighborhood`/Thumbnails (type 0x8C3CE95A, instance = family id), making
// the package when it is not there; the other pictures are kept as stored.
void writeFamilyThumbnail(const std::filesystem::path& neighborhood, uint32_t family,
                          const std::vector<uint8_t>& png);

// The user's neighborhoods folder the app saves to when nothing else is
// given: ~/Documents/opensims2/Neighborhoods (the game uses
// ~/Documents/EA Games/The Sims 2/Neighborhoods).
std::filesystem::path defaultUserNeighborhoods();

} // namespace os2::vm
