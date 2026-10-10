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
    float4 color; // diffuse rgb, alpha multiplier
    float alphaRef;
    uint flags;
};

struct Frame {
    float4x4 viewProj;
    float4 lightDir; // xyz: direction the light travels, world space
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
    float4 color = float4(tex.rgb * draw.color.rgb, tex.a * draw.color.a);
    if ((draw.flags & kAlphaTest) && tex.a < draw.alphaRef)
        discard_fragment();
    if (draw.flags & kChecker) {
        int2 tile = int2(floor(in.world.xy));
        color.rgb *= ((tile.x + tile.y) & 1) ? 0.82 : 1.0;
    }
    if (draw.flags & kLighting) {
        float3 n = normalize(front ? in.normal : -in.normal);
        float diffuse = max(dot(n, -frame.lightDir.xyz), 0.0);
        color.rgb *= 0.45 + 0.55 * diffuse;
    }
    return color;
}
