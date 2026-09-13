#include "../ThirdParty/3Dmigoto/DirectX11/NativeCameraCommandCredits.h"
#include <stdio.h>
#include <stdlib.h>

using namespace NativeCameraCredits;
static Commands commands;
static Replay replay, nested;
static unsigned checks;
static void Check(bool value) {
    ++checks;
    if (!value) { fprintf(stderr, "FAILED check %u\n", checks); exit(1); }
}
static Credit Make(uint64_t generation, float yaw, bool valid = true) {
    Credit c = {};
    c.epoch = 1; c.generation = generation; c.yaw = yaw; c.valid = valid;
    c.nativeView[0] = c.nativeView[5] = c.nativeView[10] = c.nativeView[15] = 1;
    return c;
}
int main() {
    Credit a = Make(1, .1f), b = Make(2, .4f), out = {};
    Check(commands.Record(0x1000, 16, a));
    Check(commands.TakeReplay(0x1000, &replay));
    const uint64_t firstSerial = replay.serial;
    // Native buffer reused while an independent replay snapshot remains alive.
    Check(commands.Record(0x1000, 16, b));
    Check(commands.TakeReplay(0x1000, &nested));
    Check(nested.serial != firstSerial);
    Check(replay.Consume(a.nativeView, 1, &out) && out.generation == 1);
    Check(nested.Consume(b.nativeView, 1, &out) && out.generation == 2);
    Check(replay.Complete() && nested.Complete());

    commands.Clear();
    Credit unknown = Make(3, 0, false);
    Check(commands.Record(0x2000, 16, unknown));
    Check(commands.Record(0x2000, 96, a));
    Check(commands.TakeReplay(0x2000, &replay));
    Check(!replay.Complete());
    Check(!replay.Consume(unknown.nativeView, 1, &out) && !replay.poisoned);
    Check(replay.Consume(a.nativeView, 1, &out));
    Check(!replay.Consume(a.nativeView, 1, &out) && replay.poisoned);

    commands.Clear();
    Check(commands.Record(0x2000, 16, a));
    Check(commands.Record(0x2000, 96, b));
    Check(commands.TakeReplay(0x2000, &replay));
    Credit other = a; other.nativeView[12] = 5;
    Check(!replay.Consume(other.nativeView, 1, &out) && replay.poisoned);
    Check(!replay.Consume(b.nativeView, 1, &out)); // no search-ahead recovery

    commands.Clear();
    Check(commands.Record(0x1000, 16, a));
    Check(commands.TakeReplay(0x1000, &replay));
    Check(!replay.Consume(a.nativeView, 2, &out)); // old epoch
    Check(commands.TakeReplay(0x9999, &replay) && replay.count == 0);
    Check(!replay.Consume(a.nativeView, 1, &out));

    commands.Clear();
    Check(commands.Record(0x1000, 16, a));
    Check(!commands.Record(0x1000, 16, b)); // unobserved reuse
    Check(commands.Failed());
    Check(!commands.TakeReplay(0x1000, &replay));
    commands.Clear();
    Check(!commands.Record(0x1000, kBufferBytes - 48, a));
    commands.Clear();
    Check(!commands.Record(0x1000, 17, a));

    commands.Clear();
    for (unsigned i = 0; i < kBuffers; ++i)
        Check(commands.Record(0x1000 + i * 0x10000, 16, a));
    Check(!commands.Record(0xFFFF0000, 16, b));
    Check(commands.Failed());
    commands.Clear();
    Check(!commands.TakeReplay(0, &replay));
    printf("PASS: %u native command-credit checks (optimized C++)\n", checks);
    return 0;
}
