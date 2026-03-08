#pragma once
#include <cmath>

// Column-major 4x4 matrix (matches glTF / OpenGL convention)
// m[col*4 + row]
struct mat4 {
    float m[16];
};

inline mat4 mat4_identity() {
    mat4 r = {};
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
    return r;
}

inline void quat_normalize(float* q) {
    float len = sqrtf(q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3]);
    if (len < 1e-12f) {
        q[0] = q[1] = q[2] = 0; q[3] = 1.0f;
    } else {
        float inv_len = 1.0f / len;
        q[0] *= inv_len; q[1] *= inv_len; q[2] *= inv_len; q[3] *= inv_len;
    }
}

inline void quat_mul(const float* a, const float* b, float* r) {
    float x = a[3]*b[0] + a[0]*b[3] + a[1]*b[2] - a[2]*b[1];
    float y = a[3]*b[1] - a[0]*b[2] + a[1]*b[3] + a[2]*b[0];
    float z = a[3]*b[2] + a[0]*b[1] - a[1]*b[0] + a[2]*b[3];
    float w = a[3]*b[3] - a[0]*b[0] - a[1]*b[1] - a[2]*b[2];
    r[0] = x; r[1] = y; r[2] = z; r[3] = w;
}

inline mat4 mat4_mul(const mat4& a, const mat4& b) {
    mat4 r;
    for (int col = 0; col < 4; col++) {
        for (int row = 0; row < 4; row++) {
            float sum = 0;
            for (int k = 0; k < 4; k++) sum += a.m[k*4 + row] * b.m[col*4 + k];
            r.m[col*4 + row] = sum;
        }
    }
    return r;
}

// Build a mat4 from TRS (translate, rotate[xyzw], scale)
inline mat4 mat4_from_trs(const float* t, const float* r, const float* s) {
    mat4 m = mat4_identity();
    float x = r[0], y = r[1], z = r[2], w = r[3];
    m.m[0]  = (1 - 2*y*y - 2*z*z) * s[0];
    m.m[1]  = (2*x*y + 2*z*w)      * s[0];
    m.m[2]  = (2*x*z - 2*y*w)      * s[0];
    m.m[4]  = (2*x*y - 2*z*w)      * s[1];
    m.m[5]  = (1 - 2*x*x - 2*z*z)  * s[1];
    m.m[6]  = (2*y*z + 2*x*w)      * s[1];
    m.m[8]  = (2*x*z + 2*y*w)      * s[2];
    m.m[9]  = (2*y*z - 2*x*w)      * s[2];
    m.m[10] = (1 - 2*x*x - 2*y*y)  * s[2];
    m.m[12] = t[0]; m.m[13] = t[1]; m.m[14] = t[2];
    return m;
}

// General 4x4 invert. Forces affine integrity on result.
inline mat4 mat4_invert(const mat4& m) {
    mat4 inv;
    float s0 = m.m[0]*m.m[5]  - m.m[4]*m.m[1];
    float s1 = m.m[0]*m.m[6]  - m.m[4]*m.m[2];
    float s2 = m.m[0]*m.m[7]  - m.m[4]*m.m[3];
    float s3 = m.m[1]*m.m[6]  - m.m[5]*m.m[2];
    float s4 = m.m[1]*m.m[7]  - m.m[5]*m.m[3];
    float s5 = m.m[2]*m.m[7]  - m.m[6]*m.m[3];
    float c5 = m.m[10]*m.m[15] - m.m[14]*m.m[11];
    float c4 = m.m[9]*m.m[15]  - m.m[13]*m.m[11];
    float c3 = m.m[9]*m.m[14]  - m.m[13]*m.m[10];
    float c2 = m.m[8]*m.m[15]  - m.m[12]*m.m[11];
    float c1 = m.m[8]*m.m[14]  - m.m[12]*m.m[10];
    float c0 = m.m[8]*m.m[13]  - m.m[12]*m.m[9];
    float det = s0*c5 - s1*c4 + s2*c3 + s3*c2 - s4*c1 + s5*c0;
    if (std::abs(det) < 1e-6f) return mat4_identity();
    float idet = 1.0f / det;
    inv.m[0]  = ( m.m[5]*c5  - m.m[6]*c4  + m.m[7]*c3)  * idet;
    inv.m[1]  = (-m.m[1]*c5  + m.m[2]*c4  - m.m[3]*c3)  * idet;
    inv.m[2]  = ( m.m[13]*s5 - m.m[14]*s4 + m.m[15]*s3) * idet;
    inv.m[3]  = (-m.m[9]*s5  + m.m[10]*s4 - m.m[11]*s3) * idet;
    inv.m[4]  = (-m.m[4]*c5  + m.m[6]*c2  - m.m[7]*c1)  * idet;
    inv.m[5]  = ( m.m[0]*c5  - m.m[2]*c2  + m.m[3]*c1)  * idet;
    inv.m[6]  = (-m.m[12]*s5 + m.m[14]*s2 - m.m[15]*s1) * idet;
    inv.m[7]  = ( m.m[8]*s5  - m.m[10]*s2 + m.m[11]*s1) * idet;
    inv.m[8]  = ( m.m[4]*c4  - m.m[5]*c2  + m.m[7]*c0)  * idet;
    inv.m[9]  = (-m.m[0]*c4  + m.m[1]*c2  - m.m[3]*c0)  * idet;
    inv.m[10] = ( m.m[12]*s4 - m.m[13]*s2 + m.m[15]*s0) * idet;
    inv.m[11] = (-m.m[8]*s4  + m.m[9]*s2  - m.m[11]*s0) * idet;
    inv.m[12] = (-m.m[4]*c3  + m.m[5]*c1  - m.m[6]*c0)  * idet;
    inv.m[13] = ( m.m[0]*c3  - m.m[1]*c1  + m.m[2]*c0)  * idet;
    inv.m[14] = (-m.m[12]*s3 + m.m[13]*s1 - m.m[14]*s0) * idet;
    inv.m[15] = ( m.m[8]*s3  - m.m[9]*s1  + m.m[10]*s0) * idet;
    // Force affine integrity (no perspective component)
    inv.m[3] = inv.m[7] = inv.m[11] = 0.0f;
    inv.m[15] = 1.0f;
    return inv;
}
