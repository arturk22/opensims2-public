#include "scenegraph.hpp"

#include "gmdc.hpp"
#include "image.hpp"
#include "txmt.hpp"

namespace os2 {

namespace {

constexpr uint32_t kMaxCount = 0x10000;

uint32_t count(BinReader& r, const char* what) {
    const uint32_t n = r.u32();
    if (n > kMaxCount)
        throw FormatError(std::string("bad ") + what + " count");
    return n;
}

NodeRef readNodeRef(BinReader& r) {
    NodeRef n;
    n.enabled = r.u8();
    n.dependent = r.u8();
    n.index = r.u32();
    return n;
}

Quat readQuat(BinReader& r) {
    Quat q;
    q.x = r.f32();
    q.y = r.f32();
    q.z = r.f32();
    q.w = r.f32();
    return q;
}

// "cExtension" wrapper shared by the extension blocks: header + version.
uint32_t readExtensionHeader(BinReader& r) {
    readEmbeddedHeader(r, "cExtension");
    return r.u32();
}

template <class T>
T parseWhole(std::span<const uint8_t> body, const char* what) {
    BinReader r(body);
    auto v = T::read(r);
    r.expectEnd(what);
    return v;
}

} // namespace

TransformNode TransformNode::read(BinReader& r) {
    TransformNode t;
    t.version = r.u32();
    t.tree = CompositionTreeNode::read(r);
    t.children.resize(count(r, "transform child"));
    for (auto& c : t.children)
        c = readNodeRef(r);
    t.translation = {r.f32(), r.f32(), r.f32()};
    t.rotation = readQuat(r);
    t.jointReference = r.i32();
    return t;
}

TransformNode TransformNode::parse(std::span<const uint8_t> body) { return parseWhole<TransformNode>(body, "cTransformNode"); }

RenderableNode RenderableNode::read(BinReader& r) {
    readEmbeddedHeader(r, "cRenderableNode");
    RenderableNode n;
    n.version = r.u32();
    readEmbeddedHeader(r, "cBoundedNode");
    n.boundedVersion = r.u32();
    readEmbeddedHeader(r, "cTransformNode");
    n.transform = TransformNode::read(r);
    n.unknown1 = r.i16();
    n.groups.resize(count(r, "render group"));
    for (auto& g : n.groups)
        g = r.str7();
    n.unknown2 = r.i32();
    n.unknown3 = r.u8();
    return n;
}

ShapeRefNode ShapeRefNode::read(BinReader& r) {
    ShapeRefNode s;
    s.version = r.u32();
    s.renderable = RenderableNode::read(r);
    s.shapes.resize(count(r, "shape ref"));
    for (auto& l : s.shapes) {
        l.flags = r.u16();
        l.linkIndex = r.i32();
    }
    s.unknown1 = r.i32();
    s.blendValues.resize(count(r, "blend value"));
    for (auto& v : s.blendValues)
        v = r.u32();
    if (s.version >= 21) {
        s.blendNames.resize(s.blendValues.size());
        for (auto& n : s.blendNames)
            n = r.str7();
    }
    auto d = r.bytes(count(r, "shape ref data"));
    s.data.assign(d.begin(), d.end());
    s.unknown2 = r.i32();
    return s;
}

ShapeRefNode ShapeRefNode::parse(std::span<const uint8_t> body) { return parseWhole<ShapeRefNode>(body, "cShapeRefNode"); }

ResourceNode ResourceNode::read(BinReader& r) {
    ResourceNode n;
    n.version = r.u32();
    n.typeCode = r.u8();
    if (n.typeCode == 1) {
        n.resource = SgResource::read(r);
        n.tree = CompositionTreeNode::read(r);
        n.children.resize(count(r, "resource node child"));
        for (auto& c : n.children)
            c = readNodeRef(r);
        n.unknown1 = r.u8();
    } else if (n.typeCode == 0) {
        n.graph = ObjectGraphNode::read(r);
        n.children.resize(r.u8());
        for (auto& c : n.children) {
            c.enabled = r.u8();
            c.index = r.u32();
        }
    } else {
        throw FormatError("cResourceNode: unknown type code " + std::to_string(n.typeCode));
    }
    n.unknown2 = r.i32();
    return n;
}

ResourceNode ResourceNode::parse(std::span<const uint8_t> body) { return parseWhole<ResourceNode>(body, "cResourceNode"); }

ExtensionValue ExtensionValue::read(BinReader& r) {
    ExtensionValue v;
    v.type = Type(r.u8());
    v.name = r.str7();
    switch (v.type) {
    case Int: v.i = r.i32(); break;
    case Float: v.f[0] = r.f32(); break;
    case Vector: v.f = {r.f32(), r.f32(), r.f32(), 0}; break;
    case String: v.s = r.str7(); break;
    case Array:
        v.items.resize(count(r, "extension array"));
        for (auto& it : v.items)
            it = read(r);
        break;
    case Quaternion: v.f = {r.f32(), r.f32(), r.f32(), r.f32()}; break;
    case Binary: {
        auto b = r.bytes(count(r, "extension binary"));
        v.bytes.assign(b.begin(), b.end());
        break;
    }
    default: throw FormatError("cExtension: unknown value type " + std::to_string(int(v.type)));
    }
    return v;
}

const ExtensionValue* ExtensionValue::find(std::string_view n) const {
    for (const auto& it : items)
        if (it.name == n)
            return &it;
    return nullptr;
}

DataListExtension DataListExtension::read(BinReader& r) {
    DataListExtension d;
    d.version = r.u32();
    d.extensionVersion = readExtensionHeader(r);
    d.root = ExtensionValue::read(r);
    return d;
}

DataListExtension DataListExtension::parse(std::span<const uint8_t> body) {
    return parseWhole<DataListExtension>(body, "cDataListExtension");
}

BoneDataExtension BoneDataExtension::read(BinReader& r) {
    BoneDataExtension b;
    b.version = r.u32();
    b.extensionVersion = readExtensionHeader(r);
    for (auto& x : b.unknown)
        x = r.i32();
    b.unknownFloat = r.f32();
    if (b.version >= 4)
        b.rotation = readQuat(r);
    return b;
}

BoneDataExtension BoneDataExtension::parse(std::span<const uint8_t> body) {
    return parseWhole<BoneDataExtension>(body, "cBoneDataExtension");
}

TagExtension TagExtension::read(BinReader& r) {
    TagExtension t;
    t.version = r.u32();
    t.extensionVersion = readExtensionHeader(r);
    t.tag = r.str7();
    return t;
}

TagExtension TagExtension::parse(std::span<const uint8_t> body) { return parseWhole<TagExtension>(body, "cTagExtension"); }

LightRefNode LightRefNode::read(BinReader& r) {
    LightRefNode n;
    n.version = r.u32();
    n.renderable = RenderableNode::read(r);
    n.flags = r.u16();
    n.lightLink = r.i32();
    n.unknown = r.u16();
    return n;
}

LightRefNode LightRefNode::parse(std::span<const uint8_t> body) { return parseWhole<LightRefNode>(body, "cLightRefNode"); }

ViewerRefNode ViewerRefNode::parse(std::span<const uint8_t> body) {
    BinReader r(body);
    ViewerRefNode n;
    n.version = r.u32();
    readEmbeddedHeader(r, "cViewerRefNodeBase");
    n.baseVersion = r.u32();
    n.renderable = RenderableNode::read(r);
    auto rest = r.bytes(r.remaining());
    n.camera.assign(rest.begin(), rest.end());
    return n;
}

ViewerRefNodeRecursive ViewerRefNodeRecursive::read(BinReader& r) {
    ViewerRefNodeRecursive n;
    n.version = r.u32();
    readEmbeddedHeader(r, "cViewerRefNodeBase");
    n.baseVersion = r.u32();
    n.renderable = RenderableNode::read(r);
    n.unknown = r.u8();
    n.name = r.str7();
    auto d = r.bytes(n.data.size());
    std::copy(d.begin(), d.end(), n.data.begin());
    return n;
}

ViewerRefNodeRecursive ViewerRefNodeRecursive::parse(std::span<const uint8_t> body) {
    return parseWhole<ViewerRefNodeRecursive>(body, "cViewerRefNodeRecursive");
}

StandardLight StandardLight::read(std::string_view cls, BinReader& r) {
    size_t values;
    if (cls == "cAmbientLight" || cls == "cDirectionalLight")
        values = 5;
    else if (cls == "cPointLight")
        values = 7;
    else if (cls == "cSpotLight")
        values = 9;
    else
        throw FormatError("unknown light class " + std::string(cls));
    StandardLight l;
    l.className = cls;
    l.version = r.u32();
    readEmbeddedHeader(r, "cStandardLightBase");
    l.baseVersion = r.u32();
    l.resource = SgResource::read(r);
    readEmbeddedHeader(r, "cLightT");
    l.lightTVersion = r.u32();
    l.lightTResource = SgResource::read(r);
    l.referent = ReferentNode::read(r);
    l.name = r.str7();
    l.values.resize(values);
    for (auto& v : l.values)
        v = r.f32();
    return l;
}

StandardLight StandardLight::parse(std::string_view cls, std::span<const uint8_t> body) {
    BinReader r(body);
    auto l = read(cls, r);
    r.expectEnd(l.className.c_str());
    return l;
}

Shape Shape::read(BinReader& r) {
    Shape s;
    s.version = r.u32();
    s.resource = SgResource::read(r);
    s.referent = ReferentNode::read(r);
    if (s.version != 6) {
        s.unknown.resize(count(r, "shape unknown"));
        for (auto& x : s.unknown)
            x = r.u32();
    }
    s.items.resize(count(r, "shape item"));
    for (auto& it : s.items) {
        it.lod = r.i32();
        it.enabled = r.u8();
        if (s.version >= 8) {
            it.geometryName = r.str7();
        } else {
            it.flag = r.u8();
            it.linkIndex = r.i32();
        }
    }
    s.parts.resize(count(r, "shape part"));
    for (auto& p : s.parts) {
        p.subset = r.str7();
        p.material = r.str7();
        auto d = r.bytes(p.data.size());
        std::copy(d.begin(), d.end(), p.data.begin());
    }
    return s;
}

Shape Shape::parse(std::span<const uint8_t> body) { return parseWhole<Shape>(body, "cShape"); }

GeometryNode GeometryNode::read(BinReader& r) {
    GeometryNode g;
    g.version = r.u32();
    g.graph = ObjectGraphNode::read(r);
    g.resource = SgResource::read(r);
    if (g.version >= 11) {
        g.unknown1 = r.i16();
        g.unknown2 = r.u8();
    }
    g.builderCount = count(r, "geometry builder");
    if (g.builderCount)
        throw FormatError("cGeometryNode: embedded geometry builders have no known size");
    return g;
}

GeometryNode GeometryNode::parse(std::span<const uint8_t> body) {
    BinReader r(body);
    GeometryNode g;
    try {
        g = read(r);
        r.expectEnd("cGeometryNode");
    } catch (const FormatError&) {
        // Version 5 nodes embed cGeometryBuilder/cIndexedMeshBuilder data; keep it raw.
        BinReader again(body);
        g.version = again.u32();
        g.graph = ObjectGraphNode::read(again);
        g.resource = SgResource::read(again);
        if (g.version >= 11) {
            g.unknown1 = again.i16();
            g.unknown2 = again.u8();
        }
        g.builderCount = count(again, "geometry builder");
        if (!g.builderCount)
            throw;
        auto rest = again.bytes(again.remaining());
        g.builders.assign(rest.begin(), rest.end());
    }
    return g;
}

size_t measureRcolBlock(std::string_view cls, std::span<const uint8_t> body) {
    BinReader r(body);
    try {
        if (cls == "cTransformNode") TransformNode::read(r);
        else if (cls == "cShapeRefNode") ShapeRefNode::read(r);
        else if (cls == "cResourceNode") ResourceNode::read(r);
        else if (cls == "cDataListExtension") DataListExtension::read(r);
        else if (cls == "cBoneDataExtension") BoneDataExtension::read(r);
        else if (cls == "cTagExtension") TagExtension::read(r);
        else if (cls == "cLightRefNode") LightRefNode::read(r);
        else if (cls == "cViewerRefNodeRecursive") ViewerRefNodeRecursive::read(r);
        else if (cls == "cAmbientLight" || cls == "cDirectionalLight" || cls == "cPointLight" || cls == "cSpotLight")
            StandardLight::read(cls, r);
        else if (cls == "cShape") Shape::read(r);
        else if (cls == "cGeometryNode") GeometryNode::read(r);
        else if (cls == "cImageData") ImageData::read(r);
        else if (cls == "cLevelInfo") LevelInfo::read(r);
        else if (cls == "cMaterialDefinition") MaterialDefinition::read(r);
        else if (cls == "cGeometryDataContainer") GeometryData::read(r);
        else return std::string::npos;
    } catch (const FormatError&) {
        return std::string::npos;
    }
    return r.pos();
}

} // namespace os2
