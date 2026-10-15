// Base texture, optional light map (walls, floors, terrain) and optional wall mask.

cbuffer Frame : register(b0, space3) {
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
    float4 eye;
    float4 specular;
    float4 emissive;
    float4 ambient;
    float shadowCount;
    float viewportX;
    float viewportY;
    float clipOn;
    float4 clipPlane;
    float4 lightMapMix; // x: mix(slot 0, slot 1, x)
};

struct Light {
    float4 direction;
    float4 color;
    float4 params;
    float4 spot;
};

static const uint kMaxLights = 16;

cbuffer Lights : register(b1, space3) {
    Light lights[kMaxLights];
};

struct ShadowRecord {
    float4 u;
    float4 v;
    float4 rect;
    float4 misc;
    float4 color;
};

cbuffer Shadows : register(b2, space3) {
    ShadowRecord shadows[16];
};

Texture2D baseTexture : register(t0, space2);
SamplerState baseSampler : register(s0, space2);
Texture2D lightMapTexture : register(t1, space2);
SamplerState lightMapSampler : register(s1, space2);
Texture2D maskTexture : register(t2, space2);
SamplerState maskSampler : register(s2, space2);
Texture2D shadowAtlas : register(t3, space2);
SamplerState shadowSampler : register(s3, space2);
Texture2D reflectionTex : register(t4, space2);
SamplerState reflectionSampler : register(s4, space2);
Texture2D lightMapNext : register(t5, space2);
SamplerState lightMapNextSampler : register(s5, space2);

struct Input {
    float4 position : SV_Position;
    float3 world : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float2 uv : TEXCOORD2;
    float2 baseUv : TEXCOORD3;
    float2 lightUv : TEXCOORD4;
    float3 localPos : TEXCOORD5;
    float4 color : TEXCOORD6;
};

static const uint kLighting = 1;
static const uint kAlphaTest = 2;
static const uint kMasked = 8;
static const uint kVertexColor = 16384;
static const uint kLightMapped = 65536;
static const uint kWater = 524288;
static const uint kTwoSidedLight = 1u << 25;
static const uint kWallMask = 1u << 27;
static const uint kMirror = 1u << 28;
static const uint kShadowCaster = 1u << 29;

float3 simShadows(float3 world, bool objects) {
    float3 kept = 1.0;
    const uint count = min((uint)shadowCount, 16u);
    for (uint i = 0; i < count; ++i) {
        const ShadowRecord s = shadows[i];
        if (objects && s.misc.y > 0.5) {
            continue;
        }
        if (world.z < s.misc.z - 1.5 || world.z > s.misc.z + 1.5) {
            continue;
        }
        const float2 uv = float2(dot(s.u.xyz, world) + s.u.w, dot(s.v.xyz, world) + s.v.w);
        if (any(uv < s.rect.xy) || any(uv > s.rect.zw)) {
            continue;
        }
        float3 acc = 0.0;
        [unroll] for (int j = -2; j <= 2; ++j) {
            [unroll] for (int q = -2; q <= 2; ++q) {
                acc += shadowAtlas.Sample(shadowSampler, clamp(uv + float2(q, j) * s.misc.x * 0.5, s.rect.xy, s.rect.zw)).rgb;
            }
        }
        acc /= 25.0;
        if (s.misc.y > 0.5) {
            const float cover = saturate((1.0 - acc.g) / (1.0 - 0.63));
            kept *= lerp(1.0, s.color.rgb, smoothstep(0.3, 0.48, cover));
        } else {
            kept *= acc;
        }
    }
    return kept;
}

float3 lightVector(Light light, float3 world, out float att) {
    att = 1.0;
    if (light.params.z < 0.5) {
        return light.direction.xyz;
    }
    const float3 delta = light.direction.xyz - world;
    const float dist = length(delta);
    const float safe = max(dist, 1e-6);
    att = dist < light.params.y ? 1.0 / (1.0 + light.params.x * dist * dist) : 0.0;
    if (light.spot.w >= -1.0) {
        const float rho = dot(-delta / safe, light.spot.xyz);
        att *= saturate((rho - light.spot.w) / max(light.params.w - light.spot.w, 1e-4));
    }
    return delta / safe;
}

