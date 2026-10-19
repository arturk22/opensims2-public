#include <simd/simd.h>

#include <cmath>
#include <cstring>
#include <cstdio>

static int failures = 0;

static void check(bool cond, const char* what) {
    if (!cond) {
        std::printf("FAIL %s\n", what);
        ++failures;
    }
}

static bool near(float a, float b, float eps = 1e-5f) { return std::fabs(a - b) <= eps; }

static bool near3(simd_float3 a, simd_float3 b) { return near(a.x, b.x) && near(a.y, b.y) && near(a.z, b.z); }

int main() {
    check(sizeof(simd_float3) == 16 && alignof(simd_float3) == 16, "float3 is 16 bytes, 16-aligned");
    check(sizeof(simd_float4) == 16 && alignof(simd_float4) == 16, "float4 layout");
    check(sizeof(simd_float4x4) == 64 && alignof(simd_float4x4) == 16, "float4x4 layout");

    const simd_float4x4 m =
        simd_matrix(simd_float4(1, 2, 3, 4), simd_float4(5, 6, 7, 8), simd_float4(9, 10, 11, 12), simd_float4(13, 14, 15, 16));
    float raw[16];
    std::memcpy(raw, &m, sizeof raw);
    check(raw[0] == 1 && raw[1] == 2 && raw[4] == 5 && raw[5] == 6, "column 0 then column 1 in memory");

    const simd_float4 col0 = simd_mul(m, simd_float4(1, 0, 0, 0));
    check(col0.x == 1 && col0.y == 2 && col0.z == 3 && col0.w == 4, "matrix times +X is column 0");

    simd_float4x4 translated = matrix_identity_float4x4;
    translated.columns[3] = simd_float4(3, 4, 5, 1);
    const simd_float4x4 back = simd_mul(simd_inverse(translated), translated);
    check(near(back.columns[0].x, 1) && near(back.columns[1].y, 1) && near(back.columns[2].z, 1) &&
              near(back.columns[3].x, 0) && near(back.columns[3].w, 1),
          "inverse undoes a translation");

    const simd_quatf q = simd_quaternion(3.14159265f / 2, simd_float3(0, 0, 1));
    const simd_float3 turned = simd_act(q, simd_float3(1, 0, 0));
    check(near3(turned, simd_float3(0, 1, 0)), "90 deg about Z sends +X to +Y");
    const simd_float4x4 rot = simd_matrix4x4(q);
    const simd_float4 turned4 = simd_mul(rot, simd_float4(1, 0, 0, 1));
    check(near(turned4.x, 0) && near(turned4.y, 1) && near(turned4.z, 0), "quaternion matrix matches simd_act");

    const simd_quatf ends = simd_slerp(simd_quaternion(0, 0, 0, 1), q, 1);
    check(near(ends.vector.z, q.vector.z) && near(ends.vector.w, q.vector.w), "slerp at 1 is the destination");

    simd_float3 p{1, 2, 3};
    const simd_float2 xy = p.xy;
    check(xy.x == 1 && xy.y == 2, "read .xy");
    p.xy = simd_float2(9, 8);
    check(p.x == 9 && p.y == 8 && p.z == 3, "write .xy leaves z");

    const simd_float4 qv{1, 2, 3, 4};
    const simd_float3 divided = qv.xyz / qv.w;
    check(near3(divided, simd_float3(0.25f, 0.5f, 0.75f)), ".xyz / .w");
    const simd_float3 lead = simd_make_float3(qv);
    const simd_float2 lead2 = simd_make_float2(qv);
    check(lead.x == 1 && lead.y == 2 && lead.z == 3 && lead2.x == 1 && lead2.y == 2, "simd_make_float3/2 of a float4: its leading lanes");

    const simd_float4x4 rows = simd_matrix_from_rows(simd_float4(1, 2, 3, 4), simd_float4(5, 6, 7, 8),
                                                     simd_float4(9, 10, 11, 12), simd_float4(13, 14, 15, 16));
    check(rows.columns[0].x == 1 && rows.columns[0].y == 5 && rows.columns[1].x == 2, "rows become columns");

    check(simd_equal(simd_float3(1, 2, 3), simd_float3(1, 2, 3)), "equal vectors");
    check(!simd_equal(simd_float3(1, 2, 3), simd_float3(1, 2, 4)), "one component differs");

    if (failures) {
        std::printf("%d simd checks failed\n", failures);
        return 1;
    }
    std::printf("simd: ok\n");
    return 0;
}
