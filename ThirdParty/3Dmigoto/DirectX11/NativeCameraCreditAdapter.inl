// Included inside VRPose after native-look ownership state is declared.
// Camera-credit integration. Native arguments, camera matrices and commands
// are always passed through unchanged. Only the render cancellation consumes
// this sidecar. No heap allocation, GPU reads or periodic file output.
namespace CameraCreditAdapter {
using NativeCameraCredits::Credit;
using NativeCameraCredits::Replay;
static NativeCameraCredits::Commands commands;
static SRWLOCK lock = SRWLOCK_INIT;
static Credit slots[3] = {};
static const DWORD_PTR slotRvas[3] = { 0xD076F0, 0xD23230, 0xD22EE0 };
static BYTE *module = NULL;
static volatile LONG ready = 0, epoch = 1;
static uint64_t generation = 0;
static __declspec(thread) Replay *replay = NULL;
static __declspec(thread) Replay replayStack[8];
static __declspec(thread) Credit current = {};
static __declspec(thread) unsigned depth = 0;
typedef void *(__fastcall *BuildFn)(float *, const float *, const float *, const float *);
typedef void *(__fastcall *CopyFn)(void *, const void *);
typedef uintptr_t (__fastcall *CameraFn)(void *, const float *);
typedef void (__fastcall *ExecuteFn)(void *);
static BuildFn build = NULL;
static CopyFn copy = NULL;
static CameraFn record = NULL, setCamera = NULL;
static ExecuteFn execute = NULL;

static int Slot(const void *p, unsigned offset = 0)
{
    for (int i = 0; i < 3; ++i)
        if (p == module + slotRvas[i] + offset) return i;
    return -1;
}
static LONG Epoch() { return InterlockedCompareExchange(&epoch, 0, 0); }
static void RecordingFault()
{
    AcquireSRWLockExclusive(&lock);
    commands.Fail();
    ReleaseSRWLockExclusive(&lock);
}

static void *__fastcall Build(float *out, const float *position,
    const float *forward, const float *up)
{
    const void *caller = _ReturnAddress();
    if (!InterlockedCompareExchange(&ready, 0, 0)) return build(out, position, forward, up);
    const int slot = Slot(out, 0x40);
    Credit value = {};
    value.epoch = Epoch();
    value.valid = slot == 0 && (caller == module + 0x69BB54 || caller == module + 0x69CF4A)
        && NativeStateFollowSelected() &&
        !InterlockedCompareExchange(&sUpstreamCameraYieldingToScript, 0, 0);
    if (value.valid) {
        value.yaw = GetCullingCancellation();
        value.pitch = GetCullingCancellationPitch();
    }
    void *result = build(out, position, forward, up);
    if (slot >= 0) {
        memcpy(value.nativeView, out, sizeof(value.nativeView));
        AcquireSRWLockExclusive(&lock);
        value.generation = ++generation;
        if (value.epoch != (uint64_t)Epoch()) value.valid = false;
        slots[slot] = value;
        ReleaseSRWLockExclusive(&lock);
    }
    return result;
}

static void *__fastcall Copy(void *destination, const void *source)
{
    if (!InterlockedCompareExchange(&ready, 0, 0)) return copy(destination, source);
    const int dst = Slot(destination), src = Slot(source);
    Credit value = {};
    if (dst >= 0 && src >= 0) {
        AcquireSRWLockShared(&lock);
        value = slots[src];
        if (!NativeCameraCredits::SameNativeView(value.nativeView,
            (const float *)((const BYTE *)source + 0x40))) value.valid = false;
        ReleaseSRWLockShared(&lock);
    }
    void *result = copy(destination, source);
    if (dst >= 0) {
        const float *matrix = (const float *)((const BYTE *)destination + 0x40);
        AcquireSRWLockExclusive(&lock);
        if (src < 0 || value.generation != slots[src].generation ||
            !NativeCameraCredits::SameNativeView(value.nativeView, matrix) ||
            value.epoch != (uint64_t)Epoch()) value.valid = false;
        memcpy(value.nativeView, matrix, sizeof(value.nativeView));
        slots[dst] = value;
        ReleaseSRWLockExclusive(&lock);
    }
    return result;
}

static uintptr_t __fastcall Record(void *recorder, const float *matrix)
{
    if (!InterlockedCompareExchange(&ready, 0, 0)) return record(recorder, matrix);
    Credit value = {};
    const int slot = Slot(matrix, 0x40);
    AcquireSRWLockShared(&lock);
    if (slot >= 0) value = slots[slot];
    if (!NativeCameraCredits::SameNativeView(value.nativeView, matrix)) value.valid = false;
    memcpy(value.nativeView, matrix, sizeof(value.nativeView));
    ReleaseSRWLockShared(&lock);
    // The original may flush/execute old commands. Never hold our lock here.
    const uintptr_t result = record(recorder, matrix);
    BYTE *buffer = *(BYTE **)((BYTE *)recorder + 0x10);
    const unsigned cursor = *(unsigned *)((BYTE *)recorder + 0x18);
    if (!buffer || cursor < 17 || cursor > 0x2000) { RecordingFault(); return result; }
    const unsigned offset = (cursor - 16) * 4;
    if (*(unsigned *)(buffer + offset - 4) != 0x70000014 ||
        !NativeCameraCredits::SameNativeView(value.nativeView, (float *)(buffer + offset))) {
        RecordingFault(); return result;
    }
    AcquireSRWLockExclusive(&lock);
    commands.Record((uintptr_t)buffer, offset, value);
    ReleaseSRWLockExclusive(&lock);
    return result;
}

static void __fastcall Execute(void *buffer)
{
    if (!InterlockedCompareExchange(&ready, 0, 0)) { execute(buffer); return; }
    if (depth >= 8) {
        Replay *outer = replay;
        replay = NULL;
        current.valid = false;
        execute(buffer);
        replay = outer;
        current.valid = false;
        return;
    }
    Replay &local = replayStack[depth];
    AcquireSRWLockExclusive(&lock);
    commands.TakeReplay((uintptr_t)buffer, &local);
    ReleaseSRWLockExclusive(&lock);
    Replay *outer = replay;
    replay = &local;
    ++depth;
    execute(buffer);
    --depth;
    replay = outer;
    if (!local.Complete()) current.valid = false;
    // The engine's last camera persists across batches; so does its credit.
    // Do not restore an earlier camera merely because nested replay returned.
}

static uintptr_t __fastcall SetCamera(void *camera, const float *matrix)
{
    if (InterlockedCompareExchange(&ready, 0, 0)) {
        current.valid = false;
        if (_ReturnAddress() == module + 0x7EEAE4 && replay) {
            if (!replay->Consume(matrix, Epoch(), &current)) current.valid = false;
        }
    }
    return setCamera(camera, matrix);
}
} // namespace CameraCreditAdapter

