// Camera-facing particle quads. Positions are already in world space.

cbuffer Frame : register(b0, space1) {
    column_major float4x4 viewProj;
};

struct Input {
    float3 position : TEXCOORD0;
    float2 uv : TEXCOORD1;
    float4 color : TEXCOORD2;
};

struct Output {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : TEXCOORD1;
};

Output main(Input input) {
    Output output;
    output.position = mul(viewProj, float4(input.position, 1.0));
    output.uv = input.uv;
    output.color = input.color;
    return output;
}
