#include <metal_stdlib>
using namespace metal;

// Compiled at runtime with MTL::Device::newLibrary (no offline Metal compiler needed).

struct Vertex {
    packed_float3 position;
    packed_float3 normal;
    packed_float2 uv;
    packed_float2 uv2; // light map
    uint color; // D3DCOLOR 0xAARRGGBB
};

enum : uint {
    kLighting = 1,  // lit with the frame's directional light
    kAlphaTest = 2, // discard below alphaRef
    kChecker = 4,   // one-tile checkerboard (test floor)
    kMasked = 8,    // alpha times the mask texture, laid over model XY
    kUnderlay = 16, // texture 2 shows through where the base texture is transparent (opaque result)
    kOverlay1 = 32, // texture 3 drawn over the base by its alpha
    kOverlay2 = 64, // texture 4 likewise
    kEnvCube = 128,        // + env cube (texture 6) x env.rgb along the reflection vector
    kEnvLocked = 256,      // ...with the reflection vector in camera space
    kNormalMap = 512,      // texture 5 is a height map bumping the normal
    kAlphaReplicate = 1024, // colour = base texture alpha
    kAlphaOnly = 2048,     // the base texture only gives alpha
    kCelShaded = 4096,     // ramp (texture 7) from a camera-relative light
    kCelEdge = 8192,       // ...with the edge ramp (texture 8)
    kVertexColor = 16384,  // times the vertex colour and alpha
    kOverlay3 = 32768,     // texture 9 drawn over the base by its alpha, after overlay 2
    kLightMapped = 65536,  // walls, floors, terrain: 2 x texture x light map (textures 10/11) x diffuse
    kDetail = 131072,      // terrain: detail texture (12) at uv x misc.z over the base by its alpha
    kMaskAlphaOnly = 262144, // terrain paints: alpha from the mask alone
    kWater = 524288,         // WaterAnimatingTextures: two animated scales of the base (speed misc.w), lit x1
    kOverlayMore = 1u << 20, // bits 20-24: textures 13-17 over the base by their alpha, after overlay 3
    kTwoSidedLight = 1u << 25, // lit from both sides, wrapped (Sim hair)
    kUnderlayFlat = 1u << 26,  // the underlay's average colour (its smallest mip)
    kWallMask = 1u << 27,      // walls with a door or window: alpha test against the mask (texture 1)
    kMirror = 1u << 28,        // the reflecting mirror: the reflection texture (18) at this pixel
    kShadowCaster = 1u << 29,  // a shadow silhouette: the draw's colour, alpha-tested by the texture
    kPoolWater = 1u << 30,     // RegularWavesHLSL: waves (misc), env cube x (1 - N.V) x 0.5 + highlight, added
    kCutout = 1u << 31,        // stdMatCutoutTextureName: alpha from texture 20 (uv 0), dropped under 127
};

struct Draw {
    float4x4 model;
    float4 color;    // diffuse rgb, alpha multiplier
    float4 specular; // rgb, power in w (0: none)
    float4 emissive; // rgb
    float4 mask;     // mask uv = model xy * mask.xy + mask.zw
    float4 env;      // env cube coefficient rgb, mip bias in w
    float4 uv0;      // base uv: u' = dot(uv0.xyz, (u, v, 1))
    float4 uv1;      //          v' = dot(uv1.xyz, (u, v, 1))
    float4 misc;     // x: cel edge offset, y: bump strength, z: detail uv scale, w: water speed
    float4 maskV;    // kWallMask: mask u = dot(mask.xyz, p) + mask.w, v = dot(maskV.xyz, p) + maskV.w (model space)
    float alphaRef;
    uint flags;
    uint lightSet;   // index into the light sets (buffer 3)
};

constant uint kMaxLights = 16;

