#include <metal_stdlib>
using namespace metal;

// Compiled at runtime with MTL::Device::newLibrary (no offline Metal compiler needed).

struct Vertex {
    packed_float3 position;
    packed_float3 normal;
    packed_float2 uv;
};

enum : uint {
    kLighting = 1,  // lit with the frame's directional light
    kAlphaTest = 2, // discard below alphaRef
    kChecker = 4,   // one-tile checkerboard (test floor)
};

struct Draw {
    float4x4 model;
    float4 color;    // diffuse rgb, alpha multiplier
    float4 specular; // rgb, power in w
    float4 emissive; // rgb
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
    float4 eye;     // xyz: camera position
    float4 ambient; // rgb: skylight colour times strength
    Light lights[kMaxLights];
    uint lightCount;
};

struct VertexOut {
    float4 position [[position]];
    float3 world;
    float3 normal;
    float2 uv;
};

vertex VertexOut vertex_main(uint vid [[vertex_id]],
                             const device Vertex* vertices [[buffer(0)]],
                             constant Draw& draw [[buffer(1)]],
                             constant Frame& frame [[buffer(2)]]) {
    float4 world = draw.model * float4(float3(vertices[vid].position), 1.0);
    VertexOut out;
    out.position = frame.viewProj * world;
    out.world = world.xyz;
    out.normal = (draw.model * float4(float3(vertices[vid].normal), 0.0)).xyz;
    out.uv = vertices[vid].uv;
    return out;
}

fragment float4 fragment_main(VertexOut in [[stage_in]], bool front [[front_facing]],
                              constant Draw& draw [[buffer(1)]],
                              constant Frame& frame [[buffer(2)]],
                              texture2d<float> base [[texture(0)]],
                              sampler smp [[sampler(0)]]) {
    float4 tex = base.sample(smp, in.uv);
    if ((draw.flags & kAlphaTest) && tex.a < draw.alphaRef)
        discard_fragment();
    float3 albedo = tex.rgb;
    if (draw.flags & kChecker) {
        int2 tile = int2(floor(in.world.xy));
        albedo *= ((tile.x + tile.y) & 1) ? 0.82 : 1.0;
    }
    float4 color = float4(albedo * draw.color.rgb, tex.a * draw.color.a);
    if (draw.flags & kLighting) {
        // Per-pixel version of fixed-function lighting: emissive plus diffuse
        // times (ambient + lights) saturates at 1 before it modulates the
        // texture, and specular is added on top.
        float3 n = normalize(front ? in.normal : -in.normal);
        float3 v = normalize(frame.eye.xyz - in.world);
        float3 diffuse = frame.ambient.rgb;
        float3 spec = 0.0;
        for (uint i = 0; i < min(frame.lightCount, kMaxLights); ++i) {
            float3 l = frame.lights[i].direction.xyz;
            float ndl = dot(n, l);
            if (ndl <= 0.0)
                continue;
            diffuse += frame.lights[i].color.rgb * ndl;
            spec += frame.lights[i].color.rgb * pow(max(dot(n, normalize(l + v)), 0.0), draw.specular.w);
        }
        color.rgb = albedo * saturate(draw.emissive.rgb + draw.color.rgb * diffuse) + saturate(draw.specular.rgb * spec);
    }
    return color;
}
