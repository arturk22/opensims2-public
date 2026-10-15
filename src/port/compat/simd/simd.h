#pragma once

// Stand-in for Apple's <simd/simd.h> on Windows. The Mac build keeps the
// system header. Layout matches the Apple types the renderer copies into
// GPU buffers: column-major float4x4, simd_float3 padded to 16 bytes.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4201)
#endif

struct simd_float2;
struct simd_float3;
struct simd_float4;

struct simd_float2 {
    float x = 0, y = 0;
    simd_float2() = default;
    simd_float2(float s) : x(s), y(s) {} // a scalar fills every lane, as Clang's vectors
    simd_float2(float x_, float y_) : x(x_), y(y_) {}
    float& operator[](int i) { return (&x)[i]; }
    float operator[](int i) const { return (&x)[i]; }
};

// Overlays the first two floats of a float3/float4 so `.xy` reads and writes
// them. Copy stays trivial: only operator=(simd_float2) is user-declared.
struct SimdSwizzle2 {
    float x, y;
    operator simd_float2() const { return {x, y}; }
    SimdSwizzle2& operator=(simd_float2 v) {
        x = v.x;
        y = v.y;
        return *this;
    }
};

struct SimdSwizzle3 {
    float x, y, z;
    operator simd_float3() const;
};

struct alignas(16) simd_float3 {
    union {
        struct {
            float x, y, z, _pad;
        };
        SimdSwizzle2 xy;
        float v[4];
    };
    simd_float3() : x(0), y(0), z(0), _pad(0) {}
    simd_float3(float s) : x(s), y(s), z(s), _pad(0) {}
    simd_float3(float x_, float y_, float z_) : x(x_), y(y_), z(z_), _pad(0) {}
    float& operator[](int i) { return v[i]; }
    float operator[](int i) const { return v[i]; }
    simd_float3& operator+=(simd_float3 o) {
        x += o.x;
        y += o.y;
        z += o.z;
        return *this;
    }
    simd_float3& operator-=(simd_float3 o) {
        x -= o.x;
        y -= o.y;
        z -= o.z;
        return *this;
    }
    simd_float3& operator*=(float s) {
        x *= s;
        y *= s;
        z *= s;
        return *this;
    }
    simd_float3& operator/=(float s) {
        x /= s;
        y /= s;
        z /= s;
        return *this;
    }
};

struct alignas(16) simd_float4 {
    union {
        struct {
            float x, y, z, w;
        };
        SimdSwizzle2 xy;
        SimdSwizzle3 xyz;
        float v[4];
    };
    simd_float4() : x(0), y(0), z(0), w(0) {}
    simd_float4(float s) : x(s), y(s), z(s), w(s) {}
    simd_float4(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}
    float& operator[](int i) { return v[i]; }
    float operator[](int i) const { return v[i]; }
    simd_float4& operator+=(simd_float4 o) {
        x += o.x;
        y += o.y;
        z += o.z;
        w += o.w;
        return *this;
    }
    simd_float4& operator-=(simd_float4 o) {
        x -= o.x;
        y -= o.y;
        z -= o.z;
        w -= o.w;
        return *this;
    }
    simd_float4& operator*=(float s) {
        x *= s;
        y *= s;
        z *= s;
        w *= s;
        return *this;
    }
    simd_float4& operator/=(float s) {
        x /= s;
        y /= s;
        z /= s;
        w /= s;
        return *this;
    }
};

inline SimdSwizzle3::operator simd_float3() const { return {x, y, z}; }

struct alignas(16) simd_float4x4 {
    simd_float4 columns[4];
    simd_float4x4()
        : columns{simd_float4(1, 0, 0, 0), simd_float4(0, 1, 0, 0), simd_float4(0, 0, 1, 0), simd_float4(0, 0, 0, 1)} {}
    simd_float4x4(simd_float4 c0, simd_float4 c1, simd_float4 c2, simd_float4 c3) : columns{c0, c1, c2, c3} {}
};

struct alignas(16) simd_quatf {
    simd_float4 vector;
    simd_quatf() : vector(0, 0, 0, 1) {}
    simd_quatf(float x, float y, float z, float w) : vector(x, y, z, w) {}
    explicit simd_quatf(simd_float4 v) : vector(v) {}
};

struct simd_int2 {
    int x = 0, y = 0;
    simd_int2() = default;
    simd_int2(int x_, int y_) : x(x_), y(y_) {}
};

