#pragma once

#include "rcol.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace os2 {

// Scenegraph blocks of CRES (cResourceNode + cTransformNode tree + extensions),
// SHPE (cShape) and GMND (cGeometryNode). Each block has a read() that consumes
// exactly its bytes and a parse() for a whole block body.

using NodeRef = ObjectGraphNode::Extension; // {enabled, dependent, block index}

struct Quat {
    float x = 0, y = 0, z = 0, w = 1;
};
using Vec3 = std::array<float, 3>;

// cTransformNode: a joint/locator with children (block indices) and a local
// transform. Appears as a top-level CRES block and embedded in cBoundedNode.
struct TransformNode {
    uint32_t version = 0;
    CompositionTreeNode tree; // tree.graph.fileName is the node's name
    std::vector<NodeRef> children;
    Vec3 translation{0, 0, 0};
    Quat rotation;
    int32_t jointReference = 0x7FFFFFFF; // bone index, 0x7FFFFFFF if none

    static TransformNode read(BinReader& r);
    static TransformNode parse(std::span<const uint8_t> body);
    const std::string& name() const { return tree.graph.fileName; }
};

// cRenderableNode: a transform node plus the render groups it belongs to.
struct RenderableNode {
    uint32_t version = 0;
    uint32_t boundedVersion = 0; // cBoundedNode wrapper
    TransformNode transform;
    int16_t unknown1 = 0;
    std::vector<std::string> groups; // e.g. "Practical"
    int32_t unknown2 = 0;
    uint8_t unknown3 = 0;

    static RenderableNode read(BinReader& r); // reads the embedded header too
};

// cShapeRefNode: places shapes (SHPE links) at a transform, with optional
// morph blend weights (and, in version 21, the blend names).
struct ShapeRefNode {
    struct ShapeLink {
        uint16_t flags = 0;
        int32_t linkIndex = 0; // RCOL link index of the SHPE
    };
    uint32_t version = 0;
    RenderableNode renderable;
    std::vector<ShapeLink> shapes;
    int32_t unknown1 = 0;
    std::vector<uint32_t> blendValues;
    std::vector<std::string> blendNames; // version 21, one per blend value
    std::vector<uint8_t> data;
    int32_t unknown2 = -1;

    static ShapeRefNode read(BinReader& r);
    static ShapeRefNode parse(std::span<const uint8_t> body);
};

// cResourceNode: root of a CRES. typeCode 1 (objects): name + composition tree;
// typeCode 0 (lot-generated scenery): a bare graph node. `children` are the
// top-level transform blocks.
struct ResourceNode {
    uint32_t version = 0;
    uint8_t typeCode = 0;
    SgResource resource;      // typeCode 1
    CompositionTreeNode tree; // typeCode 1
    ObjectGraphNode graph;    // typeCode 0
    std::vector<NodeRef> children;
    uint8_t unknown1 = 0; // typeCode 1
    int32_t unknown2 = 0;

    static ResourceNode read(BinReader& r);
    static ResourceNode parse(std::span<const uint8_t> body);
    const ObjectGraphNode& rootGraph() const { return typeCode == 1 ? tree.graph : graph; }
};

// cExtension value tree (cDataListExtension and friends).
struct ExtensionValue {
    enum Type : uint8_t {
        Int = 0x02,
        Float = 0x03,
        Vector = 0x05,
        String = 0x06,
        Array = 0x07,
        Quaternion = 0x08,
        Binary = 0x09,
    };
    Type type = Int;
    std::string name;
    int32_t i = 0;
    std::array<float, 4> f{0, 0, 0, 0}; // Float uses f[0], Vector f[0..2]
    std::string s;
    std::vector<uint8_t> bytes;
    std::vector<ExtensionValue> items;

    static ExtensionValue read(BinReader& r);
    const ExtensionValue* find(std::string_view name) const; // child of an Array
};

struct DataListExtension {
    uint32_t version = 0;
    uint32_t extensionVersion = 0;
    ExtensionValue root;

    static DataListExtension read(BinReader& r);
    static DataListExtension parse(std::span<const uint8_t> body);
};

struct BoneDataExtension {
    uint32_t version = 0;
    uint32_t extensionVersion = 0;
    std::array<int32_t, 3> unknown{0, 0, 0};
    float unknownFloat = 0;
    Quat rotation; // version 4

