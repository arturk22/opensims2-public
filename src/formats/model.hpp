#pragma once

#include "gmdc.hpp"
#include "txmt.hpp"

#include <array>
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
        int mask = -1; // index into masks: multiplies the part's alpha
    };

    // Alpha mask laid over the model's XY plane: value (x, y) sits at
    // model-space (x, y) * spacing, interpolated in between.
    struct AlphaMask {
        uint32_t width = 0, height = 0;
        float spacing = 1.0f;
        std::vector<uint8_t> values; // row-major: values[y * width + x]
    };

    std::string name; // CRES resource name
    std::vector<Part> parts;
    std::vector<AlphaMask> masks;
    std::map<std::string, MaterialDefinition> materials; // by material name
    std::vector<std::string> warnings;                   // references that could not be resolved
};

// `cresName` may omit the "_cres" suffix. `materials` (subset -> material)
// replaces the SHPE's material of those subsets, e.g. with the design picked
// for an object placed on a lot.
ObjectModel loadObjectModel(ResourceIndex& index, std::string_view cresName,
                            const std::map<std::string, std::string>& materials = {});

// CRES name of an object: OBJD -> "3D Object Names" STR# (instance 0x85, same
// group) -> entry modelIndex(), in US English (other languages hold junk).
// Empty if the object has no 3D model.
std::string objectCresName(ResourceIndex& index, const Tgi& objd);

// CRES name for an object given as a CRES name (with or without "_cres"), an
// OBJD GUID ("0x...") or part of an OBJD file name (case-insensitive). If
// `match` is given it receives a description of the object found. Throws when
// nothing matches.
std::string resolveObjectCres(ResourceIndex& index, std::string_view query, std::string* match = nullptr);

// TXTR name of a material's base texture ("..._txtr"), or empty.
std::string baseTextureName(const MaterialDefinition& m);

// The StandardMaterial properties the renderer uses, with the game's defaults
// for missing ones.
struct StandardMaterial {
    enum class Blend { None, Alpha, Additive };
    enum class Cull { Clockwise, CounterClockwise, None }; // which winding is culled

    std::string baseTexture; // TXTR name, empty if untextured
    std::array<float, 3> diffuse{1, 1, 1};
    std::array<float, 3> specular{0, 0, 0};
    float specularPower = 20.0f; // most common value; absent in about a third of the materials
    std::array<float, 3> emissive{0, 0, 0};
    float alphaMultiplier = 1.0f;
    float untexturedAlpha = 1.0f; // diffuse alpha when there is no base texture
    Blend blend = Blend::None;
    bool alphaTest = false;
    float alphaRef = 127.0f / 255.0f;
    Cull cull = Cull::Clockwise;
    bool lighting = true;
    int layer = 0;
    bool visible = true; // false for "Null" and "WallMask" materials (parts the game does not draw)
};
StandardMaterial standardMaterial(const MaterialDefinition& m);

} // namespace os2