inline const simd_float4x4 matrix_identity_float4x4{simd_float4(1, 0, 0, 0), simd_float4(0, 1, 0, 0),
                                                    simd_float4(0, 0, 1, 0), simd_float4(0, 0, 0, 1)};

inline simd_float2 simd_make_float2(float x, float y) { return {x, y}; }
inline simd_float3 simd_make_float3(float x, float y, float z) { return {x, y, z}; }
inline simd_float4 simd_make_float4(float x, float y, float z, float w) { return {x, y, z, w}; }
inline simd_float4 simd_make_float4(simd_float3 v, float w) { return {v.x, v.y, v.z, w}; }
inline simd_float4 simd_make_float4(simd_float2 v, float z, float w) { return {v.x, v.y, z, w}; }
inline simd_int2 simd_make_int2(int x, int y) { return {x, y}; }

inline simd_float2 operator+(simd_float2 a, simd_float2 b) { return {a.x + b.x, a.y + b.y}; }
inline simd_float2 operator-(simd_float2 a, simd_float2 b) { return {a.x - b.x, a.y - b.y}; }
inline simd_float2 operator*(simd_float2 a, simd_float2 b) { return {a.x * b.x, a.y * b.y}; }
inline simd_float2 operator/(simd_float2 a, simd_float2 b) { return {a.x / b.x, a.y / b.y}; }
inline simd_float2 operator-(simd_float2 a) { return {-a.x, -a.y}; }

