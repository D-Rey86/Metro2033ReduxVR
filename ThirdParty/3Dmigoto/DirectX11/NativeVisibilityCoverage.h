#pragma once
#include <math.h>
#include <string.h>

namespace NativeVisibilityCoverage {
// Row-vector native matrices; headset projections are row-major column-vector.
// Retain the native depth columns and only widen the lateral clip inequalities.
// A roll-independent envelope includes both eyes without a stale roll sample.
inline bool Widen(const float *view, const float *projection, const float *vp,
    const float eyes[2][16], float *output)
{
    if (!view || !projection || !vp || !output) return false;
    for (unsigned i = 0; i < 16; ++i)
        if (!isfinite(view[i]) || !isfinite(projection[i]) || !isfinite(vp[i]))
            return false;
    // Do not reinterpret orthographic, oblique or asymmetric native projections.
    const unsigned zero[] = {1,2,3,4,6,7,8,9,12,13,15};
    for (unsigned i = 0; i < sizeof(zero)/sizeof(zero[0]); ++i)
        if (fabsf(projection[zero[i]]) > 0.00001f) return false;
    if (projection[0] <= 0 || projection[5] <= 0 ||
        fabsf(projection[11] - 1) > 0.00001f) return false;
    // Match the buffered view/projection product, not a live unrelated camera.
    for (unsigned r = 0; r < 4; ++r) for (unsigned c = 0; c < 4; ++c) {
        float value = 0;
        for (unsigned k = 0; k < 4; ++k) value += view[r*4+k]*projection[k*4+c];
        if (fabsf(value-vp[r*4+c]) > 0.0001f*(1+fabsf(value))) return false;
    }
    float radius = 0;
    for (unsigned e = 0; e < 2; ++e) {
        const float *p = eyes[e];
        for (unsigned i = 0; i < 16; ++i) if (!isfinite(p[i])) return false;
        if (p[0] <= 0 || p[5] <= 0) return false;
        for (int x = -1; x <= 1; x += 2) for (int y = -1; y <= 1; y += 2) {
            float tx = (x-p[2])/p[0], ty = (y-p[6])/p[5];
            float distance = sqrtf(tx*tx+ty*ty);
            if (!isfinite(distance) || distance > 8) return false;
            if (distance > radius) radius = distance;
        }
    }
    if (radius <= 0) return false;
    const float target = 1/radius;
    const float sx = target < projection[0] ? target/projection[0] : 1;
    const float sy = target < projection[5] ? target/projection[5] : 1;
    memcpy(output, vp, 16*sizeof(float));
    for (unsigned r = 0; r < 4; ++r) {
        output[r*4] *= sx;
        output[r*4+1] *= sy;
    }
    return true;
}
}
