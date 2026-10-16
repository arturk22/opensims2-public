Texture2D sprite : register(t0, space2);
SamplerState spriteSampler : register(s0, space2);

struct Input {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : TEXCOORD1;
};

float4 main(Input input) : SV_Target {
    return sprite.Sample(spriteSampler, input.uv) * input.color;
}