    static BoneDataExtension read(BinReader& r);
    static BoneDataExtension parse(std::span<const uint8_t> body);
};

struct TagExtension {
    uint32_t version = 0;
    uint32_t extensionVersion = 0;
    std::string tag;

    static TagExtension read(BinReader& r);
    static TagExtension parse(std::span<const uint8_t> body);
};

// cLightRefNode: places a light (RCOL link to an LGHT resource) at a transform.
struct LightRefNode {
    uint32_t version = 0;
    RenderableNode renderable;
    uint16_t flags = 0;
    int32_t lightLink = 0;
    uint16_t unknown = 0;

    static LightRefNode read(BinReader& r);
    static LightRefNode parse(std::span<const uint8_t> body);
};

// cViewerRefNode / cViewerRefNodeRecursive: cameras and render-to-texture
// views (e.g. mirror reflections). The camera parameters are kept raw.
struct ViewerRefNode {
    uint32_t version = 0;
    uint32_t baseVersion = 0; // cViewerRefNodeBase
    RenderableNode renderable;
    std::vector<uint8_t> camera; // cViewerRefNode: the rest of the block

    static ViewerRefNode parse(std::span<const uint8_t> body); // size only known from the block bounds
};

struct ViewerRefNodeRecursive {
    uint32_t version = 0;
    uint32_t baseVersion = 0;
    RenderableNode renderable;
    uint8_t unknown = 0;
    std::string name; // e.g. "screenreflection"
    std::array<uint8_t, 64> data{};

    static ViewerRefNodeRecursive read(BinReader& r);
    static ViewerRefNodeRecursive parse(std::span<const uint8_t> body);
};

// Light resources (LGHT): cAmbientLight, cDirectionalLight, cPointLight and
// cSpotLight share cStandardLightBase + cLightT and differ in their parameters.
struct StandardLight {
    std::string className;
    uint32_t version = 0;
    uint32_t baseVersion = 0;
    SgResource resource;
    uint32_t lightTVersion = 0;
    SgResource lightTResource;
    ReferentNode referent;
    std::string name; // light node name, e.g. "gardenuplight_tree"
    // cStandardLightBase::ReadFrom (Mac BV 0xca2fb4): DiffuseScale,
    // RelSpecularScale, colour RGB; then a point (0xca3358): FalloffRate,
    // range; a spot (0xca3208): FalloffRate, cone angle, cone penumbra,
    // range. The range read is recomputed from the intensity at once
    // (OnLightRadiantPropertiesChanged, docs/lighting-mac.md 6).
    std::vector<float> values;

    static StandardLight read(std::string_view className, BinReader& r);
    static StandardLight parse(std::string_view className, std::span<const uint8_t> body);
};

// cShape (SHPE): geometry per level of detail and the material of each subset.
struct Shape {
    struct Item {
        int32_t lod = 0;
        uint8_t enabled = 0;
        uint8_t flag = 0;         // version < 8
        std::string geometryName; // GMND resource name (version 8)
        int32_t linkIndex = -1;   // RCOL link index of the GMND (version < 8)
    };
    struct Part {
        std::string subset;   // GMDC group name
        std::string material; // TXMT name without the "_txmt" suffix
        std::array<uint8_t, 9> data{};
    };
    uint32_t version = 0;
    SgResource resource;
    ReferentNode referent;
    std::vector<uint32_t> unknown; // version != 6
    std::vector<Item> items;
    std::vector<Part> parts;

    static Shape read(BinReader& r);
    static Shape parse(std::span<const uint8_t> body);
};

// cGeometryNode (GMND): points at a GMDC through the RCOL link list.
struct GeometryNode {
    uint32_t version = 0;
    ObjectGraphNode graph;
    SgResource resource;
    int16_t unknown1 = 0;
    uint8_t unknown2 = 0;
    uint32_t builderCount = 0;          // embedded legacy builders (version 5 only)
    std::vector<uint8_t> builders;      // their raw bytes, not decoded

    static GeometryNode read(BinReader& r); // throws if builders are present (size unknown)
    static GeometryNode parse(std::span<const uint8_t> body);
};

// Exact size of a known block class at the start of `body`, or npos when the
// class is unknown or its size cannot be determined without the next header.
size_t measureRcolBlock(std::string_view className, std::span<const uint8_t> body);

} // namespace os2
