#pragma once

#include "gmdc.hpp"
#include "txmt.hpp"

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace os2 {

class ResourceIndex;

// The renderable content of an object's CRES, resolved across packages:
// CRES -> cShapeRefNode -> SHPE (lowest LOD) -> GMND -> GMDC groups, with each
// group's material taken from the SHPE parts and resolved to its TXMT.
//
// Object shapes are rigid: their GMDC vertices are local to the scenegraph
// node that references them, so each part is moved into object space with the
// node's transform composed with all of its ancestors' (rest pose).
struct ObjectModel {
    struct Part {
        std::string node;     // cShapeRefNode name
        std::string shape;    // SHPE resource name
        std::string material; // TXMT name without "_txmt", empty if none
        MeshGroup mesh;
    };

    std::string name; // CRES resource name
    std::vector<Part> parts;
    std::map<std::string, MaterialDefinition> materials; // by material name
    std::vector<std::string> warnings;                   // references that could not be resolved
};

// `cresName` may omit the "_cres" suffix.
ObjectModel loadObjectModel(ResourceIndex& index, std::string_view cresName);

// CRES name of an object: OBJD -> "3D Object Names" STR# (instance 0x85, same
// group) -> entry modelIndex(), in US English (other languages hold junk).
// Empty if the object has no 3D model.
std::string objectCresName(ResourceIndex& index, const Tgi& objd);

// TXTR name of a material's base texture ("..._txtr"), or empty.
std::string baseTextureName(const MaterialDefinition& m);

} // namespace os2
