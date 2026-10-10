#include <metal_stdlib>
using namespace metal;

// Compiled at runtime with MTL::Device::newLibrary (no offline Metal compiler needed).

struct Vertex {
    packed_float3 position;
    packed_float3 color;
};

struct Uniforms {
    float4x4 mvp;
};

struct VertexOut {
    float4 position [[position]];
    float3 color;
};

vertex VertexOut vertex_main(uint vid [[vertex_id]],
                             const device Vertex* vertices [[buffer(0)]],
                             constant Uniforms& u [[buffer(1)]]) {
    VertexOut out;
    out.position = u.mvp * float4(float3(vertices[vid].position), 1.0);
    out.color = vertices[vid].color;
    return out;
}

fragment float4 fragment_main(VertexOut in [[stage_in]]) {
    return float4(in.color, 1.0);
}
