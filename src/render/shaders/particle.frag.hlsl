// Effect particle: vertex colour times the effect texture.

Texture2D particleTexture : register(t0, space2);
SamplerState particleSampler : register(s0, space2);

struct Input {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : TEXCOORD1;
};

float4 main(Input input) : SV_Target {
    return input.color * particleTexture.Sample(particleSampler, input.uv);
}
