#pragma once
#include <cmath>
#include <cstdint>

// Capture-derived effect settings, independent of particle count, camera axes
// and interpolation time. This identifies effect templates, not emitter owners.
namespace FlameParticleProfile {
enum Kind { None, Stream, LocalNozzle };
inline Kind Classify(uint64_t vs, uint64_t ps, const float *cb)
{
    if (!cb) return None;
    static const float profiles[][12] = {
        {.125f,.125f,8,64, 0,1.5f,1,0, 0,.1f,0,0},
        {.125f,.125f,8,64, 0,1,1,0, 0,.1f,0,0},
        {.125f,.125f,8,64, 1,2,1,0, 0,.1f,0,0},
        {.125f,.0625f,8,48, 1,.5f,1,0, .05f,0,0,0},
        {.5f,1.f/3,2,6, 0,1.25f,1,0, 0,.1f,0,0},
        {.125f,.0625f,8,128, 1,1,1,1, .05f,0,0,0},
    };
    static const uint64_t vertex[] = {
        0x2403427473C35A13ull,0x2403427473C35A13ull,
        0xE1FDE311D1E35018ull,0xE1FDE311D1E35018ull,
        0x2403427473C35A13ull,0xE1FDE311D1E35018ull};
    static const uint64_t pixel[] = {
        0x74E951E588F5E7C3ull,0x74E951E588F5E7C3ull,
        0xFB0FCA1B2C641F6Eull,0x252FCE3C8790BFBCull,
        0xF5CBE24C2E4098EBull,0xFB0FCA1B2C641F6Eull};
    for (int p=0; p<6; ++p) {
        if (vs != vertex[p] || ps != pixel[p]) continue;
        bool match = true;
        for (int i=0; i<12; ++i) {
            const float x = cb[12+i];
            // Captures 1438 and2082: nozzle scale is .3 and1 respectively.
            if (p==5 && i==5) {
                if (!std::isfinite(x) || x<.2999f || x>1.0001f) match=false;
            } else if (!std::isfinite(x) || std::fabs(x-profiles[p][i])>.0001f)
                match=false;
        }
        if (match) return p==5 ? LocalNozzle : Stream;
    }
    return None;
}
}
