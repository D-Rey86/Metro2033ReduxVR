// Included inside VRPose. Only the main world list's buffered-camera frustum.
namespace VisibilityCoverageAdapter {
typedef uintptr_t (__fastcall *BuildFn)(void *, const float *, unsigned);
static BuildFn original = NULL;
static BYTE *module = NULL;
static uintptr_t __fastcall Build(void *out, const float *vp, unsigned mask)
{
    __declspec(align(16)) float widened[16];
    bool use = false;
    if (_ReturnAddress() == module + 0x812B14 && vp == (float *)(module + 0xD22FA0)
        && mask == 0x1F && G && G->vrStereoParamsValid
        && VRMenu::ExpandedVisibilityActive() && NativeStateFollowSelected()
        && !InterlockedCompareExchange(&sUpstreamCameraYieldingToScript, 0, 0)) {
        __try {
            use = NativeVisibilityCoverage::Widen((float *)(module + 0xD22F20),
                (float *)(module + 0xD22F60), vp, G->vrEyeProjection4x4, widened);
        } __except (EXCEPTION_EXECUTE_HANDLER) { use = false; }
    }
    return original(out, use ? widened : vp, mask);
}
}
static void InstallNativeVisibilityCoverage()
{
    using namespace VisibilityCoverageAdapter;
    module = (BYTE *)GetModuleHandleA(NULL);
    static const BYTE prologue[] = {0x48,0x8B,0xC4,0x55,0x48,0x8D,0x68,0xA1,
        0x48,0x81,0xEC,0x90,0x00,0x00,0x00};
    const BYTE *call = module + 0x812B0F, *source = module + 0x812AFD;
    if (memcmp(module + 0x8E93C0, prologue, sizeof(prologue)) || call[0] != 0xE8 ||
        call + 5 + *(const INT32 *)(call+1) != module + 0x8E93C0 ||
        source[0] != 0x48 || source[1] != 0x8D || source[2] != 0x15 ||
        source + 7 + *(const INT32 *)(source+3) != module + 0xD22FA0) {
        LogInfo("VRPose visibility coverage: signature rejected; native unchanged\n");
        return;
    }
    SIZE_T id = 0;
    DWORD error = cHookMgr.Hook(&id, (LPVOID *)&original, module+0x8E93C0, Build);
    LogInfo("VRPose visibility coverage: %s; main world list only, native-mode A/B\n",
        !error && original ? "installed" : "install failed");
}
