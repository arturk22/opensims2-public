#pragma once

#include "gmdc.hpp"
#include "skeleton.hpp"
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
        // Skinned parts (mesh.bones not empty): the GMDC's inverse bind pose
        // of each bone id, i.e. model space -> bone space in the rest pose.
        std::vector<Xform> bindPoses;
        // Rigid parts: the skeleton bone (index in ObjectModel::skeleton)
        // of the part's cShapeRefNode (the skeleton has the shape nodes as
        // bones), -1 if none; animating that bone moves the part.
        int bone = -1;
        // Extra TXTR names layered with the base texture (Sim skins): the
        // underlay shows where the base texture is transparent (skin under
        // clothes), overlays are drawn over it by their alpha (eyes, brows).
        std::string underlay;
        bool underlayFlat = false; // the underlay's average colour instead (a hairdo's scalp shows skin)
        std::vector<std::string> overlays;
        bool shadowPlane = false; // a ground/wall shadow plane: drawn only while graphicsOptions().guob
    };

    // Alpha mask laid over the model's XY plane: value (x, y) sits at
    // model-space (x, y) * spacing, interpolated in between.
    struct AlphaMask {
        uint32_t width = 0, height = 0;
        float spacing = 1.0f;
        float originX = 0, originY = 0; // model xy of value (0, 0)
        std::vector<uint8_t> values; // row-major: values[y * width + x]
        // A wall mask instead (walls with a door or window, docs/shaders-mac.md
        // 5): the values (0 = hole) span u, v in [0, 1] with u = planeU.xyz . p
        // + planeU.w and v likewise for a model-space point p, clamped; the
        // part is alpha tested against them.
        bool planar = false;
        std::array<float, 4> planeU{0, 0, 0, 0}, planeV{0, 0, 0, 0};
    };

    std::string name; // CRES resource name
    std::vector<Part> parts;
    Skeleton skeleton; // the CRES's transform nodes; skinned parts use its joint ids
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

// The CRES (name without "_cres") whose scenegraph root node is `node`
// (case-insensitive), the first in the index's order; empty if none. The
// animations' snap events name a model by its root node ("tray:__foodTray__"
// is templateFoodTray, "doll1:__doll__" the dollhouse's dolls
// dollFemale01...), not always by its CRES. The first call reads every CRES.
std::string cresByRootNode(ResourceIndex& index, std::string_view node);

// TXTR name of a material's base texture ("..._txtr"), or empty.
std::string baseTextureName(const MaterialDefinition& m);

// What the renderer needs of a TXMT, for every material type, as the game's
// material shaders (Res/Materials/Materials.package, "*.matShad") set it up:
// StandardMaterial and its defaults, the deprecated Phong*/Diffuse*/BaseTexture*
// types that map onto it (diffuse times deprecatedStdMatInvDiffuseCoeffMultiplier),
// the Sim materials, walls, floors, CelShadedMaterial, VertexAlpha, ...
struct StandardMaterial {
    // Additive: srcAlpha + one ("additive"); AddNoSrcAlpha: one + one ("addNoSrcAlphaScale").
    enum class Blend { None, Alpha, Additive, AddNoSrcAlpha, Premultiplied, Multiply }; // Premultiplied: one + (1 - src alpha); Multiply: src·dst + dst·(1 − srcα)
    enum class Cull { Clockwise, CounterClockwise, None }; // which winding is culled
    enum class Shading {
        Standard,   // texture x (emissive + diffuse x lights) + specular + env cube
        Sim,        // Sim materials: no specular, lights scaled by simMaterialLightingScale (0.8)
        CelShaded,  // two-tone ramp from a camera-relative light, white rim
        VertexColor, // VertexAlpha: vertex colour and alpha, unlit
        Water,       // WaterAnimatingTextures (LowEndWater): two animated scales of the texture, lit x1
        TerrainWater, // TerrainWaterShader: the env cube reflection only, alpha from the vertex colour
        PoolWater     // RegularWavesHLSL (poolWater-1): additive rippled reflection with a fresnel term and a highlight
    };
    enum class TexAnim { None, Transform, Tile };
    enum class Waveform { Sawtooth, Triangular, Sine };

    // One animated component of the 'transform' texture coordinate animation:
    // value = start + (end - start) x waveform(time x speed); speed 0 = off.
    struct Wave {
        Waveform waveform = Waveform::Triangular;
        float speed = 0;
        std::array<float, 2> start{0, 0}, end{0, 0};
    };

