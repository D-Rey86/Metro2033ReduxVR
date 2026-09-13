// Executes the actual adapter implementation with mocked native functions.
// This verifies wiring/order without claiming the real engine ABI is proven.
#include <windows.h>
#include <intrin.h>
#include <stdio.h>
#include <stdlib.h>
#include "../ThirdParty/3Dmigoto/DirectX11/NativeCameraCommandCredits.h"
static unsigned checks;
static void Check(bool v) { ++checks; if (!v) { fprintf(stderr, "FAIL %u\n", checks); exit(1); } }
static void *mockCaller;
static void *TestReturnAddress() { return mockCaller; }
#define _ReturnAddress TestReturnAddress
static bool selected = true;
static LONG sUpstreamCameraYieldingToScript = 0;
static float mockYaw = .1f, mockPitch = .02f;
static bool NativeStateFollowSelected() { return selected; }
static float GetCullingCancellation() { return mockYaw; }
static float GetCullingCancellationPitch() { return mockPitch; }
static void LogInfo(const char *, ...) {}
struct MockHookManager {
    DWORD Hook(SIZE_T *, LPVOID *, LPVOID, LPVOID) { return 1; }
} cHookMgr;
#include "../ThirdParty/3Dmigoto/DirectX11/NativeCameraCreditAdapter.inl"
#undef _ReturnAddress

