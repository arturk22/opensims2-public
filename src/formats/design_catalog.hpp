#pragma once

// The design choices of objects (Buy and Build mode's Design tool,
// cTSUserToolObjectDesign / cTSWinProcObjectDesignDialog): the material
// overrides (MMAT, type 0x4C697E5A, binary property sets in
// Catalog/Materials/Materials.package, 2869 in the base game) name, for a
// model ("modelName", a CRES) and one of its subsets ("subsetName"), a
// material that may stand in for the model's own ("name"), in a colour
// family ("family"; the same family across states and subsets), with
// "defaultMaterial" on the one the model ships with and
// "materialStateFlags" for the dirty/broken variants the objects switch to
// themselves. The game's dialog shows up to two subsets
// (nTSSG::GetDesignModeInfo: the first two design-modable subsets of the
// model's shapes) as two rows of swatches; an object with none is "not
// designable" (UIText STR# 0x8D #15).

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace os2 {

class ResourceIndex;

struct DesignMaterial {
    std::string model;    // CRES name, lower case, without "_cres"
    std::string subset;   // lower case
    std::string material; // TXMT name without "_txmt"
    std::string family;
    uint32_t objectGuid = 0;
    uint32_t stateFlags = 0; // materialStateFlags: 0 clean, 1/2 the variants
    bool isDefault = false;
};

class DesignCatalog {
public:
    static DesignCatalog load(ResourceIndex& index);
    // From MMAT property sets already read (tests).
    void add(const std::map<std::string, std::string>& properties);

    // The subsets of a model that have design choices, in the order given
    // (the model's subsets, as its shapes list them), at most `limit`.
    std::vector<std::string> subsets(const std::string& model, const std::vector<std::string>& order,
                                     size_t limit = 2) const;
    // The choices for one subset: one per material (clean state only), the
    // default first, then by name.
    std::vector<const DesignMaterial*> options(const std::string& model, const std::string& subset) const;
    size_t size() const { return all_.size(); }

    // "SofaMoroccan_cres" -> "sofamoroccan".
    static std::string key(std::string name);

private:
    std::vector<DesignMaterial> all_;
    std::map<std::string, std::vector<size_t>> byModel_;
};

} // namespace os2
