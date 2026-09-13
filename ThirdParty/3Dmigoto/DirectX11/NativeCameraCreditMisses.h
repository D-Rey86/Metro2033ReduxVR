#pragma once
#include "NativeCameraCommandCredits.h"

// Temporary bounded diagnostics. No ownership/rendering decisions depend on
// these records; the existing 0.0001 acceptance threshold remains unchanged.
namespace NativeCameraCredits {
enum MissKind { NoCredit, OldEpoch, NonFiniteView, RotationGap, TranslationGap, MissKinds };
struct MissSample {
    unsigned frame;
    MissKind kind;
    uint64_t generation, creditEpoch, epoch;
    float rotationGap, translationGap;
};
struct MissStats {
    uint64_t count[MissKinds];
    unsigned first[MissKinds], last[MissKinds];
    float maxRotationGap, maxTranslationGap;
    unsigned used;
    MissSample samples[32];
    void Clear() { memset(this, 0, sizeof(*this)); }
    void Record(const Credit &credit, uint64_t epoch, const float *view, unsigned frame)
    {
        MissSample s = {};
        s.frame = frame; s.epoch = epoch;
        // An invalid replay may retain old storage; never imply provenance.
        s.creditEpoch = credit.valid ? credit.epoch : 0;
        s.generation = credit.valid ? credit.generation : 0;
        s.kind = NoCredit;
        if (credit.valid && credit.epoch != epoch) s.kind = OldEpoch;
        else if (credit.valid) {
            bool finite = true;
            for (unsigned r = 0; r < 3; ++r) for (unsigned c = 0; c < 4; ++c) {
                const float a = view[r*4+c], b = credit.nativeView[c*4+r];
                if (!isfinite(a) || !isfinite(b)) { finite = false; continue; }
                const float gap = fabsf(a-b);
                float &maxGap = c == 3 ? s.translationGap : s.rotationGap;
                if (gap > maxGap) maxGap = gap;
            }
            s.kind = !finite ? NonFiniteView :
                (s.rotationGap > .0001f ? RotationGap : TranslationGap);
        }
        if (!count[s.kind]++) first[s.kind] = frame;
        last[s.kind] = frame;
        if (s.rotationGap > maxRotationGap) maxRotationGap = s.rotationGap;
        if (s.translationGap > maxTranslationGap) maxTranslationGap = s.translationGap;
        // At most one example of a reason per frame; fixed 32-record ceiling.
        for (unsigned i = 0; i < used; ++i)
            if (samples[i].kind == s.kind && samples[i].frame == frame) return;
        if (used < 32) samples[used++] = s;
    }
};
} // namespace NativeCameraCredits
