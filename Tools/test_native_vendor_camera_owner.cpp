#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
static BYTE image[0xC00000];
static BYTE *testModule = image;
static HMODULE TestGetModuleHandleA(LPCSTR) { return (HMODULE)testModule; }
static unsigned checks;
static void Check(bool ok) { ++checks; if (!ok) { fprintf(stderr, "FAIL %u\n", checks); exit(1); } }
static void LogInfo(const char *, ...) {}
static volatile LONG sNativeVendorCameraOwnerInstalled = 0;
struct HookManager {
	unsigned calls = 0;
	DWORD Hook(SIZE_T *, LPVOID *original, LPVOID target, LPVOID hook) {
		++calls;
		if (target == image + 0x42DE3C) *original = (LPVOID)hookDeactivate;
		else if (target == image + 0x42DDE0) *original = (LPVOID)hookActivate;
		else return 1;
		(void)hook;
		return 0;
	}
	static uintptr_t __fastcall hookActivate(void *) { return 17; }
	static uintptr_t __fastcall hookDeactivate(void *) { return 23; }
} cHookMgr;
#define GetModuleHandleA TestGetModuleHandleA
#include "../ThirdParty/3Dmigoto/DirectX11/NativeVendorCameraOwner.inl"
#undef GetModuleHandleA

struct FakeVendorOwner { BYTE bytes[0x1590]; };
static void SetSelection(FakeVendorOwner *owner, LONG selected, WORD count)
{
	*(void **)owner->bytes = image + 0xB7A760;
	*(LONG *)(owner->bytes + 0x1534) = selected;
	*(WORD *)(owner->bytes + 0x322) = count;
}

static void SetEquippedWeaponTrack(FakeVendorOwner *owner, bool active)
{
	owner->bytes[0x1571] = active ? 1 : 0;
	*(void **)(owner->bytes + 0x1578) = active ? owner : NULL;
}

int main()
{
	// An early attempt before Metro exposes the class must remain retryable.
	InstallNativeVendorCameraOwner();
	Check(sNativeVendorCameraOwnerInstalled == 0); Check(cHookMgr.calls == 0);
	memcpy(image + 0x42DDE0, "\x40\x53\x48\x83\xEC\x20\x48\x8B\xD9", 9);
	memcpy(image + 0x42DE3C, "\x40\x53\x48\x83\xEC\x20\x80\x79\x60\x01", 10);
	*(void **)(image + 0xB7A8D8) = image + 0x42DDE0;
	*(void **)(image + 0xB7A8E0) = image + 0x42DE3C;
	InstallNativeVendorCameraOwner();
	Check(sNativeVendorCameraOwnerInstalled == 1); Check(cHookMgr.calls == 2);
	FakeVendorOwner a = {}, b = {};
	SetSelection(&a, 0, 1);
	SetSelection(&b, 1, 2);
	Check(VendorCameraOwnerAdapter::Activate(&a) == 17); Check(NativeVendorCameraOwnerActive());
	Check(NativeVendorCameraOwnerPointer() == &a);
	SetSelection(&a, -1, 1); Check(!NativeVendorCameraOwnerActive());
	SetSelection(&a, 1, 1); Check(!NativeVendorCameraOwnerActive());
	SetEquippedWeaponTrack(&a, true); Check(NativeVendorCameraOwnerActive());
	SetEquippedWeaponTrack(&a, false); Check(!NativeVendorCameraOwnerActive());
	SetSelection(&a, 0, 1); Check(NativeVendorCameraOwnerActive());
	*(void **)a.bytes = NULL; Check(!NativeVendorCameraOwnerActive());
	*(void **)a.bytes = image + 0xB7A760; Check(NativeVendorCameraOwnerActive());
	Check(VendorCameraOwnerAdapter::Deactivate(&b) == 23); Check(NativeVendorCameraOwnerActive());
	Check(NativeVendorCameraOwnerPointer() == &a);
	Check(VendorCameraOwnerAdapter::Activate(&b) == 17); Check(NativeVendorCameraOwnerActive());
	Check(NativeVendorCameraOwnerPointer() == &b);
	Check(VendorCameraOwnerAdapter::Deactivate(&a) == 23); Check(NativeVendorCameraOwnerActive());
	Check(NativeVendorCameraOwnerPointer() == &b);
	Check(VendorCameraOwnerAdapter::Deactivate(&b) == 23); Check(!NativeVendorCameraOwnerActive());
	Check(NativeVendorCameraOwnerPointer() == NULL);
	printf("PASS: %u native vendor-owner adapter checks\n", checks);
}