static void ResetNativeCameraCreditEpoch()
{
    using namespace CameraCreditAdapter;
    AcquireSRWLockExclusive(&lock);
    // Keep queued opcode order; old epochs cannot supply cancellation.
    InterlockedIncrement(&epoch);
    ReleaseSRWLockExclusive(&lock);
}

bool GetNativeCameraRenderCredit(const float *view, float *yaw, float *pitch)
{
    using namespace CameraCreditAdapter;
    if (!InterlockedCompareExchange(&ready, 0, 0) || !NativeStateFollowSelected() ||
        InterlockedCompareExchange(&sUpstreamCameraYieldingToScript, 0, 0)) return false;
    bool valid = current.valid && current.epoch == (uint64_t)Epoch();
    for (unsigned row = 0; row < 3 && valid; ++row)
        for (unsigned col = 0; col < 4; ++col)
            if (!isfinite(view[row * 4 + col]) ||
                fabsf(view[row * 4 + col] - current.nativeView[col * 4 + row]) > 0.0001f)
                valid = false;
    if (!valid) return false;
    *yaw = current.yaw;
    *pitch = current.pitch;
    return true;
}

static void InstallNativeCameraCreditAdapter()
{
    using namespace CameraCreditAdapter;
    module = (BYTE *)GetModuleHandleA(NULL);
    struct Hook { DWORD_PTR rva; const BYTE *bytes; unsigned length; LPVOID *original; LPVOID replacement; };
    static const BYTE b[] = {0x48,0x83,0xEC,0x38,0x41,0x0F,0x28,0x21};
    static const BYTE c[] = {0x48,0x89,0x5C,0x24,0x08,0x57,0x48,0x83,0xEC,0x20};
    static const BYTE r[] = {0x40,0x53,0x48,0x83,0xEC,0x20,0x0F,0x28,0x02};
    static const BYTE e[] = {0x48,0x8B,0xC4,0x55,0x53,0x41,0x56,0x41,0x57};
    static const BYTE s[] = {0x40,0x53,0x48,0x83,0xEC,0x20,0x48,0x8B,0xD9};
    Hook hooks[] = {
        {0xD560,b,sizeof(b),(LPVOID *)&build,(LPVOID)Build},
        {0xD5350,c,sizeof(c),(LPVOID *)&copy,(LPVOID)Copy},
        {0xD6BE0,r,sizeof(r),(LPVOID *)&record,(LPVOID)Record},
        {0x7EE2E0,e,sizeof(e),(LPVOID *)&execute,(LPVOID)Execute},
        {0x7E44D0,s,sizeof(s),(LPVOID *)&setCamera,(LPVOID)SetCamera}
    };
    for (unsigned i = 0; i < _countof(hooks); ++i)
        if (!module || memcmp(module + hooks[i].rva, hooks[i].bytes, hooks[i].length)) {
            LogInfo("VRPose camera-credit integration: signature rejected at +%llx; disabled\n",
                (unsigned long long)hooks[i].rva); return;
        }
    for (unsigned i = 0; i < _countof(hooks); ++i) {
        SIZE_T id = 0;
        const DWORD error = cHookMgr.Hook(&id, hooks[i].original,
            module + hooks[i].rva, hooks[i].replacement);
        if (error || !*hooks[i].original) {
            LogInfo("VRPose camera-credit integration: install failed; disabled\n"); return;
        }
    }
    InterlockedExchange(&ready, 1);
    LogInfo("VRPose camera-credit integration: generation/copy/command/replay adapters installed\n");
}