struct Light {
    float4 direction; // xyz: unit vector towards the light, world space (a point light: its position)
    float4 color;     // rgb: colour times intensity, w: relative specular scale
    float4 params;    // a point light: x: FalloffRate C (I / (1 + C d^2)), y: range, z: 1, w: a spot's inner cone cosine
    float4 spot;      // a spot: xyz where it shines, w the outer cone cosine (< -1: a point light)
};

// Unit vector towards a light and its attenuation at `world` (directional
// lights: their direction, 1; the lamps' practical point lights:
// I / (1 + C d^2) inside the range, 0 beyond, docs/lighting-mac.md 6).
float3 lightVector(constant Light& light, float3 world, thread float& att) {
    att = 1.0;
    if (light.params.z < 0.5)
        return light.direction.xyz;
    const float3 d = light.direction.xyz - world;
    const float dist = max(length(d), 1e-4);
    att = dist < light.params.y ? 1.0 / (1.0 + light.params.x * dist * dist) : 0.0;
    // A spot's cone (cSpotLight; D3D's spot falloff 1): full inside the
    // inner cone, nothing outside the outer, linear in the cosine between.
    if (light.spot.w >= -1.0) {
        const float rho = dot(-d / dist, light.spot.xyz);
        att *= saturate((rho - light.spot.w) / max(light.params.w - light.spot.w, 1e-4));
    }
    return d / dist;
}

struct LightSet {
    float4 ambient; // rgb, w: number of lights
    Light lights[kMaxLights];
};

struct Frame {
    float4x4 viewProj;
    float4x4 view;
    float4 eye;      // xyz: camera position
    float4 params;      // x: mix of light map 1 over light map 0, y: time in seconds
    float4 environment; // rgb: the environment light, scaling env cube reflections
    float4 clip;        // a plane (xyz normal, w offset): what is behind it is clipped when viewport.z is 1
    float4 viewport;    // xy: the target's size in pixels, z: clip on, w: ground clip height (world z)
    float4x4 screen;    // the view's viewProj: where a mirror's pixel reads the reflection (also in the censor's tiles)
};

#define VERTEX_OUT_FIELDS \
    float4 position [[position]]; \
    float3 world; \
    float3 normal; \
    float2 uv;     /* as stored */ \
    float2 baseUv; /* with the texture coordinate animation */ \
    float2 local;  /* model-space xy */ \
    float3 localPos; /* model-space position */ \
    float2 lightUv; \
    float4 color;

// What the fragments get.
struct VertexOut {
    VERTEX_OUT_FIELDS
};
// What the vertices give: the same and the clip distance of the reflection
// pass (only what is in front of the mirror is drawn into the reflection)
// and the ground clip (nothing below the lot's earth skirt).
struct VertexOutClip {
    VERTEX_OUT_FIELDS
    float clipDistance [[clip_distance]] [2];
};

vertex VertexOutClip vertex_main(uint vid [[vertex_id]],
                             const device Vertex* vertices [[buffer(0)]],
                             constant Draw& draw [[buffer(1)]],
                             constant Frame& frame [[buffer(2)]]) {
    const Vertex v = vertices[vid];
    float4 world = draw.model * float4(float3(v.position), 1.0);
    VertexOutClip out;
    out.position = frame.viewProj * world;
    out.clipDistance[0] = frame.viewport.z > 0.5 ? dot(float4(world.xyz, 1.0), frame.clip) : 1.0;
    out.clipDistance[1] = world.z - frame.viewport.w;
    out.world = world.xyz;
    out.normal = (draw.model * float4(float3(v.normal), 0.0)).xyz;
    out.uv = v.uv;
    const float3 uv1 = float3(v.uv, 1.0);
    out.baseUv = float2(dot(draw.uv0.xyz, uv1), dot(draw.uv1.xyz, uv1));
    out.local = float3(v.position).xy;
    out.localPos = float3(v.position);
    out.lightUv = v.uv2;
    out.color = float4((v.color >> 16) & 0xFF, (v.color >> 8) & 0xFF, v.color & 0xFF, v.color >> 24) / 255.0;
    return out;
}

