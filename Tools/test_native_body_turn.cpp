#include "../ThirdParty/3Dmigoto/DirectX11/NativeBodyTurnIntent.h"
#include <stdio.h>
#include <stdlib.h>
static unsigned checks;
static void Check(bool value) {
    ++checks;
    if (!value) { fprintf(stderr, "FAIL %u\n", checks); exit(1); }
}
int main() {
    NativeBodyTurn::Intent intent;
    Check(intent.Take() == 0);
    intent.Publish(30, true, false);
    Check(intent.Take() == 30);
    Check(intent.Take() == 0); // repeated native update cannot duplicate input
    intent.Publish(30, true, false);
    intent.Publish(-15, true, false);
    Check(intent.Take() == -15); // newest intent, not a stale 30-15 backlog
    intent.Publish(30, true, false);
    intent.Publish(0, false, false);
    Check(intent.Take() == 0); // release before consumption
    intent.Publish(30, true, false);
    intent.Clear();
    Check(intent.Take() == 0); // menu, tracking loss, toggle, recenter
    intent.Publish(494, true, true);
    intent.Publish(0, true, true);
    Check(intent.Take() == 494); // preserve one snap across latched polls
    for (unsigned i = 0; i < 100; ++i) {
        intent.Publish(0, true, true);
        Check(intent.Take() == 0); // holding cannot repeat the snap
    }
    intent.Publish(0, false, true);
    intent.Publish(-494, true, true);
    Check(intent.Take() == -494);
    intent.Publish(494, true, true);
    intent.Publish(10, true, false);
    Check(intent.Take() == 10); // mode change cannot replay a snap
    intent.Publish(30, true, false);
    (void)intent.Take(); // blocked native ownership consumes/discards request
    Check(intent.Take() == 0); // no catch-up turn on return
    for (unsigned i = 0; i < 10000; ++i) intent.Publish(30, true, false);
    Check(intent.Take() == 30); // producer-only stall remains bounded
    Check(intent.Take() == 0);
    printf("PASS: %u native body-turn intent checks\n", checks);
    return 0;
}
