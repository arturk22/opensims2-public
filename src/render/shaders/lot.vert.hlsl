// Lot and object vertices. Matrices are column-major, matching os2 simd_float4x4.

cbuffer Frame : register(b0, space1) {
    column_major float4x4 viewProj;
    column_major float4x4 model;
    float4 color;
    float4 uv0;
    float4 uv1;
    float4 maskU;
    float4 maskV;
    uint flags;
    float alphaRef;
    float lightGain;
    float groundClip;
};

struct Input {
    float3 position : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float2 uv : TEXCOORD2;
    float2 uv2 : TEXCOORD3;
    uint color : TEXCOORD4;
};

struct Output {
    float4 position : SV_Position;
    float3 world : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float2 uv : TEXCOORD2;
    float2 baseUv : TEXCOORD3;
    float2 lightUv : TEXCOORD4;
    float3 localPos : TEXCOORD5;
    float4 color : TEXCOORD6;
};

Output main(Input input) {
    const float4 world = mul(model, float4(input.position, 1.0));
    Output output;
    output.position = mul(viewProj, world);
    output.world = world.xyz;
    output.normal = mul((float3x3)model, input.normal);
    output.uv = input.uv;
    const float3 uvh = float3(input.uv, 1.0);
    output.baseUv = float2(dot(uv0.xyz, uvh), dot(uv1.xyz, uvh));
    output.lightUv = input.uv2;
    output.localPos = input.position;
    const float a = float((input.color >> 24) & 0xFF) / 255.0;
    const float r = float((input.color >> 16) & 0xFF) / 255.0;
    const float g = float((input.color >> 8) & 0xFF) / 255.0;
    const float b = float(input.color & 0xFF) / 255.0;
    output.color = float4(r, g, b, a);
    return output;
}