static BYTE image[0xE00000];
static __declspec(align(16)) BYTE stream[0x8000];
static float produced[16];
static unsigned consumed;
static bool flushInsideRecord;
static bool validResults[8];
static float yawResults[8];
struct Recorder { BYTE pad[16]; BYTE *buffer; unsigned cursor; };
static void *FakeBuild(float *out, const float *, const float *, const float *) {
    memcpy(out, produced, sizeof(produced)); return out;
}
static void *FakeCopy(void *dst, const void *src) { memcpy(dst, src, 0x350); return dst; }
static uintptr_t FakeRecord(void *r, const float *matrix) {
    Recorder *rec = (Recorder *)r;
    float saved[16]; memcpy(saved, matrix, sizeof(saved));
    if (flushInsideRecord) {
        flushInsideRecord = false;
        CameraCreditAdapter::Execute(rec->buffer);
        rec->cursor = 0;
    }
    const unsigned aligned = (rec->cursor + 4) & ~3u;
    unsigned *data = (unsigned *)rec->buffer;
    while (rec->cursor + 1 < aligned) data[rec->cursor++] = 0x70000001;
    data[rec->cursor++] = 0x70000014;
    memcpy(data + rec->cursor, saved, 64);
    rec->cursor += 16;
    data[rec->cursor] = 0x70000000;
    return 123;
}
static uintptr_t FakeSet(void *, const float *matrix) {
    float view[12];
    for (unsigned r = 0; r < 3; ++r)
        for (unsigned c = 0; c < 4; ++c) view[r*4+c] = matrix[c*4+r];
    float y = -999, p = -999;
    Check(consumed < 8);
    validResults[consumed] = GetNativeCameraRenderCredit(view, &y, &p);
    yawResults[consumed++] = y;
    return 456;
}
static void FakeExecute(void *buffer) {
    unsigned *p = (unsigned *)buffer;
    while (*p != 0x70000000) {
        if (*p == 0x70000001) { ++p; continue; }
        Check(*p++ == 0x70000014);
        void *saved = mockCaller;
        mockCaller = image + 0x7EEAE4;
        Check(CameraCreditAdapter::SetCamera(NULL, (float *)p) == 456);
        mockCaller = saved;
        p += 16;
    }
}
static void Init() {
    using namespace CameraCreditAdapter;
    module = image; build = FakeBuild; copy = FakeCopy;
    record = FakeRecord; execute = FakeExecute; setCamera = FakeSet;
    ready = 1; epoch = 1; generation = 0;
    depth = 0; replay = NULL; current.valid = false;
    commands.Clear(); memset(slots, 0, sizeof(slots));
    memset(produced, 0, sizeof(produced));
    produced[0] = produced[5] = produced[10] = produced[15] = 1;
    consumed = 0; selected = true; sUpstreamCameraYieldingToScript = 0;
    flushInsideRecord = false;
    mockYaw = .1f;
}
static float *Global() { return (float *)(image + 0xD07730); }
static float *Previous() { return (float *)(image + 0xD22F20); }
static void Publish() {
    mockCaller = image + 0x69BB54;
    Check(CameraCreditAdapter::Build(Global(), NULL, NULL, NULL) == Global());
}
static void BufferCamera() {
    using namespace CameraCreditAdapter;
    Copy(image + 0xD23230, image + 0xD076F0);
    Copy(image + 0xD22EE0, image + 0xD23230);
}
int main() {
    Init(); Publish(); BufferCamera();
    Recorder rec = {}; rec.buffer = stream;
    Check(CameraCreditAdapter::Record(&rec, Previous()) == 123);
    // Advance both native state and its buffers before replaying old commands.
    mockYaw = .4f; Publish(); BufferCamera();
    CameraCreditAdapter::Execute(stream);
    Check(consumed == 1 && validResults[0] && yawResults[0] == .1f);
    rec.cursor = 0; consumed = 0;
    CameraCreditAdapter::Record(&rec, Previous());
    CameraCreditAdapter::Execute(stream);
    Check(validResults[0] && yawResults[0] == .4f);

    Init(); Publish(); BufferCamera(); rec.cursor = 0;
    CameraCreditAdapter::Record(&rec, Previous());
    ResetNativeCameraCreditEpoch();
    CameraCreditAdapter::Execute(stream);
    Check(!validResults[0]);

    Init(); Publish(); BufferCamera(); rec.cursor = 0;
    // Untracked camera has the identical matrix, but MUST NOT borrow credit.
    CameraCreditAdapter::Record(&rec, produced);
    CameraCreditAdapter::Record(&rec, Previous());
    CameraCreditAdapter::Execute(stream);
    Check(consumed == 2 && !validResults[0] && validResults[1]);

    Init(); Publish(); BufferCamera(); rec.cursor = 0;
    Previous()[12] += 1; // native rewrite not observed by sidecar
    CameraCreditAdapter::Record(&rec, Previous());
    CameraCreditAdapter::Execute(stream);
    Check(!validResults[0]);

    Init(); Publish(); BufferCamera(); rec.cursor = 0;
    CameraCreditAdapter::Record(&rec, Previous());
    CameraCreditAdapter::depth = 8;
    CameraCreditAdapter::Execute(stream);
    Check(!validResults[0] && !CameraCreditAdapter::current.valid);

    Init(); Publish(); BufferCamera(); rec.cursor = 0;
    CameraCreditAdapter::Record(&rec, Previous());
    mockYaw = .4f; Publish(); BufferCamera();
    flushInsideRecord = true;
    CameraCreditAdapter::Record(&rec, Previous());
    Check(consumed == 1 && validResults[0] && yawResults[0] == .1f);
    CameraCreditAdapter::Execute(stream);
    Check(consumed == 2 && validResults[1] && yawResults[1] == .4f);

    // Verify the acceptance rule and repeated rejection after diagnostic removal.
    float view[12] = { 1,0,0,0, 0,1,0,0, 0,0,1,0 };
    float testYaw = -999, testPitch = -999;
    view[3] = .00011f;
    Check(!GetNativeCameraRenderCredit(view, &testYaw, &testPitch));
    Check(testYaw == -999 && testPitch == -999);
    view[0] = 1.01f;
    Check(!GetNativeCameraRenderCredit(view, &testYaw, &testPitch));
    view[0] = 1; view[3] = .00009f;
    Check(GetNativeCameraRenderCredit(view, &testYaw, &testPitch));
    view[3] = .00011f;
    for (unsigned i = 0; i < 80; ++i) {
        Check(!GetNativeCameraRenderCredit(view, &testYaw, &testPitch));
    }
    printf("PASS: %u native adapter checks (mocked engine, actual C++ adapters)\n", checks);
    return 0;
}