    Shading shading = Shading::Standard;
    std::string baseTexture; // TXTR name, empty if untextured
    std::array<float, 3> diffuse{1, 1, 1};
    std::array<float, 3> specular{0, 0, 0};
    float specularPower = 0; // 0 = no specular (the matShad default)
    std::array<float, 3> emissive{0, 0, 0};
    float alphaMultiplier = 1.0f;
    float untexturedAlpha = 1.0f; // diffuse alpha when there is no base texture
    Blend blend = Blend::None;
    bool alphaTest = false;
    float alphaRef = 127.0f / 255.0f; // passes when alpha >= alphaRef
    bool depthWrite = true;           // blended parts only write depth when this is set (Sim hair)
    Cull cull = Cull::Clockwise;
    bool lighting = true;
    // Walls, floors and terrain: the game lights them only with its light
    // maps (no direct lights, docs/shaders-mac.md sections 5-7): walls
    // 2 x wallpaper x light map (diffuse 1 here), floors 2 x base x light
    // map x diffuse, terrain 2 x 0.9 x light map x canvas (see lot_lightmap.hpp).
    bool lightMapped = false;
    // The lot terrain (materials built in code, type "Terrain"): the base
    // texture at uv x textureScale with a detail texture at uv x detailScale
    // laid over it by the detail's alpha; paints take their alpha from the
    // mask alone (maskAlphaOnly).
    float textureScale = 1;
    std::string detailTexture; // TXTR name, empty if none
    float detailScale = 1;
    bool maskAlphaOnly = false;
    // Draw order: stdMatLayer x 8, +7 when blended (+3 for blended Sim hair);
    // walls and floors 0, water 5. Lower layers are drawn first.
    int layer = 0;
    // Blended Sim hair parts ("..._alpha5_...", "..._alpha11_..."): the
    // number, drawn in increasing order after the opaque hair (inferred);
    // 0 for every other material.
    int alphaOrder = 0;
    bool hair = false; // a material of a Sim's hair part (SimHair, or its SimSkin scalp)
    bool visible = true; // false for "Null" and "WallMask" materials (parts the game does not draw)
    // SimpleMirrorReflection: a mirror's quad, drawn as the blank mirror
    // colour unless the renderer's reflections are on (Renderer::setReflections).
    bool mirror = false;
    bool alphaReplicate = false; // stdMatBaseTextureAlphaReplicate: colour = texture alpha
    bool textureColor = true;    // false: the base texture only gives alpha (TextureAlpha)
    bool clampU = false, clampV = false; // stdMatBaseTextureAddressingU/V = clamp

    // stdMatEnvCubeMode = reflection (with a non-zero coefficient and lighting):
    // colour += cube(reflection vector) x envCubeCoef.
    std::string envCube; // TXTR name of the cube, empty if off
    std::array<float, 3> envCubeCoef{0, 0, 0};
    float envCubeBlur = 0;        // mip bias
    bool envCubeLockedToCamera = false; // reflection vector in camera space

    // stdMatNormalMapTextureName: a grey height map ("-bump") the game turns
    // into a normal map.
    std::string normalMap; // TXTR name, empty if off
    // stdMatCutoutTextureName (docs/shaders-mac.md 13.3): set by the game on
    // a counter top under a sink ("<material>-cutout-<texture>",
    // CreateAndRegisterObjectCutoutMaterial); the DX8 path's first lit pass
    // takes its alpha from this texture (UV 0) and drops what is under 127.
    std::string cutoutTexture; // TXTR name, empty if off ("0")

    TexAnim texAnim = TexAnim::None;
    float tileSpeed = 0;                 // tiles per second
    std::array<float, 2> tiles{1, 1};    // tiles across U and V
    std::array<float, 2> animOrigin{0.5f, 0.5f};
    Wave rotation, scale, translation;   // rotation in turns

    // CelShadedMaterial (celShading.matShad, docs/shaders-mac.md 13.4): the
    // ramps (TXTR names), the edge's offset, light damping and alpha
    // invert, the ramp's row from the vertex alpha (csRouteColorFromAlpha),
    // the specular ramp and shininess, and the outline pass.
    std::string celRamp, celEdgeRamp, celSpecRamp;
    float celEdgeOffset = 0.3f;
    float celEdgeDamping = 0;
    bool celEdgeInvert = true;
    bool celRouteFromAlpha = true;
    float celShininess = 100;
    bool celOutline = false;
    std::array<float, 3> celOutlineColor{1, 0.3f, 0};
    float celOutlineAlpha = 1;
    float celOutlineWidth = 0.01f;
    float waterSpeed = 0.3f; // Water: texture animation speed
    // PoolWater: water.matShad's wave constants (wmXRepeat, wmYRepeat,
    // wmXWaveHeight, wmXSpeed; the Y height and speed are the X ones here).
    std::array<float, 4> poolWaves{1, 1.8f, 0.1f, 3};
};
StandardMaterial standardMaterial(const MaterialDefinition& m);

} // namespace os2
