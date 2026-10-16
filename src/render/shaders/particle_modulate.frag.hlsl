// Modulate particle: the destination times the texture colour, faded to 1 by
// alpha (the pipeline blends destination colour x source).

Texture2D particleTexture : register(t0, space2);
SamplerState particleSampler : register(s0, space2);

struct Input {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : TEXCOORD1;
};

float4 main(Input input) : SV_Target {
    float4 c = input.color * particleTexture.Sample(particleSampler, input.uv);
    return float4(lerp(float3(1.0, 1.0, 1.0), c.rgb, c.a), 1.0);
}