// The normal map the game makes of a "-bump" height map when it loads it
// (ConvertHeightMapToNormalMapFast, not in the Mac binary): the height's
// slope per texel times `strength`, in texture space (inferred).
float3 heightNormal(float2 uv, texture2d<float> height, sampler smp, float strength) {
    const float2 size = float2(height.get_width(), height.get_height());
    const float h = height.sample(smp, uv).r;
    const float hu = height.sample(smp, uv + float2(1.0 / size.x, 0)).r;
    const float hv = height.sample(smp, uv + float2(0, 1.0 / size.y)).r;
    return normalize(float3((h - hu) * strength, (h - hv) * strength, 1.0));
}

// A Sim's shadow as the floors see it (nTSSG::cShadow, docs/lighting-mac.md 9).
struct Shadow {
    float4 u, v;  // world -> atlas uv: dot(xyz, p) + w
    float4 rect;  // the caster's tile (uv min, max)
    float4 misc;  // x: blur (uv), y: outside, z: the floor under the caster
    float4 color; // outside: the shadow colour
};

// What the Sims' shadows leave of a floor's colour at `world` (1: none).
// The silhouette seen from the light, blurred over +-blur texels (here a
// 5 x 5 box of bilinear taps) inside the caster's tile; inside it
// multiplies (ShadowShader), outside the whole shadow takes the shadow
// colour where the blurred silhouette passes the overlay's alpha test
// (OverlayShadowMaterial: > 100 of 255; its edge softened a little here).
// `objects`: an object's upward face (a rug): only the inside shadows, the
// game's quad a hair over the floor covering what is no higher.
float3 simShadows(float3 world, uint count, constant Shadow* shadows, texture2d<float> atlas, sampler smp, bool objects) {
    float3 k = 1.0;
    for (uint i = 0; i < count; ++i) {
        constant Shadow& s = shadows[i];
        if (objects && s.misc.y > 0.5)
            continue;
        if (world.z < s.misc.z - 0.6 || world.z > s.misc.z + (objects ? 0.03 : 0.3))
            continue;
        const float2 uv = float2(dot(s.u.xyz, world) + s.u.w, dot(s.v.xyz, world) + s.v.w);
        if (any(uv < s.rect.xy) || any(uv > s.rect.zw))
            continue;
        float3 acc = 0.0;
        for (int j = -2; j <= 2; ++j)
            for (int q = -2; q <= 2; ++q)
                acc += atlas.sample(smp, clamp(uv + float2(q, j) * s.misc.x * 0.5, s.rect.xy, s.rect.zw)).rgb;
        acc /= 25.0;
        if (s.misc.y > 0.5) {
            const float cover = saturate((1.0 - acc.g) / (1.0 - 0.63));
            k *= mix(float3(1.0), s.color.rgb, smoothstep(0.3, 0.48, cover));
        } else {
            k *= acc;
        }
    }
    return k;
}