inline simd_float3 operator+(simd_float3 a, simd_float3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline simd_float3 operator-(simd_float3 a, simd_float3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline simd_float3 operator*(simd_float3 a, simd_float3 b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }
inline simd_float3 operator/(simd_float3 a, simd_float3 b) { return {a.x / b.x, a.y / b.y, a.z / b.z}; }
inline simd_float3 operator-(simd_float3 a) { return {-a.x, -a.y, -a.z}; }

inline simd_float4 operator+(simd_float4 a, simd_float4 b) { return {a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w}; }
inline simd_float4 operator-(simd_float4 a, simd_float4 b) { return {a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w}; }
inline simd_float4 operator*(simd_float4 a, simd_float4 b) { return {a.x * b.x, a.y * b.y, a.z * b.z, a.w * b.w}; }
inline simd_float4 operator/(simd_float4 a, simd_float4 b) { return {a.x / b.x, a.y / b.y, a.z / b.z, a.w / b.w}; }
inline simd_float4 operator-(simd_float4 a) { return {-a.x, -a.y, -a.z, -a.w}; }

inline simd_float2 operator+(simd_float2 a, float s) { return a + simd_float2(s); }
inline simd_float2 operator+(float s, simd_float2 a) { return simd_float2(s) + a; }
inline simd_float2 operator-(simd_float2 a, float s) { return a - simd_float2(s); }
inline simd_float2 operator*(simd_float2 a, float s) { return {a.x * s, a.y * s}; }
inline simd_float2 operator*(float s, simd_float2 a) { return a * s; }
inline simd_float2 operator/(simd_float2 a, float s) { return {a.x / s, a.y / s}; }

inline simd_float3 operator+(simd_float3 a, float s) { return a + simd_float3(s); }
inline simd_float3 operator+(float s, simd_float3 a) { return simd_float3(s) + a; }
inline simd_float3 operator-(simd_float3 a, float s) { return a - simd_float3(s); }
inline simd_float3 operator*(simd_float3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline simd_float3 operator*(float s, simd_float3 a) { return a * s; }
inline simd_float3 operator/(simd_float3 a, float s) { return {a.x / s, a.y / s, a.z / s}; }

inline simd_float4 operator+(simd_float4 a, float s) { return a + simd_float4(s); }
inline simd_float4 operator+(float s, simd_float4 a) { return simd_float4(s) + a; }
inline simd_float4 operator-(simd_float4 a, float s) { return a - simd_float4(s); }
inline simd_float4 operator*(simd_float4 a, float s) { return {a.x * s, a.y * s, a.z * s, a.w * s}; }
inline simd_float4 operator*(float s, simd_float4 a) { return a * s; }
inline simd_float4 operator/(simd_float4 a, float s) { return {a.x / s, a.y / s, a.z / s, a.w / s}; }

inline float simd_dot(simd_float2 a, simd_float2 b) { return a.x * b.x + a.y * b.y; }
inline float simd_dot(simd_float3 a, simd_float3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float simd_dot(simd_float4 a, simd_float4 b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }

inline float simd_length_squared(simd_float2 a) { return simd_dot(a, a); }
inline float simd_length_squared(simd_float3 a) { return simd_dot(a, a); }
inline float simd_length_squared(simd_float4 a) { return simd_dot(a, a); }
inline float simd_length(simd_float2 a) { return std::sqrt(simd_length_squared(a)); }
inline float simd_length(simd_float3 a) { return std::sqrt(simd_length_squared(a)); }
inline float simd_length(simd_float4 a) { return std::sqrt(simd_length_squared(a)); }
inline float simd_distance(simd_float2 a, simd_float2 b) { return simd_length(a - b); }
inline float simd_distance(simd_float3 a, simd_float3 b) { return simd_length(a - b); }

inline simd_float2 simd_normalize(simd_float2 a) {
    const float len = simd_length(a);
    return len > 0 ? a * simd_float2(1.f / len) : simd_float2(0);
}
inline simd_float3 simd_normalize(simd_float3 a) {
    const float len = simd_length(a);
    return len > 0 ? a * simd_float3(1.f / len) : simd_float3(0);
}
inline simd_float4 simd_normalize(simd_float4 a) {
    const float len = simd_length(a);
    return len > 0 ? a * simd_float4(1.f / len) : simd_float4(0);
}

inline simd_float3 simd_cross(simd_float3 a, simd_float3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

inline simd_float2 simd_min(simd_float2 a, simd_float2 b) { return {std::min(a.x, b.x), std::min(a.y, b.y)}; }
inline simd_float3 simd_min(simd_float3 a, simd_float3 b) {
    return {std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)};
}
inline simd_float4 simd_min(simd_float4 a, simd_float4 b) {
    return {std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z), std::min(a.w, b.w)};
}
inline simd_float2 simd_max(simd_float2 a, simd_float2 b) { return {std::max(a.x, b.x), std::max(a.y, b.y)}; }
inline simd_float3 simd_max(simd_float3 a, simd_float3 b) {
    return {std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)};
}
inline simd_float4 simd_max(simd_float4 a, simd_float4 b) {
    return {std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z), std::max(a.w, b.w)};
}
inline simd_float3 simd_clamp(simd_float3 v, simd_float3 lo, simd_float3 hi) { return simd_min(simd_max(v, lo), hi); }
inline simd_float3 simd_mix(simd_float3 a, simd_float3 b, simd_float3 t) { return a + (b - a) * t; }
inline float simd_reduce_max(simd_float3 a) { return std::max(a.x, std::max(a.y, a.z)); }

// Call sites use this as a boolean ("the box is empty"), so this is true when
// every component matches. Apple's simd_equal returns a mask; nothing here
// inspects that mask element by element.
inline bool simd_equal(simd_float3 a, simd_float3 b) { return a.x == b.x && a.y == b.y && a.z == b.z; }

inline simd_float4x4 simd_matrix(simd_float4 c0, simd_float4 c1, simd_float4 c2, simd_float4 c3) {
    return {c0, c1, c2, c3};
}
inline simd_float4x4 simd_matrix_from_rows(simd_float4 r0, simd_float4 r1, simd_float4 r2, simd_float4 r3) {
    return simd_matrix(simd_float4(r0.x, r1.x, r2.x, r3.x), simd_float4(r0.y, r1.y, r2.y, r3.y),
                       simd_float4(r0.z, r1.z, r2.z, r3.z), simd_float4(r0.w, r1.w, r2.w, r3.w));
}
inline simd_float4x4 simd_transpose(simd_float4x4 m) {
    return simd_matrix_from_rows(m.columns[0], m.columns[1], m.columns[2], m.columns[3]);
}

inline simd_float4 simd_mul(simd_float4x4 a, simd_float4 v) {
    return a.columns[0] * v.x + a.columns[1] * v.y + a.columns[2] * v.z + a.columns[3] * v.w;
}
inline simd_float4x4 simd_mul(simd_float4x4 a, simd_float4x4 b) {
    return simd_matrix(simd_mul(a, b.columns[0]), simd_mul(a, b.columns[1]), simd_mul(a, b.columns[2]),
                       simd_mul(a, b.columns[3]));
}

inline simd_float4x4 simd_inverse(simd_float4x4 m) {
    float a[4][8]{};
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r)
            a[r][c] = m.columns[c][r];
        a[c][4 + c] = 1.f;
    }
    for (int col = 0; col < 4; ++col) {
        int pivot = col;
        float best = std::fabs(a[col][col]);
        for (int r = col + 1; r < 4; ++r) {
            const float v = std::fabs(a[r][col]);
            if (v > best) {
                best = v;
                pivot = r;
            }
        }
        if (!(best > 0.f))
            return m;
        if (pivot != col) {
            for (int k = 0; k < 8; ++k)
                std::swap(a[col][k], a[pivot][k]);
        }
        const float div = a[col][col];
        for (int k = 0; k < 8; ++k)
            a[col][k] /= div;
        for (int r = 0; r < 4; ++r) {
            if (r == col)
                continue;
            const float f = a[r][col];
            for (int k = 0; k < 8; ++k)
                a[r][k] -= f * a[col][k];
        }
    }
    simd_float4x4 out;
    for (int c = 0; c < 4; ++c)
        out.columns[c] = simd_float4(a[0][4 + c], a[1][4 + c], a[2][4 + c], a[3][4 + c]);
    return out;
}

