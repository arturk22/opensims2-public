#include <metal_stdlib>
using namespace metal;

// Compiled at runtime with MTL::Device::newLibrary (no offline Metal compiler needed).

struct Vertex {
    packed_float3 position;
    packed_float3 normal;
    packed_float2 uv;
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
    float4 misc;     // x: cel edge offset, y: bump strength
    float alphaRef;
    uint flags;
};

constant uint kMaxLights = 8;

struct Light {
    float4 direction; // xyz: unit vector towards the light, world space
    float4 color;     // rgb: colour times intensity
};

struct Frame {
    float4x4 viewProj;
    float4x4 view;
    float4 eye;     // xyz: camera position
    float4 ambient; // rgb: skylight colour times strength
    Light lights[kMaxLights];
    uint lightCount;
};

struct VertexOut {
    float4 position [[position]];
    float3 world;
    float3 normal;
    float2 uv;     // as stored
    float2 baseUv; // with the texture coordinate animation
    float2 local;  // model-space xy
    float4 color;
};

vertex VertexOut vertex_main(uint vid [[vertex_id]],
                             const device Vertex* vertices [[buffer(0)]],
                             constant Draw& draw [[buffer(1)]],
                             constant Frame& frame [[buffer(2)]]) {
    const Vertex v = vertices[vid];
    float4 world = draw.model * float4(float3(v.position), 1.0);
    VertexOut out;
    out.position = frame.viewProj * world;
    out.world = world.xyz;
    out.normal = (draw.model * float4(float3(v.normal), 0.0)).xyz;
    out.uv = v.uv;
    const float3 uv1 = float3(v.uv, 1.0);
    out.baseUv = float2(dot(draw.uv0.xyz, uv1), dot(draw.uv1.xyz, uv1));
    out.local = float3(v.position).xy;
    out.color = float4((v.color >> 16) & 0xFF, (v.color >> 8) & 0xFF, v.color & 0xFF, v.color >> 24) / 255.0;
    return out;
}

// Normal bumped by a height map, with the tangent frame taken from the screen
// derivatives of position and uv (the game converts its "-bump" height maps
// into normal maps when it loads them).
float3 bumpedNormal(float3 n, float3 world, float2 uv, texture2d<float> height, sampler smp, float strength) {
    const float2 size = float2(height.get_width(), height.get_height());
    const float h = height.sample(smp, uv).r;
    const float hu = height.sample(smp, uv + float2(1.0 / size.x, 0)).r;
    const float hv = height.sample(smp, uv + float2(0, 1.0 / size.y)).r;
    const float3 tn = normalize(float3((h - hu) * strength, (h - hv) * strength, 1.0));
    const float3 dp1 = dfdx(world), dp2 = dfdy(world);
    const float2 duv1 = dfdx(uv), duv2 = dfdy(uv);
    const float3 dp2perp = cross(dp2, n), dp1perp = cross(n, dp1);
    const float3 t = dp2perp * duv1.x + dp1perp * duv2.x;
    const float3 b = dp2perp * duv1.y + dp1perp * duv2.y;
    const float m = max(dot(t, t), dot(b, b));
    if (m <= 1e-20)
        return n;
    const float inv = rsqrt(m);
    return normalize(t * inv * tn.x + b * inv * tn.y + n * tn.z);
}

fragment float4 fragment_main(VertexOut in [[stage_in]], bool front [[front_facing]],
                              constant Draw& draw [[buffer(1)]],
                              constant Frame& frame [[buffer(2)]],
                              texture2d<float> base [[texture(0)]],
                              texture2d<float> mask [[texture(1)]],
                              texture2d<float> underlay [[texture(2)]],
                              texture2d<float> overlay1 [[texture(3)]],
                              texture2d<float> overlay2 [[texture(4)]],
                              texture2d<float> height [[texture(5)]],
                              texturecube<float> envCube [[texture(6)]],
                              texture2d<float> ramp [[texture(7)]],
                              texture2d<float> edgeRamp [[texture(8)]],
                              sampler smp [[sampler(0)]],
                              sampler maskSmp [[sampler(1)]],
                              sampler cubeSmp [[sampler(2)]],
                              sampler pointSmp [[sampler(3)]]) {
    float4 tex = base.sample(smp, in.baseUv);
    if (draw.flags & kAlphaReplicate)
        tex.rgb = tex.a;
    if (draw.flags & kAlphaOnly)
        tex.rgb = 1.0;
    if (draw.flags & kUnderlay)
        tex = float4(mix(underlay.sample(smp, in.uv).rgb, tex.rgb, tex.a), 1.0);
    if (draw.flags & kOverlay1) {
        float4 o = overlay1.sample(smp, in.uv);
        tex.rgb = mix(tex.rgb, o.rgb, o.a);
    }
    if (draw.flags & kOverlay2) {
        float4 o = overlay2.sample(smp, in.uv);
        tex.rgb = mix(tex.rgb, o.rgb, o.a);
    }
    if (draw.flags & kVertexColor)
        tex *= in.color;
    if (draw.flags & kMasked)
        tex.a *= mask.sample(maskSmp, in.local * draw.mask.xy + draw.mask.zw).r;
    if ((draw.flags & kAlphaTest) && tex.a < draw.alphaRef)
        discard_fragment();
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

    if (draw.flags & kLighting) {
        if (draw.flags & kNormalMap)
            n = bumpedNormal(n, in.world, in.baseUv, height, smp, draw.misc.y);
        // Per-pixel version of fixed-function lighting: emissive plus diffuse
        // times (ambient + lights) saturates at 1 before it modulates the
        // texture, and specular is added on top.
        float3 v = normalize(frame.eye.xyz - in.world);
        float3 diffuse = frame.ambient.rgb;
        float3 spec = 0.0;
        for (uint i = 0; i < min(frame.lightCount, kMaxLights); ++i) {
            float3 l = frame.lights[i].direction.xyz;
            float ndl = dot(n, l);
            if (ndl <= 0.0)
                continue;
            diffuse += frame.lights[i].color.rgb * ndl;
            if (draw.specular.w > 0.0)
                spec += frame.lights[i].color.rgb * pow(max(dot(n, normalize(l + v)), 0.0), draw.specular.w);
        }
        color.rgb = albedo * saturate(draw.emissive.rgb + draw.color.rgb * diffuse) + saturate(draw.specular.rgb * spec);
        if (draw.flags & kEnvCube) {
            float3 r = reflect(-v, n);
            if (draw.flags & kEnvLocked)
                r = (frame.view * float4(r, 0)).xyz;
            color.rgb += envCube.sample(cubeSmp, r, bias(draw.env.w)).rgb * draw.env.rgb;
        }
    }
    return color;
}
