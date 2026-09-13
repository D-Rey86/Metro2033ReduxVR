#pragma once
#include <cmath>
#include <cstdint>

// Negative ownership evidence only. A world beam seen outside the player's
// measured reach must not become a player beam merely because the eye approaches.
// World positions are used so head/controller motion cannot move this history.
class NpcLaserHistory {
    struct Entry {
        float position[3] = {};
        uint32_t frame = 0;
        bool valid = false;
    };
    Entry entries[32];
public:
    bool Observe(uint32_t frame, const float world[3], float eyeDistanceSquared) {
        if (!std::isfinite(eyeDistanceSquared)) return false;
        for (int i = 0; i < 3; ++i)
            if (!std::isfinite(world[i])) return false;
        int match = -1, freeSlot = -1;
        float nearest = 0.35f * 0.35f;
        for (int i = 0; i < 32; ++i) {
            Entry &e = entries[i];
            if (e.valid && (frame < e.frame || frame - e.frame > 2))
                e.valid = false;
            if (!e.valid) { if (freeSlot < 0) freeSlot = i; continue; }
            float distance = 0;
            for (int j = 0; j < 3; ++j) {
                const float delta = world[j] - e.position[j];
                distance += delta * delta;
            }
            if (distance < nearest) { nearest = distance; match = i; }
        }
        // Captured player maximum is 1.320m. Do not seed from a shell rejection
        // or an eye-plane rejection: both can describe a valid player animation.
        if (match < 0 && eyeDistanceSquared <= 1.5f * 1.5f) return false;
        const int slot = match >= 0 ? match : freeSlot;
        if (slot >= 0) {
            Entry &e = entries[slot];
            for (int j = 0; j < 3; ++j) e.position[j] = world[j];
            e.frame = frame;
            e.valid = true;
        }
        return true;
    }
};