fragment float4 fragment_main(VertexOut in [[stage_in]], bool front [[front_facing]],
                              constant Draw& draw [[buffer(1)]],
                              constant Frame& frame [[buffer(2)]],
                              constant LightSet* lightSets [[buffer(3)]],
                              constant Shadow* shadows [[buffer(4)]],
                              texture2d<float> base [[texture(0)]],
                              texture2d<float> mask [[texture(1)]],
                              texture2d<float> underlay [[texture(2)]],
                              texture2d<float> overlay1 [[texture(3)]],
                              texture2d<float> overlay2 [[texture(4)]],
                              texture2d<float> height [[texture(5)]],
                              texturecube<float> envCube [[texture(6)]],
                              texture2d<float> ramp [[texture(7)]],
                              texture2d<float> edgeRamp [[texture(8)]],
                              texture2d<float> overlay3 [[texture(9)]],
                              texture2d<float> lightMap0 [[texture(10)]],
                              texture2d<float> lightMap1 [[texture(11)]],
                              texture2d<float> detail [[texture(12)]],
                              array<texture2d<float>, 5> moreOverlays [[texture(13)]],
                              texture2d<float> reflection [[texture(18)]],
                              texture2d<float> shadowAtlas [[texture(19)]],
                              texture2d<float> cutout [[texture(20)]],
                              sampler smp [[sampler(0)]],
                              sampler maskSmp [[sampler(1)]],
                              sampler cubeSmp [[sampler(2)]],
                              sampler pointSmp [[sampler(3)]]) {
    if (draw.flags & kMirror) {
        // MirrorReflectionMaterial: the reflected scene at the mirror's pixel
        // of the view (clip space -> 0..1, v down), wherever this pass draws.
        const float4 c = frame.screen * float4(in.world, 1.0);
        return float4(reflection.sample(maskSmp, float2(c.x / c.w * 0.5 + 0.5, 0.5 - c.y / c.w * 0.5)).rgb, 1.0);
    }
    float4 tex = base.sample(smp, in.baseUv);
    if (draw.flags & kWater) {
        // SinglePassWater: the texture scaled 1 -> 2 (sawtooth) at phases 0
        // and 0.5, crossfaded so that neither shows its wrap (the scale's
        // origin, 0, and the waveform's start are inferred).
        const float p = fract(frame.params.y * draw.misc.w);
        const float4 t0 = base.sample(smp, in.baseUv * (1.0 + p));
        const float4 t1 = base.sample(smp, in.baseUv * (1.0 + fract(p + 0.5)));
        tex = mix(t1, t0, 1.0 - abs(2.0 * p - 1.0));
    }
    if (draw.flags & kDetail) {
        // terrain.matShad: lerpTextureAlpha(detail, canvas).
        const float4 d = detail.sample(smp, in.uv * draw.misc.z);
        tex.rgb = mix(tex.rgb, d.rgb, d.a);
    }
    if (draw.flags & kMaskAlphaOnly)
        tex.a = 1.0;
    if (draw.flags & kAlphaReplicate)
        tex.rgb = tex.a;
    if (draw.flags & kAlphaOnly)
        tex.rgb = 1.0;
    if (draw.flags & kUnderlay) {
        const float3 under = (draw.flags & kUnderlayFlat) ? underlay.sample(smp, float2(0.5), level(16.0)).rgb
                                                          : underlay.sample(smp, in.uv).rgb;
        tex = float4(mix(under, tex.rgb, tex.a), 1.0);
    }
    if (draw.flags & kOverlay1) {
        float4 o = overlay1.sample(smp, in.uv);
        tex.rgb = mix(tex.rgb, o.rgb, o.a);
    }
    if (draw.flags & kOverlay2) {
        float4 o = overlay2.sample(smp, in.uv);
        tex.rgb = mix(tex.rgb, o.rgb, o.a);
    }
    if (draw.flags & kOverlay3) {
        float4 o = overlay3.sample(smp, in.uv);
        tex.rgb = mix(tex.rgb, o.rgb, o.a);
    }
    for (uint i = 0; i < 5; ++i)
        if (draw.flags & (kOverlayMore << i)) {
            float4 o = moreOverlays[i].sample(smp, in.uv);
            tex.rgb = mix(tex.rgb, o.rgb, o.a);
        }
    if (draw.flags & kVertexColor)
        tex *= in.color;
    if (draw.flags & kMasked)
        tex.a *= mask.sample(maskSmp, in.local * draw.mask.xy + draw.mask.zw).r;
    // The cutout (StandardMaterialDiffusePassDX8, lit only): "mov r0.a,
    // t2.a" with t2 the cutout texture at uv 0, then alphaTest 127
    // acceptIfGreaterOrEqual, which takes the place of the material's own.
    const bool cut = (draw.flags & kCutout) && (draw.flags & kLighting);
    const float cutAlpha = cut ? cutout.sample(maskSmp, in.uv).a : 1.0;
    if (cut && cutAlpha < 127.0 / 255.0)
        discard_fragment();
    if (!cut && (draw.flags & kAlphaTest) && tex.a < draw.alphaRef)
        discard_fragment();
    if (draw.flags & kShadowCaster)
        return float4(draw.color.rgb, 1.0);
    if (draw.flags & kWallMask) {
        // PS1BasicWallRendering: alpha test > 127 on the wall mask (clamped).
        const float2 m = saturate(float2(dot(draw.mask.xyz, in.localPos) + draw.mask.w,
                                         dot(draw.maskV.xyz, in.localPos) + draw.maskV.w));
        if (mask.sample(maskSmp, m).r < 0.5)
            discard_fragment();
    }
    float3 albedo = tex.rgb;
    if (draw.flags & kChecker) {
        int2 tile = int2(floor(in.world.xy));
        albedo *= ((tile.x + tile.y) & 1) ? 0.82 : 1.0;
    }
    float4 color = float4(albedo * draw.color.rgb, tex.a * draw.color.a);
    float3 n = normalize(front ? in.normal : -in.normal);

    if (draw.flags & kCelShaded) {
        // celShading.matShad: two-tone ramp of N.L for a light fixed to the
        // camera ((1, 0, 0.5) in Direct3D's left-handed camera space), then a
        // rim from the edge ramp at N.E + offset.
        const float3 nc = normalize((frame.view * float4(n, 0)).xyz);
        const float ndl = saturate(dot(nc, float3(1, 0, -0.5)));
        color.rgb *= ramp.sample(pointSmp, float2(ndl, 0.5)).rgb;
        if (draw.flags & kCelEdge) {
            const float3 e = normalize(-(frame.view * float4(in.world, 1)).xyz);
            const float4 edge = edgeRamp.sample(pointSmp, float2(saturate(dot(nc, e) + draw.misc.x), 0.5));
            color.rgb = mix(edge.rgb, color.rgb, edge.a);
        }
        return color;
    }

    if (draw.flags & kPoolWater) {
        // water.matShad RegularWavesHLSL (poolWater-1): two sine waves along
        // x and y (misc: x/y repeat, height, speed; the phase takes the
        // position, z x height and time x speed) tilt the normal; one + one:
        // the env cube along R x (1 - N.V) x the texture lights x 0.5, plus
        // the highlight (saturate(R.L) + 0.1)^15 of the first light.
        const float t = frame.params.y;
        const float tx = in.world.x * draw.misc.x + in.world.z * draw.misc.z + t * draw.misc.w;
        const float ty = in.world.y * draw.misc.y + in.world.z * draw.misc.z + t * draw.misc.w;
        const float3 wn = normalize(float3(cos(tx) * -draw.misc.x * draw.misc.z + n.x,
                                           cos(ty) * -draw.misc.y * draw.misc.z + n.y, n.z));
        const float3 v = normalize(frame.eye.xyz - in.world);
        const float3 r = reflect(-v, wn);
        const float fresnel = dot(v, wn);
        constant LightSet& set = lightSets[draw.lightSet];
        float hi = 0.0;
        if (set.ambient.w > 0.0) {
            float att;
            const float3 l = lightVector(set.lights[0], in.world, att);
            hi = pow(saturate(dot(r, l)) + 0.1, 15.0);
        }
        const float3 refl = envCube.sample(cubeSmp, r, bias(draw.env.w)).rgb * draw.env.rgb;
        return float4(saturate(refl * (1.0 - fresnel) * frame.environment.rgb * 0.5 + hi), 1.0);
    }
    if ((draw.flags & kLighting) && (draw.flags & kLightMapped)) {
        // Walls, floors and terrain (docs/shaders-mac.md 5-7): no lights,
        // 2 x texture x light map (x the floor's diffuse, the terrain's 0.9;
        // walls pass 1), crossfading between two light maps on a lighting
        // state change.
        const float3 lm = mix(lightMap0.sample(maskSmp, in.lightUv).rgb, lightMap1.sample(maskSmp, in.lightUv).rgb,
                              frame.params.x);
        color.rgb = saturate(2.0 * albedo * lm * draw.color.rgb);
        // The Sims' shadows fall on floors and terrain (not on walls).
        if (frame.params.z > 0.5 && n.z > 0.7)
            color.rgb *= simShadows(in.world, uint(frame.params.z), shadows, shadowAtlas, maskSmp, false);
    } else if ((draw.flags & kLighting) && (draw.flags & kWater)) {
        // Fixed-function light, ambient = diffuse = stdMatDiffCoef, x1.
        constant LightSet& set = lightSets[draw.lightSet];
        float3 diffuse = set.ambient.rgb;
        for (uint i = 0; i < min(uint(set.ambient.w), kMaxLights); ++i) {
            float att;
            const float3 l = lightVector(set.lights[i], in.world, att);
            diffuse += set.lights[i].color.rgb * att * max(dot(n, l), 0.0);
        }
        color.rgb = albedo * saturate(draw.color.rgb * diffuse);
    } else if (draw.flags & kLighting) {
        // The game's DX8 StandardMaterial (docs/shaders-mac.md 4.2-4.3), per
        // pixel: lit = saturate(0.5 x ((ambient + lights) x diffuse +
        // emissive)), colour = saturate(2 x texture x lit), so light reaches
        // twice the texture; Sims scale their diffuse by 0.8 (0.4 in all).
        // Specular: Phong (R.L) to the material's power x 0.4, added.
        float3 v = normalize(frame.eye.xyz - in.world);
        float3 r = reflect(-v, n);
        constant LightSet& set = lightSets[draw.lightSet];
        const uint count = min(uint(set.ambient.w), kMaxLights);
        float3 spec = 0.0;
        if (draw.specular.w > 0.0)
            for (uint i = 0; i < count; ++i) {
                float att;
                const float3 l = lightVector(set.lights[i], in.world, att);
                if (dot(n, l) <= 0.0 || att <= 0.0)
                    continue;
                spec += set.lights[i].color.rgb * att * set.lights[i].color.w * pow(max(dot(r, l), 0.0), draw.specular.w);
            }
        if (draw.flags & kNormalMap) {
            // DX8StandardMaterialDiffuseBumpPixelShader (docs/shaders-mac.md
            // 13.2): the first directional light and the first point/spot
            // by pixel, the others by vertex: v0 = sat(0.5 x (vertex lights
            // + ambient + emissive)), lit = (sat(n.L1) x c1 / 2 + sat(n.L2) x
            // c3 / 2 + v0) x diffuse, colour = sat(2 x lit x texture). On the
            // Mac the vertices have no tangents (skipTangentsInVertexData
            // below ps 2): L1 in tangent space is (0, 0, N.L1), so n.L1 =
            // n.z x N.L1; L2 is read from the unwritten t2, (-1, -1, -1), and
            // c3 is the third light's colour (both inferred from the
            // registers' defaults).
            const float3 bn = heightNormal(in.baseUv, height, smp, draw.misc.y);
            int first = -1, firstPoint = -1;
            for (uint i = 0; i < count; ++i) {
                if (set.lights[i].params.z < 0.5 && first < 0)
                    first = int(i);
                if (set.lights[i].params.z >= 0.5 && firstPoint < 0)
                    firstPoint = int(i);
            }
            float3 vertexLight = set.ambient.rgb + draw.emissive.rgb;
            float nl1 = 0.0;
            for (uint i = 0; i < count; ++i) {
                float att;
                const float3 l = lightVector(set.lights[i], in.world, att);
                const float ndl = dot(n, l);
                if (int(i) == first)
                    nl1 = saturate(bn.z * clamp(ndl, -1.0, 1.0));
                else if (int(i) != firstPoint)
                    vertexLight += set.lights[i].color.rgb * att * max(ndl, 0.0);
            }
            const float3 v0 = saturate(0.5 * vertexLight);
            float3 lit = v0;
            if (first >= 0)
                lit += nl1 * 0.5 * set.lights[first].color.rgb;
            if (firstPoint >= 0 && count > 2)
                lit += saturate(-(bn.x + bn.y + bn.z)) * 0.5 * set.lights[2].color.rgb;
            color.rgb = saturate(2.0 * albedo * lit * draw.color.rgb);
        } else {
            float3 diffuse = set.ambient.rgb;
            for (uint i = 0; i < count; ++i) {
                float att;
                const float3 l = lightVector(set.lights[i], in.world, att);
                float ndl = dot(n, l);
                // Hair: strands lit from either side, never fully dark (the
                // inner layers' normals point every way, often down into the
                // shell); the far side only a little, or every lamp behind
                // and below washes the hair out (red hair came out salmon).
                if (draw.flags & kTwoSidedLight)
                    ndl = max(0.08, ndl > 0.0 ? ndl : -0.3 * ndl);
                if (ndl <= 0.0 || att <= 0.0)
                    continue;
                diffuse += set.lights[i].color.rgb * att * ndl;
            }
            // Ambient only (no lights in the set): 0.5 x ambient x diffuse, no emissive.
            const float3 emissive = set.ambient.w > 0.0 ? draw.emissive.rgb : float3(0.0);
            color.rgb = saturate(2.0 * albedo * saturate(0.5 * (emissive + draw.color.rgb * diffuse)));
        }
        color.rgb = saturate(color.rgb + saturate(draw.specular.rgb * spec));
        if (frame.params.z > 0.5 && n.z > 0.7 && color.a > 0.99)
            color.rgb *= simShadows(in.world, uint(frame.params.z), shadows, shadowAtlas, maskSmp, true);
        if (draw.flags & kEnvCube) {
            if (draw.flags & kEnvLocked)
                r = (frame.view * float4(r, 0)).xyz;
            color.rgb += envCube.sample(cubeSmp, r, bias(draw.env.w)).rgb * draw.env.rgb * frame.environment.rgb;
        }
        if (cut)
            color.a = cutAlpha;
    }
    return color;
}

