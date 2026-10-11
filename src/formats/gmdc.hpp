#pragma once

#include "rcol.hpp"

#include <array>
#include <cstdint>
#include <ostream>
#include <span>
#include <string>
#include <vector>

namespace os2 {

// Element identities of cGeometryDataContainer.
namespace gmdc {
constexpr uint32_t Vertices = 0x5B830781;
constexpr uint32_t Normals = 0x3B83078B;
constexpr uint32_t UVs = 0xBB8307AB;
constexpr uint32_t BlendIndices = 0x1C4AFC56;
constexpr uint32_t BlendWeights = 0x5C4AFC5C;
constexpr uint32_t BoneAssignments = 0xFBD70111;
constexpr uint32_t BoneWeights = 0x3BD70105;
constexpr uint32_t TangentNormals = 0x89D92BA0;
constexpr uint32_t Binormals = 0x9BB38AFB;
constexpr uint32_t VertexMorphDeltas = 0x5CF2CFE1;
constexpr uint32_t MorphVertexMap = 0xDCF2CFDC; // u32 per vertex: morph numbers of the delta streams
constexpr uint32_t NormalMorphDeltas = 0xCB6F3A6A;
constexpr uint32_t Colors = 0xDB830795;      // D3DCOLOR per vertex (VertexAlpha ground shadows)
constexpr uint32_t ColorDeltas = 0xCB7206A1; // morph deltas of some stream (OpenTS2's table is unclear)

const char* elementName(uint32_t identity); // "" if unknown
} // namespace gmdc

// cGeometryDataContainer (GMDC): vertex streams, links between them, triangle
// groups, bone transforms and bounding meshes. Versions 1, 2 and 4; version 4
// stores index lists as u16, older versions as u32.
struct GeometryData {
    struct Element {
        uint32_t number = 0;   // count used by the game (not always the value count)
        uint32_t identity = 0;
        uint32_t repeat = 0;
        uint32_t blockFormat = 0; // 0: float, 1: float2, 2: float3, 4: u32
        uint32_t setFormat = 0;
        std::vector<uint32_t> raw; // values as 32-bit words, `components()` per value
        size_t rawOffset = 0;      // where `raw` starts in the bytes read (the block body), for patching
        std::vector<uint32_t> items;

        uint32_t components() const;
        size_t valueCount() const { return raw.size() / components(); }
        float f(size_t value, uint32_t component) const;
    };

    // A link selects the elements that make up one vertex buffer.
    struct Link {
        std::vector<uint32_t> elements; // indices into `elements`
        uint32_t referencedSize = 0;    // vertex count
        uint32_t activeElements = 0;
        std::vector<std::vector<uint32_t>> aliases; // for elements[0..2]: vertex -> value index (empty = identity)
    };

    struct Group {
        uint32_t primitiveType = 0; // 2 = triangle list
        int32_t linkIndex = -1;
        std::string name;
        std::vector<uint32_t> faces;
        uint32_t opacity = 0;
        std::vector<uint32_t> usedJoints;
    };

    struct Transform {
        std::array<float, 4> rotation{0, 0, 0, 1}; // quaternion x, y, z, w
        std::array<float, 3> translation{0, 0, 0};
    };

    struct BlendName {
        std::string blendGroup;
        std::string element;
    };

    struct SimpleMesh {
        std::vector<std::array<float, 3>> vertices;
        std::vector<uint32_t> faces;
    };

    uint32_t version = 0;
    SgResource resource;
    std::vector<Element> elements;
    std::vector<Link> links;
    std::vector<Group> groups;
    std::vector<Transform> transforms;
    std::vector<BlendName> blendNames;
    SimpleMesh boundingMesh;
    std::vector<SimpleMesh> joints;

    static GeometryData read(BinReader& r);
    static GeometryData parse(std::span<const uint8_t> body);
};

// A group resolved into flat vertex arrays.
struct MeshGroup {
    std::string name;
    std::vector<std::array<float, 3>> positions;
    std::vector<std::array<float, 3>> normals; // empty if the link has none
    std::vector<std::array<float, 2>> uvs;     // empty if the link has none
    std::vector<std::array<float, 2>> uvs2;    // light-map coordinates (lot surfaces), empty if none
    std::vector<uint32_t> colors;              // D3DCOLOR 0xAARRGGBB, empty if the link has none
    std::vector<uint32_t> triangles;
    // Skinning, empty if the link has no bone assignments: up to four bones
    // per vertex as GMDC bone ids (indices into GeometryData::transforms and
    // the CRES joint references; 0xFF = unused slot), with their weights.
    std::vector<std::array<uint8_t, 4>> bones;
    std::vector<std::array<float, 4>> weights;
    // Morphs (Sim faces, body fitness), empty if none: per vertex a u32 whose
    // byte k (low byte first) names the morph (index into morphNames, the
    // GMDC's blend names) that the k-th delta stream moves the vertex by;
    // unused streams have zero deltas (checked on the N001 faces: the
    // non-zero deltas of stream k match the non-zero byte k, not byte 3-k as
    // OpenTS2 reads it).
    std::vector<std::string> morphNames; // blend element names ("x_nose", "l_brow", ...)
    std::vector<uint32_t> morphMap;
    std::array<std::vector<std::array<float, 3>>, 4> morphDeltas;
};

// Triangle groups of a GMDC; skips groups that are not triangle lists.
std::vector<MeshGroup> buildMeshes(const GeometryData& g);

// Writes meshes as Wavefront OBJ, one `g` (and `usemtl` when a material is
// given) per group. OBJ indices are global and 1-based, so the writer keeps
// running counts of positions, texture coordinates and normals. Coordinates
// are converted from the game's Z-up space to OBJ's Y-up.
struct ObjWriter {
    std::ostream& out;
    size_t vBase = 0, vtBase = 0, vnBase = 0;

    void write(const MeshGroup& m, const std::string& material);
};

} // namespace os2
