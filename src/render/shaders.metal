#include <metal_stdlib>
using namespace metal;

// Compiled at runtime with MTL::Device::newLibrary (no offline Metal compiler needed).

struct Vertex {
    packed_float3 position;
    packed_float3 normal;
};

struct Instance {
    float4x4 model;
    float4 tint; // rgb color; w > 0.5 draws a one-tile checkerboard
};

struct Frame {
    float4x4 viewProj;
    float4 lightDir; // xyz: direction the light travels, world space
};

struct VertexOut {
    float4 position [[position]];
    float3 world;
    float3 normal;
    float4 tint;
};

vertex VertexOut vertex_main(uint vid [[vertex_id]], uint iid [[instance_id]],
                             const device Vertex* vertices [[buffer(0)]],
                             const device Instance* instances [[buffer(1)]],
                             constant Frame& frame [[buffer(2)]]) {
    const device Instance& inst = instances[iid];
    float4 world = inst.model * float4(float3(vertices[vid].position), 1.0);
    VertexOut out;
    out.position = frame.viewProj * world;
    out.world = world.xyz;
    out.normal = (inst.model * float4(float3(vertices[vid].normal), 0.0)).xyz;
    out.tint = inst.tint;
    return out;
}

fragment float4 fragment_main(VertexOut in [[stage_in]], constant Frame& frame [[buffer(2)]]) {
    float3 color = in.tint.rgb;
    if (in.tint.w > 0.5) {
        int2 tile = int2(floor(in.world.xy));
        color *= ((tile.x + tile.y) & 1) ? 0.82 : 1.0;
    }
    float diffuse = max(dot(normalize(in.normal), -frame.lightDir.xyz), 0.0);
    return float4(color * (0.35 + 0.65 * diffuse), 1.0);
}
