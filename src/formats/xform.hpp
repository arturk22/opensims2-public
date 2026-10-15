#pragma once

#include "scenegraph.hpp"

#include <cmath>

namespace os2 {

inline Quat operator*(const Quat& a, const Quat& b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

inline Quat conjugate(const Quat& q) { return {-q.x, -q.y, -q.z, q.w}; }

inline Quat normalized(const Quat& q) {
    const float n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (n == 0)
        return {};
    return {q.x / n, q.y / n, q.z / n, q.w / n};
}

// Normalized linear interpolation along the shorter arc.
inline Quat nlerp(const Quat& a, Quat b, float t) {
    if (a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w < 0)
        b = {-b.x, -b.y, -b.z, -b.w};
    return normalized({a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t});
}

// Spherical interpolation along the shorter arc, as the game's quaternion
// interpolation (Mac BV 0xdc75a0): normalized linear when the two are within
// |cos| >= 0.95 of each other.
inline Quat slerp(const Quat& a, Quat b, float t) {
    float d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    if (d < 0)
        b = {-b.x, -b.y, -b.z, -b.w}, d = -d;
    if (d >= 0.95f)
        return nlerp(a, b, t);
    const float theta = std::atan2(std::sqrt(1 - d * d), d);
    // The part of b orthogonal to a, normalized.
    const Quat v = normalized({b.x - a.x * d, b.y - a.y * d, b.z - a.z * d, b.w - a.w * d});
    const float c = std::cos(theta * t), s = std::sin(theta * t);
    return {a.x * c + v.x * s, a.y * c + v.y * s, a.z * c + v.z * s, a.w * c + v.w * s};
}

// Rotation about X, then Y, then Z (q = qz * qy * qx), angles in radians:
// the game's nGZSceneDBAnim::EulerToQuaternion (Mac BV 0x1b95508), the same
// formula term for term.
inline Quat quatFromEulerXYZ(float x, float y, float z) {
    const float sx = std::sin(x * 0.5f), cx = std::cos(x * 0.5f);
    const float sy = std::sin(y * 0.5f), cy = std::cos(y * 0.5f);
    const float sz = std::sin(z * 0.5f), cz = std::cos(z * 0.5f);
    return {cz * cy * sx - sz * cx * sy, cz * cx * sy + sz * cy * sx, sz * cx * cy - cz * sx * sy,
            cz * cx * cy + sz * sx * sy};
}

// Rigid transform: rotation (unit quaternion) then translation.
struct Xform {
    Quat q;
    Vec3 t{0, 0, 0};

    Vec3 rotate(const Vec3& v) const {
        // v + 2w(q x v) + 2 q x (q x v)
        const Vec3 u{q.x, q.y, q.z};
        auto cross = [](const Vec3& a, const Vec3& b) {
            return Vec3{a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
        };
        const Vec3 c1 = cross(u, v);
        const Vec3 c2 = cross(u, c1);
        return {v[0] + 2 * (q.w * c1[0] + c2[0]), v[1] + 2 * (q.w * c1[1] + c2[1]), v[2] + 2 * (q.w * c1[2] + c2[2])};
    }
    Vec3 apply(const Vec3& v) const {
        const Vec3 r = rotate(v);
        return {r[0] + t[0], r[1] + t[1], r[2] + t[2]};
    }
    // this ∘ local
    Xform then(const Xform& local) const { return {q * local.q, apply(local.t)}; }
    Xform inverse() const {
        Xform inv{conjugate(q), {0, 0, 0}};
        const Vec3 r = inv.rotate(t);
        inv.t = {-r[0], -r[1], -r[2]};
        return inv;
    }
};

// Column-major 3x4 affine matrix (rotation columns + translation), for blending.
struct Affine {
    float m[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};

    static Affine from(const Xform& x) {
        Affine a;
        const Vec3 cx = x.rotate({1, 0, 0}), cy = x.rotate({0, 1, 0}), cz = x.rotate({0, 0, 1});
        for (int k = 0; k < 3; ++k) {
            a.m[k] = cx[k];
            a.m[3 + k] = cy[k];
            a.m[6 + k] = cz[k];
            a.m[9 + k] = x.t[k];
        }
        return a;
    }
    Vec3 point(const Vec3& v) const {
        return {m[0] * v[0] + m[3] * v[1] + m[6] * v[2] + m[9], m[1] * v[0] + m[4] * v[1] + m[7] * v[2] + m[10],
                m[2] * v[0] + m[5] * v[1] + m[8] * v[2] + m[11]};
    }
    Vec3 vector(const Vec3& v) const {
        return {m[0] * v[0] + m[3] * v[1] + m[6] * v[2], m[1] * v[0] + m[4] * v[1] + m[7] * v[2],
                m[2] * v[0] + m[5] * v[1] + m[8] * v[2]};
    }
};

} // namespace os2