// The censor's compositing quad (CensorCompositingMaterial: the censor
// texture copied over the view target with point filtering): a screen
// rectangle at the depth of its box's nearest point, showing its tile of
// the low-resolution censor render, so each texel is a mosaic block.
struct CensorQuad {
    float4 rect;   // pixels: x0, y0, x1, y1 (y down)
    float4 uvRect; // the tile in the censor texture: u0, v0, u1, v1
    float4 target; // xy: the target's size in pixels, z: depth
};

struct CensorOut {
    float4 position [[position]];
    float2 uv;
};

vertex CensorOut censor_vs(uint vid [[vertex_id]], constant CensorQuad& q [[buffer(0)]]) {
    const float2 corner = float2(vid & 1, vid >> 1);
    const float2 px = mix(q.rect.xy, q.rect.zw, corner);
    CensorOut out;
    out.position = float4(px.x / q.target.x * 2.0 - 1.0, 1.0 - px.y / q.target.y * 2.0, q.target.z, 1.0);
    out.uv = mix(q.uvRect.xy, q.uvRect.zw, corner);
    return out;
}

fragment float4 censor_fs(CensorOut in [[stage_in]], texture2d<float> censorTexture [[texture(0)]],
                          sampler pointSmp [[sampler(0)]]) {
    return float4(censorTexture.sample(pointSmp, in.uv).rgb, 1.0);
}
