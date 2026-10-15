cbuffer Screen : register(b0, space1) {
    float2 size;
    float2 pad;
};

struct Input {
    float2 pixel : TEXCOORD0;
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
    const float2 clip = float2(input.pixel.x / size.x * 2.0 - 1.0, 1.0 - input.pixel.y / size.y * 2.0);
    output.position = float4(clip, 0.0, 1.0);
    output.uv = input.uv;
    output.color = input.color;
    return output;
}
