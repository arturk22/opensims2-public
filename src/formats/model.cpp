#include "model.hpp"
#include "formats/log_once.hpp"

#include "graphics_options.hpp"

#include "resindex.hpp"
#include "scenegraph.hpp"
#include "simantics.hpp"
#include "strings.hpp"
#include "xform.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <optional>

namespace os2 {

namespace {

constexpr uint32_t kTypeCres = 0xE519C933, kTypeShpe = 0xFC6EB1F7, kTypeGmnd = 0x7BA3838C, kTypeGmdc = 0xAC4F8687,
                   kTypeTxmt = 0x49596978, kTypeObjd = 0x4F424A44;

// Design choice for an object's ground shadow subset: no shadow (there is no
// such TXMT; inferred from the name and from where it is used).
constexpr std::string_view kShadowOff = "guoboff";

// A ground shadow plane: the subsets "groundshadow" and "*wallshadow" (as
// app/pie_menu's picking leaves out), hidden when the Shadows option has no guob.
bool shadowPlane(const std::string& mesh, const std::string&) {
    auto low = [](std::string v) {
        for (auto& c : v)
            c = char(std::tolower(static_cast<unsigned char>(c)));
        return v;
    };
    return low(mesh).find("shadow") != std::string::npos;
}

bool endsWith(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}

std::string withSuffix(std::string_view name, std::string_view suffix) {
    std::string s(name);
    if (!endsWith(s, suffix))
        s += suffix;
    return s;
}

// RCOL links of the CRES/SHPE carry the full TGI; older ones leave instanceHi out.
std::optional<ResourceIndex::Location> findLink(ResourceIndex& index, Tgi tgi) {
    if (auto loc = index.find(tgi))
        return loc;
    tgi.instanceHi = 0;
    return index.find(tgi);
}

const Rcol::Block& firstBlock(const Rcol& rc, std::string_view cls, const std::string& what) {
    const auto* b = rc.find(cls);
    if (!b)
        throw FormatError(what + " has no " + std::string(cls));
    return *b;
}

// Local transforms and parent links of the CRES blocks that form the node tree.
struct NodeTree {
    std::vector<std::optional<Xform>> local;
    std::vector<int> parent;

    explicit NodeTree(const Rcol& cres) : local(cres.blocks.size()), parent(cres.blocks.size(), -1) {
        auto link = [&](size_t from, const std::vector<NodeRef>& children) {
            for (const auto& c : children)
                if (c.index < parent.size())
                    parent[c.index] = int(from);
        };
        auto setLocal = [&](size_t i, const TransformNode& t) {
            local[i] = Xform{t.rotation, t.translation};
            link(i, t.children);
        };
        for (size_t i = 0; i < cres.blocks.size(); ++i) {
            const auto& b = cres.blocks[i];
            if (b.className == "cTransformNode")
                setLocal(i, TransformNode::parse(b.body));
            else if (b.className == "cShapeRefNode")
                setLocal(i, ShapeRefNode::parse(b.body).renderable.transform);
            else if (b.className == "cLightRefNode")
                setLocal(i, LightRefNode::parse(b.body).renderable.transform);
            else if (b.className == "cResourceNode")
                link(i, ResourceNode::parse(b.body).children);
        }
    }