float4 main(Input input, bool front : SV_IsFrontFace) : SV_Target {
    if (clipOn > 0.5 && dot(clipPlane.xyz, input.world) + clipPlane.w < 0.0) {
        discard;
    }
    if ((flags & kMirror) != 0) {
        const float2 uv = input.position.xy / max(float2(viewportX, viewportY), float2(1.0, 1.0));
        return float4(reflectionTex.Sample(reflectionSampler, uv).rgb, 1.0);
    }
    if (groundClip > -1e8 && input.world.z < groundClip) {
        discard;
    }
    float4 tex = baseTexture.Sample(baseSampler, input.baseUv);
    if ((flags & kVertexColor) != 0) {
        tex *= input.color;
    }
    if ((flags & kWallMask) != 0) {
        const float u = dot(maskU.xyz, input.localPos) + maskU.w;
        const float v = dot(maskV.xyz, input.localPos) + maskV.w;
        tex.a *= maskTexture.Sample(maskSampler, float2(u, v)).a;
    } else if ((flags & kMasked) != 0) {
        const float2 muv = input.localPos.xy * maskU.xy + maskU.zw;
        tex.a *= maskTexture.Sample(maskSampler, muv).a;
    }
    if ((flags & kAlphaTest) != 0 && tex.a < alphaRef) {
        discard;
    }
    if ((flags & kShadowCaster) != 0) {
        return float4(color.rgb, 1.0);
    }
    const float alpha = tex.a * color.a;
    const float3 n = normalize(front ? input.normal : -input.normal);
    const bool receive = shadowCount > 0.5 && n.z > 0.35;
    if ((flags & kLightMapped) != 0) {
        const float3 lit = lerp(lightMapTexture.Sample(lightMapSampler, input.lightUv).rgb,
                                lightMapNext.Sample(lightMapNextSampler, input.lightUv).rgb, saturate(lightMapMix.x));
        float3 rgb = saturate(tex.rgb * color.rgb * lit * lightGain);
        if (receive) {
            rgb *= simShadows(input.world, false);
        }
        return float4(rgb, alpha);
    }
    const uint count = min((uint)ambient.w, kMaxLights);
    if ((flags & kLighting) != 0 && (flags & kWater) != 0) {
        float3 diffuse = ambient.rgb;
        for (uint i = 0; i < count; ++i) {
            float att;
            const float3 l = lightVector(lights[i], input.world, att);
            diffuse += lights[i].color.rgb * att * max(dot(n, l), 0.0);
        }
        return float4(tex.rgb * saturate(color.rgb * diffuse), alpha);
    }
    if ((flags & kLighting) != 0) {
        const float3 view = normalize(eye.xyz - input.world);
        const float3 mirror = reflect(-view, n);
        float3 diffuse = ambient.rgb;
        float3 spec = 0.0;
        for (uint i = 0; i < count; ++i) {
            float att;
            const float3 l = lightVector(lights[i], input.world, att);
            float ndl = dot(n, l);
            if ((flags & kTwoSidedLight) != 0) {
                ndl = max(0.08, ndl > 0.0 ? ndl : -0.3 * ndl);
            }
            if (ndl <= 0.0 || att <= 0.0) {
                continue;
            }
            diffuse += lights[i].color.rgb * att * ndl;
            if (specular.w > 0.0) {
                spec += lights[i].color.rgb * att * lights[i].color.w * pow(max(dot(mirror, l), 0.0), specular.w);
            }
        }
        const float3 emissiveTerm = ambient.w > 0.0 ? emissive.rgb : 0.0;
        float3 rgb = saturate(2.0 * tex.rgb * saturate(0.5 * (emissiveTerm + color.rgb * diffuse)));
        rgb = saturate(rgb + saturate(specular.rgb * spec));
        if (receive && alpha > 0.99) {
            rgb *= simShadows(input.world, true);
        }
        return float4(rgb, alpha);
    }
    return float4(tex.rgb * color.rgb, alpha);
}
