#pragma once

// The character file of a baby born in play (Make New Character's baby
// path, characters.cpp), as cTSCASSystem::ProcessCreateBabyRequest makes
// it (docs/cas-mac.md §17.4): the parents' LxNR face structures mixed
// region by region (FaceBaker::descendant with DetermineDescendantFace's
// regions), the hair colour, skin and eyes the genes express, a baby
// outfit drawn at random, written as a new <P>_User<NNNNN>.package with
// the baby's GUID; the LxNR goes with it, so the baby has its parents'
// face when it grows into a toddler (the Age primitive bakes that age's
// face from it).

#include "vm.hpp"

#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace os2 {
class ResourceIndex;
}

namespace os2::vm {

struct BabyRequest {
    int16_t baby = 0, parentA = 0, parentB = 0; // neighbor IDs (A: the first operand, the mother's side)
    bool female = false;
    std::map<std::string, std::string> dna;            // the baby's SDNA
    std::vector<std::pair<std::string, int>> regions;  // face region -> parent (0 = A, 1 = B)
};

// Called by Make New Character for every baby once its genes are drawn.
std::function<void(VM&, const BabyRequest&)>& babyCharacterWriter();

// Installs a writer that finds the parents' character files in
// `charactersDir` (by their OBJD GUIDs), and writes the babies' to
// `outDir` (created), numbered after the highest <P>_User file of either
// folder. `written` (optional) gets neighbor ID -> file.
void installBabyCharacterWriter(ResourceIndex& index, const std::filesystem::path& charactersDir,
                                const std::filesystem::path& outDir,
                                std::map<int16_t, std::filesystem::path>* written = nullptr);

} // namespace os2::vm