inline simd_quatf simd_quaternion(float x, float y, float z, float w) { return {x, y, z, w}; }
inline simd_quatf simd_quaternion(float angle, simd_float3 axis) {
    axis = simd_normalize(axis);
    const float h = angle * 0.5f;
    const float s = std::sin(h);
    return {axis.x * s, axis.y * s, axis.z * s, std::cos(h)};
}

inline simd_quatf simd_normalize(simd_quatf q) { return simd_quatf(simd_normalize(q.vector)); }

inline float simd_dot(simd_quatf a, simd_quatf b) { return simd_dot(a.vector, b.vector); }

inline simd_quatf simd_slerp(simd_quatf a, simd_quatf b, float t) {
    float d = simd_dot(a, b);
    if (d < 0) {
        b.vector = -b.vector;
        d = -d;
    }
    if (d > 0.9995f)
        return simd_normalize(simd_quatf(a.vector + (b.vector - a.vector) * t));
    d = std::min(d, 1.f);
    const float theta = std::acos(d);
    const float s = std::sin(theta);
    const float w0 = std::sin((1.f - t) * theta) / s;
    const float w1 = std::sin(t * theta) / s;
    return simd_quatf(a.vector * w0 + b.vector * w1);
}

inline simd_float4x4 simd_matrix4x4(simd_quatf q) {
    const float x = q.vector.x, y = q.vector.y, z = q.vector.z, w = q.vector.w;
    const float xx = x * x, yy = y * y, zz = z * z;
    const float xy = x * y, xz = x * z, yz = y * z;
    const float wx = w * x, wy = w * y, wz = w * z;
    return simd_matrix(simd_float4(1 - 2 * (yy + zz), 2 * (xy + wz), 2 * (xz - wy), 0),
                       simd_float4(2 * (xy - wz), 1 - 2 * (xx + zz), 2 * (yz + wx), 0),
                       simd_float4(2 * (xz + wy), 2 * (yz - wx), 1 - 2 * (xx + yy), 0), simd_float4(0, 0, 0, 1));
}

// Rotates v by the unit quaternion q (v' = q v q*).
inline simd_float3 simd_act(simd_quatf q, simd_float3 v) {
    const simd_float3 u{q.vector.x, q.vector.y, q.vector.z};
    const simd_float3 t = simd_cross(u, v) * 2.f;
    return v + t * q.vector.w + simd_cross(u, t);
}

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

static_assert(sizeof(simd_float2) == 8, "simd_float2");
static_assert(alignof(simd_float2) == 4 || alignof(simd_float2) == 8, "simd_float2 align");
static_assert(sizeof(simd_float3) == 16, "simd_float3 is 16 bytes, like Apple simd");
static_assert(alignof(simd_float3) == 16, "simd_float3 align");
static_assert(sizeof(simd_float4) == 16, "simd_float4");
static_assert(alignof(simd_float4) == 16, "simd_float4 align");
static_assert(sizeof(simd_float4x4) == 64, "simd_float4x4");
static_assert(alignof(simd_float4x4) == 16, "simd_float4x4 align");
static_assert(sizeof(simd_quatf) == 16, "simd_quatf");
static_assert(offsetof(simd_float4x4, columns) == 0, "column-major matrix starts at column 0");