    Xform world(size_t i) const {
        Xform w;
        for (int n = int(i), guard = 0; n >= 0 && guard < 256; n = parent[size_t(n)], ++guard)
            if (local[size_t(n)])
                w = local[size_t(n)]->then(w);
        return w;
    }
};

} // namespace

std::string objectCresName(ResourceIndex& index, const Tgi& objd) {
    const auto objdBytes = index.read(objd);
    if (!objdBytes)
        throw FormatError("OBJD not found");
    const auto def = ObjectDefinition::parse(*objdBytes);
    const auto names = index.read(Tgi{0x53545223, objd.group, 0x85, 0});
    if (!names)
        return {};
    return StringTable::parse(*names).get(def.modelIndex(), lang::EnglishUS);
}

std::string cresByRootNode(ResourceIndex& index, std::string_view node) {
    static std::map<const ResourceIndex*, std::map<std::string, std::string>> cache;
    auto lower = [](std::string v) {
        for (char& c : v)
            c = char(std::tolower(static_cast<unsigned char>(c)));
        return v;
    };
    auto [it, fresh] = cache.try_emplace(&index);
    if (fresh)
        for (const auto& loc : index.ofType(0xE519C933)) {
            try {
                const auto bytes = index.read(loc);
                const Skeleton sk = Skeleton::fromCres(Rcol(bytes));
                if (sk.bones.empty() || sk.name.empty())
                    continue;
                std::string name = sk.name;
                if (lower(name).ends_with("_cres"))
                    name.resize(name.size() - 5);
                it->second.try_emplace(lower(sk.bones[0].name), name);
            } catch (const std::exception& err) {
                os2::logOnce("CRES skeletons", std::to_string(loc.entry->tgi.instance), err);
            }
        }
    const auto f = it->second.find(lower(std::string(node)));
    return f == it->second.end() ? std::string() : f->second;
}

std::string baseTextureName(const MaterialDefinition& m) {
    auto tex = m.property("stdMatBaseTextureName");
    if (tex.empty())
        return {};
    return withSuffix(tex, "_txtr");
}

namespace {

// Numbers of a property value: "0.8,0.8,0.8,1", "(0.5, 0.5)", "1e+002".
std::vector<float> numbers(const std::string& v) {
    std::vector<float> out;
    const char* p = v.c_str();
    while (*p) {
        if (*p == '(' || *p == ')' || *p == ',' || *p == ' ' || *p == '\t') {
            ++p;
            continue;
        }
        char* end = nullptr;
        const float f = std::strtof(p, &end);
        if (end == p)
            break;
        out.push_back(f);
        p = end;
    }
    return out;
}

bool contains(std::string_view s, std::string_view part) { return s.find(part) != std::string_view::npos; }

// The deprecated material types (deprecated.matShad): each forces some
// StandardMaterial parameters before running the standard shaders.
struct DeprecatedType {
    std::string_view name;
    int baseTexture;      // 1 on, 0 off
    bool blend;           // alpha blending, alpha = diffuse alpha
    bool mask;            // alpha test at alphaRefValue, alpha = diffuse alpha
    bool envCube;         // reflection with coefficient = specular x reflectivity
    bool normalMap;
    bool additive;        // unlit, additive, alpha = blendStrength
    bool alphaReplicate;
    bool unlit;
};
constexpr DeprecatedType kDeprecated[] = {
    // name                                base blend mask  env    bump   add    repl   unlit
    {"Phong",                              0, false, false, false, false, false, false, false},
    {"DiffuseMaterial",                    0, false, false, false, false, false, false, false},
    {"PhongAlpha",                         0, true,  false, false, false, false, false, false},
    {"DiffuseAlphaMaterial",               0, true,  false, false, false, false, false, false},
    {"PhongTexture",                       1, false, false, false, false, false, false, false},
    {"BaseTextureMaterial",                1, false, false, false, false, false, false, false},
    {"PhongTextureSpec",                   1, false, false, false, false, false, false, false},
    {"PhongTextureMask",                   1, false, true,  false, false, false, false, false},
    {"PhongTextureSpecMask",               1, false, true,  false, false, false, false, false},
    {"PhongTextureAlpha",                  1, true,  false, false, false, false, false, false},
    {"BaseTextureAlphaMaterial",           1, true,  false, false, false, false, false, false},
    {"UnlitTexture",                       1, false, false, false, false, false, false, true},
    {"UnlitTextureAlpha",                  1, true,  false, false, false, false, false, true},
    {"AdditiveTextureBlend",               1, false, false, false, false, true,  false, true},
    {"AdditiveTextureAlphaBlend",          1, false, false, false, false, true,  true,  true},
    {"PhongTextureBump",                   1, false, false, false, true,  false, false, false},
    {"PhongTextureMaskBump",               1, false, true,  false, true,  false, false, false},
    {"PhongTextureCubeMapReflection",      1, false, false, true,  false, false, false, false},
    {"PhongAlphaCubeMapReflection",        0, true,  false, true,  false, false, false, false},
    {"PhongTextureAlphaCubeMapReflection", 1, true,  false, true,  false, false, false, false},
    {"SteamPlume",                         1, false, false, false, false, false, false, false},
    {"SteamTop",                           1, false, false, false, false, false, false, false},
};

StandardMaterial::Waveform waveform(const std::string& v) {
    if (v == "sawtooth")
        return StandardMaterial::Waveform::Sawtooth;
    if (v == "sine")
        return StandardMaterial::Waveform::Sine;
    return StandardMaterial::Waveform::Triangular;
}

} // namespace

StandardMaterial standardMaterial(const MaterialDefinition& m) {
    StandardMaterial out;
    const std::string& type = m.materialType;
    auto has = [&](const char* name) { return !m.property(name).empty(); };
    auto flag = [&](const char* name, bool fallback) {
        auto v = m.property(name);
        return v.empty() ? fallback : v == "true" || v == "1";
    };
    auto number = [&](const char* name, float fallback) {
        auto v = numbers(m.property(name));
        return v.empty() ? fallback : v[0];
    };
    auto vec = [&](const char* name, auto& dst) {
        auto v = numbers(m.property(name));
        for (size_t i = 0; i < v.size() && i < dst.size(); ++i)
            dst[i] = v[i];
    };

    // --- StandardMaterial parameters, with standard.matShad's defaults ---
    std::array<float, 4> diffuse{1, 1, 1, 1};
    vec("stdMatDiffCoef", diffuse);
    vec("stdMatSpecCoef", out.specular);
    vec("stdMatEmissiveCoef", out.emissive);
    out.specularPower = number("stdMatSpecPower", 0);
    out.alphaMultiplier = number("stdMatAlphaMultiplier", 1);
    out.untexturedAlpha = number("stdMatUntexturedDiffAlpha", 1);
    // Every TXMT that names a base texture also enables it; materials built
    // in code (lot terrain) only give the name.
    bool baseEnabled = flag("stdMatBaseTextureEnabled", has("stdMatBaseTextureName"));
    bool normalEnabled = flag("stdMatNormalMapTextureEnabled", false);
    const auto blend = m.property("stdMatAlphaBlendMode");
    out.blend = blend == "blend"                ? StandardMaterial::Blend::Alpha
              : blend == "additive"             ? StandardMaterial::Blend::Additive
              : blend == "addNoSrcAlphaScale"   ? StandardMaterial::Blend::AddNoSrcAlpha
              : blend == "premultiplied"        ? StandardMaterial::Blend::Premultiplied
              : blend == "multiply"             ? StandardMaterial::Blend::Multiply
                                                : StandardMaterial::Blend::None;
    out.alphaTest = flag("stdMatAlphaTestEnabled", false);
    out.alphaRef = number("stdMatAlphaRefValue", 127) / 255.0f;
    const auto cull = m.property("stdMatCullMode");
    out.cull = cull == "none" ? StandardMaterial::Cull::None
             : cull == "cullCounterClockwise" ? StandardMaterial::Cull::CounterClockwise
                                              : StandardMaterial::Cull::Clockwise;
    out.lighting = flag("stdMatLightingEnabled", true);
    const int stdLayer = int(number("stdMatLayer", 0));
    out.alphaReplicate = flag("stdMatBaseTextureAlphaReplicate", false);
    out.clampU = m.property("stdMatBaseTextureAddressingU") == "clamp";
    out.clampV = m.property("stdMatBaseTextureAddressingV") == "clamp";
    bool envReflection = m.property("stdMatEnvCubeMode") == "reflection";
    std::string envName = m.property("stdMatEnvCubeTextureName");
    vec("stdMatEnvCubeCoef", out.envCubeCoef);
    out.envCubeBlur = number("stdMatEnvCubeBlurFactor", 0);
    out.envCubeLockedToCamera = flag("stdMatEnvCubeLockedToCamera", false);

    const auto anim = m.property("stdMatTextureCoordAnimMode");
    if (anim == "tile") {
        out.texAnim = StandardMaterial::TexAnim::Tile;
        out.tileSpeed = number("stdMatTextureCoordTileAnimSpeed", 0);
        vec("stdMatTextureCoordAnimNumTiles", out.tiles);
    } else if (anim == "transform") {
        out.texAnim = StandardMaterial::TexAnim::Transform;
        vec("stdMatTextureCoordTfAnimOrigin", out.animOrigin);
        auto wave = [&](const std::string& what, StandardMaterial::Wave& w, std::array<float, 2> start,
                        std::array<float, 2> end, bool pairs) {
            const std::string p = "stdMatTextureCoordTfAnim" + what;
            w.waveform = waveform(m.property(p + "Waveform"));
            w.speed = number((p + "Speed").c_str(), 0);
            if (pairs) {
                w.start = start, w.end = end;
                vec((p + "Start").c_str(), w.start);
                vec((p + "End").c_str(), w.end);
            } else {
                std::array<float, 2> se{start[0], end[0]};
                vec((p + "StartEnd").c_str(), se);
                w.start = {se[0], se[0]};
                w.end = {se[1], se[1]};
            }
        };
        wave("Rot", out.rotation, {0, 0}, {1, 1}, false);
        wave("Scale", out.scale, {1, 1}, {2, 2}, false);
        wave("Trans", out.translation, {0, 0}, {0, 0}, true);
    }

    // --- Material types ---
    for (const auto& d : kDeprecated) {
        if (type != d.name)
            continue;
        baseEnabled = d.baseTexture == 1;
        normalEnabled = d.normalMap;
        if (d.blend) {
            out.blend = StandardMaterial::Blend::Alpha;
            out.alphaMultiplier = diffuse[3];
        }
        if (d.mask) {
            out.alphaTest = true;
            out.alphaRef = number("alphaRefValue", 0) / 255.0f;
            out.alphaMultiplier = diffuse[3];
        }
        if (d.envCube) {
            envReflection = true;
            out.envCubeBlur = number("blurFactor", 0);
            const float reflectivity = number("reflectivity", 1);
            for (int i = 0; i < 3; ++i)
                out.envCubeCoef[i] = out.specular[i] * reflectivity;
        }
        if (d.additive) {
            out.blend = StandardMaterial::Blend::Additive;
            out.alphaMultiplier = number("blendStrength", 1);
        }
        out.alphaReplicate = out.alphaReplicate || d.alphaReplicate;
        if (d.unlit)
            out.lighting = false;
        // Undoes a multiplication done by the exporter (go2sco).
        const float inv = number("deprecatedStdMatInvDiffuseCoeffMultiplier", 1);
        for (int i = 0; i < 3; ++i)
            diffuse[i] *= inv;
    }

    bool simHair = false;
    if (type == "SimStandardMaterial" || type == "SimSkin" || type == "SimHair") {
        // sims.matShad / DX9SimMaterials.matShad.
        out.shading = StandardMaterial::Shading::Sim;
        out.specularPower = 0; // "no sim material can have specular"
        // Hair is drawn with SimHair(), which no TXMT names: the outfit
        // builder (Mac BV 0x82bb70) renames the SimStandardMaterials of a
        // hair part (GZPS type 1) to it (markHairPart).
        simHair = type == "SimHair";
        out.hair = simHair || m.property("os2HairPart") == "1";
        if (simHair && out.blend == StandardMaterial::Blend::Alpha) {
            // SimHairAlphaState: blended, but writes depth and drops alpha <= 60.
            out.depthWrite = true;
            out.alphaTest = true;
            out.alphaRef = 61.0f / 255.0f;
            // "_alpha<N>" in the material name orders the layers of a hairdo.
            const std::string& name = m.resource.fileName;
            for (size_t at = name.find("_alpha"); at != std::string::npos; at = name.find("_alpha", at + 1)) {
                size_t k = at + 6;
                int n = 0;
                while (k < name.size() && name[k] >= '0' && name[k] <= '9')
                    n = n * 10 + (name[k++] - '0');
                if (k > at + 6)
                    out.alphaOrder = n;
            }
        }
    } else if (type.rfind("Wallpaper", 0) == 0 || type.rfind("Floor", 0) == 0 || type == "Terrain") {
        // Walls, floors and the terrain are lit by light maps in their own
        // shaders, with no specular, no env cube and (on the Mac's DX8 path,
        // without the incidence maps) no bump. Walls ignore the diffuse.
        out.specularPower = 0;
        out.lightMapped = true;
        envReflection = false;
        normalEnabled = false;
        if (type.rfind("Wallpaper", 0) == 0)
            diffuse = {1, 1, 1, 1};
        if (type == "Terrain") {
            out.textureScale = number("osTextureScale", 1);
            if (auto d = m.property("osDetailTextureName"); !d.empty())
                out.detailTexture = withSuffix(d, "_txtr");
            out.detailScale = number("osDetailTextureScale", 1);
            out.maskAlphaOnly = flag("osMaskAlphaOnly", false);
        }
    } else if (type == "SimpleMirrorReflection") {
        // mirrors.matShad: with $mirrorReflectionsEnabled false (the Reflection
        // graphics option off: docs/shaders-mac.md 9) the mirror quad is
        // BlankMirrorMaterial: one flat colour, colorScalar (0.5, 0.6, 0.75),
        // no light, no texture. With it on (MirrorReflectionMaterial) the
        // renderer shows the scene reflected in it (StandardMaterial::mirror).
        out.mirror = true;
        out.lighting = false;
        diffuse = {0.5f, 0.6f, 0.75f, 1};
        out.specular = {0, 0, 0};
        out.emissive = {0, 0, 0};
        out.specularPower = 0;
        baseEnabled = false;
        normalEnabled = false;
        envReflection = false;
    } else if (type == "TextureAlpha") {
        out.lighting = false;
        out.blend = StandardMaterial::Blend::Alpha;
        out.textureColor = false;
        out.alphaMultiplier = diffuse[3];
        baseEnabled = has("stdMatBaseTextureName");
    } else if (type == "AnimatedTexture") {
        out.lighting = false;
        diffuse = {1, 1, 1, 1};
        baseEnabled = has("stdMatBaseTextureName");
        out.texAnim = StandardMaterial::TexAnim::Tile;
        out.tileSpeed = number("playSpeed", 15);
        out.tiles = {number("tileCountU", 16), number("tileCountV", 1)};
    } else if (type == "VertexAlpha") {
        out.shading = StandardMaterial::Shading::VertexColor;
        out.lighting = false;
        out.blend = StandardMaterial::Blend::Alpha;
        baseEnabled = false;
    } else if (type == "CelShadedMaterial") {
        out.shading = StandardMaterial::Shading::CelShaded;
        out.celRamp = withSuffix(m.property("csRampTexture").empty() ? "64shade" : m.property("csRampTexture"), "_txtr");
        if (flag("csAddEdge", true)) {
            const auto edge = m.property("csEdgeRampTexture");
            out.celEdgeRamp = withSuffix(edge.empty() ? "blackwhite" : edge, "_txtr");
            out.celEdgeOffset = number("csEdgeOffset", 0.3f);
        }
        envReflection = false;
        out.specularPower = 0;
    } else if (type == "WaterAnimatingTextures" || type == "WaterMorphAnimatingTextures") {
        // water.matShad, DX8 path (LowEndWater/SinglePassWater): the texture
        // at two scales animated 1 -> 2 (sawtooth, waterSpeed, phases 0 and
        // 0.5) crossfaded, times the fixed-function light (ambient = diffuse
        // = stdMatDiffCoef, x1); always alpha blended, no z write, no
        // specular, no env cube.
        out.shading = StandardMaterial::Shading::Water;
        out.blend = StandardMaterial::Blend::Alpha;
        out.specularPower = 0;
        envReflection = false;
        normalEnabled = false;
        out.texAnim = StandardMaterial::TexAnim::None;
        out.waterSpeed = number("waterSpeed", 0.3f);
    } else if (type == "TerrainWater") {
        // TerrainWaterShader (terrain.matShad, docs/shaders-mac.md 9): the
        // reflection of the env cube x colorScalar (1, 1, 1) x the lights,
        // alpha from the vertex colour; no diffuse, no specular.
        out.shading = StandardMaterial::Shading::TerrainWater;
        out.blend = StandardMaterial::Blend::Alpha;
        out.specularPower = 0;
        diffuse = {0, 0, 0, 1};
        out.untexturedAlpha = 1;
    } else if (contains(type, "Water")) {
        // water.matShad: alpha blended over the water layer (5).
        out.blend = StandardMaterial::Blend::Alpha;
    } else if (blend.empty() && type != "StandardMaterial") {
        // Types not described above rarely carry the StandardMaterial blend
        // settings; their names say how they blend.
        if (contains(type, "Additive"))
            out.blend = StandardMaterial::Blend::Additive;
        else if (contains(type, "Alpha"))
            out.blend = StandardMaterial::Blend::Alpha;
    }

    // DetermineLightingMode: "additive" materials are never lit; neither are
    // those whose diffuse and specular coefficients are all 0 (unlit, the
    // texture times a black diffuse: the same colour as lit).
    if (out.blend == StandardMaterial::Blend::Additive && out.shading == StandardMaterial::Shading::Standard)
        out.lighting = false;
    out.diffuse = {diffuse[0], diffuse[1], diffuse[2]};
    if (baseEnabled)
        out.baseTexture = baseTextureName(m);
    // Not on Sims: the Mac game draws every Sim material with the fixed
    // function path, which has no normal map (docs/shaders-mac.md 8).
    if (normalEnabled && out.lighting && out.shading != StandardMaterial::Shading::CelShaded &&
        out.shading != StandardMaterial::Shading::Sim)
        if (auto n = m.property("stdMatNormalMapTextureName"); !n.empty())
            out.normalMap = withSuffix(n, "_txtr");
    // CheckForCubeMapping: off when the coefficients add up to < 0.003; unlit
    // materials have no env cube.
    const float coef = out.envCubeCoef[0] + out.envCubeCoef[1] + out.envCubeCoef[2];
    if (envReflection && coef >= 0.003f && out.lighting)
        out.envCube = withSuffix(envName.empty() ? "reflectionCubeTemplate-envcube" : envName, "_txtr");

    const bool blended = out.blend != StandardMaterial::Blend::None;
    // Premultiplied (lot imposters' walls and slices) keeps the depth write
    // its alpha-tested passes have.
    if (blended && !simHair && out.blend != StandardMaterial::Blend::Premultiplied)
        out.depthWrite = false;
    out.layer = stdLayer * 8 + (blended ? (simHair ? 3 : 7) : 0);
    // The terrain's paints are passes of the terrain's own shader
    // (TerrainCanvasShader / TerrainPaint), drawn with the terrain before the
    // objects: the objects' blended ground shadow planes (GUOB) go over them.
    if (type == "Terrain" && blended)
        out.layer = stdLayer * 8 + 6;
    // The water layer (5) in stdMatLayer units, after the layers below it
    // and their blended parts (the terrain paints, layer 0 + 7): drawn
    // before them, without writing depth, the water of N001_Lot82's moat
    // was painted over by the sand and the lake bed (live QA, round 16).
    if (contains(type, "Water"))
        out.layer = (stdLayer + 5) * 8;
    if (type == "CelShadedMaterial")
        out.layer = int(number("csMatLayer", 0)) * 8;
    // WallMask parts are the shapes doors and windows cut out of walls.
    out.visible = type != "Null" && type != "WallMask";
    return out;
}

ObjectModel loadObjectModel(ResourceIndex& index, std::string_view cresName,
                            const std::map<std::string, std::string>& materials) {
    ObjectModel model;
    model.name = withSuffix(cresName, "_cres");
    auto cresLoc = index.findByName(model.name, kTypeCres);
    if (!cresLoc)
        throw FormatError("CRES not found: " + model.name);
    const auto cresBytes = index.read(*cresLoc);
    Rcol cres(cresBytes);

    const NodeTree tree(cres);
    model.skeleton = Skeleton::fromCres(cres, true);
    for (size_t bi = 0; bi < cres.blocks.size(); ++bi) {
        const auto& block = cres.blocks[bi];
        if (block.className != "cShapeRefNode")
            continue;
        const auto ref = ShapeRefNode::parse(block.body);
        const Xform toObject = tree.world(bi);
        int carrier = -1; // the shape node's own bone
        for (int n = int(bi), guard = 0; n >= 0 && carrier < 0 && guard < 256; n = tree.parent[size_t(n)], ++guard)
            for (size_t b = 0; b < model.skeleton.bones.size(); ++b)
                if (model.skeleton.bones[b].block == size_t(n))
                    carrier = int(b);
        for (const auto& link : ref.shapes) {
            if (link.linkIndex < 0 || size_t(link.linkIndex) >= cres.links.size()) {
                model.warnings.push_back("shape link " + std::to_string(link.linkIndex) + " out of range");
                continue;
            }
            const Tgi shpeTgi = cres.links[size_t(link.linkIndex)];
            if (shpeTgi.type != kTypeShpe)
                continue;
            auto shpeLoc = findLink(index, shpeTgi);
            if (!shpeLoc) {
                model.warnings.push_back("SHPE not found for link " + std::to_string(link.linkIndex));
                continue;
            }
            const auto shpeBytes = index.read(*shpeLoc);
            Rcol shpeRc(shpeBytes);
            const auto shape = Shape::parse(firstBlock(shpeRc, "cShape", "SHPE").body);
            if (shape.items.empty())
                continue;

            // Lowest level of detail is the most detailed.
            const auto* item = &shape.items[0];
            for (const auto& it : shape.items)
                if (it.lod < item->lod)
                    item = &it;

            std::optional<ResourceIndex::Location> gmndLoc;
            if (!item->geometryName.empty())
                gmndLoc = index.findByName(item->geometryName, kTypeGmnd);
            else if (item->linkIndex >= 0 && size_t(item->linkIndex) < shpeRc.links.size())
                gmndLoc = findLink(index, shpeRc.links[size_t(item->linkIndex)]);
            if (!gmndLoc) {
                model.warnings.push_back("GMND not found for " + shape.resource.fileName);
                continue;
            }
            const auto gmndBytes = index.read(*gmndLoc);
            Rcol gmndRc(gmndBytes);
            std::optional<ResourceIndex::Location> gmdcLoc;
            for (const auto& l : gmndRc.links)
                if (l.type == kTypeGmdc && (gmdcLoc = findLink(index, l)))
                    break;
            if (!gmdcLoc) {
                model.warnings.push_back("GMDC not found for " + shape.resource.fileName);
                continue;
            }
            const auto gmdcBytes = index.read(*gmdcLoc);
            Rcol gmdcRc(gmdcBytes);
            const auto geometry = GeometryData::parse(firstBlock(gmdcRc, "cGeometryDataContainer", "GMDC").body);

            for (auto& mesh : buildMeshes(geometry)) {
                for (auto& p : mesh.positions)
                    p = toObject.apply(p);
                for (auto& n : mesh.normals)
                    n = toObject.rotate(n);
                ObjectModel::Part part;
                part.node = ref.renderable.transform.name();
                part.bone = carrier;
                part.shape = shape.resource.fileName;
                for (const auto& p : shape.parts)
                    if (p.subset == mesh.name)
                        part.material = p.material;
                // A design choice naming a material the game no longer ships
                // keeps the shape's own.
                if (auto it = materials.find(mesh.name); it != materials.end())
                    if (it->second == kShadowOff || index.findByName(withSuffix(it->second, "_txmt"), kTypeTxmt))
                        part.material = it->second;
                // Kept whatever the Shadows option: the renderer skips these parts
                // while the option has no guob, so changing it needs no reload.
                part.shadowPlane = shadowPlane(mesh.name, part.material);
                if (!mesh.bones.empty())
                    for (const auto& t : geometry.transforms)
                        part.bindPoses.push_back(
                            Xform{{t.rotation[0], t.rotation[1], t.rotation[2], t.rotation[3]}, t.translation});
                part.mesh = std::move(mesh);
                model.parts.push_back(std::move(part));
            }
        }
    }

    for (const auto& part : model.parts) {
        if (part.material.empty() || model.materials.count(part.material))
            continue;
        // Hidden: shadows turned off, and the volume a mirror reflects.
        if (part.material == kShadowOff || part.material == "mirrorboundingboxmaterial") {
            MaterialDefinition hidden;
            hidden.materialType = "Null";
            model.materials.emplace(part.material, std::move(hidden));
            continue;
        }
        auto loc = index.findByName(withSuffix(part.material, "_txmt"), kTypeTxmt);
        // Counter tops cut out for a sink ("..._clean-cutout-0") are made by
        // the game at run time from the plain top.
        if (auto cut = part.material.find("-cutout"); !loc && cut != std::string::npos)
            loc = index.findByName(part.material.substr(0, cut) + "_txmt", kTypeTxmt);
        if (!loc) {
            model.warnings.push_back("TXMT not found: " + part.material);
            continue;
        }
        const auto bytes = index.read(*loc);
        Rcol rc(bytes);
        model.materials.emplace(part.material,
                                MaterialDefinition::parse(firstBlock(rc, "cMaterialDefinition", "TXMT").body));
    }
    return model;
}

std::string resolveObjectCres(ResourceIndex& index, std::string_view query, std::string* match) {
    if (index.findByName(withSuffix(query, "_cres"), kTypeCres)) {
        if (match)
            *match = "CRES " + withSuffix(query, "_cres");
        return std::string(query);
    }
    const bool isGuid = query.starts_with("0x") || query.starts_with("0X");
    const uint32_t guid = isGuid ? uint32_t(std::stoul(std::string(query), nullptr, 16)) : 0;
    auto lower = [](std::string_view in) {
        std::string s(in);
        for (auto& c : s)
            c = char(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    const auto needle = lower(query);
    for (const auto& loc : index.ofType(kTypeObjd)) {
        const auto def = ObjectDefinition::parse(index.read(loc));
        if (isGuid ? def.guid() != guid : lower(def.fileName).find(needle) == std::string::npos)
            continue;
        auto cres = objectCresName(index, loc.entry->tgi);
        if (cres.empty())
            continue;
        if (match) {
            char buf[32];
            std::snprintf(buf, sizeof buf, "0x%08X", def.guid());
            *match = "object \"" + def.fileName + "\" (GUID " + buf + ") -> " + cres;
        }
        return cres;
    }
    throw FormatError("no object or CRES matches '" + std::string(query) + "'");
}

} // namespace os2
