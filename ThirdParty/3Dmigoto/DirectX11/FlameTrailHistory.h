#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

// Render-side continuity, not engine particle IDs. Match an effect template
// key and nearby native trajectory; never retain the current gun pose for an
// already observed particle. Unmatched/recycled particles start at current pose.
class FlameTrailHistory {
    struct Entry {
        uint64_t key;
        float pos[3], velocity[3], transform[12];
        double time;
        unsigned frame;
    };
    std::vector<Entry> entries;
#ifdef FLAME_TRAIL_TEST_METRICS
    uint64_t retained=0, born=0;
#endif
public:
    void Clear() {
        entries.clear();
#ifdef FLAME_TRAIL_TEST_METRICS
        retained=born=0;
#endif
    }
    size_t Size() const { return entries.size(); }
#ifdef FLAME_TRAIL_TEST_METRICS
    uint64_t Retained() const { return retained; }
    uint64_t Born() const { return born; }
#endif
    bool Observe(uint64_t key, const float pos[3], const float velocity[3],
        double time, unsigned frame, const float spawn[12], float result[12])
    {
        for (int i=0; i<3; ++i)
            if (!std::isfinite(pos[i]) || !std::isfinite(velocity[i])) return false;
        for (int i=0; i<12; ++i) if (!std::isfinite(spawn[i])) return false;
        if (!std::isfinite(time)) return false;
        Entry *best = nullptr;
        float bestScore = 1.e30f;
        for (auto &e : entries) {
            const double dt=time-e.time;
            if (e.key!=key || frame<e.frame || dt<0 || dt>.2) continue;
            float direct=0, predicted=0, dv=0, speed2=0;
            for (int i=0; i<3; ++i) {
                const float d=pos[i]-e.pos[i];
                const float p=d-e.velocity[i]*static_cast<float>(dt);
                const float v=velocity[i]-e.velocity[i];
                direct+=d*d; predicted+=p*p; dv+=v*v;
                speed2+=e.velocity[i]*e.velocity[i];
            }
            // A repeated draw of identical payload shares the same history;
            // different particles cannot consume an entry twice in one frame.
            if (e.frame==frame) {
                if (direct<1.e-10f && dv<1.e-8f) {
                    memcpy(result,e.transform,sizeof(e.transform)); return true;
                }
                continue;
            }
            const float radius=.06f+std::sqrt(speed2)*static_cast<float>(dt)*.6f;
            const float error=direct<predicted ? direct : predicted;
            if (error>radius*radius || dv>9.f+speed2*.25f) continue;
            const float score=error+dv*.0001f;
            if (score<bestScore) { best=&e; bestScore=score; }
        }
        if (!best) {
            // Recycle expired entries before growing; hard bounded at2048.
            for (auto &e : entries)
                if (time-e.time>.2 || time<e.time || frame<e.frame) { best=&e; break; }
            if (!best && entries.size()<2048) {
                entries.push_back(Entry{}); best=&entries.back();
            }
            if (!best) { memcpy(result,spawn,sizeof(float)*12); return true; }
#ifdef FLAME_TRAIL_TEST_METRICS
            ++born;
#endif
            best->key=key;
            memcpy(best->transform,spawn,sizeof(best->transform));
        }
#ifdef FLAME_TRAIL_TEST_METRICS
        else ++retained;
#endif
        memcpy(best->pos,pos,sizeof(best->pos));
        memcpy(best->velocity,velocity,sizeof(best->velocity));
        best->time=time; best->frame=frame;
        memcpy(result,best->transform,sizeof(best->transform));
        return true;
    }
};
