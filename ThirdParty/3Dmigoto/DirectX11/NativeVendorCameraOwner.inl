// Included inside VRPose. Identifies the native trade/customize camera owner.
namespace VendorCameraOwnerAdapter {
typedef uintptr_t (__fastcall *OwnerEventFn)(void *);
static OwnerEventFn originalActivate = NULL;
static OwnerEventFn originalDeactivate = NULL;
static PVOID volatile activeOwner = NULL;

static uintptr_t __fastcall Activate(void *owner)
{
	if (InterlockedCompareExchange(&sNativeVendorCameraOwnerInstalled, 0, 0) == 1)
		InterlockedExchangePointer(&activeOwner, owner);
	return originalActivate ? originalActivate(owner) : 0;
}

static uintptr_t __fastcall Deactivate(void *owner)
{
	const uintptr_t result = originalDeactivate ? originalDeactivate(owner) : 0;
	InterlockedCompareExchangePointer(&activeOwner, NULL, owner);
	return result;
}
}

static bool NativeVendorCameraOwnerActive()
{
	return InterlockedCompareExchange(&sNativeVendorCameraOwnerInstalled, 0, 0) == 1
		&& InterlockedCompareExchangePointer(
			&VendorCameraOwnerAdapter::activeOwner, NULL, NULL) != NULL;
}

static void InstallNativeVendorCameraOwner()
{
	using namespace VendorCameraOwnerAdapter;
	if (InterlockedCompareExchange(&sNativeVendorCameraOwnerInstalled, 0, 0) != 0)
		return;
	BYTE *base = (BYTE *)GetModuleHandleA(NULL);
	if (!base)
		return;
	BYTE *activate = base + 0x42DDE0;
	BYTE *deactivate = base + 0x42DE3C;
	static const BYTE activatePrologue[] = {
		0x40,0x53,0x48,0x83,0xEC,0x20,0x48,0x8B,0xD9
	};
	static const BYTE deactivatePrologue[] = {
		0x40,0x53,0x48,0x83,0xEC,0x20,0x80,0x79,0x60,0x01
	};
	// These adjacent vtable entries uniquely tie the two functions to the
	// trade/customize controller that owns trade_camera_track.
	if (memcmp(activate, activatePrologue, sizeof(activatePrologue)) != 0 ||
		memcmp(deactivate, deactivatePrologue, sizeof(deactivatePrologue)) != 0 ||
		*(void **)(base + 0xB7A8D8) != activate ||
		*(void **)(base + 0xB7A8E0) != deactivate) {
		// Metro decrypts/initializes this UI class later than the player-camera
		// code on some starts. Stay inert and retry from the frame update; never
		// hook an unverified address or permanently reject an unavailable class.
		return;
	}
	// Hook release first. A partial installation can then only clear an owner;
	// it cannot latch vendor ownership without a matching release path.
	SIZE_T deactivateId = 0, activateId = 0;
	DWORD error = cHookMgr.Hook(&deactivateId,
		(LPVOID *)&originalDeactivate, deactivate, Deactivate);
	if (!error && originalDeactivate)
		error = cHookMgr.Hook(&activateId,
			(LPVOID *)&originalActivate, activate, Activate);
	const bool installed = !error && originalActivate && originalDeactivate;
	InterlockedExchange(&sNativeVendorCameraOwnerInstalled, installed ? 1 : -1);
	LogInfo("VRPose vendor camera owner: %s at trade/customize +0x42DDE0/+0x42DE3C\n",
		installed ? "installed" : "install failed");
}
