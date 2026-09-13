#include <windows.h>
#include <intrin.h>
#include <stdio.h>
#include <stdlib.h>
#include "../ThirdParty/3Dmigoto/DirectX11/NativeVisibilityCoverage.h"
struct Globals { bool vrStereoParamsValid; float vrEyeProjection4x4[2][16]; } globals = {};
static Globals *G = &globals;
static bool selected = true;
namespace VRMenu {
struct Settings { bool expandedVisibility; };
static Settings settings = { true };
static bool appliedExpandedVisibility = true;
static bool ExpandedVisibilityActive() { return appliedExpandedVisibility; }
}
static LONG sUpstreamCameraYieldingToScript = 0;
static bool NativeStateFollowSelected() { return selected; }
static void LogInfo(const char *, ...) {}
struct HookManager { DWORD Hook(SIZE_T *,LPVOID *,LPVOID,LPVOID) { return 1; } } cHookMgr;
static void *caller;
static void *TestReturnAddress() { return caller; }
#define _ReturnAddress TestReturnAddress
#include "../ThirdParty/3Dmigoto/DirectX11/NativeVisibilityCoverage.inl"
#undef _ReturnAddress
static __declspec(align(16)) BYTE image[0xE00000];
static float observed[16];
static unsigned observedMask, checks;
static const float *observedPointer;
static uintptr_t __fastcall Original(void *,const float *vp,unsigned mask) {
    memcpy(observed,vp,sizeof(observed)); observedMask=mask; observedPointer=vp; return 123;
}
static void Check(bool ok) { ++checks; if(!ok) { fprintf(stderr,"FAIL %u\n",checks); exit(1); } }
int main() {
    using namespace VisibilityCoverageAdapter;
    module=image; original=Original; caller=image+0x812B14;
    float *v=(float *)(image+0xD22F20), *p=(float *)(image+0xD22F60), *vp=(float *)(image+0xD22FA0);
    v[0]=v[5]=v[10]=v[15]=1;
    p[0]=1.17f;p[5]=2.11f;p[10]=1.002f;p[11]=1;p[14]=-.1f;
    memcpy(vp,p,64); globals.vrStereoParamsValid=true;
    for(unsigned e=0;e<2;++e) globals.vrEyeProjection4x4[e][0]=globals.vrEyeProjection4x4[e][5]=.8f;
    Check(Build(NULL,vp,0x1F)==123);Check(observedPointer!=vp);Check(observed[5]<vp[5]);Check(observedMask==0x1F);
    for(unsigned r=0;r<4;++r)for(unsigned c=2;c<4;++c)Check(observed[r*4+c]==vp[r*4+c]);
    Check(vp[5]==2.11f); // No mutation of Metro's shared matrix.
    caller=image+0x69D19B; Build(NULL,vp,0x1F);Check(observedPointer==vp);
    caller=image+0x812B14; Build(NULL,vp,0x3F);Check(observedPointer==vp);
    VRMenu::settings.expandedVisibility=false;Build(NULL,vp,0x1F);Check(observedPointer!=vp); // Saved menu choice is next-launch only.
    VRMenu::appliedExpandedVisibility=false;Build(NULL,vp,0x1F);Check(observedPointer==vp);
    VRMenu::settings.expandedVisibility=true;Build(NULL,vp,0x1F);Check(observedPointer==vp);
    VRMenu::appliedExpandedVisibility=true;
    selected=false; Build(NULL,vp,0x1F);Check(observedPointer==vp);selected=true;
    sUpstreamCameraYieldingToScript=1;Build(NULL,vp,0x1F);Check(observedPointer==vp);sUpstreamCameraYieldingToScript=0;
    globals.vrStereoParamsValid=false;Build(NULL,vp,0x1F);Check(observedPointer==vp);globals.vrStereoParamsValid=true;
    vp[5]=3;Build(NULL,vp,0x1F);Check(observedPointer==vp);vp[5]=p[5];
    float unrelated[16];memcpy(unrelated,vp,64);Build(NULL,unrelated,0x1F);Check(observedPointer==unrelated);
    printf("PASS: %u visibility adapter checks (mocked engine)\n",checks);
}
