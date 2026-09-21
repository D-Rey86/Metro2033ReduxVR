#include "VRPose.h"
#include "JournalButtonPulse.h"
#include "JournalPresentation.h"
#include "NativeCameraCommandCredits.h"
#include "NativeBodyTurnIntent.h"
#include "NativeVisibilityCoverage.h"
#include "VRCompatibility.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <unordered_set>
#include <algorithm>
#include <utility>
#include <vector>
#include <sstream>
#include <string>
#include <openvr.h>
#include <d3dcompiler.h>
#include <intrin.h>

#include "lock.h"
#include "log.h"
#include "Globals.h"
#include "Overlay.h"
#include "cursor.h"       // InstallHookLate, for the virtual gamepad below
#include "DLLMainHook.h"  // cHookMgr, for the inline fire hook
#include "StereoTwin.h"
#include "StereoSinglePass.h"
#include "StereoCensus.h"
#include "OpenXRBackend.h"
#include "VRMenu.h"

// After Globals.h, which pulls in windows.h - tlhelp32.h depends on the
// Windows base types and won't compile standalone. Xinput.h is the same:
// included any earlier it fails with "No Target Architecture".
#include <tlhelp32.h>
#include <Xinput.h>

namespace {

	bool sAttemptedInit = false;
	vr::IVRSystem *sVRSystem = nullptr;
	VRBackend::OpenXRRuntime sOpenXRRuntime;
	unsigned sFrameCounter = 0;
	uint32_t sRecommendedEyeWidth = 0;
	uint32_t sRecommendedEyeHeight = 0;
	bool sCompositorFramePacingEnabled = false;

	void CompatibilityLog(const char *format, ...)
	{
		static FILE *file = nullptr;
		if (!file) {
			if (fopen_s(&file, "vr_compatibility_log.txt", "w") != 0 || !file)
				return;
			setvbuf(file, nullptr, _IONBF, 0);
			SYSTEMTIME now = {};
			GetLocalTime(&now);
			fprintf(file, "Metro2033ReduxVR compatibility report - "
				"%04u-%02u-%02u %02u:%02u:%02u\n",
				now.wYear, now.wMonth, now.wDay,
				now.wHour, now.wMinute, now.wSecond);
		}
		va_list args;
		va_start(args, format);
		vfprintf(file, format, args);
		va_end(args);
	}

	// OpenVR works in metres. The game's world units appear to be metres
	// too - a view matrix captured inside a room had a translation of
	// (-12.99, 1.56, 32.61), and 1.56 is a plausible eye height in
	// metres, while the recovered near/far planes of 0.1 and 40.1 also
	// read as metres. If stereo separation ends up looking too strong or
	// too weak in the headset, this is the single number to tune: it
	// scales the eye offset (half an IPD, ~0.032m) into game units.
	const float kMetresToGameUnits = 1.0f;
	static float TrackingWorldScale()
	{
		const float value = VRMenu::GetSettings().worldScale;
		return max(0.5f, min(2.0f, value));
	}

	std::unordered_set<void *> sKnownNonMatrixInputLayouts;
	std::unordered_set<void *> sKnownWeaponXformInputLayouts;

	// Metro2033ReduxVR milestone 2: private copy of the back buffer,
	// created lazily to match its dimensions/format, that gets handed
	// to the OpenVR compositor. We deliberately do NOT submit the swap
	// chain's back buffer directly - swap chain buffers can carry usage
	// flags and lifetimes the compositor doesn't accept, and their
	// contents are only valid until Present. One CopyResource per frame
	// into a plain texture we own sidesteps that whole class of
	// problem. See the creation code for why it carries no MiscFlags.
	// One per eye: the game renders eyes on alternating frames, so each
	// eye's most recent image has to be held and re-submitted on the
	// frames belonging to the other eye. Without this the compositor
	// would only ever have one fresh eye and the other would be stale or
	// black.
	ID3D11Texture2D *sCompositorTexture[2] = { nullptr, nullptr };
	ID3D11Texture2D *sScaledCompositorTexture[2] = { nullptr, nullptr };
	ID3D11RenderTargetView *sScaledCompositorRTV[2] = { nullptr, nullptr };
	ID3D11ShaderResourceView *sScaledCompositorSRV[2] = { nullptr, nullptr };
	UINT sScaledCompositorWidth = 0;
	UINT sScaledCompositorHeight = 0;
	DXGI_FORMAT sScaledCompositorFormat = DXGI_FORMAT_UNKNOWN;
	ID3D11VertexShader *sScaleVertexShader = nullptr;
	ID3D11PixelShader *sScalePixelShader = nullptr;
	ID3D11SamplerState *sScaleSampler = nullptr;
	ID3D11Buffer *sScaleParametersCB = nullptr;
	ID3D11RasterizerState *sScaleRasterizer = nullptr;
	ID3D11DepthStencilState *sScaleDepthStencil = nullptr;
	ID3D11BlendState *sScaleBlendState = nullptr;
	ID3D11Texture2D *sHighResolutionEyeTexture[2] = { nullptr, nullptr };
	ID3D11RenderTargetView *sHighResolutionEyeRTV[2] = { nullptr, nullptr };
	ID3D11ShaderResourceView *sHighResolutionSceneSRV = nullptr;
	ID3D11Texture2D *sHighResolutionSceneSource = nullptr;
	ID3D11PixelShader *sHighResolutionScenePS = nullptr;
	ID3D11Buffer *sHighResolutionSceneCB = nullptr;
	UINT sHighResolutionWidth = 0;
	UINT sHighResolutionHeight = 0;
	unsigned sHighResolutionFrame = 0xFFFFFFFFu;

	void ReleaseHighResolutionSceneResources()
	{
		for (int eye = 0; eye < 2; ++eye) {
			if (sHighResolutionEyeRTV[eye]) { sHighResolutionEyeRTV[eye]->Release(); sHighResolutionEyeRTV[eye] = nullptr; }
			if (sHighResolutionEyeTexture[eye]) { sHighResolutionEyeTexture[eye]->Release(); sHighResolutionEyeTexture[eye] = nullptr; }
		}
		if (sHighResolutionSceneSRV) { sHighResolutionSceneSRV->Release(); sHighResolutionSceneSRV = nullptr; }
		if (sHighResolutionSceneSource) { sHighResolutionSceneSource->Release(); sHighResolutionSceneSource = nullptr; }
		if (sHighResolutionScenePS) { sHighResolutionScenePS->Release(); sHighResolutionScenePS = nullptr; }
		if (sHighResolutionSceneCB) { sHighResolutionSceneCB->Release(); sHighResolutionSceneCB = nullptr; }
		sHighResolutionWidth = sHighResolutionHeight = 0;
		sHighResolutionFrame = 0xFFFFFFFFu;
	}

	void ReleaseScaledSubmissionResources()
	{
		for (int eye = 0; eye < 2; eye++) {
			if (sScaledCompositorSRV[eye]) { sScaledCompositorSRV[eye]->Release(); sScaledCompositorSRV[eye] = nullptr; }
			if (sScaledCompositorRTV[eye]) { sScaledCompositorRTV[eye]->Release(); sScaledCompositorRTV[eye] = nullptr; }
			if (sScaledCompositorTexture[eye]) { sScaledCompositorTexture[eye]->Release(); sScaledCompositorTexture[eye] = nullptr; }
		}
		sScaledCompositorWidth = 0;
		sScaledCompositorHeight = 0;
		sScaledCompositorFormat = DXGI_FORMAT_UNKNOWN;
	}
	bool sEyeTextureValid[2] = { false, false };

	// The raw HMD pose as of the most recent UpdateVRPose. Because
	// UpdateVRPose runs after Present, the pose recorded at the end of
	// frame N-1 is the one frame N was actually rendered with - which is
	// exactly the pose still sitting here when we submit frame N.
	vr::HmdMatrix34_t sLastPolledPose;
	bool sLastPolledPoseValid = false;
	// The initial pitch/roll expressed in the levelled yaw-reference frame.
	// Stored inverted so startup maps to identity without making that tilted
	// frame define the axis used by later physical yaw.
	float sReferenceTiltInverse3x3[9] = { 1,0,0, 0,1,0, 0,0,1 };
	bool sReferenceTiltInverseValid = false;
	vr::TrackedDevicePose_t sFrameRenderPoses[vr::k_unMaxTrackedDeviceCount];
	bool sFrameRenderPosesValid = false;

	void GetTrackedPosesForFrame(vr::TrackedDevicePose_t out[vr::k_unMaxTrackedDeviceCount])
	{
		if (sFrameRenderPosesValid) {
			memcpy(out, sFrameRenderPoses, sizeof(sFrameRenderPoses));
			return;
		}
		memset(out, 0, sizeof(sFrameRenderPoses));
		if (sVRSystem)
			sVRSystem->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0.0f,
				out, vr::k_unMaxTrackedDeviceCount);
	}

	bool IsOpenXRSelected()
	{
		char runtime[32] = {};
		const DWORD runtimeLen = GetEnvironmentVariableA(
			"METRO2033_VR_RUNTIME", runtime, sizeof(runtime));
		if (runtimeLen > 0 && _stricmp(runtime, "openxr") == 0)
			return true;
		// Steam may launch the game from a process that does not inherit the
		// shell environment used to start it. A marker beside d3d11.dll makes
		// the runtime choice deterministic for this installation.
		return GetFileAttributesA("openxr_runtime.txt") != INVALID_FILE_ATTRIBUTES;
	}
	float sHeadReferencePos[3] = { 0, 0, 0 };
	bool sHeadReferencePosValid = false;

	// The pose each eye's held texture was rendered at. Submitted
	// alongside the texture so the compositor can reproject both eyes to
	// a common pose: with alternate-eye rendering one eye's image is
	// always a frame older than the other, and without telling the
	// compositor that, the two eyes disagree during head motion and the
	// image visibly doubles.
	vr::HmdMatrix34_t sEyeTexturePose[2];
	UINT sCompositorTexWidth = 0;
	UINT sCompositorTexHeight = 0;
	DXGI_FORMAT sCompositorTexFormat = DXGI_FORMAT_UNKNOWN;
	vr::VROverlayHandle_t sVideoOverlay = vr::k_ulOverlayHandleInvalid;
	bool sVideoOverlayVisible = false;
	vr::VROverlayHandle_t sMenuOverlay = vr::k_ulOverlayHandleInvalid;
	bool sMenuOverlayVisible = false;
	vr::VROverlayHandle_t sPerfOverlay = vr::k_ulOverlayHandleInvalid;
	bool sPerfOverlayVisible = false;

	// A flat screen fixed in the room: `distance` metres ahead of where the head
	// is looking at this moment, gravity-levelled (yaw only), at eye height plus
	// `heightOffset`. Captured when the screen appears and then left alone, so
	// it stays put like a cinema screen instead of swimming with every head
	// movement. Poses are in the standing universe, which is also the
	// compositor's tracking space (SubmitFrameToCompositor sets it).
	bool BuildRoomFixedOverlayTransform(float distance, float heightOffset,
		vr::HmdMatrix34_t *out)
	{
		if (!sLastPolledPoseValid || !out)
			return false;
		const vr::HmdMatrix34_t &h = sLastPolledPose;
		float fx = -h.m[0][2], fz = -h.m[2][2];
		const float len = sqrtf(fx * fx + fz * fz);
		if (len < 0.2f)
			return false;   // looking nearly straight up or down: no stable heading
		fx /= len;
		fz /= len;
		// Columns: right, up, back. The quad's visible face points back at the viewer.
		const vr::HmdMatrix34_t m = {{
			{ -fz, 0.0f, -fx, h.m[0][3] + fx * distance },
			{ 0.0f, 1.0f, 0.0f, h.m[1][3] + heightOffset },
			{ fx, 0.0f, -fz, h.m[2][3] + fz * distance }
		}};
		*out = m;
		return true;
	}

	// Anchors `handle` in the room if the head pose allows it, otherwise falls
	// back to the previous head-relative placement.
	void PlaceOverlayInRoom(vr::IVROverlay *overlay, vr::VROverlayHandle_t handle,
		float distance, float heightOffset)
	{
		vr::HmdMatrix34_t roomFixed;
		if (BuildRoomFixedOverlayTransform(distance, heightOffset, &roomFixed)) {
			overlay->SetOverlayTransformAbsolute(handle, vr::TrackingUniverseStanding, &roomFixed);
			return;
		}
		const vr::HmdMatrix34_t hmdToOverlay = {{
			{ 1.0f, 0.0f, 0.0f, 0.0f },
			{ 0.0f, 1.0f, 0.0f, heightOffset },
			{ 0.0f, 0.0f, 1.0f, -distance }
		}};
		overlay->SetOverlayTransformTrackedDeviceRelative(handle,
			vr::k_unTrackedDeviceIndex_Hmd, &hmdToOverlay);
	}

	void HideVideoOverlay()
	{
		if (!sVideoOverlayVisible || sVideoOverlay == vr::k_ulOverlayHandleInvalid)
			return;
		vr::IVROverlay *overlay = vr::VROverlay();
		if (overlay)
			overlay->HideOverlay(sVideoOverlay);
		sVideoOverlayVisible = false;
	}

	bool ShowVideoOverlay(ID3D11Texture2D *texture)
	{
		if (!texture)
			return false;
		vr::IVROverlay *overlay = vr::VROverlay();
		if (!overlay)
			return false;

		if (sVideoOverlay == vr::k_ulOverlayHandleInvalid) {
			vr::EVROverlayError err = overlay->CreateOverlay(
				"metro2033reduxvr.prerendered_video",
				"Metro 2033 Redux VR pre-rendered video", &sVideoOverlay);
			if (err == vr::VROverlayError_KeyInUse)
				err = overlay->FindOverlay("metro2033reduxvr.prerendered_video",
					&sVideoOverlay);
			if (err != vr::VROverlayError_None ||
				sVideoOverlay == vr::k_ulOverlayHandleInvalid) {
				LogInfo("VRPose video overlay: creation failed (%d)\n", err);
				sVideoOverlay = vr::k_ulOverlayHandleInvalid;
				return false;
			}

			// OpenVR coordinates are +X right, +Y up, -Z forward. A real
			// head-relative quad gives both eyes one coherent finite-depth plane;
			// copying the same pixels into two scene-eye submissions does not.
			vr::HmdMatrix34_t hmdToOverlay = {{
				{ 1.0f, 0.0f, 0.0f,  0.0f },
				{ 0.0f, 1.0f, 0.0f,  0.0f },
				{ 0.0f, 0.0f, 1.0f, -2.0f }
			}};
			overlay->SetOverlayTransformTrackedDeviceRelative(sVideoOverlay,
				vr::k_unTrackedDeviceIndex_Hmd, &hmdToOverlay);
			overlay->SetOverlayWidthInMeters(sVideoOverlay, 3.2f);
			overlay->SetOverlayAlpha(sVideoOverlay, 1.0f);
			overlay->SetOverlayFlag(sVideoOverlay,
				vr::VROverlayFlags_IgnoreTextureAlpha, true);
			LogInfo("VRPose video overlay: initialized as 3.2m head-relative quad at 2.0m\n");
		}

		vr::Texture_t overlayTexture = {};
		overlayTexture.handle = texture;
		overlayTexture.eType = vr::TextureType_DirectX;
		overlayTexture.eColorSpace = vr::ColorSpace_Auto;
		vr::EVROverlayError err = overlay->SetOverlayTexture(sVideoOverlay,
			&overlayTexture);
		if (err != vr::VROverlayError_None) {
			static unsigned failureLogs = 0;
			if (failureLogs++ < 10)
				LogInfo("VRPose video overlay: SetOverlayTexture failed (%d)\n", err);
			HideVideoOverlay();
			return false;
		}
		if (!sVideoOverlayVisible) {
			// Theatre screen: fixed in the room where the player is looking when
			// the video starts, rather than locked to the head.
			PlaceOverlayInRoom(overlay, sVideoOverlay, 2.0f, 0.0f);
			err = overlay->ShowOverlay(sVideoOverlay);
			if (err != vr::VROverlayError_None) {
				LogInfo("VRPose video overlay: ShowOverlay failed (%d)\n", err);
				return false;
			}
			sVideoOverlayVisible = true;
			LogInfo("VRPose video overlay: visible\n");
		}
		return true;
	}

	void HideMenuOverlay()
	{
		if (!sMenuOverlayVisible || sMenuOverlay == vr::k_ulOverlayHandleInvalid)
			return;
		vr::IVROverlay *overlay = vr::VROverlay();
		if (overlay)
			overlay->HideOverlay(sMenuOverlay);
		sMenuOverlayVisible = false;
	}

	bool ShowMenuOverlay(ID3D11Texture2D *texture)
	{
		if (!texture || !VRMenu::IsOpen()) {
			HideMenuOverlay();
			return false;
		}
		vr::IVROverlay *overlay = vr::VROverlay();
		if (!overlay)
			return false;

		if (sMenuOverlay == vr::k_ulOverlayHandleInvalid) {
			vr::EVROverlayError err = overlay->CreateOverlay(
				"metro2033reduxvr.settings_menu",
				"Metro 2033 Redux VR settings", &sMenuOverlay);
			if (err == vr::VROverlayError_KeyInUse)
				err = overlay->FindOverlay("metro2033reduxvr.settings_menu",
					&sMenuOverlay);
			if (err != vr::VROverlayError_None ||
				sMenuOverlay == vr::k_ulOverlayHandleInvalid) {
				LogInfo("VRPose menu overlay: creation failed (%d)\n", err);
				sMenuOverlay = vr::k_ulOverlayHandleInvalid;
				return false;
			}
			vr::HmdMatrix34_t hmdToOverlay = {{
				{ 1.0f, 0.0f, 0.0f,  0.0f },
				{ 0.0f, 1.0f, 0.0f,  0.0f },
				{ 0.0f, 0.0f, 1.0f, -2.0f }
			}};
			overlay->SetOverlayTransformTrackedDeviceRelative(sMenuOverlay,
				vr::k_unTrackedDeviceIndex_Hmd, &hmdToOverlay);
			overlay->SetOverlayWidthInMeters(sMenuOverlay, 1.55f);
			overlay->SetOverlayAlpha(sMenuOverlay, 1.0f);
			overlay->SetOverlayFlag(sMenuOverlay,
				vr::VROverlayFlags_IgnoreTextureAlpha, true);
			// Above Metro's 2D layer (5), below the performance HUD (10).
			overlay->SetOverlaySortOrder(sMenuOverlay, 8);
			LogInfo("VRPose menu overlay: initialized as dedicated 2048-square head-relative quad\n");
		}

		vr::Texture_t overlayTexture = {};
		overlayTexture.handle = texture;
		overlayTexture.eType = vr::TextureType_DirectX;
		overlayTexture.eColorSpace = vr::ColorSpace_Auto;
		vr::EVROverlayError err = overlay->SetOverlayTexture(sMenuOverlay,
			&overlayTexture);
		if (err != vr::VROverlayError_None) {
			static unsigned failureLogs = 0;
			if (failureLogs++ < 10)
				LogInfo("VRPose menu overlay: SetOverlayTexture failed (%d)\n", err);
			HideMenuOverlay();
			return false;
		}
		if (!sMenuOverlayVisible) {
			// Re-anchored in the room each time the menu opens.
			PlaceOverlayInRoom(overlay, sMenuOverlay, 2.0f, 0.0f);
			err = overlay->ShowOverlay(sMenuOverlay);
			if (err != vr::VROverlayError_None) {
				LogInfo("VRPose menu overlay: ShowOverlay failed (%d)\n", err);
				return false;
			}
			sMenuOverlayVisible = true;
			LogInfo("VRPose menu overlay: visible\n");
		}
		return true;
	}

	// --- Motion-control feasibility: locating the game's player state ---
	//
	// We know the camera's world position every frame (recovered from the
	// game's own view matrix), and we run inside the game's process. That
	// turns "find the player struct" from blind scanning into a targeted
	// search: look for those exact floats in memory, then keep only the
	// addresses that still match as the value changes. Survivors are the
	// engine's live camera state, and whatever sits around them is the
	// rest of the player state - including, hopefully, a separate aim
	// direction, which is the value that decides whether motion-controlled
	// weapons are feasible at all.
	//
	// Read-only throughout, and on a background thread so a multi-hundred-
	// MB sweep can't stall the render thread.
	CRITICAL_SECTION sScanLock;
	bool sScanLockInit = false;
	float sCameraWorldPos[3] = { 0.0f, 0.0f, 0.0f };
	bool sCameraWorldPosValid = false;
	float sViewYaw = 0.0f, sViewPitch = 0.0f;
	bool sViewAnglesValid = false;
	// The camera's forward direction in world space. Better search target
	// than Euler angles: the angle-pair hunt found nothing real, which
	// says the engine stores orientation as a vector or matrix rather
	// than as pitch/yaw floats. A forward vector is also directly useful -
	// writing it is how we would aim the weapon from a controller.
	float sViewForward[3] = { 0.0f, 0.0f, 0.0f };
	bool sViewForwardValid = false;
	float sViewRotation[9] = { 0 };
	bool sViewRotationValid = false;
	HANDLE sScannerThread = NULL;

	// The lock guards the camera state shared between the render thread and
	// the (now retired) scanner thread. It used to be initialised inside
	// EnsureMemoryScannerStarted, which quietly made every reader below
	// depend on the scanner being switched on - so retiring the scanner and
	// enabling motion aiming in the same build meant GetViewAngleSnapshot
	// entered an uninitialised CRITICAL_SECTION on the very first Present
	// and took the game down before it rendered a frame. Initialising it
	// here, on first use, ties its lifetime to the state it actually
	// protects instead of to an unrelated feature flag.
	void EnsureScanLock()
	{
		static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
		BOOL pending = FALSE;
		if (InitOnceBeginInitialize(&once, 0, &pending, NULL)) {
			if (pending) {
				InitializeCriticalSection(&sScanLock);
				sScanLockInit = true;
				InitOnceComplete(&once, 0, NULL);
			}
		}
	}

	void GetCameraSnapshot(float out[3], bool *valid)
	{
		EnsureScanLock();
		EnterCriticalSection(&sScanLock);
		out[0] = sCameraWorldPos[0];
		out[1] = sCameraWorldPos[1];
		out[2] = sCameraWorldPos[2];
		*valid = sCameraWorldPosValid;
		LeaveCriticalSection(&sScanLock);
	}

	bool LooksLikeCandidate(const float *p, const float ref[3], float eps)
	{
		return fabsf(p[0] - ref[0]) < eps &&
		       fabsf(p[1] - ref[1]) < eps &&
		       fabsf(p[2] - ref[2]) < eps;
	}

	// Angles could be stored either order, either pitch sign, and in
	// radians or degrees - we do not know the engine's convention, and
	// guessing wrong wastes an entire test run. So build every plausible
	// adjacent-pair encoding and match any of them. Searching for a PAIR
	// rather than a single float keeps false positives manageable: a lone
	// float match across ~670M floats would swamp the candidate cap.
	struct AnglePattern { float a, b; };

	void BuildAnglePatterns(float yaw, float pitch, AnglePattern out[8], int *count)
	{
		const float kRadToDeg = 57.2957795f;
		int n = 0;
		out[n++] = { yaw, pitch };
		out[n++] = { pitch, yaw };
		out[n++] = { yaw, -pitch };
		out[n++] = { -pitch, yaw };
		out[n++] = { yaw * kRadToDeg, pitch * kRadToDeg };
		out[n++] = { pitch * kRadToDeg, yaw * kRadToDeg };
		out[n++] = { yaw * kRadToDeg, -pitch * kRadToDeg };
		out[n++] = { -pitch * kRadToDeg, yaw * kRadToDeg };
		*count = n;
	}

	bool MatchesAnyAnglePattern(const float *p, const AnglePattern *pats, int count, float eps)
	{
		for (int i = 0; i < count; i++) {
			// Scale tolerance with magnitude so degree-encoded values
			// aren't held to a radian-sized tolerance.
			float scale = 1.0f + fabsf(pats[i].a) * 0.01f;
			if (fabsf(p[0] - pats[i].a) < eps * scale &&
			    fabsf(p[1] - pats[i].b) < eps * scale)
				return true;
		}
		return false;
	}

	void GetViewAngleSnapshot(float *yaw, float *pitch, bool *valid)
	{
		EnsureScanLock();
		EnterCriticalSection(&sScanLock);
		*yaw = sViewYaw;
		*pitch = sViewPitch;
		*valid = sViewAnglesValid;
		LeaveCriticalSection(&sScanLock);
	}

	void GetViewRotationSnapshot(float out[9], bool *valid)
	{
		EnsureScanLock();
		EnterCriticalSection(&sScanLock);
		memcpy(out, sViewRotation, sizeof(float) * 9);
		*valid = sViewRotationValid;
		LeaveCriticalSection(&sScanLock);
	}

	// Does a 3x3 rotation sit at p, in any of the layouts an engine might
	// plausibly use? `stride` is the distance in floats between rows: 3 for
	// a packed 3x3, 4 for the rows of a 3x4 or 4x4.
	bool MatchesRotation(const float *p, const float R[9], int stride, bool transposed, float eps)
	{
		for (int row = 0; row < 3; row++) {
			for (int col = 0; col < 3; col++) {
				float v = p[row * stride + col];
				// isfinite FIRST. Every comparison against NaN is false, so
				// a "reject when the difference is too large" test silently
				// ACCEPTS NaN - and uninitialised memory is full of it. That
				// is what produced ~157k phantom matches that never pruned:
				// every sampled candidate was NaN. (Same trap as the
				// projection validation earlier - worth remembering that a
				// range check is not a validity check.)
				if (!std::isfinite(v))
					return false;
				float expected = transposed ? R[col * 3 + row] : R[row * 3 + col];
				if (fabsf(v - expected) > eps)
					return false;
			}
		}
		return true;
	}

	bool MatchesAnyRotationLayout(const float *p, const float R[9], size_t floatsAvailable, float eps)
	{
		if (floatsAvailable >= 9) {
			if (MatchesRotation(p, R, 3, false, eps)) return true;
			if (MatchesRotation(p, R, 3, true, eps)) return true;
		}
		if (floatsAvailable >= 11) {
			if (MatchesRotation(p, R, 4, false, eps)) return true;
			if (MatchesRotation(p, R, 4, true, eps)) return true;
		}
		return false;
	}

	void GetViewForwardSnapshot(float out[3], bool *valid)
	{
		EnsureScanLock();
		EnterCriticalSection(&sScanLock);
		out[0] = sViewForward[0];
		out[1] = sViewForward[1];
		out[2] = sViewForward[2];
		*valid = sViewForwardValid;
		LeaveCriticalSection(&sScanLock);
	}

	// Our OWN stack, so the scan can exclude it. The previous run's only
	// survivors were the scanner's own pattern array - we were finding our
	// search terms and reporting them as hits. Determined by querying the
	// region containing a local, which is exact rather than a guess at
	// address-range prefixes (the guess missed 0x3B... having been written
	// for 0x4B...).
	uintptr_t sOwnStackLow = 0, sOwnStackHigh = 0;

	void RecordOwnStackRange()
	{
		int onStack = 0;
		MEMORY_BASIC_INFORMATION mbi;
		if (VirtualQuery(&onStack, &mbi, sizeof(mbi)) == sizeof(mbi)) {
			sOwnStackLow = (uintptr_t)mbi.AllocationBase;
			sOwnStackHigh = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
		}
	}

	bool IsOurOwnMemory(const void *p)
	{
		uintptr_t a = (uintptr_t)p;
		return sOwnStackLow != 0 && a >= sOwnStackLow && a < sOwnStackHigh;
	}

	// --- Unknown-value scanner ---------------------------------------
	//
	// Four content searches (position, Euler angles, forward vector, full
	// rotation matrix) each found only transient copies. The values we can
	// compute come from the RENDER camera, which carries head bob and
	// sway; the engine's underlying player orientation is evidently stored
	// in some other form. So stop guessing at the form entirely and search
	// by BEHAVIOUR instead: whatever holds the player's orientation must
	// change when the player turns and hold still when they don't.
	//
	// Self-driving: we already measure the game's own yaw, so the scanner
	// can tell "turning" from "still" by itself. The player just plays
	// naturally, alternating between the two, and each alternation prunes.
	// Once the set is small, stop eyeballing values and let the code find
	// the real one: accumulate a Pearson correlation between each
	// candidate and the components of our known view direction. Whatever
	// IS the player's orientation will correlate near +/-1 with one of
	// them; unrelated survivors will not. Running sums, so no need to
	// store sample history.
	struct Correlator {
		double n, sx, sxx, sy, syy, sxy;
		void Add(double x, double y)
		{
			n += 1; sx += x; sxx += x * x; sy += y; syy += y * y; sxy += x * y;
		}
		double R() const
		{
			double den = (n * sxx - sx * sx) * (n * syy - sy * sy);
			if (den <= 1e-12) return 0.0;
			return (n * sxy - sx * sy) / sqrt(den);
		}
	};

	struct TrackedValue {
		float *addr;
		float lastValue;
		// Score rather than eliminate. Hard filtering means a single
		// imperfect cycle - one "still" period where the value drifts a
		// touch, or a turn too gentle to register - destroys the real
		// candidate permanently. That is what collapsed a run to two
		// candidates correlating at only 0.81. Counting good and bad
		// behaviour instead tolerates a noisy test, which matters a lot
		// when the tester is juggling two controllers.
		unsigned good;
		unsigned bad;
		Correlator vsFwdX, vsFwdY, vsFwdZ;
	};

	std::vector<TrackedValue> sTracked;
	bool sUnknownScanArmed = false;

	// Once a candidate correlates at ~1.0 we verify it really is part of a
	// contiguous direction vector before writing anything, and record where
	// that vector starts plus its sign convention (the heap copies
	// correlate at -1.0, i.e. they store the opposite direction).
	float *sAimVectorBase = nullptr;
	float sAimVectorSign = 1.0f;
	bool sAimOverrideReady = false;

	// A tentative lock has to prove itself before we write anything.
	// Matching once is not evidence: an earlier attempt locked onto a
	// static (0,1,0) world up-axis constant, purely because the camera
	// happened to be pointing straight up at that instant during a
	// transition. Memory is full of such constants, and they match
	// perfectly for exactly one sample and never again. Requiring the
	// candidate to keep matching while the view genuinely MOVES rejects
	// every constant.
	float *sAimCandidateBase = nullptr;
	float sAimCandidateSign = 1.0f;
	int sAimCandidateHits = 0;
	float sAimCandidateLastFwd[3] = { 0, 0, 0 };

	// Only floats plausibly part of an orientation - finite and within a
	// modest range. Filtering here keeps the initial candidate set to
	// something storable; a raw snapshot of every float would be tens of
	// millions of entries.
	bool PlausibleOrientationComponent(float v)
	{
		if (!std::isfinite(v))
			return false;
		// Orientation components live in [-1,1]; angles in radians within
		// about +/-pi. Allow a little slack for both, but EXCLUDE values
		// very near zero. Zero is overwhelmingly the most common float in
		// memory - counters, padding, cleared fields - and admitting it
		// burned the seed's whole budget after only 78.7 MB, so the real
		// value may never have been reached. A genuine orientation
		// component sits at exactly zero only momentarily.
		float a = fabsf(v);
		if (a < 0.0005f)
			return false;
		return a <= 3.2f;
	}

	// Local copy - VRPose::WrapAngle is declared further down and isn't
	// visible from this anonymous namespace.
	float WrapAngleLocal(float a)
	{
		while (a > 3.14159265f) a -= 6.28318531f;
		while (a < -3.14159265f) a += 6.28318531f;
		return a;
	}

	// Collect every plausible orientation float in the game's smaller
	// private heaps. Large regions are skipped - those are asset and
	// texture buffers, not player state - which keeps the snapshot to
	// something we can hold in memory.
	void UnknownScanSeed(std::vector<BYTE> &buffer, SIZE_T chunkBytes)
	{
		sTracked.clear();

		MEMORY_BASIC_INFORMATION mbi;
		BYTE *addr = NULL;
		size_t scanned = 0;
		const SIZE_T kMaxRegionBytes = 64 * 1024 * 1024;

		while (VirtualQuery(addr, &mbi, sizeof(mbi)) == sizeof(mbi)) {
			BYTE *regionEnd = (BYTE *)mbi.BaseAddress + mbi.RegionSize;

			bool usable = (mbi.State == MEM_COMMIT) &&
				(mbi.Type == MEM_PRIVATE) &&
				!(mbi.Protect & PAGE_GUARD) &&
				!(mbi.Protect & PAGE_NOACCESS) &&
				(mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY)) &&
				mbi.RegionSize <= kMaxRegionBytes;

			if (usable) {
				for (SIZE_T off = 0; off + sizeof(float) <= mbi.RegionSize; ) {
					SIZE_T want = mbi.RegionSize - off;
					if (want > chunkBytes)
						want = chunkBytes;

					SIZE_T got = 0;
					if (!ReadProcessMemory(GetCurrentProcess(), (BYTE *)mbi.BaseAddress + off,
					                       buffer.data(), want, &got) || got < sizeof(float))
						break;

					const float *base = (const float *)buffer.data();
					size_t count = got / sizeof(float);
					for (size_t i = 0; i < count; i++) {
						if (!PlausibleOrientationComponent(base[i]))
							continue;
						float *a = (float *)((BYTE *)mbi.BaseAddress + off + i * sizeof(float));
						if (IsOurOwnMemory(a))
							continue;
						TrackedValue tv;
						memset(&tv, 0, sizeof(tv));
						tv.addr = a;
						tv.lastValue = base[i];
						sTracked.push_back(tv);
						if (sTracked.size() >= 20000000)
							break;
					}
					scanned += got;
					if (sTracked.size() >= 20000000)
						break;
					off += got;
				}
			}

			if (sTracked.size() >= 20000000)
				break;
			addr = regionEnd;
			if (addr < (BYTE *)mbi.BaseAddress)
				break;
		}

		LogInfo("VRPose unknown: seeded from %.1f MB - tracking %zu candidate floats\n",
			scanned / (1024.0 * 1024.0), sTracked.size());
	}

	// Keep candidates that behaved as required: `mustChange` when the
	// player turned, or held steady when they didn't.
	void UnknownScanFilter(bool mustChange)
	{
		std::vector<TrackedValue> survivors;
		survivors.reserve(sTracked.size() / 2 + 16);

		for (TrackedValue &tv : sTracked) {
			float now = 0.0f;
			SIZE_T got = 0;
			if (!ReadProcessMemory(GetCurrentProcess(), tv.addr, &now, sizeof(now), &got) ||
			    got != sizeof(now) || !std::isfinite(now))
				continue;

			// Tolerance matters: 1e-5 was far too tight. Metro's camera is
			// never truly still - weapon sway and breathing nudge it - so
			// the genuine orientation value jitters during a "still"
			// period. Deliberate turning moves a direction component by
			// ~0.05 or more; idle sway is an order of magnitude smaller.
			const float kChangeThreshold = 0.02f;
			bool changed = fabsf(now - tv.lastValue) > kChangeThreshold;

			if (changed == mustChange)
				tv.good++;
			else
				tv.bad++;
			tv.lastValue = now;

			// Drop only on a consistently poor record, never on one bad
			// cycle. Needs a few observations before judging at all.
			unsigned total = tv.good + tv.bad;
			bool keep = (total < 4) || (tv.good * 4 >= total * 3); // >= 75% correct
			if (keep)
				survivors.push_back(tv);
		}

		sTracked.swap(survivors);
		LogInfo("VRPose unknown: after '%s' - %zu candidates remain (scored, >=75%% correct kept)\n",
			mustChange ? "changed while turning" : "unchanged while still", sTracked.size());
	}


	DWORD WINAPI MemoryScannerThread(LPVOID)
	{
		// Note our own stack FIRST, so the sweep can skip it. Last run's
		// only survivors were this thread's own pattern array - we were
		// finding our own search terms and reporting them as discoveries.
		RecordOwnStackRange();
		LogInfo("VRPose scan: memory scanner started - hunting the camera forward vector "
			"(own stack %p-%p excluded)\n", (void *)sOwnStackLow, (void *)sOwnStackHigh);

		// Fixed 4 MB working buffer, allocated once and reused for every
		// region. Never grows, so the scanner's memory cost is constant
		// regardless of how large the game's heaps get.
		const SIZE_T kScanChunkBytes = 4 * 1024 * 1024;
		std::vector<BYTE> buffer(kScanChunkBytes);

		std::vector<float *> candidates;
		bool haveCandidates = false;

		// A full sweep is expensive (~2.7 GB). Allow only a couple, so a
		// level change wiping out every candidate can't put us into an
		// endless re-sweep loop - which is what stuttered the game and
		// then brought it down.
		int sweepsDone = 0;
		const int kMaxSweeps = 2;

		// DISABLED. Content searching - looking for a value we can compute -
		// was tried four ways (position, Euler angles, forward vector, full
		// rotation matrix) and every one found only transient stack copies,
		// because what we can compute is the RENDER camera, complete with
		// head bob and sway, which the engine never stores verbatim.
		//
		// Worse, this loop runs 200 passes at 1.5s each: FIVE MINUTES before
		// the behavioural scan below even starts. Every test run was paying
		// that cost first, and at least one ended before reaching the part
		// that actually works. Straight to the behavioural scan now.
		static const bool kRunContentScan = false;
		for (int pass = 0; kRunContentScan && pass < 200; pass++) {
			Sleep(1500);

			float rot[9];
			bool valid = false;
			GetViewRotationSnapshot(rot, &valid);
			if (!valid)
				continue;

			// Refuse to search while the matrix is close to identity.
			// Games are full of identity matrices - default transforms,
			// unused bones, reset states - so an identity-ish target
			// matches an enormous number of unrelated places and never
			// prunes. That is exactly what happened: ~149k candidates
			// that barely moved across dozens of passes.
			{
				const float ident[9] = { 1,0,0, 0,1,0, 0,0,1 };
				float dev = 0.0f;
				for (int i = 0; i < 9; i++)
					dev += fabsf(rot[i] - ident[i]);
				if (dev < 0.35f) {
					static int sIdentLogCount = 0;
					if (sIdentLogCount < 3) {
						LogInfo("VRPose scan: camera rotation is too close to identity "
							"(deviation %.3f) to be a usable search key - look further off-axis. "
							"Waiting.\n", dev);
						sIdentLogCount++;
					}
					continue;
				}
			}

			static unsigned sRotLogCounter = 0;
			if ((sRotLogCounter++ % 10) == 0) {
				LogInfo("VRPose scan: searching for rotation [%.4f %.4f %.4f / %.4f %.4f %.4f / %.4f %.4f %.4f]\n",
					rot[0], rot[1], rot[2], rot[3], rot[4], rot[5], rot[6], rot[7], rot[8]);
			}

			if (!haveCandidates && sweepsDone >= kMaxSweeps) {
				LogInfo("VRPose scan: candidate set was emptied and the sweep budget (%d) is spent; "
					"stopping rather than repeatedly rescanning the whole address space\n", kMaxSweeps);
				break;
			}

			if (!haveCandidates) {
				sweepsDone++;
				// First sweep: walk every committed, readable,
				// writable region and record anything matching the
				// camera position. Plenty of false positives are
				// expected here - later passes prune them.
				MEMORY_BASIC_INFORMATION mbi;
				BYTE *addr = NULL;
				size_t scannedBytes = 0;

				while (VirtualQuery(addr, &mbi, sizeof(mbi)) == sizeof(mbi)) {
					BYTE *regionEnd = (BYTE *)mbi.BaseAddress + mbi.RegionSize;

					bool readable = (mbi.State == MEM_COMMIT) &&
						!(mbi.Protect & PAGE_GUARD) &&
						!(mbi.Protect & PAGE_NOACCESS) &&
						(mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY |
						                PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));

					if (readable && mbi.RegionSize >= sizeof(float) * 3) {
						// Copy out before inspecting: reading the live pages
						// races the game freeing them between VirtualQuery
						// and the read, which faults and kills the process.
						//
						// Read in FIXED-SIZE CHUNKS rather than whole
						// regions. Sizing the buffer to the region meant
						// allocating hundreds of MB at a time for Metro's
						// larger heaps, which stuttered the game and
						// eventually exhausted memory. The chunk buffer is
						// allocated once and reused.
						for (SIZE_T off = 0; off + sizeof(float) * 3 <= mbi.RegionSize; ) {
							SIZE_T want = mbi.RegionSize - off;
							if (want > kScanChunkBytes)
								want = kScanChunkBytes;

							SIZE_T got = 0;
							if (!ReadProcessMemory(GetCurrentProcess(), (BYTE *)mbi.BaseAddress + off,
							                       buffer.data(), want, &got) || got < sizeof(float) * 3)
								break;

							const float *base = (const float *)buffer.data();
							size_t count = got / sizeof(float);
							for (size_t i = 0; i + 8 < count; i++) {
								if (MatchesAnyRotationLayout(&base[i], rot, count - i, 0.002f)) {
									float *addr = (float *)((BYTE *)mbi.BaseAddress + off + i * sizeof(float));
									if (!IsOurOwnMemory(addr))
										candidates.push_back(addr);
									if (candidates.size() > 200000)
										break;
								}
							}
							scannedBytes += got;

							if (candidates.size() > 200000)
								break;
							// Overlap by two floats so a triple straddling a
							// chunk boundary is not missed.
							off += got - sizeof(float) * 2;
						}
					}

					if (candidates.size() > 200000)
						break;
					addr = regionEnd;
					if (addr < (BYTE *)mbi.BaseAddress) // wrapped
						break;
				}

				LogInfo("VRPose scan: initial sweep scanned %.1f MB, found %zu candidate addresses "
					"holding the camera's 3x3 rotation matrix\n",
					scannedBytes / (1024.0 * 1024.0), candidates.size());
				haveCandidates = true;
				continue;
			}

			// Later passes: keep only addresses that STILL hold the
			// camera position now that it has moved. Coincidental
			// matches die off fast; the engine's real camera state
			// tracks it forever.
			// Same fault risk here - a candidate's page can be freed
			// between passes - so read these through ReadProcessMemory too
			// rather than dereferencing a pointer into the game's heap.
			std::vector<float *> survivors;
			survivors.reserve(candidates.size());
			for (float *c : candidates) {
				float probe[12];
				SIZE_T got = 0;
				if (ReadProcessMemory(GetCurrentProcess(), c, probe, sizeof(probe), &got) &&
				    got == sizeof(probe) && MatchesAnyRotationLayout(probe, rot, 12, 0.002f)) {
					survivors.push_back(c);
				}
			}
			candidates.swap(survivors);

			LogInfo("VRPose scan: pass %d - %zu addresses still hold the camera rotation matrix\n",
				pass, candidates.size());

			// When the set is still huge, dump a couple of examples anyway.
			// Knowing WHAT is matching (an identity matrix? a zeroed block?)
			// diagnoses a bad search key far faster than the count alone,
			// which told us nothing for dozens of passes.
			if (candidates.size() > 80) {
				static int sSampleLogCount = 0;
				if (sSampleLogCount < 6) {
					for (size_t i = 0; i < candidates.size() && i < 2; i++) {
						float probe[9] = { 0 };
						SIZE_T got = 0;
						ReadProcessMemory(GetCurrentProcess(), candidates[i], probe, sizeof(probe), &got);
						LogInfo("VRPose scan:   sample %zu at %p = [%.4f %.4f %.4f / %.4f %.4f %.4f / %.4f %.4f %.4f]\n",
							i, (void *)candidates[i],
							probe[0], probe[1], probe[2], probe[3], probe[4],
							probe[5], probe[6], probe[7], probe[8]);
					}
					sSampleLogCount++;
				}
			}

			// Once it has narrowed down, report the addresses so we can
			// start examining what lives around them.
			// Print the surviving addresses once the set is small enough to
			// be useful. The first cutoff was 24, which a real run then sat
			// just above (26) - so the whole scan succeeded and reported
			// nothing. Err on the generous side; a few dozen lines cost
			// nothing next to losing a run.
			if (!candidates.empty() && candidates.size() <= 80) {
				for (size_t i = 0; i < candidates.size(); i++) {
					float *c = candidates[i];
					float probe[3] = { 0.0f, 0.0f, 0.0f };
					SIZE_T got = 0;
					ReadProcessMemory(GetCurrentProcess(), c, probe, sizeof(probe), &got);

					// Classify by querying the region rather than guessing
					// from address prefixes - the prefix guess was written
					// for 0x4B... and silently mislabelled 0x3B... stack
					// addresses as heap last run. MEM_IMAGE is a module's
					// static data; MEM_PRIVATE with a large allocation is
					// typically heap; thread stacks are private too but sit
					// inside their own reserved allocation, so report the
					// region details and let the addresses speak.
					MEMORY_BASIC_INFORMATION mbi2;
					const char *kind = "unknown";
					if (VirtualQuery(c, &mbi2, sizeof(mbi2)) == sizeof(mbi2)) {
						if (mbi2.Type == MEM_IMAGE)
							kind = "module static data - PROMISING (stable across runs)";
						else if (mbi2.Type == MEM_MAPPED)
							kind = "mapped file";
						else
							kind = "heap or stack";
					}
					LogInfo("VRPose scan:   candidate %zu at %p = (%.4f %.4f %.4f)  [%s]\n",
						i, (void *)c, probe[0], probe[1], probe[2], kind);
				}
			}
			if (candidates.empty()) {
				LogInfo("VRPose scan: all candidates eliminated - the camera position is probably stored "
					"in a different layout (separate floats, doubles, or recomputed per frame). "
					"Restarting the sweep.\n");
				haveCandidates = false;
			}
		}

		LogInfo("VRPose scan: scanner finished\n");

		// ---- Unknown-value phase -------------------------------------
		// Content searches are exhausted; switch to behaviour. We know the
		// game's yaw, so we can drive this ourselves: seed once, then each
		// cycle observe whether the player turned and apply the matching
		// filter. No timed phases and no instructions to follow - the
		// player just plays, turning sometimes and holding still others.
		LogInfo("VRPose unknown: starting behavioural scan. Alternate freely between "
			"turning with the stick and holding still - each alternation prunes.\n");

		UnknownScanSeed(buffer, kScanChunkBytes);

		float prevYaw = 0.0f;
		bool havePrevYaw = false;

		for (int cycle = 0; cycle < 400 && sTracked.size() > 1; cycle++) {
			Sleep(1200);

			float yaw = 0.0f, pitch = 0.0f;
			bool valid = false;
			GetViewAngleSnapshot(&yaw, &pitch, &valid);
			if (!valid)
				continue;

			if (!havePrevYaw) {
				prevYaw = yaw;
				havePrevYaw = true;
				continue;
			}

			float turned = fabsf(WrapAngleLocal(yaw - prevYaw));
			prevYaw = yaw;

			// Ignore ambiguous middle ground - a small drift is neither a
			// deliberate turn nor genuinely still, and filtering on it
			// would discard good candidates.
			if (turned > 0.15f)
				UnknownScanFilter(true);
			else if (turned < 0.01f)
				UnknownScanFilter(false);
			else
				continue;

			// List once the set is small enough to read. Threshold was 40,
			// which a real run then plateaued just above (109) - so the
			// scan succeeded and printed nothing, the same mistake the
			// earlier position hunt made at 24 vs 26.
			//
			// Log the game's yaw and its sine/cosine on the same line: if a
			// candidate IS the orientation, its value will track one of
			// those, which identifies the real one among the survivors far
			// faster than staring at raw addresses.
			// Small enough to identify properly. Accumulate each candidate's
			// correlation against the components of our known forward
			// vector, then report the strongest - that picks the real one
			// out of dozens of survivors without guesswork.
			// Deliberately generous. Three separate runs have now been
			// wasted by a cutoff sitting just below where the candidate set
			// settled (24 vs 26, 40 vs 109, 250 vs 268). Correlating a few
			// thousand values costs nothing, and only hits above |r| = 0.9
			// are printed, so there is no reason to gate this tightly.
			if (sTracked.size() <= 20000 && !sTracked.empty()) {
				float fwdNow[3];
				bool fwdValid = false;
				GetViewForwardSnapshot(fwdNow, &fwdValid);

				if (fwdValid) {
					for (TrackedValue &tv : sTracked) {
						float now = 0.0f;
						SIZE_T got = 0;
						if (!ReadProcessMemory(GetCurrentProcess(), tv.addr, &now, sizeof(now), &got) ||
						    got != sizeof(now) || !std::isfinite(now))
							continue;
						tv.vsFwdX.Add(now, fwdNow[0]);
						tv.vsFwdY.Add(now, fwdNow[1]);
						tv.vsFwdZ.Add(now, fwdNow[2]);
					}

					// Runs EVERY cycle. This whole block used to sit behind a
					// "log at most 6 times" throttle, which meant the lock
					// verification also got only six attempts - and since an
					// inconclusive sample (view barely moved, or swung too
					// hard) consumes one without proving anything, it ran out
					// before a candidate could accumulate the hits it needed.
					// That is why the same address kept reappearing as
					// tentative and never advanced. Only the LOGGING is
					// throttled now; the verification runs continuously.
					static int sCorrLogCount = 0;
					if (sTracked[0].vsFwdX.n >= 12) {
						bool verbose = sCorrLogCount < 6;
						if (verbose)
							sCorrLogCount++;
						if (verbose)
							LogInfo("VRPose unknown: correlating %zu candidates against the view direction "
								"(%.0f samples). |r| near 1.0 means it IS that component.\n",
								sTracked.size(), sTracked[0].vsFwdX.n);

						// Rank and always report the top few, rather than
						// printing only what clears a fixed bar. A run that
						// produces no output teaches us nothing, and a
						// threshold guess is exactly what has cost us runs
						// already. Seeing the BEST scores tells us whether
						// we are close or nowhere near, either way.
						std::vector<std::pair<double, size_t>> ranked;
						ranked.reserve(sTracked.size());
						for (size_t i = 0; i < sTracked.size(); i++) {
							double rx = sTracked[i].vsFwdX.R();
							double ry = sTracked[i].vsFwdY.R();
							double rz = sTracked[i].vsFwdZ.R();
							double best = fabs(rx);
							if (fabs(ry) > best) best = fabs(ry);
							if (fabs(rz) > best) best = fabs(rz);
							ranked.push_back(std::make_pair(best, i));
						}
						std::sort(ranked.begin(), ranked.end());

						// Try to lock onto a writable aim vector. A single
						// component correlating at 1.0 isn't enough to write
						// safely - we need to know where the vector starts.
						// The candidate could be its x, y or z, so test each
						// alignment and require ALL THREE components to
						// match our forward direction (allowing for the
						// inverted sign the heap copies use). Anything less
						// and we'd be writing into a structure we don't
						// understand.
						// Validate an existing tentative lock first: has it
						// kept matching while the view actually moved?
						if (!sAimOverrideReady && sAimCandidateBase) {
							float moved = fabsf(fwdNow[0] - sAimCandidateLastFwd[0]) +
								fabsf(fwdNow[1] - sAimCandidateLastFwd[1]) +
								fabsf(fwdNow[2] - sAimCandidateLastFwd[2]);

							// Tolerance has to allow for a frame of lag. We sample
							// the view matrix at one point in the frame and the
							// game's own copy updates at another, so during a
							// brisk turn a perfectly genuine copy differs by far
							// more than a hair. A 0.03 window was rejecting real
							// hits after one or two ticks - they tracked, then
							// failed the instant the view moved quickly.
							float v[3] = { 0, 0, 0 };
							SIZE_T got = 0;
							bool ok = ReadProcessMemory(GetCurrentProcess(), sAimCandidateBase, v, sizeof(v), &got) &&
								got == sizeof(v) &&
								fabsf(v[0] - sAimCandidateSign * fwdNow[0]) < 0.10f &&
								fabsf(v[1] - sAimCandidateSign * fwdNow[1]) < 0.10f &&
								fabsf(v[2] - sAimCandidateSign * fwdNow[2]) < 0.10f;

							// Judge only on moderate movement. Too little proves
							// nothing; a violent swing is dominated by lag rather
							// than by whether this is the right value.
							if (moved < 0.05f || moved > 0.8f) {
								// Inconclusive - don't count it either way.
							} else if (ok) {
								sAimCandidateHits++;
								memcpy(sAimCandidateLastFwd, fwdNow, sizeof(fwdNow));
								if (sAimCandidateHits >= 3) {
									sAimVectorBase = sAimCandidateBase;
									sAimVectorSign = sAimCandidateSign;
									sAimOverrideReady = true;
									LogInfo("VRPose aim: CONFIRMED %p after tracking the view through "
										"%d genuine changes (sign %+.0f). Enabling controller override.\n",
										(void *)sAimVectorBase, sAimCandidateHits, sAimVectorSign);
									LogOverlay(LOG_DIRE, "VR: aim override ENGAGED - point the controller now\n");
								}
							} else {
								LogInfo("VRPose aim: candidate %p stopped tracking after %d hits - "
									"rejecting (probably a constant, not the camera)\n",
									(void *)sAimCandidateBase, sAimCandidateHits);
								sAimCandidateBase = nullptr;
								sAimCandidateHits = 0;
							}
						}

						if (!sAimOverrideReady && !sAimCandidateBase) {
							for (size_t k = 0; k < ranked.size() && !sAimOverrideReady; k++) {
								size_t i = ranked[ranked.size() - 1 - k].second;
								if (ranked[ranked.size() - 1 - k].first < 0.999)
									break;
								float *cand = sTracked[i].addr;

								// Skip stack copies. The first attempt locked
								// onto 0x0000001DFB5FC130 - a perfect match,
								// but a per-frame scratch temporary the game
								// rewrites immediately, so writing there does
								// nothing. Every stack address seen in this
								// process has been below 0x100'00000000
								// (0x1D..., 0x3B..., 0x4B..., 0x9D..., 0xFB...)
								// while the persistent heap copies sit well
								// above it (0x1E9..., 0x26C...). Only heap
								// state is worth writing to.
								if ((uintptr_t)cand < 0x0000010000000000ull)
									continue;

								for (int slot = 0; slot < 3 && !sAimOverrideReady; slot++) {
									float *base = cand - slot;
									float v[3] = { 0, 0, 0 };
									SIZE_T got = 0;
									if (!ReadProcessMemory(GetCurrentProcess(), base, v, sizeof(v), &got) ||
									    got != sizeof(v))
										continue;
									if (!std::isfinite(v[0]) || !std::isfinite(v[1]) || !std::isfinite(v[2]))
										continue;

									// Refuse to start a lock while the camera points
									// near a cardinal axis. (0,1,0), (1,0,0) and
									// friends exist all over memory as constants,
									// and a match against one is meaningless.
									float mx = fabsf(fwdNow[0]), my = fabsf(fwdNow[1]), mz = fabsf(fwdNow[2]);
									float largest = mx > my ? (mx > mz ? mx : mz) : (my > mz ? my : mz);
									if (largest > 0.97f)
										continue;

									for (int s = 0; s < 2; s++) {
										float sign = (s == 0) ? 1.0f : -1.0f;
										if (fabsf(v[0] - sign * fwdNow[0]) < 0.02f &&
										    fabsf(v[1] - sign * fwdNow[1]) < 0.02f &&
										    fabsf(v[2] - sign * fwdNow[2]) < 0.02f) {
											// TENTATIVE only - it must keep matching
											// through several real view changes before
											// we write anything to it.
											sAimCandidateBase = base;
											sAimCandidateSign = sign;
											sAimCandidateHits = 1;
											memcpy(sAimCandidateLastFwd, fwdNow, sizeof(fwdNow));
											LogInfo("VRPose aim: tentative candidate %p (sign %+.0f) reads "
												"(%.4f %.4f %.4f) vs forward (%.4f %.4f %.4f) - verifying "
												"it tracks before writing.\n",
												(void *)base, sign, v[0], v[1], v[2],
												fwdNow[0], fwdNow[1], fwdNow[2]);
											break;
										}
									}
								}
							}
						}

						size_t show = verbose ? (ranked.size() < 12 ? ranked.size() : 12) : 0;
						for (size_t k = 0; k < show; k++) {
							size_t i = ranked[ranked.size() - 1 - k].second;
							double rx = sTracked[i].vsFwdX.R();
							double ry = sTracked[i].vsFwdY.R();
							double rz = sTracked[i].vsFwdZ.R();
							double best = ranked[ranked.size() - 1 - k].first;
							const char *which = (fabs(rx) >= fabs(ry) && fabs(rx) >= fabs(rz)) ? "fwd.x"
								: ((fabs(ry) >= fabs(rz)) ? "fwd.y" : "fwd.z");
							float now = 0.0f;
							SIZE_T got = 0;
							ReadProcessMemory(GetCurrentProcess(), sTracked[i].addr, &now, sizeof(now), &got);
							LogInfo("VRPose unknown:   #%zu %p = %.5f  |r|=%.4f vs %s "
								"(rx=%.3f ry=%.3f rz=%.3f)\n",
								k, (void *)sTracked[i].addr, now, best, which, rx, ry, rz);
						}
					}
				}
			}
		}

		LogInfo("VRPose unknown: behavioural scan finished with %zu candidates\n", sTracked.size());
		return 0;
	}

	void ExtractRotation3x3(const vr::HmdMatrix34_t &hmdPose, float out[9])
	{
		for (int row = 0; row < 3; row++) {
			for (int col = 0; col < 3; col++) {
				out[row * 3 + col] = hmdPose.m[row][col];
			}
		}
	}

	// Converts an OpenVR-space rotation (right-handed, +Y up, +X right,
	// -Z forward - see openvr.h's HmdMatrix34_t comment) into whatever
	// row-major 3x3 convention the game's m_V/m_iV expect. Operates on
	// the DELTA rotation (see UpdateVRPose), not an absolute orientation.
	//
	// First live test (identity passthrough) showed yaw inverted: turning
	// the head left made the world move left in view instead of right
	// (the correct behavior is the opposite - turning left should reveal
	// what's to your left by making the previous view slide right). Fixed
	// (user-confirmed) by conjugating with S=diag(-1,1,1) (R'=S*R*S),
	// which inverts yaw (X-Z plane rotation).
	//
	// Second live test (after the yaw fix, plus an unrelated translation
	// bug fix) showed pitch ALSO inverted (up/down backwards) - a
	// separate problem from yaw, unaffected by the diag(-1,1,1) fix
	// (which leaves pitch, an X-axis rotation, unchanged). Extended to
	// S=diag(-1,-1,1) (flip both X and Y, keep Z), which inverts yaw AND
	// pitch while leaving roll at whatever it was under the original
	// identity copy (the X-only and Y-only inversions each flip roll
	// once, so flipping both together cancels back out - roll is thus
	// still unvalidated, watch for it specifically next test: tilt your
	// head sideways and see if the world tilts the opposite way).
	void ConvertOpenVRRotationToGameSpace(const float in3x3[9], float out3x3[9])
	{
		out3x3[0] = in3x3[0];
		out3x3[1] = in3x3[1];
		out3x3[2] = -in3x3[2];
		out3x3[3] = in3x3[3];
		out3x3[4] = in3x3[4];
		out3x3[5] = -in3x3[5];
		out3x3[6] = -in3x3[6];
		out3x3[7] = -in3x3[7];
		out3x3[8] = in3x3[8];
	}

}

// The frame number of the most recent confirmed shot (RedirectShot's own
// per-shot log). File scope, not a Globals field: HackerContext.cpp's decal
// diagnostic (see BeginDecalCB) needs it to narrow candidate logging to
// draws that happen right after a real shot, and Globals has bitten this
// project twice when fields were added without a clean rebuild.
static unsigned sGlobalLastShotFrame = 0xFFFFFFFF;

namespace VRPose {
	bool PresentMenuOverlay(ID3D11Texture2D *texture)
	{
		return ShowMenuOverlay(texture);
	}

	static volatile LONG sTraceFrames = 4;   // the first frames of every session
	static volatile LONG sSuccessfulSubmits = 0;

	void TraceStep(const char *step)
	{
		if (InterlockedCompareExchange(&sTraceFrames, 0, 0) <= 0)
			return;
		CompatibilityLog("trace frame=%u tid=%lu step=%s\n",
			G ? G->frame_no : 0u, GetCurrentThreadId(), step);
	}

	void TraceFrameEnd()
	{
		if (InterlockedCompareExchange(&sTraceFrames, 0, 0) > 0)
			InterlockedDecrement(&sTraceFrames);
	}

	void ArmTrace(const char *reason, int frames)
	{
		CompatibilityLog("trace armed: %s\n", reason);
		InterlockedExchange(&sTraceFrames, frames);
	}

	unsigned SuccessfulCompositorFrames()
	{
		return (unsigned)InterlockedCompareExchange(&sSuccessfulSubmits, 0, 0);
	}

	void HidePerfOverlay()
	{
		if (!sPerfOverlayVisible || sPerfOverlay == vr::k_ulOverlayHandleInvalid)
			return;
		vr::IVROverlay *overlay = vr::VROverlay();
		if (overlay)
			overlay->HideOverlay(sPerfOverlay);
		sPerfOverlayVisible = false;
	}

	bool PresentPerfOverlay(ID3D11Texture2D *texture, bool contentChanged)
	{
		if (!texture)
			return false;
		vr::IVROverlay *overlay = vr::VROverlay();
		if (!overlay)
			return false;

		if (sPerfOverlay == vr::k_ulOverlayHandleInvalid) {
			vr::EVROverlayError err = overlay->CreateOverlay(
				"metro2033reduxvr.perf_hud", "Metro 2033 Redux VR performance", &sPerfOverlay);
			if (err == vr::VROverlayError_KeyInUse)
				err = overlay->FindOverlay("metro2033reduxvr.perf_hud", &sPerfOverlay);
			if (err != vr::VROverlayError_None || sPerfOverlay == vr::k_ulOverlayHandleInvalid) {
				LogInfo("VRPerf HUD overlay: creation failed (%d)\n", err);
				sPerfOverlay = vr::k_ulOverlayHandleInvalid;
				return false;
			}
			// Head-relative, below and left of the line of sight, tilted 20
			// degrees up to face the eyes so it reads without looking straight at it.
			const float c = 0.9397f, s = 0.3420f;
			const vr::HmdMatrix34_t hmdToOverlay = {{
				{ 1.0f, 0.0f, 0.0f, -0.22f },
				{ 0.0f, c, s, -0.30f },
				{ 0.0f, -s, c, -1.20f }
			}};
			overlay->SetOverlayTransformTrackedDeviceRelative(sPerfOverlay,
				vr::k_unTrackedDeviceIndex_Hmd, &hmdToOverlay);
			overlay->SetOverlayWidthInMeters(sPerfOverlay, 0.80f);
			overlay->SetOverlayAlpha(sPerfOverlay, 1.0f);
			overlay->SetOverlaySortOrder(sPerfOverlay, 10);
			contentChanged = true;
			ArmTrace("perf-hud first show", 4);
		}

		if (contentChanged || !sPerfOverlayVisible) {
			vr::Texture_t overlayTexture = {};
			overlayTexture.handle = texture;
			overlayTexture.eType = vr::TextureType_DirectX;
			overlayTexture.eColorSpace = vr::ColorSpace_Auto;
			if (overlay->SetOverlayTexture(sPerfOverlay, &overlayTexture) != vr::VROverlayError_None) {
				HidePerfOverlay();
				return false;
			}
		}
		if (!sPerfOverlayVisible) {
			if (overlay->ShowOverlay(sPerfOverlay) != vr::VROverlayError_None)
				return false;
			sPerfOverlayVisible = true;
		}
		return true;
	}

	// Metro's post-scene 2D on non-gameplay screens (press-any-key prompt, main
	// menu, chapter select), captured once into an untwinned transparent layer
	// by HackerContext and shown as ONE quad for both eyes - the S.T.A.L.K.E.R.
	// VR UI approach. Identical 2D coordinates submitted in both scene eyes
	// cannot fuse through the two asymmetric eye frusta; a compositor quad can.
	static vr::VROverlayHandle_t sUILayerOverlay = vr::k_ulOverlayHandleInvalid;
	static bool sUILayerOverlayVisible = false;
	// Latched once the quad cannot be created or keeps being refused: from then
	// on the late 2D stays in the eye images (see IsUILayerScreen).
	static bool sUILayerOverlayFailed = false;
	static int sUILayerOverlayFailures = 0;
	// Set by SubmitFrameToCompositor; read by UpdateCinemaFrame on the same thread.
	static bool sCompositorGivenUp = false;
	static bool sLastSubmitAccepted = false;

	static void NoteUILayerOverlayFailure()
	{
		if (++sUILayerOverlayFailures >= 30 && !sUILayerOverlayFailed) {
			sUILayerOverlayFailed = true;
			CompatibilityLog("ui_layer=disabled consecutive_overlay_failures=%d\n",
				sUILayerOverlayFailures);
		}
	}

	void HideUILayerOverlay()
	{
		if (!sUILayerOverlayVisible || sUILayerOverlay == vr::k_ulOverlayHandleInvalid)
			return;
		vr::IVROverlay *overlay = vr::VROverlay();
		if (overlay)
			overlay->HideOverlay(sUILayerOverlay);
		sUILayerOverlayVisible = false;
	}

	bool PresentUILayerOverlay(ID3D11Texture2D *texture)
	{
		if (!texture || sUILayerOverlayFailed)
			return false;
		vr::IVROverlay *overlay = vr::VROverlay();
		if (!overlay)
			return false;
		if (sUILayerOverlay == vr::k_ulOverlayHandleInvalid) {
			vr::EVROverlayError err = overlay->CreateOverlay(
				"metro2033reduxvr.ui_layer", "Metro 2033 Redux VR 2D layer", &sUILayerOverlay);
			if (err == vr::VROverlayError_KeyInUse)
				err = overlay->FindOverlay("metro2033reduxvr.ui_layer", &sUILayerOverlay);
			if (err != vr::VROverlayError_None || sUILayerOverlay == vr::k_ulOverlayHandleInvalid) {
				LogInfo("VRPose UI layer overlay: creation failed (%d)\n", err);
				CompatibilityLog("ui_layer=overlay-failed error=%d\n", (int)err);
				sUILayerOverlay = vr::k_ulOverlayHandleInvalid;
				sUILayerOverlayFailed = true;
				return false;
			}
			// Head-relative 2 m ahead, 2.4 m wide (about 62 degrees): Metro's
			// full-screen 2D layout at a comfortable distance. The layer holds
			// premultiplied colour plus coverage (see UILayerBlendVariant).
			const vr::HmdMatrix34_t hmdToOverlay = {{
				{ 1.0f, 0.0f, 0.0f, 0.0f },
				{ 0.0f, 1.0f, 0.0f, 0.0f },
				{ 0.0f, 0.0f, 1.0f, -2.0f }
			}};
			overlay->SetOverlayTransformTrackedDeviceRelative(sUILayerOverlay,
				vr::k_unTrackedDeviceIndex_Hmd, &hmdToOverlay);
			overlay->SetOverlayWidthInMeters(sUILayerOverlay, 2.4f);
			overlay->SetOverlayAlpha(sUILayerOverlay, 1.0f);
			overlay->SetOverlayFlag(sUILayerOverlay, vr::VROverlayFlags_IsPremultiplied, true);
			overlay->SetOverlaySortOrder(sUILayerOverlay, 5);
			CompatibilityLog("ui_layer=overlay-created\n");
		}
		vr::Texture_t overlayTexture = {};
		overlayTexture.handle = texture;
		overlayTexture.eType = vr::TextureType_DirectX;
		overlayTexture.eColorSpace = vr::ColorSpace_Auto;
		if (overlay->SetOverlayTexture(sUILayerOverlay, &overlayTexture) != vr::VROverlayError_None) {
			HideUILayerOverlay();
			NoteUILayerOverlayFailure();
			return false;
		}
		if (!sUILayerOverlayVisible) {
			if (overlay->ShowOverlay(sUILayerOverlay) != vr::VROverlayError_None) {
				NoteUILayerOverlayFailure();
				return false;
			}
			sUILayerOverlayVisible = true;
			static int sShowLogs = 0;
			if (sShowLogs++ < 20)
				CompatibilityLog("ui_layer=visible frame=%u\n", G ? G->frame_no : 0u);
		}
		sUILayerOverlayFailures = 0;
		return true;
	}

	static volatile LONG sFullscreenVideoFrame = -1;
	static volatile LONG sPauseMenuExpected = 0;
	static volatile LONG sLoadingScreenFrame = -1000;
	static volatile LONG sJournalButtonFrame = -1000;
	static volatile LONG sJournalPresented = 0;
	// BEGIN JOURNAL PRESENCE STATE
	static volatile LONG sJournalBookFrame = -1000;
	// END JOURNAL PRESENCE STATE
	static volatile LONG sJournalObjectiveFrame = -1000;
	static volatile LONG sLighterButtonFrame = -1000;
	static volatile LONG sLighterButtonSequence = 0;

	void NotifyFullscreenVideoDraw(unsigned frame)
	{
		InterlockedExchange(&sFullscreenVideoFrame, (LONG)frame);
	}

	bool IsFullscreenVideoFrame(unsigned frame)
	{
		return (unsigned)InterlockedCompareExchange(&sFullscreenVideoFrame, -1, -1) == frame;
	}

	void NotifyLoadingScreenDraw(unsigned frame)
	{
		InterlockedExchange(&sLoadingScreenFrame, (LONG)frame);
	}

	static volatile LONG sCinemaFrameActive = 0;
	// Cinema held by a load's closing panel while the level already renders.
	static volatile LONG sCinemaPanelHold = 0;
	static bool sMainMenuSeen = false;
	static volatile LONG sSceneRenderedFrame = -1000;
	static volatile LONG sGBufferFrame = -1000;
	static volatile LONG sNativeMenuModeCached = -1;

	void NotifySceneRendered(unsigned frame)
	{
		InterlockedExchange(&sSceneRenderedFrame, (LONG)frame);
	}

	void NotifyGBufferRendered(unsigned frame)
	{
		InterlockedExchange(&sGBufferFrame, (LONG)frame);
	}

	static bool ReadNativeMenuMode(int *mode, int *previous);

	// Frames since a stamp was published, clamped (999 = never / long ago).
	static unsigned StampAge(volatile LONG *stamp)
	{
		const LONG frame = InterlockedCompareExchange(stamp, -1000, -1000);
		if (frame < 0 || !G)
			return 999;
		const unsigned age = G->frame_no - (unsigned)frame;
		return age > 999 ? 999 : age;
	}

	static volatile LONG sUILayerScreen = 0;
	static bool sGBufferEverSeen = false;

	void UpdateCinemaFrame()
	{
		// Runs after Present N and decides for frame N+1; a stamp published
		// while frame N was drawn is therefore 1 old here.
		int mode = -1, previous = -1;
		ReadNativeMenuMode(&mode, &previous);
		InterlockedExchange(&sNativeMenuModeCached, mode);
		const bool introBefore = !sMainMenuSeen;   // for the UI-layer rule below
		if (mode == 1)
			sMainMenuSeen = true;
		const unsigned panelAge = StampAge(&sLoadingScreenFrame);
		const unsigned gbufAge = StampAge(&sGBufferFrame);
		const unsigned sceneAge = StampAge(&sSceneRenderedFrame);
		const bool gameplay = IsGameplayModeActive();
		// Cinema = no G-buffer this frame and (loading panel up, front-end mode 1,
		// or before the main menu/gameplay first appeared). The panel alone is not
		// enough: it also draws the prompt over the 3D fly-through. Cinema ends on the
		// first frame that fills the G-buffer again.
		const bool noScene = gbufAge > 1;
		if (!noScene)
			sGBufferEverSeen = true;
		const bool panelUp = panelAge <= 3;
		const bool wasCinemaBefore = InterlockedCompareExchange(&sCinemaFrameActive, 0, 0) != 0;
		// Frames of the current panel run spent without a 3D scene: a real load.
		static unsigned sPanelNoSceneFrames = 0;
		if (!panelUp)
			sPanelNoSceneFrames = 0;
		else if (noScene)
			sPanelNoSceneFrames++;
		const bool baseCinema = noScene &&
			(panelUp || mode == 1 || (!gameplay && !sMainMenuSeen));
		// The end of a load ("press any button") already renders the level
		// underneath - the G-buffer is back - while the loading panel still
		// covers the screen: keep the theatre until that panel goes away. Only
		// after a real load (the panel up without a scene for 10+ frames), so
		// the prompt over the start-up fly-through stays in stereo.
		const bool panelHold = !baseCinema && wasCinemaBefore && panelUp &&
			sPanelNoSceneFrames >= 10;
		const bool cinema = baseCinema || panelHold;
		InterlockedExchange(&sCinemaPanelHold, panelHold ? 1 : 0);

		// Late-2D UI layer for the next frame (HackerContext::ActivateUILayer): only
		// with a working OpenVR overlay, the menu mode stable for two Presents and
		// at least 8 frames since cinema, so a level's first frames never capture the
		// gameplay HUD. Otherwise the 2D stays in the eye images.
		static int sPrevMode = -1;
		static unsigned sFramesSinceCinema = 999;
		sFramesSinceCinema = cinema ? 0 : (sFramesSinceCinema < 999 ? sFramesSinceCinema + 1 : 999);
		const bool menuStable = mode == 1 && sPrevMode == 1;
		sPrevMode = mode;
		const bool presentable = sVRSystem && !sCompositorGivenUp && sLastSubmitAccepted &&
			!sUILayerOverlayFailed && vr::VROverlay() != nullptr;
		const bool uiLayer = presentable && !cinema && sFramesSinceCinema >= 8 &&
			((!gameplay && introBefore) || menuStable);
		InterlockedExchange(&sUILayerScreen, uiLayer ? 1 : 0);

		// Log screen-state changes (at most 300 lines).
		static int sLastMode = -2;
		static bool sLastScene = false, sLastPanel = false, sLastGameplay = false;
		static bool sLastUILayer = false;
		static int sScreenLogs = 0;
		const bool panelLive = panelAge <= 3;
		if ((mode != sLastMode || !noScene != sLastScene || panelLive != sLastPanel ||
			gameplay != sLastGameplay || uiLayer != sLastUILayer) && sScreenLogs < 300) {
			sScreenLogs++;
			CompatibilityLog("screen frame=%u mode=%d prev=%d panel_age=%u gbuf_age=%u "
				"depthscene_age=%u gameplay=%d menu_seen=%d cinema=%d ui_layer=%d\n",
				G->frame_no, mode, previous, panelAge, gbufAge, sceneAge,
				gameplay ? 1 : 0, sMainMenuSeen ? 1 : 0, cinema ? 1 : 0, uiLayer ? 1 : 0);
			sLastMode = mode;
			sLastScene = !noScene;
			sLastPanel = panelLive;
			sLastGameplay = gameplay;
			sLastUILayer = uiLayer;
		}

		const bool wasCinema = InterlockedCompareExchange(&sCinemaFrameActive, 0, 0) != 0;
		// The menu-mode reader failed (an unexpected executable): the first load
		// entered after 3D has been seen is past the start-up intro.
		if (cinema && !wasCinema && mode < 0 && sGBufferEverSeen && !sMainMenuSeen) {
			sMainMenuSeen = true;
			CompatibilityLog("screen: menu mode unreadable - start-up intro ended at frame %u\n",
				G->frame_no);
		}
		if (cinema != wasCinema) {
			static int sTransitionLogs = 0;
			if (sTransitionLogs++ < 40)
				CompatibilityLog("cinema=%d frame=%u mode=%d panel_age=%u gbuf_age=%u\n",
					cinema ? 1 : 0, G->frame_no, mode, panelAge, gbufAge);
			ArmTrace("cinema transition", 4);
			InterlockedExchange(&sCinemaFrameActive, cinema ? 1 : 0);
		}
		// vrHeadRotationValid / vrStereoParamsValid are left alone: the second eye
		// needs its matrices across the start-up sequence. A cinema frame has no 3D
		// scene, so only its flat UI is kept native (BeginUniversalUICB).
	}

	bool IsCinemaFrame()
	{
		return InterlockedCompareExchange(&sCinemaFrameActive, 0, 0) != 0;
	}

	bool IsNativeMainMenuCached()
	{
		return InterlockedCompareExchange(&sNativeMenuModeCached, -1, -1) == 1;
	}

	bool IsStartupIntro()
	{
		return !sMainMenuSeen && !IsGameplayModeActive();
	}

	bool IsUILayerScreen()
	{
		return InterlockedCompareExchange(&sUILayerScreen, 0, 0) != 0;
	}

	void NotifyJournalButton(unsigned frame)
	{
		// BEGIN JOURNAL PRESENCE EDGE
		// Read before publishing this edge: a new timestamp must not revive a
		// request that expired while the game closed the book without our chord.
		const LONG effectivePresented = IsJournalPresented() ? 1L : 0L;
		InterlockedExchange(&sJournalPresented, effectivePresented);
		// END JOURNAL PRESENCE EDGE
		InterlockedExchange(&sJournalButtonFrame, (LONG)frame);
		const LONG wasPresented = InterlockedCompareExchange(&sJournalPresented, 0, 0);
		InterlockedExchange(&sJournalPresented, wasPresented ? 0 : 1);
	}

	unsigned LastJournalButtonFrame()
	{
		return (unsigned)InterlockedCompareExchange(&sJournalButtonFrame, -1000, -1000);
	}

	bool IsJournalPresented()
	{
		// BEGIN JOURNAL PRESENCE QUERY
		if (G) return JournalPresentation::Active(
			InterlockedCompareExchange(&sJournalPresented, 0, 0) != 0,
			G->frame_no,
			(unsigned)InterlockedCompareExchange(&sJournalButtonFrame, 0, 0),
			(unsigned)InterlockedCompareExchange(&sJournalBookFrame, 0, 0));
		// END JOURNAL PRESENCE QUERY
		return InterlockedCompareExchange(&sJournalPresented, 0, 0) != 0;
	}
	// BEGIN JOURNAL PRESENCE NOTIFY
	void NotifyJournalBookDraw(unsigned frame)
	{
		InterlockedExchange(&sJournalBookFrame, (LONG)frame);
	}
	// END JOURNAL PRESENCE NOTIFY

	void NotifyLighterButton(unsigned frame)
	{
		InterlockedExchange(&sLighterButtonFrame, (LONG)frame);
		InterlockedIncrement(&sLighterButtonSequence);
	}

	unsigned LastLighterButtonFrame()
	{
		return (unsigned)InterlockedCompareExchange(
			&sLighterButtonFrame, -1000, -1000);
	}

	unsigned LighterButtonSequence()
	{
		return (unsigned)InterlockedCompareExchange(
			&sLighterButtonSequence, 0, 0);
	}

	bool ReadCameraVec(DWORD_PTR offset, float out[3]);
	bool GetControllerAim(float *outYaw, float *outPitch);
	void RecenterTrackingOrigin();
	bool IsFiring();
	float GetCullingCancellation();
	extern float sShotAimBias[2];

	unsigned LastShotFrame()
	{
		return sGlobalLastShotFrame;
	}

	static float sReticleDirection[3] = { 0.0f, 0.0f, 1.0f };
	static volatile LONG sReticleDirectionValid = 0;

	// Rebuild the same world-facing view basis used by the renderer from the
	// latest pose, rather than relying on G->vrCachedView3x4.  The latter is a
	// render-frame snapshot; a physical body turn can update the tracking pose
	// after that snapshot but before the fire hook runs.
	static bool GetCurrentRenderedViewBasis(float out[9])
	{
		if (!out || !G)
			return false;
		// Prefer the completed basis published by the renderer. This is the exact
		// view used to draw the weapon and avoids a fire/render timing race where
		// the pose may be sampled before the reference state is available.
		if (G->vrCachedViewValid) {
			const float *v = G->vrCachedView3x4;
			out[0] = v[0]; out[1] = v[1]; out[2] = v[2];
			out[3] = v[4]; out[4] = v[5]; out[5] = v[6];
			out[6] = v[8]; out[7] = v[9]; out[8] = v[10];
			return true;
		}
		if (!G->vrReferenceRotationCaptured)
			return false;

		float bodyYaw = 0.0f, bodyPitch = 0.0f;
		bool bodyValid = false;
		GetBodyHeading(&bodyYaw, &bodyPitch, &bodyValid);
		if (!bodyValid)
			return false;

		const float *d = G->vrHeadRotation3x3;
		const float headForward[3] = { d[2], d[5], d[8] };
		const float headUp[3] = { d[1], d[4], d[7] };
		const float horizontal = sqrtf(headForward[0] * headForward[0]
			+ headForward[2] * headForward[2]);
		if (horizontal < 1e-5f)
			return false;

		const float headYaw = atan2f(headForward[0], headForward[2]);
		const float clampedHeadY = headForward[1] < -1.0f ? -1.0f
			: (headForward[1] > 1.0f ? 1.0f : headForward[1]);
		const float headPitch = asinf(clampedHeadY);
		const float pitch = GetScriptedPitchOffset() + headPitch;
		const float yaw = bodyYaw + headYaw;
		const float cp = cosf(pitch), sp = sinf(pitch);
		float forward[3] = { cp * sinf(yaw), sp, cp * cosf(yaw) };
		const float fl = sqrtf(forward[0]*forward[0] + forward[1]*forward[1]
			+ forward[2]*forward[2]);
		if (fl < 1e-5f || !isfinite(fl))
			return false;
		forward[0] /= fl; forward[1] /= fl; forward[2] /= fl;

		const float fh = sqrtf(forward[0]*forward[0] + forward[2]*forward[2]);
		if (fh < 1e-5f)
			return false;
		float right[3] = { forward[2] / fh, 0.0f, -forward[0] / fh };
		float up[3] = {
			forward[1] * right[2] - forward[2] * right[1],
			forward[2] * right[0] - forward[0] * right[2],
			forward[0] * right[1] - forward[1] * right[0] };

		// Preserve the headset's actual roll, matching the renderer's view
		// construction. Ordinary body yaw therefore cannot introduce roll.
		const float towardRight = headUp[0]*right[0] + headUp[1]*right[1]
			+ headUp[2]*right[2];
		const float towardUp = headUp[0]*up[0] + headUp[1]*up[1]
			+ headUp[2]*up[2];
		const float roll = atan2f(towardRight, towardUp);
		const float cr = cosf(roll), sr = sinf(roll);
		float rolledUp[3], rolledRight[3];
		for (int i = 0; i < 3; ++i) {
			rolledUp[i] = up[i] * cr + right[i] * sr;
			rolledRight[i] = right[i] * cr - up[i] * sr;
		}
		memcpy(out + 0, rolledRight, sizeof(rolledRight));
		memcpy(out + 3, rolledUp, sizeof(rolledUp));
		memcpy(out + 6, forward, sizeof(forward));
		return true;
	}

	// The calibrated barrel axis in the corrected view-model frame.  Keep
	// this in one place: the reticle, redirected shot, and any upstream trace
	// redirect must all start from the same rendered weapon direction.
	static bool GetCanonicalBarrelViewDirection(float out[3])
	{
		if (!out)
			return false;

		float D[9] = { 1,0,0, 0,1,0, 0,0,1 };
		if (!GetWeaponRotationDelta(D))
			return false;

		static const float kModelBarrelPitch = 0.0f;
		// Final projectile-creation zero: impacts and the shared reticle track
		// each other, but their common centre sits about 0.35 degree right of
		// the iron sights. Reduce the original +0.62 degree yaw by 0.35 degree.
		static const float kModelBarrelYaw = 0.00471f;
		// The UI tab exposes this shared zero directly. X is yaw and Y is
		// pitch, stored in degrees so its +/-1.00 slider is useful for fine
		// sight calibration. Applying it here keeps the rendered reticle and
		// every redirected projectile on exactly the same ray.
		const VRMenu::Settings &menu = VRMenu::GetSettings();
		static const float kDegreesToRadians = 0.01745329252f;
		const float mp = kModelBarrelPitch + sShotAimBias[0]
			+ menu.hudPosition[1] * kDegreesToRadians;
		const float my = kModelBarrelYaw + sShotAimBias[1]
			+ menu.hudPosition[0] * kDegreesToRadians;
		const float mc = cosf(mp);
		const float m[3] = { sinf(my) * mc, sinf(mp), cosf(my) * mc };

		out[0] = D[0]*m[0] + D[1]*m[1] + D[2]*m[2];
		out[1] = D[3]*m[0] + D[4]*m[1] + D[5]*m[2];
		out[2] = D[6]*m[0] + D[7]*m[1] + D[8]*m[2];
		const float len = sqrtf(out[0]*out[0] + out[1]*out[1] + out[2]*out[2]);
		if (!isfinite(len) || len < 0.001f)
			return false;
		out[0] /= len;
		out[1] /= len;
		out[2] /= len;
		return true;
	}

	bool GetReticleDirection(float out[3])
	{
		if (!out)
			return false;
		// Return the same calibrated barrel axis used by the shot path.
		if (!GetCanonicalBarrelViewDirection(out)) {
			out[0] = 0.0f; out[1] = 0.0f; out[2] = 1.0f;
			return true;
		}
		return true;
	}

	static void InstallDirectInputProbe();
	static void ScanExistingDirectInputDevicesOnce();
	static void ScanMetroInputCallsitesOnce();
	static void ScanFlashlightActionStringXrefsOnce();
	static void InstallFlashlightInputDispatchProbe();
	static void InstallNativeCameraOwnershipObserver();
	static void InstallDirectCameraConstructionProbe();
	static void PollDirectCameraConstructionProbeHotkey();
	static void InstallUpstreamCameraBasisProbe();
	static const bool kNativeStateFollowEnabled = true;
	static void InstallNativeStateFollow();
	static float NativeFollowYawError(float headYaw);
	static void CreditNativeFollowYaw(float applied);
	static void SeedNativeFollowHistory();
	static void CreditNativeFollowPitch(float nativePitch);
	static volatile LONG sNativeStateFollowInstalled = 0;
	static unsigned sNativeFollowSettleFrames = 0;
	static float NativeFollowWrap(float angle)
	{
		return atan2f(sinf(angle), cosf(angle));
	}
	static void InstallDirectMovementHeading();
	static volatile LONG sDirectMovementHeadingInstalled = 0;
	static void PublishUpstreamDirectCancellation(float yaw, float pitch);
	static void CaptureUpstreamLegacyCancellation();
	static void RestoreUpstreamLegacyCancellation();
	static void ApplyUpstreamDirectLocomotion(float *x, float *y);
	static bool ScriptedPlayerPerformanceActive();
	static bool ScriptedPlayerCameraOwnerActive();
	static void ArmMainCameraSourceWriterProbeOnce();
	static void ScanCameraSourceWriterCallersOnce();
	void DescribeCodeAddress(DWORD64 addr, char *out, size_t outSize);

	void EnsureInitialised()
	{
		if (sAttemptedInit)
			return;
		sAttemptedInit = true;
		CompatibilityLog("runtime=openvr requested_scale=%.4f render_mode=%d\n",
			VRMenu::ResolutionScale(), (int)VRMenu::GetSettings().renderMode);

		if (IsOpenXRSelected()) {
			CompatibilityLog("initialization=openxr-selected unsupported-render-path\n");
			LogInfo("VRPose: OpenXR runtime explicitly selected; OpenVR/SteamVR initialization suppressed\n");
			if (!sOpenXRRuntime.Load() || !sOpenXRRuntime.SelectHmdSystem())
				LogInfo("VRPose: OpenXR bootstrap unavailable; session/render path is not enabled yet\n");
			return;
		}

		if (!vr::VR_IsHmdPresent()) {
			CompatibilityLog("initialization=no-hmd-present\n");
			LogInfo("VRPose: no HMD present, VR head tracking disabled (game will run unmodified)\n");
			return;
		}

		vr::EVRInitError err = vr::VRInitError_None;
		// VRApplication_Scene (was Background through milestone 1):
		// Background apps can query poses but are NOT permitted to
		// submit frames to the compositor, which milestone 2 requires
		// - see SubmitFrameToCompositor. Scene declares us as the
		// application producing the headset's view, which is exactly
		// what we now are. The VR_IsHmdPresent() guard above still
		// means this never fires (and so never forces SteamVR to
		// launch) when the user is just playing flat.
		sVRSystem = vr::VR_Init(&err, vr::VRApplication_Scene);
		if (err != vr::VRInitError_None) {
			CompatibilityLog("initialization=failed error=%d description=%s\n",
				(int)err, vr::VR_GetVRInitErrorAsEnglishDescription(err));
			LogInfo("VRPose: vr::VR_Init failed: %s\n", vr::VR_GetVRInitErrorAsEnglishDescription(err));
			sVRSystem = nullptr;
			return;
		}

		LogInfo("VRPose: OpenVR initialised successfully, head tracking active\n");
		CompatibilityLog("initialization=success\n");

		// Establish the runtime's real per-eye shape for gameplay submission.
		// The settings UI is a separate fixed-density compositor overlay.
		sVRSystem->GetRecommendedRenderTargetSize(
			&sRecommendedEyeWidth, &sRecommendedEyeHeight);
		const float requestedScale = VRMenu::ResolutionScale();
		const uint32_t requestedEyeWidth = static_cast<uint32_t>(
			sRecommendedEyeWidth * requestedScale + 0.5f);
		const uint32_t requestedEyeHeight = static_cast<uint32_t>(
			sRecommendedEyeHeight * requestedScale + 0.5f);
		LogInfo("VRPose resolution baseline: OpenVR recommended per-eye=%ux%u; "
			"menu scale=%.4f requests per-eye=%ux%u\n",
			sRecommendedEyeWidth, sRecommendedEyeHeight,
			requestedScale, requestedEyeWidth, requestedEyeHeight);
		vr::ETrackedPropertyError frequencyError = vr::TrackedProp_Success;
		const float displayFrequency = sVRSystem->GetFloatTrackedDeviceProperty(
			vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_DisplayFrequency_Float,
			&frequencyError);
		CompatibilityLog("openvr_recommended_per_eye=%ux%u display_hz=%.3f "
			"display_hz_error=%d menu_overlay=2048x2048\n",
			sRecommendedEyeWidth, sRecommendedEyeHeight, displayFrequency,
			(int)frequencyError);
	}

	static void PollOwnershipProbeHotkeys();

	// Temporary native-flashlight discovery probe. This is enabled only in the
	// separately built diagnostic DLL; normal gameplay builds leave it inert.
	static const bool kFlashlightTransitionProbeEnabled = false;
	static const bool kFlashlightConsumerTraceEnabled = false;
	static const bool kFlashlightActionXrefProbeEnabled = false;
	static const bool kFlashlightInputDispatchProbeEnabled = false;
	static const bool kGasMaskTransitionProbeEnabled = false;
	static const bool kGasMaskClassProbeEnabled = false;
	static const bool kGasMaskQueueAccessProbeEnabled = false;
	static const bool kGasMaskStableReversibleProbeEnabled = false;
	static const bool kGasMaskStateAccessProbeEnabled = false;
	static const bool kPneumaticQueueAccessProbeEnabled = false;
	static const bool kEquipmentActionTraceEnabled = false;
	struct FlashlightProbeRegion {
		uintptr_t base;
		SIZE_T size;
		std::vector<BYTE> before;
		std::vector<BYTE> middle;
		std::vector<BYTE> candidateMask;
	};
	static std::vector<FlashlightProbeRegion> sFlashlightProbeRegions;
	static bool sFlashlightProbeHasBaseline = false;
	static bool sFlashlightProbeHasMiddle = false;
	static unsigned sFlashlightProbeCompletedCycles = 0;
	static LONG sFlashlightProbeCaptureBusy = 0;
	static void PollFlashlightTransitionProbe();


	// The persisted-pointer watcher was intentionally bounded to one run. Its
	// transitions proved to be allocation/object churn rather than a stable
	// player-versus-script ownership state, so leave all sampler machinery
	// dormant until a different engine-side signal is found.
	static const bool kControlStateSamplerEnabled = false;
	static volatile LONG sControlStateSampleRequest = 0; // 1=ordinary, 2=scripted
	static void HuntSample(bool scriptedNow);

	// The first sampler run left a persisted differential, but requiring the
	// player to label every sample made the result depend on the same heuristic
	// we are trying to replace.  Load the pointer shortlist and watch it
	// passively.  This does not write memory or infer scripted state; it only
	// reports a candidate when its null/module/heap class changes in the
	// direction predicted by the two labelled seed samples.
	struct AutoControlPointer {
		DWORD rva;
		BYTE playerClass;
		BYTE scriptedClass;
		BYTE previousClass;
		bool primed;
	};
	// The hunt implementation is defined later in this file; these forward
	// declarations let the passive watcher run from the normal pose update.
	static BYTE *sHuntBase = NULL;
	static bool HuntFindRegion();
	static BYTE HuntClassify(ULONGLONG v);
	static std::vector<AutoControlPointer> sAutoControlPointers;
	static bool sAutoControlPointersLoaded = false;

	static void PollPersistedControlPointers()
	{
		if (!kControlStateSamplerEnabled)
			return;
		if (!sHuntBase && !HuntFindRegion())
			return;
		if (!sAutoControlPointersLoaded) {
			sAutoControlPointersLoaded = true;
			FILE *file = NULL;
			if (fopen_s(&file, "vr_control_state_candidates.txt", "r") == 0 && file) {
				char line[256] = {};
				while (fgets(line, sizeof(line), file)) {
					unsigned rva = 0, playerClass = 0, scriptedClass = 0;
					if (sscanf_s(line, "pointer metro.exe+0x%X playerClass=%u scriptedClass=%u",
						&rva, &playerClass, &scriptedClass) == 3) {
						AutoControlPointer c = {};
						c.rva = (DWORD)rva;
						c.playerClass = (BYTE)playerClass;
						c.scriptedClass = (BYTE)scriptedClass;
						sAutoControlPointers.push_back(c);
					}
				}
				fclose(file);
			}
			LogInfo("VRPose control-state watcher: loaded %zu persisted pointer candidates\n",
				sAutoControlPointers.size());
		}

		if (sAutoControlPointers.empty())
			return;
		BYTE *module = (BYTE *)GetModuleHandleA(NULL);
		for (AutoControlPointer &c : sAutoControlPointers) {
			ULONGLONG value = 0;
			SIZE_T got = 0;
			if (!ReadProcessMemory(GetCurrentProcess(), module + c.rva,
				&value, sizeof(value), &got) || got != sizeof(value))
				continue;
			const BYTE cls = HuntClassify(value);
			if (!c.primed) {
				c.previousClass = cls;
				c.primed = true;
				continue;
			}
			if (cls == c.previousClass)
				continue;
			const BYTE oldClass = c.previousClass;
			c.previousClass = cls;
			const bool predictedBoundary =
				(oldClass == c.playerClass && cls == c.scriptedClass) ||
				(oldClass == c.scriptedClass && cls == c.playerClass);
			if (predictedBoundary) {
				LogInfo("VRPose control-state watcher: metro.exe+0x%X class %u -> %u "
					"(expected player=%u scripted=%u) frame=%u\n",
					c.rva, (unsigned)oldClass, (unsigned)cls,
					(unsigned)c.playerClass, (unsigned)c.scriptedClass,
					G ? G->frame_no : 0);
			}
		}
	}

	static void PollControlStateSamplerHotkeys()
	{
		if (!kControlStateSamplerEnabled)
			return;
		PollPersistedControlPointers();
		static bool f7WasDown = false;
		static bool f8WasDown = false;
		const bool f7Down = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
		const bool f8Down = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
		if (f7Down && !f7WasDown) {
			InterlockedExchange(&sControlStateSampleRequest, 1);
			LogInfo("VRPose control-state sampler: F7 queued PLAYER sample\n");
			HuntSample(false);
			LogInfo("VRPose control-state sampler: PLAYER snapshot recorded\n");
			InterlockedExchange(&sControlStateSampleRequest, 0);
		}
		if (f8Down && !f8WasDown) {
			InterlockedExchange(&sControlStateSampleRequest, 2);
			LogInfo("VRPose control-state sampler: F8 queued SCRIPT sample\n");
			HuntSample(true);
			LogInfo("VRPose control-state sampler: SCRIPT snapshot recorded\n");
			InterlockedExchange(&sControlStateSampleRequest, 0);
		}
		f7WasDown = f7Down;
		f8WasDown = f8Down;
	}

	// Temporary native-flashlight discovery probe. F10 captures a process
	// snapshot; F11 records the on-state and F4 completes the off->on->off
	// correlation. This never sends input.
	static void PollFlashlightTransitionProbe()
	{
		if (!kFlashlightTransitionProbeEnabled || !G)
			return;
		static bool f10WasDown = false;
		static bool f11WasDown = false;
		static bool f4WasDown = false;
		const bool f10Down = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
		const bool f11Down = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
		const bool f4Down = (GetAsyncKeyState(VK_F4) & 0x8000) != 0;

		if (f10Down && !f10WasDown
			&& InterlockedCompareExchange(&sFlashlightProbeCaptureBusy, 1, 0) == 0) {
			sFlashlightProbeRegions.clear();
			sFlashlightProbeCompletedCycles = 0;
			sFlashlightProbeHasMiddle = false;
			SYSTEM_INFO si = {};
			GetSystemInfo(&si);
			uintptr_t address = (uintptr_t)si.lpMinimumApplicationAddress;
			const uintptr_t limit = (uintptr_t)si.lpMaximumApplicationAddress;
			SIZE_T total = 0;
			const SIZE_T kMaxBytes = 256ull * 1024ull * 1024ull;
			while (address < limit && total < kMaxBytes) {
				MEMORY_BASIC_INFORMATION mbi = {};
				if (!VirtualQuery((LPCVOID)address, &mbi, sizeof(mbi)))
					break;
				const uintptr_t next = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
				const DWORD protect = mbi.Protect & 0xFF;
				const bool readable = mbi.State == MEM_COMMIT
					&& !(mbi.Protect & PAGE_GUARD)
					&& protect != PAGE_NOACCESS && protect != PAGE_EXECUTE;
				if (readable && mbi.RegionSize <= kMaxBytes - total) {
					FlashlightProbeRegion region = {};
					region.base = (uintptr_t)mbi.BaseAddress;
					region.size = mbi.RegionSize;
					region.before.resize(region.size);
					SIZE_T got = 0;
					if (ReadProcessMemory(GetCurrentProcess(), mbi.BaseAddress,
						region.before.data(), region.size, &got) && got == region.size) {
						total += region.size;
						sFlashlightProbeRegions.push_back(std::move(region));
					}
				}
				if (next <= address)
					break;
				address = next;
			}
			sFlashlightProbeHasBaseline = !sFlashlightProbeRegions.empty();
			LogInfo("VRPose flashlight probe: F10 baseline captured regions=%zu bytes=%llu; "
				"toggle flashlight once, then press F11\n",
				sFlashlightProbeRegions.size(), (unsigned long long)total);
			InterlockedExchange(&sFlashlightProbeCaptureBusy, 0);
		}

		if (f11Down && !f11WasDown && sFlashlightProbeHasBaseline
			&& InterlockedCompareExchange(&sFlashlightProbeCaptureBusy, 1, 0) == 0) {
			// Keep the on-state snapshot for the second half of the
			// off->on->off correlation. The legacy one-toggle report below is
			// retained as a useful raw diagnostic.
			sFlashlightProbeHasMiddle = false;
			for (FlashlightProbeRegion &region : sFlashlightProbeRegions) {
				region.middle.resize(region.size);
				SIZE_T got = 0;
				if (!ReadProcessMemory(GetCurrentProcess(), (LPCVOID)region.base,
					region.middle.data(), region.size, &got) || got != region.size)
					region.middle.clear();
			}
			sFlashlightProbeHasMiddle = true;
			FILE *file = NULL;
			if (fopen_s(&file, "flashlight_differential.txt", "w") == 0 && file) {
				HMODULE module = GetModuleHandleA(NULL);
				const uintptr_t moduleBase = (uintptr_t)module;
				unsigned runs = 0;
				unsigned long long changed = 0;
				for (const FlashlightProbeRegion &region : sFlashlightProbeRegions) {
					std::vector<BYTE> after(region.size);
					SIZE_T got = 0;
					if (!ReadProcessMemory(GetCurrentProcess(), (LPCVOID)region.base,
						after.data(), region.size, &got) || got != region.size)
						continue;
					SIZE_T i = 0;
					while (i < region.size) {
						if (after[i] == region.before[i]) { ++i; continue; }
						const SIZE_T start = i;
						while (i < region.size && after[i] != region.before[i] && i - start < 32)
							++i;
						const SIZE_T count = i - start;
						changed += count;
						// Most live-game memory changes are animation, allocator, or
						// render-buffer churn. A flashlight state is expected to be a
						// small scalar, so retain only one-byte transitions in the
						// compact enum range, plus aligned 32/64-bit 0..4 values.
						bool candidate = false;
						if (count == 1) {
							candidate = region.before[start] <= 4 && after[start] <= 4;
						} else if (count == 4 && (start & 3) == 0) {
							DWORD oldValue = 0, newValue = 0;
							memcpy(&oldValue, region.before.data() + start, sizeof(oldValue));
							memcpy(&newValue, after.data() + start, sizeof(newValue));
							candidate = oldValue <= 4 && newValue <= 4;
						} else if (count == 8 && (start & 7) == 0) {
							ULONGLONG oldValue = 0, newValue = 0;
							memcpy(&oldValue, region.before.data() + start, sizeof(oldValue));
							memcpy(&newValue, after.data() + start, sizeof(newValue));
							candidate = oldValue <= 4 && newValue <= 4;
						}
						if (candidate && runs < 4096) {
							const uintptr_t where = region.base + start;
							if (moduleBase && where >= moduleBase && where < moduleBase + 0x2000000)
								fprintf(file, "metro.exe+0x%llX count=%llu before=",
									(unsigned long long)(where - moduleBase), (unsigned long long)count);
							else
								fprintf(file, "address=0x%p count=%llu before=", (void *)where,
									(unsigned long long)count);
							for (SIZE_T j = 0; j < count; ++j) fprintf(file, "%02X", region.before[start + j]);
							fprintf(file, " after=");
							for (SIZE_T j = 0; j < count; ++j) fprintf(file, "%02X", after[start + j]);
							fprintf(file, "\n");
							++runs;
						}
					}
				}
				fprintf(file, "summary changed_bytes=%llu candidate_runs=%u\n", changed, runs);
				fclose(file);
				LogInfo("VRPose flashlight probe: F11 differential written changed_bytes=%llu "
					"reported_runs=%u\n", changed, runs);
			} else {
				LogInfo("VRPose flashlight probe: could not open flashlight_differential.txt\n");
			}
			// Keep the baseline until F4 completes this off->on->off cycle.
			InterlockedExchange(&sFlashlightProbeCaptureBusy, 0);
		}

		if (f4Down && !f4WasDown && sFlashlightProbeHasBaseline
			&& sFlashlightProbeHasMiddle
			&& InterlockedCompareExchange(&sFlashlightProbeCaptureBusy, 1, 0) == 0) {
			unsigned survivors = 0;
			for (FlashlightProbeRegion &region : sFlashlightProbeRegions) {
				if (region.middle.size() != region.size) continue;
				std::vector<BYTE> after(region.size);
				SIZE_T got = 0;
				if (!ReadProcessMemory(GetCurrentProcess(), (LPCVOID)region.base,
					after.data(), region.size, &got) || got != region.size) continue;
				if (sFlashlightProbeCompletedCycles == 0)
					region.candidateMask.assign(region.size, 0);
				for (SIZE_T i = 0; i < region.size; ++i) {
					const bool exactToggle = region.before[i] == after[i]
						&& ((region.before[i] == 0 && region.middle[i] == 1)
							|| (region.before[i] == 1 && region.middle[i] == 0));
					if (sFlashlightProbeCompletedCycles == 0)
						region.candidateMask[i] = exactToggle ? 1 : 0;
					else if (i < region.candidateMask.size() && !exactToggle)
						region.candidateMask[i] = 0;
					if (i < region.candidateMask.size() && region.candidateMask[i])
						++survivors;
				}
				region.before.swap(after); // refresh the off baseline for cycle two
				region.middle.clear();
			}
			++sFlashlightProbeCompletedCycles;
			sFlashlightProbeHasMiddle = false;
			if (sFlashlightProbeCompletedCycles < 2) {
				LogInfo("VRPose flashlight probe: first cycle complete survivors=%u; "
					"turn on, press F11, turn off, press F4 again\n", survivors);
			} else {
				FILE *file = NULL;
				if (fopen_s(&file, "flashlight_multicycle_candidates.txt", "w") == 0 && file) {
					unsigned reported = 0;
					for (const FlashlightProbeRegion &region : sFlashlightProbeRegions) {
						for (SIZE_T i = 0; i < region.candidateMask.size(); ++i) {
							if (!region.candidateMask[i]) continue;
							fprintf(file, "address=0x%p off=%02X on=%02X cycles=2\n",
								(void *)(region.base + i), region.before[i],
								(BYTE)(region.before[i] ? 0 : 1));
							++reported;
						}
					}
					fprintf(file, "summary multicycle_candidates=%u\n", reported);
					fclose(file);
					LogInfo("VRPose flashlight probe: second cycle complete candidates=%u\n", reported);
				}
				sFlashlightProbeHasBaseline = false;
			}
			InterlockedExchange(&sFlashlightProbeCaptureBusy, 0);
		}
		f10WasDown = f10Down;
		f11WasDown = f11Down;
		f4WasDown = f4Down;
	}

	// HOW STALE IS THE POSE THE VIEW IS BUILT FROM?
	//
	// The view's pitch comes from the head pose alone, so if the world drags
	// vertically during fast head movement, the pose being used is behind the
	// head. Prediction is not the issue - these are WaitGetPoses render poses,
	// already predicted to photons - but AGE might be: the pose is latched at
	// Present, and the view matrix is written during the NEXT frame's rendering.
	//
	// Re-poll now, purely to compare. The pitch difference between the latched
	// pose and a fresh one IS the drag, in degrees, at whatever speed the player
	// is actually moving. Nothing here is consumed by the renderer.
	void LogPoseStaleness()
	{
		static const bool kPoseStalenessDiagnostic = false; // retired; fresh pose was diagnostic-only
		if (!kPoseStalenessDiagnostic || !sVRSystem || !sLastPolledPoseValid)
			return;
		vr::TrackedDevicePose_t fresh[vr::k_unMaxTrackedDeviceCount];
		sVRSystem->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding,
			0.0f, fresh, vr::k_unMaxTrackedDeviceCount);
		const vr::TrackedDevicePose_t &h = fresh[vr::k_unTrackedDeviceIndex_Hmd];
		if (!h.bPoseIsValid)
			return;
		float nowR[9], usedR[9];
		ExtractRotation3x3(h.mDeviceToAbsoluteTracking, nowR);
		ExtractRotation3x3(sLastPolledPose, usedR);
		// Forward is the -Z column in OpenVR; pitch = asin(forward.y).
		const float fyNow = -nowR[7], fyUsed = -usedR[7];
		const float cn = fyNow < -1.0f ? -1.0f : (fyNow > 1.0f ? 1.0f : fyNow);
		const float cu = fyUsed < -1.0f ? -1.0f : (fyUsed > 1.0f ? 1.0f : fyUsed);
		const float dPitch = (asinf(cn) - asinf(cu)) * 57.29578f;
		static float worst = 0.0f;
		if (fabsf(dPitch) > fabsf(worst))
			worst = dPitch;
		static unsigned n = 0;
		if ((n++ % 90) == 0) {
			LogInfo("VRPose posestale: pitch drift used-vs-now %+.2f deg, worst %+.2f deg\n",
				dPitch, worst);
			worst = 0.0f;
		}
	}

	void UpdateVRPose()
	{
		InstallDirectCameraConstructionProbe();
		if (kNativeStateFollowEnabled)
			InstallNativeStateFollow();
		else {
			InstallUpstreamCameraBasisProbe();
			InstallDirectMovementHeading();
		}
		PollDirectCameraConstructionProbeHotkey();
		PollOwnershipProbeHotkeys();
		PollControlStateSamplerHotkeys();
		PollFlashlightTransitionProbe();
		vr::HmdMatrix34_t currentPose{};
		bool poseValid = false;
		if (IsOpenXRSelected()) {
			XrPosef pose{};
			if (sOpenXRRuntime.GetHeadPose(&pose)) {
				const float x = pose.orientation.x, y = pose.orientation.y;
				const float z = pose.orientation.z, w = pose.orientation.w;
				currentPose.m[0][0] = 1.0f - 2.0f * (y*y + z*z);
				currentPose.m[0][1] = 2.0f * (x*y - z*w);
				currentPose.m[0][2] = 2.0f * (x*z + y*w);
				currentPose.m[1][0] = 2.0f * (x*y + z*w);
				currentPose.m[1][1] = 1.0f - 2.0f * (x*x + z*z);
				currentPose.m[1][2] = 2.0f * (y*z - x*w);
				currentPose.m[2][0] = 2.0f * (x*z - y*w);
				currentPose.m[2][1] = 2.0f * (y*z + x*w);
				currentPose.m[2][2] = 1.0f - 2.0f * (x*x + y*y);
				currentPose.m[0][3] = pose.position.x;
				currentPose.m[1][3] = pose.position.y;
				currentPose.m[2][3] = pose.position.z;
				poseValid = true;
			}
		} else if (sVRSystem) {
			vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount];
			GetTrackedPosesForFrame(poses);
			const vr::TrackedDevicePose_t &hmdPose = poses[vr::k_unTrackedDeviceIndex_Hmd];
			if (hmdPose.bDeviceIsConnected && hmdPose.bPoseIsValid) {
				currentPose = hmdPose.mDeviceToAbsoluteTracking;
				poseValid = true;
			}
		}
		if (!poseValid) {
			G->vrHeadRotationValid = false;
			return;
		}

		float rawCurrent[9];
		ExtractRotation3x3(currentPose, rawCurrent);

		// Keep the full pose (not just the rotation we consume) so the
		// frame rendered from it can be submitted tagged with it - see
		// sEyeTexturePose.
		sLastPolledPose = currentPose;
		sLastPolledPoseValid = true;

		// Capture a reference orientation on first valid pose (or after
		// a re-init) rather than treating the HMD's raw absolute-world
		// orientation as the camera rotation outright - the absolute
		// orientation is tied to SteamVR's room calibration and has no
		// relationship to the game's own forward direction. What we
		// actually want is a DELTA: how far has the head turned since
		// tracking started, applied on top of whatever the game's own
		// view rotation already is (see PatchMappedVRCameraData).
		if (!G->vrReferenceRotationCaptured) {
			// LEVEL THE REFERENCE: keep its yaw, discard pitch and roll.
			//
			// Capturing the full HMD orientation meant the reference frame
			// inherited however the head happened to be tilted at that
			// instant. The delta below is expressed IN that frame, so its
			// up-axis was tilted too - and rotating about the real vertical
			// then resolves into a mix of yaw and ROLL. Live symptom: "my
			// view is tilted when I physically turn".
			//
			// This is a second, independent cause from the one the comment
			// below describes (the R_ref^T * R_cur ordering, already fixed) -
			// which is why that fix did not end the tilting.
			//
			// A level reference is also simply the correct one: the player's
			// head tilt at start-up is not meant to define which way is up.
			static const bool kLevelReferenceRotation = true;
			if (kLevelReferenceRotation) {
				// The HMD's forward is its -Z column; flatten it onto the
				// horizontal plane and rebuild a pure rotation about world up.
				const float fx = -rawCurrent[2], fz = -rawCurrent[8];
				const float len = sqrtf(fx * fx + fz * fz);
				if (len > 1e-4f) {
					const float nx = fx / len, nz = fz / len;
					const float s = -nx, c = -nz;   // Ry(theta): -Z = (-s, 0, -c)
					float lev[9] = {
						 c, 0.0f, s,
						 0.0f, 1.0f, 0.0f,
						-s, 0.0f, c,
					};
					memcpy(G->vrReferenceRotation3x3, lev, sizeof(lev));
					// Zero the orientation at capture without reverting to a full,
					// tilted reference. If A = Y_ref^T * R_current and T0 is A
					// at capture, A * T0^T starts at identity. For a later real-
					// world yaw, A = Y_delta * T0, so the same expression becomes
					// Y_delta rather than coupling yaw into roll.
					// STARTUP-TILT ZERO: OFF.
					//
					// The reasoning above is correct for yaw - A * T0^T does
					// reduce a real-world yaw to pure Y_delta - but it strips
					// the capture tilt PERMANENTLY, and that tilt includes the
					// player's genuine, ongoing head ROLL. The horizon is then
					// level only while the head matches the capture pose.
					//
					// Measured live at level load, head held level and still:
					//     gameRoll  = -0.00 deg   (Metro does NOT roll its camera)
					//     hmdRoll   = +3.3..+3.6  (this player's habitual head tilt)
					//     deltaRoll =  0.0..+0.35 (we were reporting "level")
					// so the view never counter-rotated and the world sat tilted
					// by exactly the head tilt. Recentring only re-captured the
					// same 3.5 deg, which is why it appeared to do almost nothing.
					//
					// Roll must always come from the headset absolutely: it is
					// the only correct source for which way is up. With the zero
					// removed, delta = Y_delta * T0 - yaw composed on top of the
					// head's real orientation relative to the LEVELLED reference,
					// which is the physically correct result.
					//
					// The yaw-coupling this was guarding against does not return:
					// that was caused by the non-levelled reference, which the
					// block above still fixes. Kept as a flag because the pitch
					// half of the same term may be wanted independently - zeroing
					// startup PITCH is defensible in a way zeroing roll is not.
					static const bool kZeroStartupTilt = false;
					if (kZeroStartupTilt) {
						float levT[9], initialTilt[9];
						Transpose3x3(lev, levT);
						Multiply3x3(levT, rawCurrent, initialTilt);
						Transpose3x3(initialTilt, sReferenceTiltInverse3x3);
					} else {
						const float identity[9] = { 1,0,0, 0,1,0, 0,0,1 };
						memcpy(sReferenceTiltInverse3x3, identity, sizeof(identity));
					}
					sReferenceTiltInverseValid = true;
					LogInfo("VRPose: captured reference HMD orientation "
						"(levelled yaw; startup-tilt zero %s)\n",
						kZeroStartupTilt ? "ON" : "OFF - roll passes through");
				} else {
					// Looking straight up or down: no usable heading, so keep
					// the raw orientation and let the next capture correct it.
					memcpy(G->vrReferenceRotation3x3, rawCurrent, sizeof(rawCurrent));
					const float identity[9] = { 1,0,0, 0,1,0, 0,0,1 };
					memcpy(sReferenceTiltInverse3x3, identity, sizeof(identity));
					sReferenceTiltInverseValid = true;
					LogInfo("VRPose: captured reference HMD orientation "
						"(raw - no horizontal heading available)\n");
				}
			} else {
				memcpy(G->vrReferenceRotation3x3, rawCurrent, sizeof(rawCurrent));
				const float identity[9] = { 1,0,0, 0,1,0, 0,0,1 };
				memcpy(sReferenceTiltInverse3x3, identity, sizeof(identity));
				sReferenceTiltInverseValid = true;
				LogInfo("VRPose: captured reference HMD orientation\n");
			}
			G->vrReferenceRotationCaptured = true;
			sHeadReferencePos[0] = currentPose.m[0][3];
			sHeadReferencePos[1] = currentPose.m[1][3];
			sHeadReferencePos[2] = currentPose.m[2][3];
			sHeadReferencePosValid = true;
		}

		// The delta must be expressed in the REFERENCE's own frame, not the
		// tracking world's: R_ref^T * R_cur, not R_cur * R_ref^T.
		//
		// PatchMappedVRCameraData composes this as a local rotation of the
		// camera, so it has to be a local rotation to begin with. The two
		// forms agree for pure yaw - both are rotations about the same up
		// axis, and those commute - which is why side-to-side always looked
		// right. They diverge as the player rotates away from the reference,
		// and the difference showed up as the view slowly tilting once you
		// had turned far enough.
		float refTranspose[9], levelFrameCurrent[9], rawDelta[9];
		Transpose3x3(G->vrReferenceRotation3x3, refTranspose);
		Multiply3x3(refTranspose, rawCurrent, levelFrameCurrent);
		if (sReferenceTiltInverseValid)
			Multiply3x3(levelFrameCurrent, sReferenceTiltInverse3x3, rawDelta);
		else
			memcpy(rawDelta, levelFrameCurrent, sizeof(rawDelta));

		ConvertOpenVRRotationToGameSpace(rawDelta, G->vrHeadRotation3x3);
		G->vrHeadRotationValid = true;

		// ROLL DIAGNOSTIC: which source is tilting the horizon?
		//
		// "The world starts tilted when I load a level" has two candidate
		// causes and they need opposite fixes:
		//
		//   1. sReferenceTiltInverse3x3 - the startup-tilt zero makes the
		//      view level AT CAPTURE by cancelling whatever tilt the headset
		//      had. If the reference is captured while the HMD is tilted
		//      (loading screen, headset being put on, resting on a desk),
		//      that tilt is baked in as "level" and holding the head normally
		//      then shows the world rolled by the opposite amount.
		//   2. Metro's OWN camera roll at level start (scripted intro, lean,
		//      character animation), which the composed view inherits.
		//
		// Measure of "how tilted is the horizon": the camera's RIGHT vector
		// leaving the horizontal plane. Level means right.y == 0, so
		// asin(right.y) is the roll directly, in the same convention for
		// both matrices, making them comparable.
		//
		// hmdRoll  = the player's ACTUAL physical head tilt.
		// deltaRoll = what we hand the game after levelling + tilt-zero.
		//
		// If hmdRoll is ~0 while deltaRoll is not, cause 1 (we introduced
		// it). If both are ~0 but the world still looks tilted, cause 2 -
		// see the matching "VRPose roll: game" line from
		// PatchMappedVRCameraData.
		static const bool kRollDiagnosticEnabled = false;
		if (kRollDiagnosticEnabled && (sFrameCounter++ % 90) == 0) {
			const float *r = G->vrHeadRotation3x3;
			const float kDeg = 57.29578f;
			const float hy = rawCurrent[3] < -1.0f ? -1.0f
				: (rawCurrent[3] > 1.0f ? 1.0f : rawCurrent[3]);
			const float dy = rawDelta[3] < -1.0f ? -1.0f
				: (rawDelta[3] > 1.0f ? 1.0f : rawDelta[3]);
			LogInfo("VRPose roll: frame=%u hmdRoll=%+7.2f deg deltaRoll=%+7.2f deg "
				"tiltZero=%d\n",
				G->frame_no, asinf(hy) * kDeg, asinf(dy) * kDeg,
				sReferenceTiltInverseValid ? 1 : 0);
			LogInfo("VRPose: delta rotation [%.4f %.4f %.4f / %.4f %.4f %.4f / %.4f %.4f %.4f]\n",
				r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[8]);
		}
	}

	void ComputeRigidInverse(const float R[9], const float T[3], float outR[9], float outT[3])
	{
		Transpose3x3(R, outR);
		for (int row = 0; row < 3; row++) {
			outT[row] = -(outR[row * 3 + 0] * T[0] + outR[row * 3 + 1] * T[1] + outR[row * 3 + 2] * T[2]);
		}
	}

	void Transpose3x3(const float R[9], float outR[9])
	{
		for (int row = 0; row < 3; row++) {
			for (int col = 0; col < 3; col++) {
				outR[row * 3 + col] = R[col * 3 + row];
			}
		}
	}

	void Multiply3x3(const float A[9], const float B[9], float outC[9])
	{
		for (int row = 0; row < 3; row++) {
			for (int col = 0; col < 3; col++) {
				float sum = 0.0f;
				for (int k = 0; k < 3; k++)
					sum += A[row * 3 + k] * B[k * 3 + col];
				outC[row * 3 + col] = sum;
			}
		}
	}

	void Multiply3x4Affine(const float A[12], const float B[12], float outC[12])
	{
		// Both A and B are 3x4 with an implicit 4th row [0,0,0,1].
		// Result is also affine (implicit last row stays [0,0,0,1]),
		// so only the 3x4 part needs to be computed/stored.
		for (int row = 0; row < 3; row++) {
			for (int col = 0; col < 4; col++) {
				float sum = 0.0f;
				for (int k = 0; k < 3; k++)
					sum += A[row * 4 + k] * B[k * 4 + col];
				if (col == 3)
					sum += A[row * 4 + 3]; // B's implicit row 3 = [0,0,0,1]
				outC[row * 4 + col] = sum;
			}
		}
	}

	void Multiply4x4Affine3x4(const float P[16], const float A[12], float outC[16])
	{
		// P is a general 4x4 (e.g. projection - not affine). A is 3x4
		// with an implicit 4th row [0,0,0,1]. Result is general 4x4.
		for (int row = 0; row < 4; row++) {
			for (int col = 0; col < 4; col++) {
				float sum = 0.0f;
				for (int k = 0; k < 3; k++)
					sum += P[row * 4 + k] * A[k * 4 + col];
				if (col == 3)
					sum += P[row * 4 + 3]; // A's implicit row 3 = [0,0,0,1]
				outC[row * 4 + col] = sum;
			}
		}
	}

	void Multiply4x4(const float A[16], const float B[16], float outC[16])
	{
		for (int row = 0; row < 4; row++) {
			for (int col = 0; col < 4; col++) {
				float sum = 0.0f;
				for (int k = 0; k < 4; k++)
					sum += A[row * 4 + k] * B[k * 4 + col];
				outC[row * 4 + col] = sum;
			}
		}
	}

	void Promote3x4To4x4(const float A[12], float out[16])
	{
		for (int i = 0; i < 12; i++)
			out[i] = A[i];
		out[12] = out[13] = out[14] = 0.0f;
		out[15] = 1.0f;
	}

	bool Invert4x4(const float m[16], float outM[16])
	{
		float inv[16];

		inv[0]  =  m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15]
		         + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
		inv[4]  = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15]
		         - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
		inv[8]  =  m[4]*m[9]*m[15]  - m[4]*m[11]*m[13] - m[8]*m[5]*m[15]
		         + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
		inv[12] = -m[4]*m[9]*m[14]  + m[4]*m[10]*m[13] + m[8]*m[5]*m[14]
		         - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];

		inv[1]  = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15]
		         - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
		inv[5]  =  m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15]
		         + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
		inv[9]  = -m[0]*m[9]*m[15]  + m[0]*m[11]*m[13] + m[8]*m[1]*m[15]
		         - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
		inv[13] =  m[0]*m[9]*m[14]  - m[0]*m[10]*m[13] - m[8]*m[1]*m[14]
		         + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];

		inv[2]  =  m[1]*m[6]*m[15]  - m[1]*m[7]*m[14]  - m[5]*m[2]*m[15]
		         + m[5]*m[3]*m[14] + m[13]*m[2]*m[7]  - m[13]*m[3]*m[6];
		inv[6]  = -m[0]*m[6]*m[15]  + m[0]*m[7]*m[14]  + m[4]*m[2]*m[15]
		         - m[4]*m[3]*m[14] - m[12]*m[2]*m[7]  + m[12]*m[3]*m[6];
		inv[10] =  m[0]*m[5]*m[15]  - m[0]*m[7]*m[13]  - m[4]*m[1]*m[15]
		         + m[4]*m[3]*m[13] + m[12]*m[1]*m[7]  - m[12]*m[3]*m[5];
		inv[14] = -m[0]*m[5]*m[14]  + m[0]*m[6]*m[13]  + m[4]*m[1]*m[14]
		         - m[4]*m[2]*m[13] - m[12]*m[1]*m[6]  + m[12]*m[2]*m[5];

		inv[3]  = -m[1]*m[6]*m[11]  + m[1]*m[7]*m[10]  + m[5]*m[2]*m[11]
		         - m[5]*m[3]*m[10] - m[9]*m[2]*m[7]   + m[9]*m[3]*m[6];
		inv[7]  =  m[0]*m[6]*m[11]  - m[0]*m[7]*m[10]  - m[4]*m[2]*m[11]
		         + m[4]*m[3]*m[10] + m[8]*m[2]*m[7]   - m[8]*m[3]*m[6];
		inv[11] = -m[0]*m[5]*m[11]  + m[0]*m[7]*m[9]   + m[4]*m[1]*m[11]
		         - m[4]*m[3]*m[9]  - m[8]*m[1]*m[7]   + m[8]*m[3]*m[5];
		inv[15] =  m[0]*m[5]*m[10]  - m[0]*m[6]*m[9]   - m[4]*m[1]*m[10]
		         + m[4]*m[2]*m[9]  + m[8]*m[1]*m[6]   - m[8]*m[2]*m[5];

		float det = m[0]*inv[0] + m[1]*inv[4] + m[2]*inv[8] + m[3]*inv[12];
		if (fabsf(det) < 1e-20f)
			return false;

		det = 1.0f / det;
		for (int i = 0; i < 16; i++)
			outM[i] = inv[i] * det;
		return true;
	}

	// Bumped whenever an input layout is added to one of the classification
	// sets below, so per-thread query caches know to look again.
	static volatile LONG sLayoutSetGeneration = 0;

	void RegisterKnownNonMatrixInputLayout(void *inputLayout)
	{
		// D3D11 allows resource/state-object creation (including
		// CreateInputLayout, which calls this) from any thread, while
		// IASetVertexBuffers/IsKnownNonMatrixInputLayout below always
		// run on the single immediate-context render thread - a
		// std::unordered_set touched from both without synchronization
		// is a real data race, not just a hypothetical one, so this
		// reuses the same G->mCriticalSection pattern already used
		// elsewhere in HackerContext.cpp.
		EnterCriticalSectionPretty(&G->mCriticalSection);
		sKnownNonMatrixInputLayouts.insert(inputLayout);
		LeaveCriticalSection(&G->mCriticalSection);
		InterlockedIncrement(&sLayoutSetGeneration);
	}

	bool IsKnownNonMatrixInputLayout(void *inputLayout)
	{
		// Asked on nearly every draw by the render thread, while the set only
		// grows at input-layout creation. Answer from a per-thread last-query
		// cache and take 3Dmigoto's global lock only when the layout or the set
		// has changed since.
		thread_local void *tLastLayout = (void *)~(uintptr_t)0;
		thread_local LONG tLastGeneration = -1;
		thread_local bool tLastFound = false;
		const LONG generation = InterlockedCompareExchange(&sLayoutSetGeneration, 0, 0);
		if (inputLayout == tLastLayout && generation == tLastGeneration)
			return tLastFound;
		EnterCriticalSectionPretty(&G->mCriticalSection);
		bool found = sKnownNonMatrixInputLayouts.count(inputLayout) != 0;
		LeaveCriticalSection(&G->mCriticalSection);
		tLastLayout = inputLayout;
		tLastGeneration = generation;
		tLastFound = found;
		return found;
	}

	static std::unordered_set<void *> sInstanceMatrixLayouts;

	void RegisterInstanceMatrixInputLayout(void *inputLayout)
	{
		if (inputLayout)
			sInstanceMatrixLayouts.insert(inputLayout);
	}

	bool IsInstanceMatrixInputLayout(void *inputLayout)
	{
		return inputLayout && sInstanceMatrixLayouts.count(inputLayout) != 0;
	}

	static std::unordered_set<void *> sSkinnedLayouts;

	void RegisterSkinnedInputLayout(void *inputLayout)
	{
		if (!inputLayout)
			return;
		EnsureScanLock();
		EnterCriticalSection(&sScanLock);
		sSkinnedLayouts.insert(inputLayout);
		LeaveCriticalSection(&sScanLock);
		InterlockedIncrement(&sLayoutSetGeneration);
	}

	bool IsSkinnedInputLayout(void *inputLayout)
	{
		if (!inputLayout)
			return false;
		// Same per-thread last-query cache as IsKnownNonMatrixInputLayout.
		thread_local void *tLastLayout = (void *)~(uintptr_t)0;
		thread_local LONG tLastGeneration = -1;
		thread_local bool tLastFound = false;
		const LONG generation = InterlockedCompareExchange(&sLayoutSetGeneration, 0, 0);
		if (inputLayout == tLastLayout && generation == tLastGeneration)
			return tLastFound;
		EnsureScanLock();
		EnterCriticalSection(&sScanLock);
		bool found = sSkinnedLayouts.count(inputLayout) != 0;
		LeaveCriticalSection(&sScanLock);
		tLastLayout = inputLayout;
		tLastGeneration = generation;
		tLastFound = found;
		return found;
	}

	void RegisterKnownWeaponXformInputLayout(void *inputLayout)
	{
		EnterCriticalSectionPretty(&G->mCriticalSection);
		sKnownWeaponXformInputLayouts.insert(inputLayout);
		LeaveCriticalSection(&G->mCriticalSection);
	}

	bool IsKnownWeaponXformInputLayout(void *inputLayout)
	{
		EnterCriticalSectionPretty(&G->mCriticalSection);
		bool found = sKnownWeaponXformInputLayouts.count(inputLayout) != 0;
		LeaveCriticalSection(&G->mCriticalSection);
		return found;
	}

	void EnsureStereoParams(const float gameProjection4x4[16])
	{
		if (!sVRSystem)
			return;

		// Stereo Separation is a live eye-position scale; offsets are rebuilt from
		// OpenVR (never scaled incrementally). 63.5 mm is the menu's neutral value.
		const VRMenu::Settings &menu = VRMenu::GetSettings();
		const float separationScale = max(0.1f, min(3.0f,
			menu.stereoSeparationMm / 63.5f));
		// This runs for every main camera write, twice per write (both eyes),
		// up to a few hundred times a frame; the eye-to-head transform only
		// changes when the headset's IPD does. Refresh once per frame or when
		// the slider moves - an IPD change reaches the image a frame later.
		static unsigned sOffsetsFrame = 0xFFFFFFFF;
		static float sOffsetsScale = -1.0f;
		if (sOffsetsFrame != G->frame_no || sOffsetsScale != separationScale) {
			sOffsetsFrame = G->frame_no;
			sOffsetsScale = separationScale;
			for (int eye = 0; eye < 2; ++eye) {
				const vr::EVREye vrEye = (eye == 0) ? vr::Eye_Left : vr::Eye_Right;
				const vr::HmdMatrix34_t eyeToHead = sVRSystem->GetEyeToHeadTransform(vrEye);
				G->vrEyeOffsetView[eye][0] = eyeToHead.m[0][3] * kMetresToGameUnits * separationScale;
				G->vrEyeOffsetView[eye][1] = eyeToHead.m[1][3] * kMetresToGameUnits * separationScale;
				G->vrEyeOffsetView[eye][2] = -eyeToHead.m[2][3] * kMetresToGameUnits * separationScale;
			}
		}

		// The eye frustum TANGENTS are fixed hardware properties, but the
		// game's clip planes are not - it adjusts near/far per scene
		// (0.1/40.1 in one capture, 0.1/250 in another). The first
		// version recovered them once and froze them, which clipped
		// everything beyond that stale far plane: distant geometry
		// flickered or refused to render until approached. So rebuild
		// whenever the game's planes actually change, and only skip the
		// work when they haven't.
		static float sBuiltNear = 0.0f;
		static float sBuiltFar = 0.0f;

		// Recover the game's own clip planes from its projection so the
		// eye matrices we substitute keep depth behaving identically -
		// getting near/far wrong would break z-fighting and fog even if
		// the FOV were perfect. The game's matrix (dumped from a real
		// capture, see Notes/13) is a standard left-handed D3D
		// projection in column-vector convention:
		//
		//   [ 2n/(r-l)   0          -(r+l)/(r-l)   0          ]
		//   [ 0          2n/(t-b)   -(t+b)/(t-b)   0          ]
		//   [ 0          0           f/(f-n)      -n*f/(f-n)  ]
		//   [ 0          0           1             0          ]
		//
		// so row 3 = [0,0,1,0] confirms +Z forward, and:
		//   P22 = f/(f-n), P23 = -n*f/(f-n)
		//   => n = -P23/P22,  f = P22*n/(P22-1)
		const float P22 = gameProjection4x4[10];
		const float P23 = gameProjection4x4[11];

		// Validate hard, and do NOT latch until the numbers are sane.
		// The first version accepted whatever the first cb1 write
		// contained and set vrStereoParamsValid immediately - but menus
		// and loading screens write degenerate/uninitialised
		// projections, and one of those poisoned the eye matrices with
		// NaN for the whole session (the game rendered as a black void
		// with a growing white smear).
		//
		// NaN is the specific trap: every comparison against NaN is
		// false, so a naive "P22 <= 1.0f" range check passes it
		// straight through. isfinite() first, then range.
		bool plausible = std::isfinite(P22) && std::isfinite(P23) && P22 > 1.0f;
		float nearZ = 0.0f, farZ = 0.0f;
		if (plausible) {
			nearZ = -P23 / P22;
			farZ = P22 * nearZ / (P22 - 1.0f);
			plausible = std::isfinite(nearZ) && std::isfinite(farZ) &&
				nearZ > 1e-4f && farZ > nearZ && farZ < 1e6f;
		}
		if (!plausible) {
			// Retry on a later frame rather than giving up: this is
			// expected during menus/loading, and the real gameplay
			// projection shows up soon after. Anything already built
			// stays in use meanwhile.
			static int sRejectLogCount = 0;
			if (sRejectLogCount < 5) {
				LogInfo("VRPose stereo: ignoring implausible projection (P22=%f P23=%f -> near=%f far=%f); "
					"will retry on a later frame\n", P22, P23, nearZ, farZ);
				sRejectLogCount++;
			}
			return;
		}

		// PULL THE NEAR PLANE IN.
		//
		// The game ships a 0.1 m near plane, which is fine on a monitor where
		// nothing ever gets that close to the camera. In VR the player brings
		// the weapon to their face to use the sights, and everything within
		// 10 cm of the eye is clipped away - the gun's rear is sliced off and
		// what remains appears pinned at a fixed distance. It reads as a wall:
		// "something is blocking me from bringing it back", starting a short
		// way in. Measured: the controller pose keeps moving through its full
		// range with trackingResult=200 the whole time, so nothing upstream is
		// clamping and nothing in our transform does either.
		//
		// The FOV is unaffected. The frustum edges come from the headset's
		// tangents multiplied by near, so P[0] = 2n/(r-l) reduces to
		// 2/(rawR-rawL) - the near term cancels. Only the clip distance and the
		// depth mapping change.
		//
		// The cost is depth precision. With far = 40 the ratio is still only
		// ~1300:1 here, which a 24-bit buffer carries comfortably; if distant
		// z-fighting ever shows up, this is the first thing to put back.
		// REVERTED to the game's own value. 0.03 caused heavy flickering -
		// depth precision, as predicted - and did not help, because the weapon
		// was never being clipped: it tracks the hand 1:1 across its whole
		// range (measured: 42 cm of hand travel, 42 cm of weapon travel). The
		// near plane was not the wall.
		static const float kVRNearPlane = 0.0f;
		if (kVRNearPlane > 0.0f && nearZ > kVRNearPlane) {
			static bool logged = false;
			if (!logged) {
				logged = true;
				LogInfo("VRPose stereo: pulling near plane in from %.4f to %.4f so the "
					"weapon can be brought to the face\n", nearZ, kVRNearPlane);
			}
			nearZ = kVRNearPlane;
		}

		// Already built for exactly these clip planes - nothing to do.
		if (G->vrStereoParamsValid && nearZ == sBuiltNear && farZ == sBuiltFar)
			return;

		const bool firstBuild = !G->vrStereoParamsValid;

		for (int eye = 0; eye < 2; eye++) {
			vr::EVREye vrEye = (eye == 0) ? vr::Eye_Left : vr::Eye_Right;

			// GetProjectionRaw returns the frustum edges as TANGENTS of
			// the half-angles (i.e. extents at unit distance), in
			// OpenVR's right-handed -Z-forward space. Multiplying by our
			// near plane converts them to near-plane extents, which is
			// what the matrix form above wants.
			float rawL = 0.0f, rawR = 0.0f, rawT = 0.0f, rawB = 0.0f;
			sVRSystem->GetProjectionRaw(vrEye, &rawL, &rawR, &rawT, &rawB);

			float l = rawL * nearZ;
			float r = rawR * nearZ;
			// Y: OpenVR is +Y up like the game, but which of top/bottom
			// comes back negative is a well-known inconsistency between
			// runtimes. Rather than assume, normalise so that t > b and
			// let the matrix construction below be sign-agnostic.
			float t = rawT * nearZ;
			float b = rawB * nearZ;
			if (t < b) {
				float tmp = t;
				t = b;
				b = tmp;
			}

			// Degenerate frustum guard - if the runtime ever hands back
			// equal edges this would divide by zero and poison the
			// matrix, which is precisely the failure mode being fixed
			// here, so refuse rather than propagate it.
			if (!(fabsf(r - l) > 1e-6f) || !(fabsf(t - b) > 1e-6f)) {
				LogInfo("VRPose stereo: eye %d has a degenerate frustum (l=%f r=%f t=%f b=%f); "
					"stereo not enabled\n", eye, l, r, t, b);
				return;
			}

			float *P = G->vrEyeProjection4x4[eye];
			memset(P, 0, sizeof(float) * 16);
			// Frustum-centre offset. The negation is CORRECT and was
			// briefly removed on a wrong hunch - recording the derivation
			// so it does not get "fixed" again.
			//
			// A point at depth z hits the near plane at x*n/z. Mapping
			// [l,r] to [-1,1] gives
			//   ndc = (2*n*x/z - (r+l)) / (r-l)
			//       = (2n/(r-l)) * (x/z)  -  (r+l)/(r-l)
			// so in the form ndc*z = P[0]*x + P[2]*z, P[2] is MINUS
			// (r+l)/(r-l). Check: this maps the frustum's centre tangent
			// (r+l)/2n to ndc 0, which is what "centre" means.
			P[0]  = 2.0f * nearZ / (r - l);
			P[2]  = -(r + l) / (r - l);
			P[5]  = 2.0f * nearZ / (t - b);
			P[6]  = -(t + b) / (t - b);
			P[10] = farZ / (farZ - nearZ);
			P[11] = -nearZ * farZ / (farZ - nearZ);
			P[14] = 1.0f;

			// Inverse of the projection above. For the form
			//   [ A 0 C 0 / 0 B D 0 / 0 0 E F / 0 0 1 0 ]
			// the inverse is
			//   [ 1/A 0 0 -C/A / 0 1/B 0 -D/B / 0 0 0 1 / 0 0 1/F -E/F ]
			// Verified against the game's own m_iP from a real capture:
			// feeding its A/B/E/F through this reproduces the matrix it
			// ships, to rounding.
			const float A = P[0], B = P[5], C = P[2], D = P[6], E = P[10], F = P[11];
			float *iP = G->vrEyeInvProjection4x4[eye];
			memset(iP, 0, sizeof(float) * 16);
			iP[0]  = 1.0f / A;
			iP[3]  = -C / A;
			iP[5]  = 1.0f / B;
			iP[7]  = -D / B;
			iP[11] = 1.0f;
			iP[14] = 1.0f / F;
			iP[15] = -E / F;

			for (int i = 0; i < 16; i++) {
				if (!std::isfinite(P[i]) || !std::isfinite(iP[i])) {
					LogInfo("VRPose stereo: eye %d projection element %d is not finite; stereo not enabled\n", eye, i);
					return;
				}
			}

			// Eye offset: GetEyeToHeadTransform's translation is where
			// this eye sits relative to the head, in metres. Only X is
			// meaningfully non-zero (half the IPD). Z is negated going
			// from OpenVR's -Z-forward to the game's +Z-forward.
			// TEMPORARY diagnostic: exaggerate the interpupillary distance.
			//
			// The world fuses correctly while some props and NPCs double,
			// and every measurable thing about the matrices is right - so
			// the question is whether those objects respond to the eye
			// offset AT ALL, or whether their transform reaches the shader
			// by a route that never sees it (baked into vertex or instance
			// data, the way the viewmodel's does).
			//
			// At several times the real IPD the answer is unmistakable by
			// eye: anything driven by the constant buffers will swing wildly
			// between the two eyes, and anything ignoring them will sit
			// perfectly still. That partitions the scene by mechanism rather
			// than by guesswork. Set back to 1.0 once answered.
			// Answered: at 4x the world shrank (correct hyperstereo "dollhouse"
		// scaling, so scene geometry genuinely responds to the eye offset)
		// while the doubling props and NPCs did not move at all. Those reach
		// the shader by a route that never sees a per-eye matrix. Back to
		// life-size; the partition it revealed is the next piece of work.
		static const float kIpdExaggeration = 1.0f;

			vr::HmdMatrix34_t eyeToHead = sVRSystem->GetEyeToHeadTransform(vrEye);
			G->vrEyeOffsetView[eye][0] = eyeToHead.m[0][3] * kMetresToGameUnits * kIpdExaggeration * separationScale;
			G->vrEyeOffsetView[eye][1] = eyeToHead.m[1][3] * kMetresToGameUnits * kIpdExaggeration * separationScale;
			G->vrEyeOffsetView[eye][2] = -eyeToHead.m[2][3] * kMetresToGameUnits * kIpdExaggeration * separationScale;

			// Only log the full derivation on the first build - after
			// that this runs whenever the game changes its clip planes,
			// which would otherwise flood the log.
			if (firstBuild) {
				LogInfo("VRPose stereo: eye %d raw tangents L=%.4f R=%.4f T=%.4f B=%.4f -> "
					"near-plane l=%.4f r=%.4f t=%.4f b=%.4f; offset=(%.4f %.4f %.4f)\n",
					eye, rawL, rawR, rawT, rawB, l, r, t, b,
					G->vrEyeOffsetView[eye][0], G->vrEyeOffsetView[eye][1], G->vrEyeOffsetView[eye][2]);
				LogInfo("VRPose stereo: eye %d projection [%.4f %.4f %.4f %.4f / %.4f %.4f %.4f %.4f / "
					"%.4f %.4f %.4f %.4f / %.4f %.4f %.4f %.4f]\n", eye,
					P[0], P[1], P[2], P[3], P[4], P[5], P[6], P[7],
					P[8], P[9], P[10], P[11], P[12], P[13], P[14], P[15]);
			}
		}

		static int sRebuildLogCount = 0;
		if (firstBuild || sRebuildLogCount < 10) {
			LogInfo("VRPose stereo: %s eye projections for game clip planes near=%.4f far=%.4f\n",
				firstBuild ? "built" : "REBUILT (game changed its clip planes)", nearZ, farZ);
			if (!firstBuild)
				sRebuildLogCount++;
		}

		sBuiltNear = nearZ;
		sBuiltFar = farZ;
		G->vrStereoParamsValid = true;
	}

	void AdvanceStereoEye()
	{
		// True stereo renders both eyes every frame, so there is no eye to
		// advance - and flipping anyway would swing the camera by a full
		// IPD from frame to frame while both passes still used the same
		// matrices, which reads as the whole world shaking sideways.
		if (StereoTwin::gDoubleDraw) {
			G->vrCurrentEye = 0;
			return;
		}
		G->vrCurrentEye ^= 1;
	}

	void NotifyCameraPosition(const float worldPos[3])
	{
		EnsureScanLock();
		EnterCriticalSection(&sScanLock);
		sCameraWorldPos[0] = worldPos[0];
		sCameraWorldPos[1] = worldPos[1];
		sCameraWorldPos[2] = worldPos[2];
		sCameraWorldPosValid = true;
		LeaveCriticalSection(&sScanLock);
	}

	void NotifyViewAngles(float yawRadians, float pitchRadians)
	{
		EnsureScanLock();
		EnterCriticalSection(&sScanLock);
		sViewYaw = yawRadians;
		sViewPitch = pitchRadians;
		sViewAnglesValid = true;
		LeaveCriticalSection(&sScanLock);
	}

	void NotifyViewForward(const float forward[3])
	{
		EnsureScanLock();
		EnterCriticalSection(&sScanLock);
		sViewForward[0] = forward[0];
		sViewForward[1] = forward[1];
		sViewForward[2] = forward[2];
		sViewForwardValid = true;
		LeaveCriticalSection(&sScanLock);
	}

	void NotifyViewRotation(const float R[9])
	{
		EnsureScanLock();
		EnterCriticalSection(&sScanLock);
		memcpy(sViewRotation, R, sizeof(float) * 9);
		sViewRotationValid = true;
		LeaveCriticalSection(&sScanLock);
	}

	void RunInputInjectionTest()
	{
		// One-shot. Waits for gameplay (the game's view angles only become
		// available once the camera buffer is being written), settles for a
		// moment, then injects a steady rightward look for a few seconds
		// while watching the game's own yaw.
		// Answered: injection moved the game's yaw 98.5 degrees, so the
		// engine accepts synthetic input. Left in place for reference but
		// disabled - it would fight the aiming loop that now uses the same
		// mechanism.
		static const bool kInjectionTestEnabled = false;
		if (!kInjectionTestEnabled)
			return;

		enum Phase { WaitingForGameplay, Settling, Injecting, Done };
		static Phase phase = WaitingForGameplay;
		static int frames = 0;
		static float yawAtStart = 0.0f;
		static float yawBeforeSettle = 0.0f;

		if (phase == Done)
			return;

		float yaw = 0.0f, pitch = 0.0f;
		bool valid = false;
		GetViewAngleSnapshot(&yaw, &pitch, &valid);
		if (!valid)
			return;

		switch (phase) {
		case WaitingForGameplay:
			// Give the level a few seconds to settle before touching
			// anything, so we aren't fighting a loading screen.
			if (++frames > 600) {
				yawBeforeSettle = yaw;
				frames = 0;
				phase = Settling;
			}
			break;

		case Settling:
			// Confirm the player is roughly still first, otherwise their
			// own stick input would be indistinguishable from ours.
			if (++frames > 120) {
				yawAtStart = yaw;
				frames = 0;
				phase = Injecting;
				LogInfo("VRPose input: starting injection test - game yaw is %.4f rad. "
					"Injecting synthetic mouse movement for ~3 seconds; do not touch the stick.\n", yaw);
			}
			break;

		case Injecting: {
			// Relative mouse movement. SendInput injects at the same level
			// as real hardware, so games reading raw input see it too -
			// which matters here, since many shooters use raw input and
			// would ignore higher-level synthesised messages.
			INPUT in;
			memset(&in, 0, sizeof(in));
			in.type = INPUT_MOUSE;
			in.mi.dwFlags = MOUSEEVENTF_MOVE;
			in.mi.dx = 6;
			in.mi.dy = 0;
			SendInput(1, &in, sizeof(INPUT));

			if (++frames > 180) {
				float delta = yaw - yawAtStart;
				// Unwrap across the +/-pi seam so a wrap doesn't read as a
				// huge change.
				while (delta > 3.14159265f) delta -= 6.28318531f;
				while (delta < -3.14159265f) delta += 6.28318531f;

				LogInfo("VRPose input: injection finished. Game yaw moved %.4f rad (%.1f deg) "
					"over ~3s of synthetic mouse input.\n", delta, delta * 57.2957795f);
				if (fabsf(delta) > 0.05f) {
					LogInfo("VRPose input: RESULT = INJECTION WORKS. The game responds to synthetic input, "
						"so motion aiming is achievable by steering its aim toward the controller "
						"direction - no memory addresses required.\n");
				} else {
					LogInfo("VRPose input: RESULT = NO RESPONSE. The game ignored synthetic mouse input "
						"(likely controller-only input handling, or it filters injected events). "
						"Motion aiming would need either XInput emulation via a virtual pad, or "
						"writing the aim state directly - back to memory work.\n");
				}
				phase = Done;
			}
			break;
		}

		default:
			break;
		}
	}

	// -----------------------------------------------------------------
	// VR controller input: movement, turning, and actions.
	//
	// Same mechanism as the aim injection - synthetic input via SendInput,
	// which the engine accepts (proven by the aim work). Keyboard events use
	// SCANCODES rather than virtual key codes because games that read raw
	// input or DirectInput see scancodes; a VK-only event is invisible to
	// them.
	//
	// The turn injection deliberately does NOT go through the aim loop's
	// book-keeping: turning is the player rotating their body, so it should
	// move the view, and the cancellation must leave it alone. Because the
	// aim loop commands only CHANGES to the controller's offset - never an
	// absolute game yaw - it ignores this rotation rather than fighting it.

	// Turn input injected this frame, in radians, handed to UpdateMotionAiming
	// so it can credit the rotation to the player's body rather than mistaking
	// it for aim. Consumed (zeroed) once read.
	static float sPendingTurnYaw = 0.0f;

	// Direct body turning retains Metro's native mouse-look processing, but
	// bypasses Windows input/device arbitration. Only native mode uses it.
	typedef bool (__fastcall *tNativeBodyLookInput)(void *, float, float, int);
	static tNativeBodyLookInput sNativeBodyLookInput = NULL;
	static SRWLOCK sNativeBodyTurnLock = SRWLOCK_INIT;
	static NativeBodyTurn::Intent sNativeBodyTurnIntent;
	static void ClearNativeBodyTurnIntent()
	{
		AcquireSRWLockExclusive(&sNativeBodyTurnLock);
		sNativeBodyTurnIntent.Clear();
		ReleaseSRWLockExclusive(&sNativeBodyTurnLock);
	}
	static int TakeNativeBodyTurnIntent()
	{
		AcquireSRWLockExclusive(&sNativeBodyTurnLock);
		const int units = sNativeBodyTurnIntent.Take();
		ReleaseSRWLockExclusive(&sNativeBodyTurnLock);
		return units;
	}
	struct NativeBodyTurnInputScope {
		bool published;
		NativeBodyTurnInputScope() : published(false) {}
		~NativeBodyTurnInputScope() { if (!published) ClearNativeBodyTurnIntent(); }
		void Publish(int units, bool held, bool snap) {
			published = true;
			AcquireSRWLockExclusive(&sNativeBodyTurnLock);
			sNativeBodyTurnIntent.Publish(units, held, snap);
			ReleaseSRWLockExclusive(&sNativeBodyTurnLock);
		}
	};

	void RequestEyeDump();   // defined further down, next to the dump itself

	// Whether the trigger is held right now, published so the renderer can tell
	// shot-time draws from ordinary ones. The muzzle flash only exists while
	// firing, so "what is drawn now that was not drawn a moment ago" isolates
	// it without needing to recognise it.
	static volatile LONG sFiringNow = 0;

	// A shot produces a short-lived vertical change in Metro's camera.  That
	// change is expected recoil, not a scripted camera handover.  The culling
	// follow loop uses this deadline to keep its scripted-camera classifier
	// from mistaking recoil for an external camera owner.  Automatic fire
	// refreshes the deadline for every shot.
	static volatile LONG sRecoilPitchIgnoreUntilFrame = -1;
	static const LONG kRecoilPitchIgnoreFrames = 18; // ~250 ms at 72 Hz

	// Whether the player is holding aim-down-sights. Published so the renderer
	// can tell whether the engine's ADS animation is what is moving the
	// viewmodel - which decides whether the movement can be cancelled while
	// keeping the recoil and sway suppression that comes with it.
	static volatile LONG sAimingNow = 0;
	// Thousandths of the magnification requested by a positively identified
	// optic. Integral hand-off keeps renderer/input thread access atomic.
	static volatile LONG sRequestedScopeScaleMilli = 1000;
	// Physical gamepad LT state, separate from the motion-controller gesture.
	static volatile LONG sGamepadAiming = 0;
	// Captured on the physical left-grip press edge. The weapon render path may
	// consume this permission while that same press remains held, but entering
	// the weapon box later cannot create permission retroactively.
	static volatile LONG sTwoHandGripAcquireAuthorized = 0;
	static volatile LONG sControlProbeLabel = 0; // 1=ordinary, 2=scripted
	static const bool kReverseEngineeringDiagnosticsEnabled = false;
	// Retired camera-side probes. The only remaining ownership diagnostic is
	// the ground-truth writable-state sampler driven by F7/F8 below.
	static const bool kRetiredCameraProbesEnabled = false;
	static volatile PVOID sLiveInputDispatcherAddress = NULL;
	static volatile PVOID sLiveKeyboardHookCallbackAddress = NULL;
	static volatile PVOID sLiveKeyboardGateA = NULL;
	static volatile PVOID sLiveKeyboardGateB = NULL;
	// Live objects learned from the fire wrapper. Used only by the temporary
	// native-ADS state diff below to discover which gameplay state changes
	// independently of the visible viewmodel animation.
	static volatile PVOID sADSProbeFireOwner = NULL;
	static volatile PVOID sADSProbeService = NULL;
	static volatile PVOID sADSProbeWeapon = NULL;
	static unsigned char sADSProbeOwnerBefore[0x1000] = {};
	static unsigned char sADSProbeServiceBefore[0x1000] = {};
	static unsigned char sADSProbeWeaponBefore[0x1000] = {};
	static volatile PVOID sPneumaticDiffWeapon = NULL;
	static volatile PVOID sPneumaticDiffService = NULL;
	static volatile PVOID sPneumaticDiffPlayer = NULL;
	static unsigned char sPneumaticWeaponBefore[0x3000] = {};
	static unsigned char sPneumaticServiceBefore[0x1000] = {};
	static unsigned char sPneumaticPlayerBefore[0x4000] = {};
	struct PneumaticPointerCandidate {
		SIZE_T playerOffset;
		void *object;
		std::vector<unsigned char> before;
	};
	static std::vector<PneumaticPointerCandidate> sPneumaticPointerCandidates;
	static volatile LONG sADSProbeCountdown = 0;
	static volatile LONG sADSProbeTargetState = 0;

	// Defined further down, next to the other controller-pose helpers.
	static bool IsWeaponNearFace();
	static bool IsScopeNearEye();
	static bool IsLeftHandNearHead();
	static bool IsLeftHandBesideHead();
	static bool IsLeftHandPulledAwayFromHead();
	static bool IsLeftHandNearWaist();
	static bool IsLeftHandNearFrontWaist();
	static bool GetLeftHandBodyLocalMeters(float out[3]);
	static bool IsLeftHandNearChest();
	static bool IsLeftHandNearMouth();
	static bool GetLeftHandHeadLocalMeters(float out[3]);
	// Two-hand aiming is allowed only while the support hand is physically
	// near the weapon.  This leaves left grip available for gestures when the
	// hand is away from the gun instead of making the button globally reserved.
	static bool IsLeftHandNearWeapon();
	static void InstallNativeFlashlightActionHook();
	static void InstallNativePneumaticActionProbe();
	static void InstallPneumaticTranslatedActionHook();
	static void InstallPneumaticPlayerListenerProbe();
	static DWORD WINAPI PneumaticStateDiffThread(void *);
	static void InstallPneumaticListenerDispatchProbe();
	static void InstallGasMaskNativeDispatchHook();
	static void InstallGasMaskToggleCallbackHook();
	static void InstallGasMaskPlayerActionHook();
	static void InstallNativeFlashlightInputManagerHook();
	static void InstallGasMaskTransitionProbe();
	static void InstallGasMaskClassProbe();
	static void UpdateNativeFlashlightGesture(bool leftGrip);
	static bool UpdateNativeChargerGesture(bool leftGrip);
	static bool UpdateNativeGasMaskGesture(bool leftGrip);
	static bool UpdateNativeFilterGesture(bool leftGrip);
	static bool InvokeNativeFilterReplacement();
	static bool ResolveMetroPlayer(void **playerOut);
	static bool QueryNativeChargerOut(void *player, bool *out,
		unsigned *equipmentStateOut = NULL);
	static bool QueryNativeGasMaskWorn(void *player, bool *wornOut,
		unsigned *equipmentStateOut = NULL);
	static void InstallNativeFilterReplacementHook();
	static void InstallNativeGasMaskDirectionProbe();
	static void DumpNativeGasMaskEquipmentServiceOnce();
	static void InstallNativeGasMaskEquipmentStateProbe();
	static bool ResolveMetroPlayerTorch(void **playerOut, void **torchOut);
	static void PollGasMaskActionStateDiff();
	static void PollGasMaskStableReversibleProbe();
	static void PollGasMaskUnknownPostActionCapture();
	static void DumpUnknownGasMaskState(void *player);
	// Clears the gameplay-only native ADS flag when the gun leaves the eye.
	// The weapon pointer itself is learned by the native dispersion hook.
	static void UpdateGameplayADSFlag(bool aiming);

	static void BeginNativeADSStateDiff(bool aiming)
	{
		void *owner = InterlockedCompareExchangePointer(&sADSProbeFireOwner, NULL, NULL);
		void *service = InterlockedCompareExchangePointer(&sADSProbeService, NULL, NULL);
		void *weapon = InterlockedCompareExchangePointer(&sADSProbeWeapon, NULL, NULL);
		if (!owner && !service && !weapon) {
			LogInfo("VRPose ADS-diff: no live fire objects yet; fire once before changing aim\n");
			return;
		}
		if (owner)
			ReadProcessMemory(GetCurrentProcess(), owner, sADSProbeOwnerBefore,
				sizeof(sADSProbeOwnerBefore), NULL);
		if (service)
			ReadProcessMemory(GetCurrentProcess(), service, sADSProbeServiceBefore,
				sizeof(sADSProbeServiceBefore), NULL);
		if (weapon)
			ReadProcessMemory(GetCurrentProcess(), weapon, sADSProbeWeaponBefore,
				sizeof(sADSProbeWeaponBefore), NULL);
		InterlockedExchange(&sADSProbeTargetState, aiming ? 1 : 0);
		InterlockedExchange(&sADSProbeCountdown, 20);
		LogInfo("VRPose ADS-diff: armed transition to %d owner=%p service=%p weapon=%p\n",
			aiming ? 1 : 0, owner, service, weapon);
	}

	static void TickNativeADSStateDiff()
	{
		LONG left = InterlockedCompareExchange(&sADSProbeCountdown, 0, 0);
		if (left <= 0 || InterlockedDecrement(&sADSProbeCountdown) != 0)
			return;

		void *objects[3] = {
			InterlockedCompareExchangePointer(&sADSProbeFireOwner, NULL, NULL),
			InterlockedCompareExchangePointer(&sADSProbeService, NULL, NULL),
			InterlockedCompareExchangePointer(&sADSProbeWeapon, NULL, NULL)
		};
		const unsigned char *before[3] = {
			sADSProbeOwnerBefore, sADSProbeServiceBefore, sADSProbeWeaponBefore
		};
		const char *names[3] = { "owner", "service", "weapon" };
		for (int which = 0; which < 3; ++which) {
			if (!objects[which])
				continue;
			unsigned char after[0x1000] = {};
			SIZE_T got = 0;
			if (!ReadProcessMemory(GetCurrentProcess(), objects[which], after, sizeof(after), &got)
				|| got != sizeof(after))
				continue;
			int logged = 0, total = 0;
			for (DWORD off = 0; off < sizeof(after); off += 4) {
				if (memcmp(before[which] + off, after + off, 4) == 0)
					continue;
				total++;
				if (logged++ < 160) {
					unsigned oldValue = 0, newValue = 0;
					memcpy(&oldValue, before[which] + off, 4);
					memcpy(&newValue, after + off, 4);
					LogInfo("VRPose ADS-diff: state=%ld %s +0x%03X %08X -> %08X\n",
						InterlockedCompareExchange(&sADSProbeTargetState, 0, 0),
						names[which], off, oldValue, newValue);
				}
			}
			LogInfo("VRPose ADS-diff: state=%ld %s changedBlocks=%d\n",
				InterlockedCompareExchange(&sADSProbeTargetState, 0, 0), names[which], total);
		}
	}

	// Live weapon-position adjustment, driven from the left stick.
	//
	// Nudging a constant, rebuilding and relaunching to judge a few centimetres
	// is a terrible loop for the one quantity the player can see directly. This
	// is the same reasoning as the scale stepper: where the eye is the
	// instrument, give the eye the control and log the number it settles on.
	// 0 = off, 1 = position, 2 = ROTATION (iron-sight zeroing).
	static volatile LONG sWeaponAdjustMode = 0;
	static float sWeaponAdjust[3] = { 0, 0, 0 };
	static float sWeaponRotAdjust[2] = { 0.0f, 0.0f };
	static float sWeaponRollAdjust = 0.0f;
	struct SavedWeaponAdjust {
		float pos[3], rot[3];
		int weaponKey;
		unsigned bodyMesh;
		std::unordered_set<unsigned> meshes;
	};
	static std::vector<SavedWeaponAdjust> sSavedWeaponAdjusts;
	// Body-draw identities registered by calibration. These keys are synthetic
	// and deliberately separate from Metro's native weapon IDs.
	static std::unordered_map<unsigned, int> sRegisteredWeaponBodies;
	static const int kRegisteredWeaponKeyBase = 100000000;
	// Keep the profile selected by the current weapon until another weapon has
	// enough distinctive mesh evidence to beat it. Without hysteresis, the
	// mesh set changes for a frame after pickup/lock-in and the matcher can
	// jump back to an older duplicate profile, visibly undoing the calibration.
	static int sSelectedWeaponProfile = -1;
	// Published by the profile matcher so the projectile hook can apply the
	// pneumatic camera-origin correction only to the Helsing viewmodel.
	static volatile LONG sHelsingActive = 0;
	static std::unordered_set<unsigned> sWeaponAdjustObservedMeshes;
	static std::unordered_set<unsigned> sRuntimeWeaponMeshes;
	static std::unordered_set<unsigned> sActiveWeaponMeshes;
	static bool sWeaponMenuPreviewDirty = false;
	static int sWeaponMenuPreviewKey = -1;
	static std::unordered_set<unsigned> sWeaponMenuPreviewMeshes;
	static unsigned sRuntimeWeaponMeshFrame = 0xFFFFFFFF;
	static bool sWeaponAdjustsLoaded = false;
	static int sCurrentWeaponKey = -1;
	static int sLatchedWeaponKey = -1;
	static int sActiveWeaponProfileKey = -1;
	static std::unordered_set<unsigned> sWeaponCalibrationBodyMeshes;
	// Headset-calibrated v4 profiles captured after every weapon was collected.
	// These are compiled into the DLL so a clean install does not depend on the
	// machine-local calibration file. A vr_weapon_calibrations.txt beside the
	// DLL is still loaded afterward and therefore remains a last-wins override.
	static const char *kEmbeddedWeaponCalibrationLines[] = {
		"v4 100011229 11229 0.0820000 -0.0400000 -0.0220000 -0.1175999 -0.0468000 0.0000000 6 12132 4716 11229 1056 3075 942",
		"v4 100009540 9540 0.0700000 0.1419999 -0.1419999 0.0000000 0.0000000 0.0000000 20 84 192 522 9540 336 576 162 2106 6693 432 2151 330 396 1392 17784 7929 6 1140 108 36",
		"v4 6273 0 0.0580000 -0.0220000 -0.0900000 0.0000000 0.0000000 0.0000000 10 12132 6273 936 1224 6840 162 609 3420 6 78",
		"v4 16251 0 0.0560000 0.0120000 0.0360000 0.0000000 0.0144000 0.0000000 6 12132 162 16251 180 4320 1296",
		"v4 100021948 21948 0.0420000 -0.0300000 -0.0280000 0.0000000 0.0000000 0.0000000 8 12132 1188 786 21948 66 162 6 69",
		"v4 100010875 10875 0.0300000 -0.0000000 0.0000000 0.0000000 0.0000000 0.0000000 6 12132 10875 48 4176 5292 162",
		"v4 100019221 19221 0.1079999 -0.0260000 0.1360000 0.0000000 0.0000000 0.0000000 6 21762 19221 810 7560 162 2184",
		"v4 100015222 15222 0.0520000 0.0440000 0.0200000 0.0000000 0.0000000 0.0000000 10 15222 17784 2517 4647 60 12 11751 288 144 273",
		"v4 100014562 14562 0.0920000 0.0100000 0.0660000 0.0000000 0.0000000 0.0000000 15 14562 18 720 21762 2796 96 8640 6837 960 1308 72 600 60 48 24",
		"v4 100040740 40740 0.0140000 -0.0060000 0.0440000 0.0000000 0.0000000 0.0000000 5 1236 7806 17784 40740 2472",
		"v4 100012060 12060 0.0080000 0.0080000 0.0660000 0.0000000 0.0000000 0.0000000 7 324 17784 7782 12060 972 978 612",
	};

	static unsigned SelectCalibrationBodyMesh(
		const std::unordered_set<unsigned> &meshes,
		const std::unordered_set<unsigned> &bodyMeshes)
	{
		// NoteViewmodelBodyDraw is restricted to the held viewmodel pass and
		// excludes the combined hand/watch draws. Prefer the largest stable body
		// draw; attachments and material children are smaller in the current
		// renderer. The fallback keeps calibration usable if a future weapon has
		// an unusually small body.
		unsigned best = 0;
		for (unsigned id : bodyMeshes) {
			if (id == 12132 || id == 6108) continue;
			if (id > best) best = id;
		}
		if (best) return best;
		for (unsigned id : meshes) {
			if (id == 12132 || id == 6108 || id <= 3000) continue;
			if (id > best) best = id;
		}
		return best;
	}

	static int RegisteredWeaponKey(unsigned bodyMesh)
	{
		return bodyMesh ? kRegisteredWeaponKeyBase + (int)bodyMesh : -1;
	}

	static int InferLegacyWeaponKey(const std::unordered_set<unsigned> &meshes)
	{
		// Legacy v2 profiles did not store an identity. Recover the two body
		// identities already established by the viewmodel census so attachments
		// and shared hand/watch meshes do not keep competing profiles alive.
		if (meshes.count(16251)) return 16251; // Bastard SMG body
		if (meshes.count(6273) || meshes.count(1224) || meshes.count(6840))
			return 6273; // revolver body cluster
		return -1;
	}

	// The complete viewmodel mesh set is not stable. Hands, watch, muzzle
	// pieces and animation attachments can appear or disappear between level
	// loads, and two variants of one weapon need not submit exactly the same
	// set. The old Jaccard score treated those incidental changes as a new gun.
	//
	// Weight a mesh by how many saved profiles contain it. Weapon-specific
	// meshes are therefore strong evidence; shared hands/watch/attachment
	// meshes are weak evidence. Normalize by the smaller weighted set so a
	// stable weapon signature still matches when the current level only
	// exposes a subset of its draws during the first few frames.
	static float WeaponMeshWeight(unsigned id)
	{
		unsigned profileCount = 0;
		for (const SavedWeaponAdjust &p : sSavedWeaponAdjusts)
			if (p.meshes.count(id))
				++profileCount;
		return 1.0f / sqrtf((float)(profileCount ? profileCount : 1));
	}

	static float WeaponProfileSimilarity(const std::unordered_set<unsigned> &a,
		const std::unordered_set<unsigned> &b)
	{
		if (a.empty() || b.empty()) return 0.0f;
		float weightA = 0.0f, weightB = 0.0f, intersection = 0.0f;
		for (unsigned id : a) {
			const float w = WeaponMeshWeight(id);
			weightA += w;
			if (b.count(id)) intersection += w;
		}
		for (unsigned id : b)
			weightB += WeaponMeshWeight(id);
		const float denominator = (std::min)(weightA, weightB);
		return denominator > 0.0f ? intersection / denominator : 0.0f;
	}

	static void SelectWeaponAdjustForMeshSet(const std::unordered_set<unsigned> &meshes,
		int weaponKey)
	{
		sActiveWeaponMeshes = meshes;
		if (sWeaponMenuPreviewDirty) {
			const bool sameKey = weaponKey >= 0 && sWeaponMenuPreviewKey >= 0
				&& weaponKey == sWeaponMenuPreviewKey;
			const bool sameMeshes = weaponKey < 0 && sWeaponMenuPreviewKey < 0
				&& meshes == sWeaponMenuPreviewMeshes;
			if (sameKey || sameMeshes)
				return;
			sWeaponMenuPreviewDirty = false;
			sWeaponMenuPreviewKey = -1;
			sWeaponMenuPreviewMeshes.clear();
		}
		int best = -1;
		float bestScore = 0.0f, secondScore = 0.0f;
		const bool authoritativeKey = weaponKey >= 0;
		for (unsigned i = 0; i < sSavedWeaponAdjusts.size(); ++i) {
			if (weaponKey >= 0 && sSavedWeaponAdjusts[i].weaponKey == weaponKey) {
				best = (int)i;
				bestScore = 1.0f;
				break;
			}
			const float score = WeaponProfileSimilarity(meshes, sSavedWeaponAdjusts[i].meshes);
			if (score > bestScore) {
				secondScore = bestScore;
				bestScore = score;
				best = (int)i;
			} else if (score > secondScore) {
				secondScore = score;
			}
		}
		if (!authoritativeKey && sSelectedWeaponProfile >= 0
			&& sSelectedWeaponProfile < (int)sSavedWeaponAdjusts.size()) {
			const float currentScore = WeaponProfileSimilarity(meshes,
				sSavedWeaponAdjusts[sSelectedWeaponProfile].meshes);
			// Retain the active weapon through transient mesh-set changes. A
			// clearly different weapon still wins once its distinctive meshes
			// provide materially stronger evidence.
			if (currentScore >= 0.65f && bestScore - currentScore < 0.15f) {
				best = sSelectedWeaponProfile;
				bestScore = currentScore;
			}
		}
		// Hands, watch and attachments are shared by several guns. A profile
		// only owns the current weapon when most of the complete rendered mesh
		// set agrees and it wins clearly over every other profile.
		if (best >= 0 && bestScore >= 0.75f
			&& (sSavedWeaponAdjusts.size() == 1 || bestScore - secondScore >= 0.10f)) {
			memcpy(sWeaponAdjust, sSavedWeaponAdjusts[best].pos, sizeof(sWeaponAdjust));
			sWeaponRotAdjust[0] = sSavedWeaponAdjusts[best].rot[0];
			sWeaponRotAdjust[1] = sSavedWeaponAdjusts[best].rot[1];
			sWeaponRollAdjust = sSavedWeaponAdjusts[best].rot[2];
		} else {
			memset(sWeaponAdjust, 0, sizeof(sWeaponAdjust));
			memset(sWeaponRotAdjust, 0, sizeof(sWeaponRotAdjust));
			sWeaponRollAdjust = 0.0f;
		}
		if (best >= 0 && bestScore >= 0.75f
			&& (sSavedWeaponAdjusts.size() == 1 || bestScore - secondScore >= 0.10f))
			sSelectedWeaponProfile = best;
		const bool applied = best >= 0 && bestScore >= 0.75f
			&& (sSavedWeaponAdjusts.size() == 1 || bestScore - secondScore >= 0.10f);
		sActiveWeaponProfileKey = applied ? sSavedWeaponAdjusts[best].weaponKey : -1;
		const bool helsingApplied = applied
			&& sSavedWeaponAdjusts[best].bodyMesh == 10926;
		InterlockedExchange(&sHelsingActive, helsingApplied ? 1 : 0);
		static int lastBest = -2;
		static bool lastApplied = false;
		if (best != lastBest || applied != lastApplied) {
			LogInfo("VRPose weapon: profile match meshes=%u best=%d score=%.3f second=%.3f applied=%d\n",
				(unsigned)meshes.size(), best, bestScore, secondScore, applied ? 1 : 0);
			lastBest = best;
			lastApplied = applied;
		}
	}

	static void LoadWeaponAdjusts()
	{
		if (sWeaponAdjustsLoaded) return;
		sWeaponAdjustsLoaded = true;
		bool migratedLegacyProfiles = false;
		auto loadProfileLine = [&](const std::string &line,
			bool externalProfile) {
			if (line.empty()) return;
			SavedWeaponAdjust p = {};
			p.weaponKey = -1;
			p.bodyMesh = 0;
			unsigned count = 0;
			std::istringstream input(line);
			if (line.compare(0, 3, "v4 ") == 0) {
				std::string version;
				if (!(input >> version >> p.weaponKey >> p.bodyMesh
					>> p.pos[0] >> p.pos[1] >> p.pos[2]
					>> p.rot[0] >> p.rot[1] >> p.rot[2] >> count)) return;
			} else if (line.compare(0, 3, "v3 ") == 0) {
				std::string version;
				if (!(input >> version >> p.weaponKey >> p.pos[0] >> p.pos[1] >> p.pos[2]
					>> p.rot[0] >> p.rot[1] >> p.rot[2] >> count)) return;
			} else if (line.compare(0, 3, "v2 ") == 0) {
				std::string version;
				if (!(input >> version >> p.pos[0] >> p.pos[1] >> p.pos[2]
					>> p.rot[0] >> p.rot[1] >> p.rot[2] >> count)) return;
			} else {
				// Legacy profiles had pitch and yaw only. Preserve them and
				// default the newly supported wrist-roll axis to zero.
				if (!(input >> p.pos[0] >> p.pos[1] >> p.pos[2]
					>> p.rot[0] >> p.rot[1] >> count)) return;
			}
			for (unsigned i = 0; i < count; ++i) {
				unsigned id = 0;
				if (!(input >> id)) break;
				p.meshes.insert(id);
			}
			if (p.weaponKey < 0)
				p.weaponKey = InferLegacyWeaponKey(p.meshes);
			// Older profiles have no native key, but their saved complete mesh
			// set still contains the stable large body draw. Promote that draw to
			// the same synthetic identity used by new calibration saves, so users
			// do not need to reacquire weapons merely to register them.
			if (p.weaponKey < 0) {
				p.bodyMesh = SelectCalibrationBodyMesh(p.meshes,
					std::unordered_set<unsigned>());
				if (p.bodyMesh) {
					p.weaponKey = RegisteredWeaponKey(p.bodyMesh);
					if (externalProfile)
						migratedLegacyProfiles = true;
				}
			}
			if (!p.meshes.empty()) sSavedWeaponAdjusts.push_back(p);
		};
		for (const char *line : kEmbeddedWeaponCalibrationLines)
			loadProfileLine(line, false);

		bool externalProfilesLoaded = false;
		FILE *f = NULL;
		if (fopen_s(&f, "vr_weapon_calibrations.txt", "r") == 0 && f) {
			externalProfilesLoaded = true;
			char buffer[4096];
			while (fgets(buffer, sizeof(buffer), f))
				loadProfileLine(buffer, true);
			fclose(f);
		}
		// Keep the last legacy entry for an identity. The file is append-history
		// from the old matcher, so later entries represent the user's latest
		// lock-in and older duplicates must not compete with them.
		std::vector<SavedWeaponAdjust> compacted;
		for (const SavedWeaponAdjust &p : sSavedWeaponAdjusts) {
			if (p.weaponKey >= 0) {
				for (auto it = compacted.begin(); it != compacted.end(); ++it) {
					if (it->weaponKey == p.weaponKey) {
						*it = p;
						goto next_profile;
					}
				}
			}
			compacted.push_back(p);
			next_profile:;
		}
		sSavedWeaponAdjusts.swap(compacted);
		sRegisteredWeaponBodies.clear();
		for (const SavedWeaponAdjust &p : sSavedWeaponAdjusts) {
			if (p.bodyMesh && p.weaponKey >= 0) {
				auto it = sRegisteredWeaponBodies.find(p.bodyMesh);
				if (it == sRegisteredWeaponBodies.end())
					sRegisteredWeaponBodies[p.bodyMesh] = p.weaponKey;
				else if (it->second != p.weaponKey)
					sRegisteredWeaponBodies.erase(it); // shared/ambiguous body
			}
		}
		if (migratedLegacyProfiles) {
			FILE *out = NULL;
			if (fopen_s(&out, "vr_weapon_calibrations.txt", "w") == 0 && out) {
				for (const SavedWeaponAdjust &p : sSavedWeaponAdjusts) {
					fprintf(out, "v4 %d %u %.7f %.7f %.7f %.7f %.7f %.7f %u",
						p.weaponKey, p.bodyMesh, p.pos[0], p.pos[1], p.pos[2],
						p.rot[0], p.rot[1], p.rot[2], (unsigned)p.meshes.size());
					for (unsigned id : p.meshes) fprintf(out, " %u", id);
					fprintf(out, "\n");
				}
				fclose(out);
			}
		}
		LogInfo("VRPose weapon: loaded %u calibration profiles (%u embedded, external override %s)\n",
			(unsigned)sSavedWeaponAdjusts.size(),
			(unsigned)ARRAYSIZE(kEmbeddedWeaponCalibrationLines),
			externalProfilesLoaded ? "present" : "absent");
	}

	static bool SaveWeaponAdjustForMeshes(const std::unordered_set<unsigned> &meshes,
		int forcedWeaponKey = -1)
	{
		LoadWeaponAdjusts();
		if (meshes.empty()) {
			LogInfo("VRPose weapon: no rendered weapon meshes observed; calibration not saved\n");
			return false;
		}
		SavedWeaponAdjust p = {};
		memcpy(p.pos, sWeaponAdjust, sizeof(p.pos));
		p.rot[0] = sWeaponRotAdjust[0];
		p.rot[1] = sWeaponRotAdjust[1];
		p.rot[2] = sWeaponRollAdjust;
		p.weaponKey = forcedWeaponKey >= 0 ? forcedWeaponKey : sCurrentWeaponKey;
		p.bodyMesh = SelectCalibrationBodyMesh(meshes, sWeaponCalibrationBodyMeshes);
		if (p.weaponKey < 0 && p.bodyMesh)
			p.weaponKey = RegisteredWeaponKey(p.bodyMesh);
		p.meshes = meshes;
		if (p.bodyMesh && p.weaponKey >= 0) {
			auto it = sRegisteredWeaponBodies.find(p.bodyMesh);
			if (it == sRegisteredWeaponBodies.end() || it->second == p.weaponKey)
				sRegisteredWeaponBodies[p.bodyMesh] = p.weaponKey;
			else if (p.weaponKey >= kRegisteredWeaponKeyBase) {
				p.bodyMesh = 0;
				p.weaponKey = -1;
			}
		}
		if (p.weaponKey >= 0) {
			for (unsigned i = 0; i < sSavedWeaponAdjusts.size(); ++i) {
				if (sSavedWeaponAdjusts[i].weaponKey != p.weaponKey) continue;
				sSavedWeaponAdjusts[i] = p;
				sSelectedWeaponProfile = (int)i;
				FILE *f = NULL;
				if (fopen_s(&f, "vr_weapon_calibrations.txt", "w") == 0 && f) {
					for (const SavedWeaponAdjust &v : sSavedWeaponAdjusts) {
						fprintf(f, "v4 %d %u %.7f %.7f %.7f %.7f %.7f %.7f %u", v.weaponKey, v.bodyMesh, v.pos[0], v.pos[1], v.pos[2], v.rot[0], v.rot[1], v.rot[2], (unsigned)v.meshes.size());
						for (unsigned id : v.meshes) fprintf(f, " %u", id);
						fprintf(f, "\n");
					}
					fclose(f);
				}
				sActiveWeaponMeshes = meshes;
				LogInfo("VRPose weapon: overwrote base-weapon profile key=%d\n", p.weaponKey);
				return true;
			}
		}
		// A lock-in is an overwrite, never a new candidate, when the current
		// mesh set is recognisably the same weapon. The previous implementation
		// only replaced one profile at 0.75 Jaccard similarity. Level/pickup
		// draw differences fell below that threshold, so every re-calibration
		// appended another profile and an older wrong one could win later.
		// Collapse all matching duplicates and keep the newly saved values.
		static const float kSameWeaponSaveScore = 0.60f;
		int best = -1;
		float bestScore = 0.0f;
		int firstMatching = -1;
		unsigned matchingProfiles = 0;
		for (unsigned i = 0; i < sSavedWeaponAdjusts.size(); ++i) {
			const float score = WeaponProfileSimilarity(p.meshes, sSavedWeaponAdjusts[i].meshes);
			if (score >= kSameWeaponSaveScore) {
				if (firstMatching < 0) firstMatching = (int)i;
				++matchingProfiles;
			}
			if (score > bestScore) {
				best = (int)i;
				bestScore = score;
			}
		}
		if (firstMatching >= 0) {
			std::vector<SavedWeaponAdjust> compacted;
			compacted.reserve(sSavedWeaponAdjusts.size() - matchingProfiles + 1);
			for (unsigned i = 0; i < sSavedWeaponAdjusts.size(); ++i) {
				const float score = WeaponProfileSimilarity(p.meshes, sSavedWeaponAdjusts[i].meshes);
				if (score >= kSameWeaponSaveScore) {
					if ((int)i == firstMatching)
						compacted.push_back(p);
					continue;
				}
				compacted.push_back(sSavedWeaponAdjusts[i]);
			}
			sSavedWeaponAdjusts.swap(compacted);
			sSelectedWeaponProfile = firstMatching;
			LogInfo("VRPose weapon: overwrote profile score=%.3f collapsed=%u duplicates\n",
				bestScore, matchingProfiles);
		} else {
			sSavedWeaponAdjusts.push_back(p);
			sSelectedWeaponProfile = (int)sSavedWeaponAdjusts.size() - 1;
			LogInfo("VRPose weapon: created profile (no existing match; best score=%.3f)\n", bestScore);
		}
		FILE *f=NULL;
		if (fopen_s(&f,"vr_weapon_calibrations.txt","w") == 0 && f) {
			for (const SavedWeaponAdjust &v:sSavedWeaponAdjusts) {
				fprintf(f,"v4 %d %u %.7f %.7f %.7f %.7f %.7f %.7f %u",v.weaponKey,v.bodyMesh,v.pos[0],v.pos[1],v.pos[2],v.rot[0],v.rot[1],v.rot[2],(unsigned)v.meshes.size());
				for (unsigned id:v.meshes) fprintf(f," %u",id);
				fprintf(f,"\n");
			}
			fclose(f);
		} else {
			LogInfo("VRPose weapon: could not write vr_weapon_calibrations.txt\n");
			return false;
		}
		sActiveWeaponMeshes = meshes;
		LogInfo("VRPose weapon: saved calibration profile %u (meshes=%u pos=%.3f %.3f %.3f rot=%.2f %.2f %.2f deg)\n",
			(unsigned)sSavedWeaponAdjusts.size(), (unsigned)p.meshes.size(), p.pos[0],p.pos[1],p.pos[2],
			p.rot[0]*57.29578f,p.rot[1]*57.29578f,p.rot[2]*57.29578f);
		return true;
	}

	static void SaveCurrentWeaponAdjust()
	{
		if (SaveWeaponAdjustForMeshes(sWeaponAdjustObservedMeshes)) {
			sWeaponMenuPreviewDirty = false;
			sWeaponMenuPreviewKey = -1;
			sWeaponMenuPreviewMeshes.clear();
		}
	}

	static unsigned sLastWeaponBodyFrame = 0xFFFFFFFF;

	void ObserveWeaponCalibrationMesh(unsigned indexCount, int baseWeaponId)
	{
		LoadWeaponAdjusts();
		if (baseWeaponId >= 0)
			sLastWeaponBodyFrame = G->frame_no;
		const bool adjusting =
			InterlockedCompareExchange(&sWeaponAdjustMode, 0, 0) != 0;
		if (sRuntimeWeaponMeshFrame != G->frame_no) {
			// Never reapply a saved profile while physical calibration is live.
			// Doing so overwrote the stick adjustment at every frame boundary.
			if (sRuntimeWeaponMeshFrame != 0xFFFFFFFF && !adjusting) {
				const int selectionKey = sCurrentWeaponKey >= 0
					? sCurrentWeaponKey : sLatchedWeaponKey;
				SelectWeaponAdjustForMeshSet(sRuntimeWeaponMeshes, selectionKey);
			}
			sRuntimeWeaponMeshes.clear();
			sRuntimeWeaponMeshFrame = G->frame_no;
			// A new frame starts a fresh candidate weapon. The base-body draw
			// will set this again before the completed set is selected next frame.
			sCurrentWeaponKey = -1;
		}
		if (baseWeaponId >= 0) {
			sCurrentWeaponKey = baseWeaponId;
			sLatchedWeaponKey = baseWeaponId;
		}
		if (adjusting) {
			sWeaponAdjustObservedMeshes.insert(indexCount);
			return;
		}
		// Select once from the previous frame's COMPLETE viewmodel mesh set.
		// Per-draw selection oscillated whenever two guns shared a hand/watch
		// mesh and is what made one saved position appear on every weapon.
		sRuntimeWeaponMeshes.insert(indexCount);
	}

	int GetRegisteredWeaponKey(unsigned indexCount)
	{
		LoadWeaponAdjusts();
		auto it = sRegisteredWeaponBodies.find(indexCount);
		return it != sRegisteredWeaponBodies.end() ? it->second : -1;
	}

	bool IsWeaponBodyRecentlySeen(unsigned maxAge)
	{
		if (sLastWeaponBodyFrame == 0xFFFFFFFF
			|| G->frame_no < sLastWeaponBodyFrame)
			return false;
		return G->frame_no - sLastWeaponBodyFrame <= maxAge;
	}

	void NoteViewmodelBodyDraw(unsigned indexCount)
	{
		// The combined hands, watch, lighter, and their small UI/material child
		// meshes are also in the viewmodel pass. They must not keep the right hand
		// in combat mode after the gun has been holstered. Full weapon bodies are
		// substantially larger than these props (the shotgun included), so use the
		// render-pass boundary plus this conservative size floor instead of a
		// per-weapon index-count list.
		if (indexCount > 3000 && indexCount != 12132 && indexCount != 6108) {
			sLastWeaponBodyFrame = G->frame_no;
			if (InterlockedCompareExchange(&sWeaponAdjustMode, 0, 0) != 0)
				sWeaponCalibrationBodyMeshes.insert(indexCount);
		}
	}
	static volatile LONG sLeftHandAdjustMode = 0;
	// Final normal-game hand placement from the headset-validated release setup.
	// A local file remains an optional per-user/controller override.
	static float sLeftHandAdjust[3] = { 0.004000f, 0.172000f, -0.178000f };
	// Authored hand-rest orientation relative to the controller: pitch, roll,
	// then wrist twist. The third component was added after the original
	// two-axis calibration proved insufficient to align the wrist.
	static float sLeftHandRotationAdjust[3] = {
		1.149999f, -0.440000f, 0.650000f
	};
	static bool sLeftHandAdjustLoaded = false;
	// The prologue hand is a different mesh/skeleton. Seed its independent
	// calibration with the final headset-verified deterministic placement, but
	// never write the normal game's file while the opening is being adjusted.
	static float sPrologueLeftHandAdjust[3] = { -0.192f, 0.166f, -0.436001f };
	static float sPrologueLeftHandRotationAdjust[3] = { 0.190f, -1.009999f, 0.0f };
	static bool sPrologueLeftHandAdjustLoaded = false;
	static volatile LONG sPrologueLeftHandLastSeenTick = 0;
	static bool sLeftHandAdjustingPrologue = false;
	// Headset-validated right-hand placement used by clean installs. A valid
	// vr_right_hand_offset.txt remains an optional per-user/controller override.
	static float sRightHandAdjust[3] = {
		0.090000f, -0.191999f, 0.002000f
	};
	static float sRightHandRotationAdjust[3] = {
		-0.940000f, -1.039998f, -0.519999f
	};
	static bool sRightHandAdjustLoaded = false;
	static volatile LONG sChargerAdjustMode = 0;
	static volatile LONG sChargerRightHandAdjustMode = 0;
	static volatile LONG sChargerBodyLastSeenTick = 0;
	// Final headset-verified charger assembly and charger-only right-hand
	// placements. Local files remain optional per-user overrides.
	static float sChargerAdjust[3] = {
		0.199767f, 0.098039f, 0.140492f
	};
	static float sChargerRotationAdjust[3] = {
		0.546974f, -0.192550f, 0.000000f
	};
	static float sChargerRightHandAdjust[3] = {
		0.071883f, -0.165728f, 0.000000f
	};
	static float sChargerRightHandRotationAdjust[3] = {
		-0.058000f, 0.000000f, 0.000000f
	};
	static bool sChargerAdjustLoaded = false;
	static bool sChargerRightHandAdjustLoaded = false;
	static volatile LONG sWatchAdjustMode = 0;
	// Final normal-game rigid wrist-watch placement. The external assembly file
	// is an optional override, not a clean-install dependency.
	static float sWatchAdjust[3] = {
		-0.907504f, 0.204000f, -0.456001f
	};
	static float sWatchRotationAdjust[3] = {
		-0.110000f, -0.020000f, 0.000000f
	};
	static bool sWatchAdjustLoaded = false;
	// Independent prologue watch assembly calibration. These defaults are the
	// final headset-verified placement on the rigid wrist-local watch baseline.
	static float sPrologueWatchAdjust[3] = { -0.450301f, 0.247900f, -0.205200f };
	static float sPrologueWatchRotationAdjust[3] = {
		0.522923f, -0.332777f, -0.148408f
	};
	static bool sPrologueWatchAdjustLoaded = false;
	static bool sWatchAdjustingPrologue = false;
	static float sWatchLegacyAdjust[3] = { 0, 0, 0 };
	static bool sWatchLegacyAdjustLoaded = false;
	static volatile LONG sWatchTimeAdjustMode = 0;
	// Shared normal/prologue casing-local face calibration, using the final
	// headset-verified placement and scale.
	static float sWatchTimeAdjustPosition[3] = {
		-0.000318f, 0.000303f, -0.002759f
	};
	static float sWatchTimeAdjustRotation[3] = {
		-0.001678f, -0.029611f, -0.014000f
	};
	static float sWatchTimeAdjustScale = 0.956011f;
	static bool sWatchTimeAdjustLoaded = false;
	static volatile LONG sLighterAdjustMode = 0;
	static volatile LONG sLighterBodyLastSeenTick = 0;
	static volatile LONG sLighterBodyVariant = 0;
	// Final headset-verified lighter-body placement under the completed left
	// hand. A local calibration file may override this, but clean installs use
	// the same distributable pose selected for the mod.
	static float sLighterAdjust[3] = {
		0.111402f, -0.105089f, 0.682597f
	};
	static float sLighterRotationAdjust[3] = {
		1.058952f, -0.266553f, 0.702228f
	};
	static bool sLighterAdjustLoaded = false;
	// Final headset-verified journal-lighter placement after the closed-root
	// lock. This differs from the standalone equipment lighter even though both
	// variants use the same model.
	static float sJournalLighterAdjust[3] = {
		0.118595f, -0.094402f, 0.686963f
	};
	static float sJournalLighterRotationAdjust[3] = {
		1.301412f, -0.319485f, 2.055675f
	};
	static bool sJournalLighterAdjustLoaded = false;
	// Prologue profiles start as exact copies of the corresponding gameplay
	// profiles. Their own files and live state make later calibration wholly
	// independent while preserving today's working placement on first launch.
	static float sPrologueLighterAdjust[3] = {
		-0.095664f, -0.029627f, 0.653397f
	};
	static float sPrologueLighterRotationAdjust[3] = {
		0.046834f, -0.478044f, -0.014435f
	};
	static bool sPrologueLighterAdjustLoaded = false;
	static float sPrologueJournalLighterAdjust[3] = {
		-0.148426f, -0.144054f, 0.822864f
	};
	static float sPrologueJournalLighterRotationAdjust[3] = {
		-0.864028f, -0.067861f, 0.319679f
	};
	static bool sPrologueJournalLighterAdjustLoaded = false;
	static unsigned sLighterAdjustingVariant = 0;
	static volatile LONG sLighterFlameAdjustMode = 0;
	// Final headset-verified whole flame and secondary flame/smoke placement.
	// These values were calibrated after the objective-directed base-pivot fix,
	// so they belong specifically to the completed wick-locked implementation.
	static float sLighterFlameAdjust[3] = {
		0.000966f, -0.009818f, 0.013574f
	};
	static float sLighterFlameSecondaryAdjust[3] = {
		0.010561f, 0.001222f, -0.000862f
	};
	static float sLighterFlameScale = 1.878490f;
	static bool sLighterFlameAdjustLoaded = false;
	static float sJournalLighterFlameAdjust[3] = {
		-0.100830f, -0.133042f, 0.094347f
	};
	static float sJournalLighterFlameSecondaryAdjust[3] = {
		-0.003379f, 0.002066f, 0.002649f
	};
	static float sJournalLighterFlameScale = 1.737870f;
	static bool sJournalLighterFlameAdjustLoaded = false;
	static float sPrologueLighterFlameAdjust[3] = {
		0.000966f, -0.009818f, 0.013574f
	};
	static float sPrologueLighterFlameSecondaryAdjust[3] = {
		0.010561f, 0.001222f, -0.000862f
	};
	static float sPrologueLighterFlameScale = 1.878490f;
	static bool sPrologueLighterFlameAdjustLoaded = false;
	static float sPrologueJournalLighterFlameAdjust[3] = {
		-0.100830f, -0.133042f, 0.094347f
	};
	static float sPrologueJournalLighterFlameSecondaryAdjust[3] = {
		-0.003379f, 0.002066f, 0.002649f
	};
	static float sPrologueJournalLighterFlameScale = 1.737870f;
	static bool sPrologueJournalLighterFlameAdjustLoaded = false;
	static unsigned sLighterFlameAdjustingVariant = 0;
	static volatile LONG sJournalAdjustMode = 0;
	// Final headset-validated page-space position (right, up, forward), Euler
	// rotation in radians (pitch, yaw, roll), and uniform scale. The older
	// controller-reconstruction calibration in vr_journal_calibration.txt is a
	// different coordinate system and is deliberately not imported.
	static float sJournalAdjustPosition[3] = {
		-0.073615f, 0.141142f, -0.058959f
	};
	static float sJournalAdjustRotation[3] = {
		0.119332f, 0.114026f, 0.000000f
	};
	static float sJournalAdjustScale = 0.605206f;
	static bool sJournalAdjustLoaded = false;

	static void LoadLeftHandAdjust()
	{
		if (sLeftHandAdjustLoaded)
			return;
		sLeftHandAdjustLoaded = true;
		FILE *f = NULL;
		if (fopen_s(&f, "vr_left_hand_offset.txt", "r") == 0 && f) {
			int count = fscanf_s(f, "%f %f %f %f %f %f", &sLeftHandAdjust[0],
				&sLeftHandAdjust[1], &sLeftHandAdjust[2],
				&sLeftHandRotationAdjust[0], &sLeftHandRotationAdjust[1],
				&sLeftHandRotationAdjust[2]);
			if (count >= 3) {
				for (int i = 0; i < 3; i++)
					if (sLeftHandAdjust[i] < -0.75f || sLeftHandAdjust[i] > 0.75f)
						sLeftHandAdjust[i] = 0.0f;
				if (count < 5 || fabsf(sLeftHandRotationAdjust[0]) > 1.5f
					|| fabsf(sLeftHandRotationAdjust[1]) > 1.5f) {
					sLeftHandRotationAdjust[0] = sLeftHandRotationAdjust[1] = 0.0f;
				}
				if (count < 6 || fabsf(sLeftHandRotationAdjust[2]) > 1.5f)
					sLeftHandRotationAdjust[2] = 0.0f;
				LogInfo("VRPose hand: loaded offset (%.3f %.3f %.3f), rotation (%.2f %.2f %.2f deg)\n",
					sLeftHandAdjust[0], sLeftHandAdjust[1], sLeftHandAdjust[2],
					sLeftHandRotationAdjust[0] * 57.29578f,
					sLeftHandRotationAdjust[1] * 57.29578f,
					sLeftHandRotationAdjust[2] * 57.29578f);
			}
			fclose(f);
		}
	}

	static bool SaveLeftHandAdjust()
	{
		FILE *f = NULL;
		if (fopen_s(&f, "vr_left_hand_offset.txt", "w") == 0 && f) {
			fprintf(f, "%.6f %.6f %.6f %.6f %.6f %.6f\n", sLeftHandAdjust[0],
				sLeftHandAdjust[1], sLeftHandAdjust[2],
				sLeftHandRotationAdjust[0], sLeftHandRotationAdjust[1],
				sLeftHandRotationAdjust[2]);
			fclose(f);
			LogInfo("VRPose hand: saved offset (%.3f %.3f %.3f), rotation (%.2f %.2f %.2f deg)\n",
				sLeftHandAdjust[0], sLeftHandAdjust[1], sLeftHandAdjust[2],
					sLeftHandRotationAdjust[0] * 57.29578f,
					sLeftHandRotationAdjust[1] * 57.29578f,
					sLeftHandRotationAdjust[2] * 57.29578f);
			return true;
		}
		LogInfo("VRPose hand: could not write vr_left_hand_offset.txt\n");
		return false;
	}

	static void LoadWatchAdjust()
	{
		if (sWatchAdjustLoaded)
			return;
		sWatchAdjustLoaded = true;
		FILE *f = NULL;
		// This is intentionally a new file. vr_watch_offset.txt contains a large
		// offset from the retired native-watch path; importing it here would move
		// the now-correct rigid assembly away from the wrist on first launch.
		if (fopen_s(&f, "vr_watch_assembly_offset.txt", "r") == 0 && f) {
			const int count = fscanf_s(f, "%f %f %f %f %f %f",
				&sWatchAdjust[0], &sWatchAdjust[1], &sWatchAdjust[2],
				&sWatchRotationAdjust[0], &sWatchRotationAdjust[1],
				&sWatchRotationAdjust[2]);
			fclose(f);
			if (count != 6) {
				sWatchAdjust[0] = sWatchAdjust[1] = sWatchAdjust[2] = 0.0f;
				sWatchRotationAdjust[0] = sWatchRotationAdjust[1]
					= sWatchRotationAdjust[2] = 0.0f;
			}
		}
		for (int i = 0; i < 3; ++i) {
			if (sWatchAdjust[i] < -1.5f || sWatchAdjust[i] > 1.5f)
				sWatchAdjust[i] = 0.0f;
			if (sWatchRotationAdjust[i] < -3.14159f
				|| sWatchRotationAdjust[i] > 3.14159f)
				sWatchRotationAdjust[i] = 0.0f;
		}
		LogInfo("VRPose watch assembly: loaded pos=(%.3f %.3f %.3f) "
			"rot=(%.1f %.1f %.1f deg)\n",
			sWatchAdjust[0], sWatchAdjust[1], sWatchAdjust[2],
			sWatchRotationAdjust[0] * 57.29578f,
			sWatchRotationAdjust[1] * 57.29578f,
			sWatchRotationAdjust[2] * 57.29578f);
	}

	static void LoadRightHandAdjust()
	{
		if (sRightHandAdjustLoaded)
			return;
		sRightHandAdjustLoaded = true;
		FILE *f = NULL;
		if (fopen_s(&f, "vr_right_hand_offset.txt", "r") != 0 || !f)
			return;
		float position[3] = {};
		float rotation[3] = {};
		const int count = fscanf_s(f, "%f %f %f %f %f %f",
			&position[0], &position[1], &position[2],
			&rotation[0], &rotation[1], &rotation[2]);
		fclose(f);
		if (count != 6)
			return;
		for (int i = 0; i < 3; ++i) {
			if (!isfinite(position[i]) || fabsf(position[i]) > 0.75f
				|| !isfinite(rotation[i]) || fabsf(rotation[i]) > 1.5f)
				return;
		}
		memcpy(sRightHandAdjust, position, sizeof(sRightHandAdjust));
		memcpy(sRightHandRotationAdjust, rotation,
			sizeof(sRightHandRotationAdjust));
		LogInfo("VRPose right hand: loaded saved offset (%.3f %.3f %.3f), "
			"rotation (%.2f %.2f %.2f deg)\n",
			sRightHandAdjust[0], sRightHandAdjust[1], sRightHandAdjust[2],
			sRightHandRotationAdjust[0] * 57.29578f,
			sRightHandRotationAdjust[1] * 57.29578f,
			sRightHandRotationAdjust[2] * 57.29578f);
	}

	static void LoadChargerAdjust()
	{
		if (sChargerAdjustLoaded)
			return;
		sChargerAdjustLoaded = true;
		FILE *f = NULL;
		if (fopen_s(&f, "vr_charger_offset.txt", "r") == 0 && f) {
			const int count = fscanf_s(f, "%f %f %f %f %f %f",
				&sChargerAdjust[0], &sChargerAdjust[1], &sChargerAdjust[2],
				&sChargerRotationAdjust[0], &sChargerRotationAdjust[1],
				&sChargerRotationAdjust[2]);
			fclose(f);
			if (count != 6) {
				const float defaultPosition[3] = {
					0.199767f, 0.098039f, 0.140492f
				};
				const float defaultRotation[3] = {
					0.546974f, -0.192550f, 0.000000f
				};
				memcpy(sChargerAdjust, defaultPosition,
					sizeof(sChargerAdjust));
				memcpy(sChargerRotationAdjust, defaultRotation,
					sizeof(sChargerRotationAdjust));
			}
		}
		for (unsigned i = 0; i < 3; ++i) {
			sChargerAdjust[i] = max(-0.75f, min(0.75f, sChargerAdjust[i]));
			sChargerRotationAdjust[i] = max(-3.14159f,
				min(3.14159f, sChargerRotationAdjust[i]));
		}
	}

	static void SaveChargerAdjust()
	{
		FILE *f = NULL;
		if (fopen_s(&f, "vr_charger_offset.txt", "w") == 0 && f) {
			fprintf(f, "%.6f %.6f %.6f %.6f %.6f %.6f\n",
				sChargerAdjust[0], sChargerAdjust[1], sChargerAdjust[2],
				sChargerRotationAdjust[0], sChargerRotationAdjust[1],
				sChargerRotationAdjust[2]);
			fclose(f);
			LogInfo("VRPose charger: saved assembly pos=(%.3f %.3f %.3f) "
				"rot=(%.1f %.1f %.1f deg)\n", sChargerAdjust[0],
				sChargerAdjust[1], sChargerAdjust[2],
				sChargerRotationAdjust[0] * 57.29578f,
				sChargerRotationAdjust[1] * 57.29578f,
				sChargerRotationAdjust[2] * 57.29578f);
		}
	}

	static void LoadChargerRightHandAdjust()
	{
		if (sChargerRightHandAdjustLoaded)
			return;
		sChargerRightHandAdjustLoaded = true;
		FILE *f = NULL;
		if (fopen_s(&f, "vr_charger_right_hand_offset.txt", "r") == 0 && f) {
			const int count = fscanf_s(f, "%f %f %f %f %f %f",
				&sChargerRightHandAdjust[0], &sChargerRightHandAdjust[1],
				&sChargerRightHandAdjust[2],
				&sChargerRightHandRotationAdjust[0],
				&sChargerRightHandRotationAdjust[1],
				&sChargerRightHandRotationAdjust[2]);
			fclose(f);
			if (count != 6) {
				const float defaultPosition[3] = {
					0.071883f, -0.165728f, 0.000000f
				};
				const float defaultRotation[3] = {
					-0.058000f, 0.000000f, 0.000000f
				};
				memcpy(sChargerRightHandAdjust, defaultPosition,
					sizeof(sChargerRightHandAdjust));
				memcpy(sChargerRightHandRotationAdjust, defaultRotation,
					sizeof(sChargerRightHandRotationAdjust));
			}
		}
		for (unsigned i = 0; i < 3; ++i) {
			sChargerRightHandAdjust[i] = max(-0.75f,
				min(0.75f, sChargerRightHandAdjust[i]));
			sChargerRightHandRotationAdjust[i] = max(-3.14159f,
				min(3.14159f, sChargerRightHandRotationAdjust[i]));
		}
	}

	static void SaveChargerRightHandAdjust()
	{
		FILE *f = NULL;
		if (fopen_s(&f, "vr_charger_right_hand_offset.txt", "w") == 0 && f) {
			fprintf(f, "%.6f %.6f %.6f %.6f %.6f %.6f\n",
				sChargerRightHandAdjust[0], sChargerRightHandAdjust[1],
				sChargerRightHandAdjust[2],
				sChargerRightHandRotationAdjust[0],
				sChargerRightHandRotationAdjust[1],
				sChargerRightHandRotationAdjust[2]);
			fclose(f);
			LogInfo("VRPose charger: saved right hand pos=(%.3f %.3f %.3f) "
				"rot=(%.1f %.1f %.1f deg)\n", sChargerRightHandAdjust[0],
				sChargerRightHandAdjust[1], sChargerRightHandAdjust[2],
				sChargerRightHandRotationAdjust[0] * 57.29578f,
				sChargerRightHandRotationAdjust[1] * 57.29578f,
				sChargerRightHandRotationAdjust[2] * 57.29578f);
		}
	}

	static void LoadPrologueLeftHandAdjust()
	{
		if (sPrologueLeftHandAdjustLoaded)
			return;
		sPrologueLeftHandAdjustLoaded = true;
		FILE *f = NULL;
		if (fopen_s(&f, "vr_prologue_left_hand_offset.txt", "r") == 0 && f) {
			const int count = fscanf_s(f, "%f %f %f %f %f %f",
				&sPrologueLeftHandAdjust[0], &sPrologueLeftHandAdjust[1],
				&sPrologueLeftHandAdjust[2],
				&sPrologueLeftHandRotationAdjust[0],
				&sPrologueLeftHandRotationAdjust[1],
				&sPrologueLeftHandRotationAdjust[2]);
			fclose(f);
			if (count != 6) {
				sPrologueLeftHandAdjust[0] = -0.192f;
				sPrologueLeftHandAdjust[1] = 0.166f;
				sPrologueLeftHandAdjust[2] = -0.436001f;
				sPrologueLeftHandRotationAdjust[0] = 0.190f;
				sPrologueLeftHandRotationAdjust[1] = -1.009999f;
				sPrologueLeftHandRotationAdjust[2] = 0.0f;
			}
		}
		for (int i = 0; i < 3; ++i) {
			if (sPrologueLeftHandAdjust[i] < -0.75f
				|| sPrologueLeftHandAdjust[i] > 0.75f)
				sPrologueLeftHandAdjust[i] = 0.0f;
			if (sPrologueLeftHandRotationAdjust[i] < -1.5f
				|| sPrologueLeftHandRotationAdjust[i] > 1.5f)
				sPrologueLeftHandRotationAdjust[i] = 0.0f;
		}
		LogInfo("VRPose prologue hand: loaded offset (%.3f %.3f %.3f), "
			"rotation (%.2f %.2f %.2f deg)\n",
			sPrologueLeftHandAdjust[0], sPrologueLeftHandAdjust[1],
			sPrologueLeftHandAdjust[2],
			sPrologueLeftHandRotationAdjust[0] * 57.29578f,
			sPrologueLeftHandRotationAdjust[1] * 57.29578f,
			sPrologueLeftHandRotationAdjust[2] * 57.29578f);
	}

	static void SavePrologueLeftHandAdjust()
	{
		FILE *f = NULL;
		if (fopen_s(&f, "vr_prologue_left_hand_offset.txt", "w") == 0 && f) {
			fprintf(f, "%.6f %.6f %.6f %.6f %.6f %.6f\n",
				sPrologueLeftHandAdjust[0], sPrologueLeftHandAdjust[1],
				sPrologueLeftHandAdjust[2],
				sPrologueLeftHandRotationAdjust[0],
				sPrologueLeftHandRotationAdjust[1],
				sPrologueLeftHandRotationAdjust[2]);
			fclose(f);
			LogInfo("VRPose prologue hand: saved independent calibration\n");
		}
	}

	static void LoadLighterAdjust()
	{
		if (sLighterAdjustLoaded)
			return;
		sLighterAdjustLoaded = true;
		FILE *f = NULL;
		if (fopen_s(&f, "vr_lighter_offset.txt", "r") == 0 && f) {
			const int count = fscanf_s(f, "%f %f %f %f %f %f",
				&sLighterAdjust[0], &sLighterAdjust[1], &sLighterAdjust[2],
				&sLighterRotationAdjust[0], &sLighterRotationAdjust[1],
				&sLighterRotationAdjust[2]);
			fclose(f);
			if (count != 6) {
				sLighterAdjust[0] = 0.111402f;
				sLighterAdjust[1] = -0.105089f;
				sLighterAdjust[2] = 0.682597f;
				sLighterRotationAdjust[0] = 1.058952f;
				sLighterRotationAdjust[1] = -0.266553f;
				sLighterRotationAdjust[2] = 0.702228f;
			}
		}
		for (int i = 0; i < 3; ++i) {
			sLighterAdjust[i] = max(-1.5f, min(1.5f, sLighterAdjust[i]));
			sLighterRotationAdjust[i] = max(-3.14159f,
				min(3.14159f, sLighterRotationAdjust[i]));
		}
		LogInfo("VRPose lighter: loaded pos=(%.3f %.3f %.3f) "
			"rot=(%.1f %.1f %.1f deg)\n",
			sLighterAdjust[0], sLighterAdjust[1], sLighterAdjust[2],
			sLighterRotationAdjust[0] * 57.29578f,
			sLighterRotationAdjust[1] * 57.29578f,
			sLighterRotationAdjust[2] * 57.29578f);
	}

	static void SaveLighterAdjust()
	{
		FILE *f = NULL;
		if (fopen_s(&f, "vr_lighter_offset.txt", "w") == 0 && f) {
			fprintf(f, "%.6f %.6f %.6f %.6f %.6f %.6f\n",
				sLighterAdjust[0], sLighterAdjust[1], sLighterAdjust[2],
				sLighterRotationAdjust[0], sLighterRotationAdjust[1],
				sLighterRotationAdjust[2]);
			fclose(f);
			LogInfo("VRPose lighter: saved pos=(%.3f %.3f %.3f) "
				"rot=(%.1f %.1f %.1f deg)\n",
				sLighterAdjust[0], sLighterAdjust[1], sLighterAdjust[2],
				sLighterRotationAdjust[0] * 57.29578f,
				sLighterRotationAdjust[1] * 57.29578f,
				sLighterRotationAdjust[2] * 57.29578f);
		}
	}

	static void LoadJournalLighterAdjust()
	{
		if (sJournalLighterAdjustLoaded)
			return;
		sJournalLighterAdjustLoaded = true;
		FILE *f = NULL;
		if (fopen_s(&f, "vr_journal_lighter_offset.txt", "r") == 0 && f) {
			const int count = fscanf_s(f, "%f %f %f %f %f %f",
				&sJournalLighterAdjust[0], &sJournalLighterAdjust[1],
				&sJournalLighterAdjust[2], &sJournalLighterRotationAdjust[0],
				&sJournalLighterRotationAdjust[1],
				&sJournalLighterRotationAdjust[2]);
			fclose(f);
			if (count != 6) {
				sJournalLighterAdjust[0] = 0.118595f;
				sJournalLighterAdjust[1] = -0.094402f;
				sJournalLighterAdjust[2] = 0.686963f;
				sJournalLighterRotationAdjust[0] = 1.301412f;
				sJournalLighterRotationAdjust[1] = -0.319485f;
				sJournalLighterRotationAdjust[2] = 2.055675f;
			}
		}
		for (int i = 0; i < 3; ++i) {
			sJournalLighterAdjust[i] = max(-1.5f,
				min(1.5f, sJournalLighterAdjust[i]));
			sJournalLighterRotationAdjust[i] = max(-3.14159f,
				min(3.14159f, sJournalLighterRotationAdjust[i]));
		}
	}

	static void SaveJournalLighterAdjust()
	{
		FILE *f = NULL;
		if (fopen_s(&f, "vr_journal_lighter_offset.txt", "w") == 0 && f) {
			fprintf(f, "%.6f %.6f %.6f %.6f %.6f %.6f\n",
				sJournalLighterAdjust[0], sJournalLighterAdjust[1],
				sJournalLighterAdjust[2], sJournalLighterRotationAdjust[0],
				sJournalLighterRotationAdjust[1],
				sJournalLighterRotationAdjust[2]);
			fclose(f);
			LogInfo("VRPose journal lighter: saved independent body calibration\n");
		}
	}

	static void LoadLighterFlameAdjust()
	{
		if (sLighterFlameAdjustLoaded)
			return;
		sLighterFlameAdjustLoaded = true;
		FILE *f = NULL;
		if (fopen_s(&f, "vr_lighter_flame_offset.txt", "r") == 0 && f) {
			const int count = fscanf_s(f, "%f %f %f %f %f %f %f",
				&sLighterFlameAdjust[0], &sLighterFlameAdjust[1],
				&sLighterFlameAdjust[2], &sLighterFlameSecondaryAdjust[0],
				&sLighterFlameSecondaryAdjust[1],
				&sLighterFlameSecondaryAdjust[2], &sLighterFlameScale);
			fclose(f);
			if (count != 7) {
				sLighterFlameAdjust[0] = 0.000966f;
				sLighterFlameAdjust[1] = -0.009818f;
				sLighterFlameAdjust[2] = 0.013574f;
				sLighterFlameSecondaryAdjust[0] = 0.010561f;
				sLighterFlameSecondaryAdjust[1] = 0.001222f;
				sLighterFlameSecondaryAdjust[2] = -0.000862f;
				sLighterFlameScale = 1.878490f;
			}
		}
		for (int i = 0; i < 3; ++i) {
			sLighterFlameAdjust[i] = max(-0.5f,
				min(0.5f, sLighterFlameAdjust[i]));
			sLighterFlameSecondaryAdjust[i] = max(-0.5f,
				min(0.5f, sLighterFlameSecondaryAdjust[i]));
		}
		sLighterFlameScale = max(0.25f, min(3.0f, sLighterFlameScale));
		LogInfo("VRPose lighter flame: loaded group=(%.3f %.3f %.3f) "
			"secondary=(%.3f %.3f %.3f) scale=%.3f\n",
			sLighterFlameAdjust[0], sLighterFlameAdjust[1],
			sLighterFlameAdjust[2], sLighterFlameSecondaryAdjust[0],
			sLighterFlameSecondaryAdjust[1],
			sLighterFlameSecondaryAdjust[2], sLighterFlameScale);
	}

	static void SaveLighterFlameAdjust()
	{
		FILE *f = NULL;
		if (fopen_s(&f, "vr_lighter_flame_offset.txt", "w") == 0 && f) {
			fprintf(f, "%.6f %.6f %.6f %.6f %.6f %.6f %.6f\n",
				sLighterFlameAdjust[0], sLighterFlameAdjust[1],
				sLighterFlameAdjust[2], sLighterFlameSecondaryAdjust[0],
				sLighterFlameSecondaryAdjust[1],
				sLighterFlameSecondaryAdjust[2], sLighterFlameScale);
			fclose(f);
			LogInfo("VRPose lighter flame: saved group=(%.3f %.3f %.3f) "
				"secondary=(%.3f %.3f %.3f) scale=%.3f\n",
				sLighterFlameAdjust[0], sLighterFlameAdjust[1],
				sLighterFlameAdjust[2], sLighterFlameSecondaryAdjust[0],
				sLighterFlameSecondaryAdjust[1],
				sLighterFlameSecondaryAdjust[2], sLighterFlameScale);
		}
	}

	static void LoadJournalLighterFlameAdjust()
	{
		if (sJournalLighterFlameAdjustLoaded)
			return;
		sJournalLighterFlameAdjustLoaded = true;
		FILE *f = NULL;
		if (fopen_s(&f, "vr_journal_lighter_flame_offset.txt", "r") == 0 && f) {
			const int count = fscanf_s(f, "%f %f %f %f %f %f %f",
				&sJournalLighterFlameAdjust[0],
				&sJournalLighterFlameAdjust[1],
				&sJournalLighterFlameAdjust[2],
				&sJournalLighterFlameSecondaryAdjust[0],
				&sJournalLighterFlameSecondaryAdjust[1],
				&sJournalLighterFlameSecondaryAdjust[2],
				&sJournalLighterFlameScale);
			fclose(f);
			if (count != 7) {
				sJournalLighterFlameAdjust[0] = -0.100830f;
				sJournalLighterFlameAdjust[1] = -0.133042f;
				sJournalLighterFlameAdjust[2] = 0.094347f;
				sJournalLighterFlameSecondaryAdjust[0] = -0.003379f;
				sJournalLighterFlameSecondaryAdjust[1] = 0.002066f;
				sJournalLighterFlameSecondaryAdjust[2] = 0.002649f;
				sJournalLighterFlameScale = 1.737870f;
			}
		}
		for (int i = 0; i < 3; ++i) {
			sJournalLighterFlameAdjust[i] = max(-0.5f,
				min(0.5f, sJournalLighterFlameAdjust[i]));
			sJournalLighterFlameSecondaryAdjust[i] = max(-0.5f,
				min(0.5f, sJournalLighterFlameSecondaryAdjust[i]));
		}
		// The journal presentation uses a different native particle depth/size
		// relation and legitimately needs more headroom than the standalone
		// lighter's established 3.0 development limit.
		sJournalLighterFlameScale = max(0.25f,
			min(6.0f, sJournalLighterFlameScale));
	}

	static void SaveJournalLighterFlameAdjust()
	{
		FILE *f = NULL;
		if (fopen_s(&f, "vr_journal_lighter_flame_offset.txt", "w") == 0 && f) {
			fprintf(f, "%.6f %.6f %.6f %.6f %.6f %.6f %.6f\n",
				sJournalLighterFlameAdjust[0],
				sJournalLighterFlameAdjust[1],
				sJournalLighterFlameAdjust[2],
				sJournalLighterFlameSecondaryAdjust[0],
				sJournalLighterFlameSecondaryAdjust[1],
				sJournalLighterFlameSecondaryAdjust[2],
				sJournalLighterFlameScale);
			fclose(f);
			LogInfo("VRPose journal lighter: saved independent flame calibration\n");
		}
	}

	static const char *LighterVariantName(unsigned variant)
	{
		static const char *names[4] = {
			"gameplay regular", "gameplay journal",
			"prologue regular", "prologue journal"
		};
		return names[variant & 3u];
	}

	static void LoadPrologueLighterAdjust(bool journal)
	{
		bool &loaded = journal ? sPrologueJournalLighterAdjustLoaded
			: sPrologueLighterAdjustLoaded;
		if (loaded)
			return;
		loaded = true;
		float *position = journal ? sPrologueJournalLighterAdjust
			: sPrologueLighterAdjust;
		float *rotation = journal ? sPrologueJournalLighterRotationAdjust
			: sPrologueLighterRotationAdjust;
		const char *fileName = journal ? "vr_prologue_journal_lighter_offset.txt"
			: "vr_prologue_lighter_offset.txt";
		FILE *f = NULL;
		if (fopen_s(&f, fileName, "r") == 0 && f) {
			const int count = fscanf_s(f, "%f %f %f %f %f %f",
				&position[0], &position[1], &position[2],
				&rotation[0], &rotation[1], &rotation[2]);
			fclose(f);
			if (count != 6) {
				const float *defaultPosition = journal
					? sJournalLighterAdjust : sLighterAdjust;
				const float *defaultRotation = journal
					? sJournalLighterRotationAdjust : sLighterRotationAdjust;
				memcpy(position, defaultPosition, 3 * sizeof(float));
				memcpy(rotation, defaultRotation, 3 * sizeof(float));
			}
		}
		for (int i = 0; i < 3; ++i) {
			position[i] = max(-1.5f, min(1.5f, position[i]));
			rotation[i] = max(-3.14159f, min(3.14159f, rotation[i]));
		}
	}

	static void SavePrologueLighterAdjust(bool journal)
	{
		const float *position = journal ? sPrologueJournalLighterAdjust
			: sPrologueLighterAdjust;
		const float *rotation = journal ? sPrologueJournalLighterRotationAdjust
			: sPrologueLighterRotationAdjust;
		const char *fileName = journal ? "vr_prologue_journal_lighter_offset.txt"
			: "vr_prologue_lighter_offset.txt";
		FILE *f = NULL;
		if (fopen_s(&f, fileName, "w") == 0 && f) {
			fprintf(f, "%.6f %.6f %.6f %.6f %.6f %.6f\n",
				position[0], position[1], position[2],
				rotation[0], rotation[1], rotation[2]);
			fclose(f);
			LogInfo("VRPose %s lighter: saved independent body calibration\n",
				journal ? "prologue journal" : "prologue");
		}
	}

	static void LoadPrologueLighterFlameAdjust(bool journal)
	{
		bool &loaded = journal ? sPrologueJournalLighterFlameAdjustLoaded
			: sPrologueLighterFlameAdjustLoaded;
		if (loaded)
			return;
		loaded = true;
		float *group = journal ? sPrologueJournalLighterFlameAdjust
			: sPrologueLighterFlameAdjust;
		float *secondary = journal
			? sPrologueJournalLighterFlameSecondaryAdjust
			: sPrologueLighterFlameSecondaryAdjust;
		float &scale = journal ? sPrologueJournalLighterFlameScale
			: sPrologueLighterFlameScale;
		const char *fileName = journal
			? "vr_prologue_journal_lighter_flame_offset.txt"
			: "vr_prologue_lighter_flame_offset.txt";
		FILE *f = NULL;
		if (fopen_s(&f, fileName, "r") == 0 && f) {
			const int count = fscanf_s(f, "%f %f %f %f %f %f %f",
				&group[0], &group[1], &group[2], &secondary[0],
				&secondary[1], &secondary[2], &scale);
			fclose(f);
			if (count != 7) {
				const float *defaultGroup = journal
					? sJournalLighterFlameAdjust : sLighterFlameAdjust;
				const float *defaultSecondary = journal
					? sJournalLighterFlameSecondaryAdjust
					: sLighterFlameSecondaryAdjust;
				memcpy(group, defaultGroup, 3 * sizeof(float));
				memcpy(secondary, defaultSecondary, 3 * sizeof(float));
				scale = journal ? sJournalLighterFlameScale : sLighterFlameScale;
			}
		}
		for (int i = 0; i < 3; ++i) {
			group[i] = max(-0.5f, min(0.5f, group[i]));
			secondary[i] = max(-0.5f, min(0.5f, secondary[i]));
		}
		scale = max(0.25f, min(journal ? 6.0f : 3.0f, scale));
	}

	static void SavePrologueLighterFlameAdjust(bool journal)
	{
		const float *group = journal ? sPrologueJournalLighterFlameAdjust
			: sPrologueLighterFlameAdjust;
		const float *secondary = journal
			? sPrologueJournalLighterFlameSecondaryAdjust
			: sPrologueLighterFlameSecondaryAdjust;
		const float scale = journal ? sPrologueJournalLighterFlameScale
			: sPrologueLighterFlameScale;
		const char *fileName = journal
			? "vr_prologue_journal_lighter_flame_offset.txt"
			: "vr_prologue_lighter_flame_offset.txt";
		FILE *f = NULL;
		if (fopen_s(&f, fileName, "w") == 0 && f) {
			fprintf(f, "%.6f %.6f %.6f %.6f %.6f %.6f %.6f\n",
				group[0], group[1], group[2], secondary[0], secondary[1],
				secondary[2], scale);
			fclose(f);
			LogInfo("VRPose %s lighter: saved independent flame calibration\n",
				journal ? "prologue journal" : "prologue");
		}
	}

	static void LoadLighterVariantAdjust(unsigned variant)
	{
		variant &= 3u;
		if (variant >= 2u)
			LoadPrologueLighterAdjust((variant & 1u) != 0);
		else if (variant == 1u)
			LoadJournalLighterAdjust();
		else
			LoadLighterAdjust();
	}

	static void SaveLighterVariantAdjust(unsigned variant)
	{
		variant &= 3u;
		if (variant >= 2u)
			SavePrologueLighterAdjust((variant & 1u) != 0);
		else if (variant == 1u)
			SaveJournalLighterAdjust();
		else
			SaveLighterAdjust();
	}

	static void GetLighterVariantAdjust(unsigned variant, float *&position,
		float *&rotation)
	{
		variant &= 3u;
		LoadLighterVariantAdjust(variant);
		if (variant == 0u) {
			position = sLighterAdjust; rotation = sLighterRotationAdjust;
		} else if (variant == 1u) {
			position = sJournalLighterAdjust;
			rotation = sJournalLighterRotationAdjust;
		} else if (variant == 2u) {
			position = sPrologueLighterAdjust;
			rotation = sPrologueLighterRotationAdjust;
		} else {
			position = sPrologueJournalLighterAdjust;
			rotation = sPrologueJournalLighterRotationAdjust;
		}
	}

	static void LoadLighterVariantFlameAdjust(unsigned variant)
	{
		variant &= 3u;
		if (variant >= 2u)
			LoadPrologueLighterFlameAdjust((variant & 1u) != 0);
		else if (variant == 1u)
			LoadJournalLighterFlameAdjust();
		else
			LoadLighterFlameAdjust();
	}

	static void SaveLighterVariantFlameAdjust(unsigned variant)
	{
		variant &= 3u;
		if (variant >= 2u)
			SavePrologueLighterFlameAdjust((variant & 1u) != 0);
		else if (variant == 1u)
			SaveJournalLighterFlameAdjust();
		else
			SaveLighterFlameAdjust();
	}

	static void GetLighterVariantFlameAdjust(unsigned variant, float *&group,
		float *&secondary, float *&scale)
	{
		variant &= 3u;
		LoadLighterVariantFlameAdjust(variant);
		if (variant == 0u) {
			group = sLighterFlameAdjust;
			secondary = sLighterFlameSecondaryAdjust;
			scale = &sLighterFlameScale;
		} else if (variant == 1u) {
			group = sJournalLighterFlameAdjust;
			secondary = sJournalLighterFlameSecondaryAdjust;
			scale = &sJournalLighterFlameScale;
		} else if (variant == 2u) {
			group = sPrologueLighterFlameAdjust;
			secondary = sPrologueLighterFlameSecondaryAdjust;
			scale = &sPrologueLighterFlameScale;
		} else {
			group = sPrologueJournalLighterFlameAdjust;
			secondary = sPrologueJournalLighterFlameSecondaryAdjust;
			scale = &sPrologueJournalLighterFlameScale;
		}
	}

	static bool IsLighterBodyRecentlyActive()
	{
		const DWORD seen = (DWORD)InterlockedCompareExchange(
			&sLighterBodyLastSeenTick, 0, 0);
		return seen != 0 && (DWORD)(GetTickCount() - seen) < 1000;
	}

	void MarkLighterBodyActive(unsigned lighterVariant)
	{
		InterlockedExchange(&sLighterBodyVariant, (LONG)(lighterVariant & 3u));
		InterlockedExchange(&sLighterBodyLastSeenTick, (LONG)GetTickCount());
	}

	void MarkChargerBodyActive()
	{
		InterlockedExchange(&sChargerBodyLastSeenTick, (LONG)GetTickCount());
	}

	bool IsChargerVisualActive()
	{
		const DWORD seen = (DWORD)InterlockedCompareExchange(
			&sChargerBodyLastSeenTick, 0, 0);
		return seen != 0 && (DWORD)(GetTickCount() - seen) < 250;
	}

	static bool IsJournalLighterRecentlyActive()
	{
		return IsLighterBodyRecentlyActive()
			&& InterlockedCompareExchange(
				&sLighterBodyVariant, 0, 0) % 2 != 0;
	}

	static unsigned ActiveLighterVariant()
	{
		return (unsigned)InterlockedCompareExchange(
			&sLighterBodyVariant, 0, 0) & 3u;
	}

	static bool IsPrologueLighterRecentlyActive()
	{
		return IsLighterBodyRecentlyActive() && ActiveLighterVariant() >= 2u;
	}

	static bool IsPrologueLeftHandRecentlyActive()
	{
		const DWORD seen = (DWORD)InterlockedCompareExchange(
			&sPrologueLeftHandLastSeenTick, 0, 0);
		return seen != 0 && (DWORD)(GetTickCount() - seen) < 1000;
	}

	void MarkPrologueLeftHandActive()
	{
		InterlockedExchange(&sPrologueLeftHandLastSeenTick, (LONG)GetTickCount());
	}

	static void SaveWatchAdjust()
	{
		FILE *f = NULL;
		if (fopen_s(&f, "vr_watch_assembly_offset.txt", "w") == 0 && f) {
			fprintf(f, "%.6f %.6f %.6f %.6f %.6f %.6f\n",
				sWatchAdjust[0], sWatchAdjust[1], sWatchAdjust[2],
				sWatchRotationAdjust[0], sWatchRotationAdjust[1],
				sWatchRotationAdjust[2]);
			fclose(f);
			LogInfo("VRPose watch assembly: saved pos=(%.3f %.3f %.3f) "
				"rot=(%.1f %.1f %.1f deg)\n",
				sWatchAdjust[0], sWatchAdjust[1], sWatchAdjust[2],
				sWatchRotationAdjust[0] * 57.29578f,
				sWatchRotationAdjust[1] * 57.29578f,
				sWatchRotationAdjust[2] * 57.29578f);
		}
	}

	static void LoadPrologueWatchAdjust()
	{
		if (sPrologueWatchAdjustLoaded)
			return;
		sPrologueWatchAdjustLoaded = true;
		FILE *f = NULL;
		if (fopen_s(&f, "vr_prologue_watch_assembly_offset.txt", "r") == 0 && f) {
			const int count = fscanf_s(f, "%f %f %f %f %f %f",
				&sPrologueWatchAdjust[0], &sPrologueWatchAdjust[1],
				&sPrologueWatchAdjust[2],
				&sPrologueWatchRotationAdjust[0],
				&sPrologueWatchRotationAdjust[1],
				&sPrologueWatchRotationAdjust[2]);
			fclose(f);
			if (count != 6) {
				sPrologueWatchAdjust[0] = -0.450301f;
				sPrologueWatchAdjust[1] = 0.247900f;
				sPrologueWatchAdjust[2] = -0.205200f;
				sPrologueWatchRotationAdjust[0] = 0.522923f;
				sPrologueWatchRotationAdjust[1] = -0.332777f;
				sPrologueWatchRotationAdjust[2] = -0.148408f;
			}
		}
		for (int i = 0; i < 3; ++i) {
			sPrologueWatchAdjust[i] = max(-1.5f,
				min(1.5f, sPrologueWatchAdjust[i]));
			sPrologueWatchRotationAdjust[i] = max(-3.14159f,
				min(3.14159f, sPrologueWatchRotationAdjust[i]));
		}
		LogInfo("VRPose prologue watch: loaded pos=(%.3f %.3f %.3f) "
			"rot=(%.1f %.1f %.1f deg)\n",
			sPrologueWatchAdjust[0], sPrologueWatchAdjust[1],
			sPrologueWatchAdjust[2],
			sPrologueWatchRotationAdjust[0] * 57.29578f,
			sPrologueWatchRotationAdjust[1] * 57.29578f,
			sPrologueWatchRotationAdjust[2] * 57.29578f);
	}

	static void SavePrologueWatchAdjust()
	{
		FILE *f = NULL;
		if (fopen_s(&f, "vr_prologue_watch_assembly_offset.txt", "w") == 0 && f) {
			fprintf(f, "%.6f %.6f %.6f %.6f %.6f %.6f\n",
				sPrologueWatchAdjust[0], sPrologueWatchAdjust[1],
				sPrologueWatchAdjust[2],
				sPrologueWatchRotationAdjust[0],
				sPrologueWatchRotationAdjust[1],
				sPrologueWatchRotationAdjust[2]);
			fclose(f);
			LogInfo("VRPose prologue watch: saved independent assembly calibration\n");
		}
	}

	static void LoadWatchLegacyAdjust()
	{
		if (sWatchLegacyAdjustLoaded)
			return;
		sWatchLegacyAdjustLoaded = true;
		FILE *f = NULL;
		if (fopen_s(&f, "vr_watch_offset.txt", "r") == 0 && f) {
			const int count = fscanf_s(f, "%f %f %f",
				&sWatchLegacyAdjust[0], &sWatchLegacyAdjust[1],
				&sWatchLegacyAdjust[2]);
			fclose(f);
			if (count != 3)
				sWatchLegacyAdjust[0] = sWatchLegacyAdjust[1]
					= sWatchLegacyAdjust[2] = 0.0f;
		}
		for (int i = 0; i < 3; ++i)
			if (sWatchLegacyAdjust[i] < -1.5f || sWatchLegacyAdjust[i] > 1.5f)
				sWatchLegacyAdjust[i] = 0.0f;
		LogInfo("VRPose watch legacy: loaded offset (%.3f %.3f %.3f)\n",
			sWatchLegacyAdjust[0], sWatchLegacyAdjust[1],
			sWatchLegacyAdjust[2]);
	}

	void GetLeftHandRotationAdjust(float out[3])
	{
		if (!out) return;
		LoadLeftHandAdjust();
		out[0] = sLeftHandRotationAdjust[0];
		out[1] = sLeftHandRotationAdjust[1];
		out[2] = sLeftHandRotationAdjust[2];
	}

	// Weapon rest-orientation offset, radians: pitch then yaw.
	//
	// Zeroing the sights is a rotation, not a position: the gun's visual axis
	// and the direction the controller steers the game's aim differ by a fixed
	// rotation, because the weapon's rest pose is captured from whatever
	// orientation the controller happened to be in when tracking settled. This
	// is that offset, made adjustable so it can be dialled in against actual
	// bullet holes rather than guessed.
	//
	// Independent of scale and position, so it survives any later change to
	// either - and unlike everything else calibrated tonight, it has an
	// objective success criterion: the rounds land where the sights point.
	// Live sight-zeroing offset that affects the SHOT ONLY (pitch, yaw).
	//
	// The zeroing mode used to drive sWeaponRotAdjust, which is added inside
	// GetControllerAim - and that feeds both the shot AND the camera-steering
	// loop. So nudging it rotated the view as well, the player re-aimed to
	// compensate, and the value they settled on was partly a hand movement.
	// Transplanting it into a shot-only constant therefore never landed:
	// three passes left the impact slightly right, then slightly low, then
	// low-right, wandering instead of converging.
	//
	// This is added in exactly the same place as kAimBias, so what is dialled
	// here IS what gets baked in - the calibration and the constant are the
	// same quantity, and the transplant is exact.
	static float sShotAimBias[2] = { 0.0f, 0.0f };

	void GetShotAimBias(float out[2])
	{
		out[0] = sShotAimBias[0];
		out[1] = sShotAimBias[1];
	}

	// Which entry of the renderer's scale table the player has stepped to.
	// Declared here because the input handler below bumps it, and a static
	// declared after its first use does not compile.
	static volatile LONG sWeaponScaleStep = 0;

	struct KeyBinding {
		WORD scanCode;
		bool held;
	};

	// Scancodes for Metro's defaults.
	enum { SC_W = 0x11, SC_A = 0x1E, SC_S = 0x1F, SC_D = 0x20,
	       SC_E = 0x12, SC_R = 0x13, SC_F = 0x21, SC_SPACE = 0x39,
	       SC_LSHIFT = 0x2A, SC_LCTRL = 0x1D };

	void SendKey(WORD scanCode, bool down)
	{
		INPUT in;
		memset(&in, 0, sizeof(in));
		in.type = INPUT_KEYBOARD;
		in.ki.wScan = scanCode;
		in.ki.dwFlags = KEYEVENTF_SCANCODE | (down ? 0 : KEYEVENTF_KEYUP);
		SendInput(1, &in, sizeof(INPUT));
	}

	// Edge-triggered: only emits an event when the desired state changes, so
	// the game sees clean presses and releases rather than a stream of
	// repeats.
	void SetKeyState(KeyBinding *k, bool wanted)
	{
		if (k->held == wanted)
			return;
		SendKey(k->scanCode, wanted);
		k->held = wanted;
	}

	void SendMouseButton(bool right, bool down)
	{
		INPUT in;
		memset(&in, 0, sizeof(in));
		in.type = INPUT_MOUSE;
		if (right)
			in.mi.dwFlags = down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
		else
			in.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
		SendInput(1, &in, sizeof(INPUT));
	}

	bool GetControllerState(vr::ETrackedControllerRole role, vr::VRControllerState_t *out)
	{
		if (!sVRSystem)
			return false;
		vr::TrackedDeviceIndex_t idx = sVRSystem->GetTrackedDeviceIndexForControllerRole(role);
		if (idx == vr::k_unTrackedDeviceIndexInvalid)
			return false;
		return sVRSystem->GetControllerState(idx, out, sizeof(*out));
	}

	// Left-Handed Mode changes only which tracked controller supplies the
	// weapon pose/aim. Gameplay stick bindings remain separate and are handled
	// by Swap Analog Sticks below.
	static vr::ETrackedControllerRole WeaponControllerRole()
	{
		return VRMenu::GetSettings().leftHandedMode
			? vr::TrackedControllerRole_LeftHand
			: vr::TrackedControllerRole_RightHand;
	}

	static void PulseFireHaptics()
	{
		if (!sVRSystem)
			return;
		static ULONGLONG lastPulse = 0;
		const ULONGLONG now = GetTickCount64();
		// Prevent duplicate hook hits or very fast automatic fire from making
		// the controller feel like a continuous harsh buzz.
		if (lastPulse != 0 && now - lastPulse < 8)
			return;
		const vr::TrackedDeviceIndex_t idx =
			sVRSystem->GetTrackedDeviceIndexForControllerRole(
				WeaponControllerRole());
		if (idx == vr::k_unTrackedDeviceIndexInvalid)
			return;
		lastPulse = now;
		sVRSystem->TriggerHapticPulse(idx, 0, 1800);
	}

	int GetWeaponScaleStep()
	{
		return (int)InterlockedCompareExchange(&sWeaponScaleStep, 0, 0);
	}

	void GetWeaponRotAdjust(float out[2])
	{
		out[0] = sWeaponRotAdjust[0];
		out[1] = sWeaponRotAdjust[1];
	}

	void GetWeaponOffsetAdjust(float out[3])
	{
		out[0] = sWeaponAdjust[0];
		out[1] = sWeaponAdjust[1];
		out[2] = sWeaponAdjust[2];
	}

	bool GetCurrentWeaponMenuAdjust(float position[3], float rotationDegrees[3])
	{
		LoadWeaponAdjusts();
		if (position) memcpy(position, sWeaponAdjust, sizeof(sWeaponAdjust));
		if (rotationDegrees) {
			rotationDegrees[0] = sWeaponRotAdjust[0] * 57.2957795131f;
			rotationDegrees[1] = sWeaponRotAdjust[1] * 57.2957795131f;
			rotationDegrees[2] = sWeaponRollAdjust * 57.2957795131f;
		}
		return IsWeaponBodyRecentlySeen(2) && sActiveWeaponProfileKey >= 0;
	}

	bool AdjustCurrentWeaponMenuPosition(int axis, float deltaMetres)
	{
		if (axis < 0 || axis >= 3 || !IsWeaponBodyRecentlySeen(2)
			|| sActiveWeaponProfileKey < 0) return false;
		const std::unordered_set<unsigned> &meshes = !sActiveWeaponMeshes.empty()
			? sActiveWeaponMeshes : sRuntimeWeaponMeshes;
		sWeaponAdjust[axis] = (std::max)(-1.0f, (std::min)(1.0f,
			sWeaponAdjust[axis] + deltaMetres));
		sWeaponMenuPreviewDirty = true;
		sWeaponMenuPreviewKey = sActiveWeaponProfileKey;
		sWeaponMenuPreviewMeshes = meshes;
		// Menu edits are a live preview. The dedicated menu save action is the
		// only path that commits these values to the persistent weapon profile.
		return true;
	}

	bool AdjustCurrentWeaponMenuRotation(int axis, float deltaDegrees)
	{
		if (axis < 0 || axis >= 3 || !IsWeaponBodyRecentlySeen(2)
			|| sActiveWeaponProfileKey < 0) return false;
		const std::unordered_set<unsigned> &meshes = !sActiveWeaponMeshes.empty()
			? sActiveWeaponMeshes : sRuntimeWeaponMeshes;
		const float deltaRadians = deltaDegrees * 0.01745329251994329577f;
		float *value = axis < 2 ? &sWeaponRotAdjust[axis] : &sWeaponRollAdjust;
		*value += deltaRadians;
		const float pi = 3.14159265358979323846f;
		while (*value > pi) *value -= 2.0f * pi;
		while (*value < -pi) *value += 2.0f * pi;
		sWeaponMenuPreviewDirty = true;
		sWeaponMenuPreviewKey = sActiveWeaponProfileKey;
		sWeaponMenuPreviewMeshes = meshes;
		// Menu edits are a live preview; do not write on every slider step.
		return true;
	}

	bool SaveCurrentWeaponMenuAdjust()
	{
		if (!IsWeaponBodyRecentlySeen(2) || sActiveWeaponProfileKey < 0) {
			LogInfo("VRPose weapon: menu save ignored (no identified held weapon)\n");
			return false;
		}
		const std::unordered_set<unsigned> &meshes = !sActiveWeaponMeshes.empty()
			? sActiveWeaponMeshes : sRuntimeWeaponMeshes;
		if (meshes.empty()) {
			LogInfo("VRPose weapon: menu save ignored (no held weapon mesh set)\n");
			return false;
		}
		const bool saved = SaveWeaponAdjustForMeshes(meshes, sActiveWeaponProfileKey);
		if (saved) {
			sWeaponMenuPreviewDirty = false;
			sWeaponMenuPreviewKey = -1;
			sWeaponMenuPreviewMeshes.clear();
			LogInfo("VRPose weapon: menu save committed current held weapon profile\n");
		}
		return saved;
	}

	bool GetCurrentLeftHandMenuAdjust(float position[3], float rotationDegrees[3])
	{
		LoadLeftHandAdjust();
		if (position) memcpy(position, sLeftHandAdjust, sizeof(sLeftHandAdjust));
		if (rotationDegrees) {
			rotationDegrees[0] = sLeftHandRotationAdjust[0] * 57.2957795131f;
			rotationDegrees[1] = sLeftHandRotationAdjust[1] * 57.2957795131f;
			rotationDegrees[2] = sLeftHandRotationAdjust[2] * 57.2957795131f;
		}
		return true;
	}

	bool AdjustCurrentLeftHandMenuPosition(int axis, float deltaMetres)
	{
		if (axis < 0 || axis >= 3) return false;
		LoadLeftHandAdjust();
		sLeftHandAdjust[axis] = (std::max)(-0.75f, (std::min)(0.75f,
			sLeftHandAdjust[axis] + deltaMetres));
		return true;
	}

	bool AdjustCurrentLeftHandMenuRotation(int axis, float deltaDegrees)
	{
		if (axis < 0 || axis >= 3) return false;
		LoadLeftHandAdjust();
		const float deltaRadians = deltaDegrees * 0.01745329251994329577f;
		sLeftHandRotationAdjust[axis] = (std::max)(-1.5f, (std::min)(1.5f,
			sLeftHandRotationAdjust[axis] + deltaRadians));
		return true;
	}

	bool SaveCurrentLeftHandMenuAdjust()
	{
		LoadLeftHandAdjust();
		return SaveLeftHandAdjust();
	}

	bool IsFiring()
	{
		return InterlockedCompareExchange(&sFiringNow, 0, 0) != 0;
	}

	bool IsAiming()
	{
		return InterlockedCompareExchange(&sAimingNow, 0, 0) != 0
			|| InterlockedCompareExchange(&sGamepadAiming, 0, 0) != 0;
	}

	float GetRequestedScopeMagnification()
	{
		return InterlockedCompareExchange(&sRequestedScopeScaleMilli, 0, 0) / 1000.0f;
	}

	// ---------------------------------------------------------------
	// VIRTUAL XBOX PAD
	//
	// The game binds a lot of actions to the gamepad that have NO keyboard
	// equivalent at all - Equipment Inventory and Lighter are the ones that
	// forced this, since neither appears anywhere in user.cfg's key binds.
	// Synthesising keystrokes therefore cannot reach the full control
	// scheme; presenting a gamepad can, and it gets the game's own preset
	// (3) doing the mapping, so the on-screen prompts match what the player
	// is actually holding.
	//
	// metro.exe imports XINPUT1_3.dll, so hooking XInputGetState in-process
	// is enough - no virtual-pad driver, nothing installed outside the game,
	// and it uses the same Nektra helper the cursor hooks already use here.
	//
	// Buttons and left-stick locomotion are synthesised. The right virtual stick
	// stays neutral so Metro's gamepad look cannot fight the mod's mouse-based
	// head/weapon steering. The left stick carries a watchdog timestamp so a VR
	// polling or rendering stall cannot latch stale movement.
	static volatile LONG sPadButtons = 0;
	// Right-stick deflection published by the culling follow when
	// kFollowViaStick is on - drives the camera as CONTROLLER input instead of
	// synthetic mouse movement. Composed with the neutral-dispatch pulse on RX.
	static volatile LONG sPadFollowRX = 0;
	static volatile LONG sPadFollowRY = 0;
	static volatile LONG sPadLX = 0;
	static volatile LONG sPadLY = 0;
	static volatile LONG sPadAxisUpdateTick = 0;
	// Metro skips its analog callback when every XInput field is exactly zero,
	// leaving the previous movement vector latched. A one-poll, sub-deadzone RX
	// sentinel on movement release forces that callback to run with four zeros.
	static volatile LONG sPadNeutralDispatchPulse = 0;
	static volatile LONG sWeaponInventoryHeld = 0;

	bool IsWeaponInventoryHeld()
	{
		return InterlockedCompareExchange(&sWeaponInventoryHeld, 0, 0) != 0;
	}

	void GetRightHandRotationAdjust(float out[3])
	{
		if (!out) return;
		LoadRightHandAdjust();
		memcpy(out, sRightHandRotationAdjust, sizeof(sRightHandRotationAdjust));
	}

	void GetPrologueLeftHandRotationAdjust(float out[3])
	{
		if (!out) return;
		LoadPrologueLeftHandAdjust();
		memcpy(out, sPrologueLeftHandRotationAdjust,
			sizeof(sPrologueLeftHandRotationAdjust));
	}

	bool IsInventoryMenuHeld()
	{
		const WORD buttons = (WORD)InterlockedCompareExchange(
			&sPadButtons, 0, 0);
		return IsWeaponInventoryHeld() ||
			(buttons & (XINPUT_GAMEPAD_Y |
				XINPUT_GAMEPAD_LEFT_SHOULDER)) != 0;
	}

	bool IsPauseMenuExpected()
	{
		return InterlockedCompareExchange(&sPauseMenuExpected, 0, 0) != 0;
	}
	// Disabled: biasing Metro's real camera fixed the visibility band but also
	// rotated simulation-space locomotion, menu staging, weapons and ballistics.
	static const float kCullingYawBiasRadians = 0.0f;
	static volatile LONG sGameplayHUDConfirmed = 0;
	static volatile LONG sGameplayModeActive = 0;
	static volatile LONG sStartupExitArmed = 0;
	static volatile LONG sLastWorldPlacedUIFrame = -1000;
	static volatile LONG sWorldPlacedUIEverSeen = 0;
	bool ReadCameraVec(DWORD_PTR offset, float out[3]);

	void NotifyGameplayHUDConfirmed()
	{
		InterlockedExchange(&sGameplayHUDConfirmed, 1);
		InterlockedExchange(&sGameplayModeActive, 1);
		// Reloading a checkpoint or loading another save dismisses Metro's
		// pause/menu screen without sending the Start/B edge that normally
		// clears sPauseMenuExpected.  Since Start was treated as a toggle, the
		// stale value then alternated on every reload and UpdateCullingFollow
		// remained disabled throughout every other gameplay session.  The ammo
		// magazine marker that calls this function is gameplay-only (menus never
		// draw it), so its return is the authoritative resume boundary.
		if (InterlockedExchange(&sPauseMenuExpected, 0) != 0)
			LogInfo("VRPose input: gameplay HUD resumed - cleared stale pause-menu state\n");
	}

	static void LoadJournalAdjust()
	{
		if (sJournalAdjustLoaded)
			return;
		sJournalAdjustLoaded = true;
		FILE *f = NULL;
		if (fopen_s(&f, "vr_journal_surface_calibration.txt", "r") == 0 && f) {
			const int count = fscanf_s(f, "%f %f %f %f %f %f %f",
				&sJournalAdjustPosition[0], &sJournalAdjustPosition[1],
				&sJournalAdjustPosition[2], &sJournalAdjustRotation[0],
				&sJournalAdjustRotation[1], &sJournalAdjustRotation[2],
				&sJournalAdjustScale);
			fclose(f);
			if (count != 7 || sJournalAdjustScale < 0.25f || sJournalAdjustScale > 3.0f) {
				sJournalAdjustPosition[0] = 0.0f;
				sJournalAdjustPosition[1] = 0.0f;
				sJournalAdjustPosition[2] = 0.0f;
				sJournalAdjustRotation[0] = sJournalAdjustRotation[1]
					= sJournalAdjustRotation[2] = 0.0f;
				sJournalAdjustScale = 1.0f;
			}
		}
		LogInfo("VRPose journal: loaded pos=(%.3f %.3f %.3f) rot=(%.1f %.1f %.1f) scale=%.3f\n",
			sJournalAdjustPosition[0], sJournalAdjustPosition[1], sJournalAdjustPosition[2],
			sJournalAdjustRotation[0] * 57.29578f, sJournalAdjustRotation[1] * 57.29578f,
			sJournalAdjustRotation[2] * 57.29578f, sJournalAdjustScale);
	}

	static void SaveJournalAdjust()
	{
		FILE *f = NULL;
		if (fopen_s(&f, "vr_journal_surface_calibration.txt", "w") == 0 && f) {
			fprintf(f, "%.6f %.6f %.6f %.6f %.6f %.6f %.6f\n",
				sJournalAdjustPosition[0], sJournalAdjustPosition[1], sJournalAdjustPosition[2],
				sJournalAdjustRotation[0], sJournalAdjustRotation[1],
				sJournalAdjustRotation[2], sJournalAdjustScale);
			fclose(f);
			LogInfo("VRPose journal: saved pos=(%.3f %.3f %.3f) rot=(%.1f %.1f %.1f) scale=%.3f\n",
				sJournalAdjustPosition[0], sJournalAdjustPosition[1], sJournalAdjustPosition[2],
				sJournalAdjustRotation[0] * 57.29578f, sJournalAdjustRotation[1] * 57.29578f,
				sJournalAdjustRotation[2] * 57.29578f, sJournalAdjustScale);
		}
	}

	static void LoadWatchTimeAdjust()
	{
		if (sWatchTimeAdjustLoaded)
			return;
		sWatchTimeAdjustLoaded = true;
		FILE *f = NULL;
		if (fopen_s(&f, "vr_watch_time_offset.txt", "r") == 0 && f) {
			const int count = fscanf_s(f, "%f %f %f %f %f %f %f",
				&sWatchTimeAdjustPosition[0], &sWatchTimeAdjustPosition[1],
				&sWatchTimeAdjustPosition[2], &sWatchTimeAdjustRotation[0],
				&sWatchTimeAdjustRotation[1], &sWatchTimeAdjustRotation[2],
				&sWatchTimeAdjustScale);
			fclose(f);
			if (count != 7) {
				sWatchTimeAdjustPosition[0] = -0.000318f;
				sWatchTimeAdjustPosition[1] = 0.000303f;
				sWatchTimeAdjustPosition[2] = -0.002759f;
				sWatchTimeAdjustRotation[0] = -0.001678f;
				sWatchTimeAdjustRotation[1] = -0.029611f;
				sWatchTimeAdjustRotation[2] = -0.014000f;
				sWatchTimeAdjustScale = 0.956011f;
			}
		}
		for (int i = 0; i < 3; ++i) {
			sWatchTimeAdjustPosition[i] = max(-1.5f,
				min(1.5f, sWatchTimeAdjustPosition[i]));
			sWatchTimeAdjustRotation[i] = max(-3.14159f,
				min(3.14159f, sWatchTimeAdjustRotation[i]));
		}
		sWatchTimeAdjustScale = max(0.1f, min(4.0f, sWatchTimeAdjustScale));
		LogInfo("VRPose watch time: loaded pos=(%.3f %.3f %.3f) "
			"rot=(%.1f %.1f %.1f) scale=%.3f\n",
			sWatchTimeAdjustPosition[0], sWatchTimeAdjustPosition[1],
			sWatchTimeAdjustPosition[2],
			sWatchTimeAdjustRotation[0] * 57.29578f,
			sWatchTimeAdjustRotation[1] * 57.29578f,
			sWatchTimeAdjustRotation[2] * 57.29578f,
			sWatchTimeAdjustScale);
	}

	static void SaveWatchTimeAdjust()
	{
		FILE *f = NULL;
		if (fopen_s(&f, "vr_watch_time_offset.txt", "w") == 0 && f) {
			fprintf(f, "%.6f %.6f %.6f %.6f %.6f %.6f %.6f\n",
				sWatchTimeAdjustPosition[0], sWatchTimeAdjustPosition[1],
				sWatchTimeAdjustPosition[2], sWatchTimeAdjustRotation[0],
				sWatchTimeAdjustRotation[1], sWatchTimeAdjustRotation[2],
				sWatchTimeAdjustScale);
			fclose(f);
			LogInfo("VRPose watch time: saved calibration\n");
		}
	}

	bool IsGameplayModeActive()
	{
		return InterlockedCompareExchange(&sGameplayModeActive, 0, 0) != 0;
	}

	void NotifyWorldPlacedUI()
	{
		InterlockedExchange(&sWorldPlacedUIEverSeen, 1);
		InterlockedExchange(&sLastWorldPlacedUIFrame, (LONG)G->frame_no);
	}

	static void PublishPadLeftStick(float x, float y)
	{
		if (x > 1.0f) x = 1.0f; if (x < -1.0f) x = -1.0f;
		if (y > 1.0f) y = 1.0f; if (y < -1.0f) y = -1.0f;
		const LONG nextX = (LONG)(x * 32767.0f);
		const LONG nextY = (LONG)(y * 32767.0f);
		const LONG previousX = InterlockedExchange(&sPadLX, nextX);
		const LONG previousY = InterlockedExchange(&sPadLY, nextY);
		InterlockedExchange(&sPadAxisUpdateTick, (LONG)GetTickCount());
		if ((previousX != 0 || previousY != 0) && nextX == 0 && nextY == 0) {
			InterlockedExchange(&sPadNeutralDispatchPulse, 1);
			static LONG sReleaseLogs = 0;
			if (InterlockedIncrement(&sReleaseLogs) <= 24)
				LogInfo("VRPose pad: left stick released (%ld,%ld)\n",
					previousX, previousY);
		}
	}

	typedef DWORD (WINAPI *tXInputGetState)(DWORD, XINPUT_STATE *);
	typedef DWORD (WINAPI *tXInputGetCapabilities)(DWORD, DWORD, XINPUT_CAPABILITIES *);
	static tXInputGetState trampoline_XInputGetState = NULL;
	static tXInputGetCapabilities trampoline_XInputGetCapabilities = NULL;
	typedef SHORT (WINAPI *tGetAsyncKeyState)(int);
	static tGetAsyncKeyState trampoline_GetAsyncKeyState = NULL;
	static HHOOK sControlKeyboardHook = NULL;
	static HWND sControlWindow = NULL;
	static WNDPROC sControlOriginalWndProc = NULL;
	typedef HHOOK (WINAPI *tSetWindowsHookExA)(int, HOOKPROC, HINSTANCE, DWORD);

	static LRESULT CALLBACK ControlKeyboardHookProc(int nCode, WPARAM wParam,
		LPARAM lParam)
	{
		if (nCode >= 0 && lParam) {
			const KBDLLHOOKSTRUCT *key = (const KBDLLHOOKSTRUCT *)lParam;
			static LONG sLoggedLabel = 0;
			static LONG sLoggedEvents = 0;
			const LONG label = InterlockedCompareExchange(&sControlProbeLabel, 0, 0);
			if (label != sLoggedLabel) { sLoggedLabel = label; sLoggedEvents = 0; }
			if (label != 0 && sLoggedEvents < 80) {
				LogInfo("VRPose control probe: low-level keyboard label=%s "
					"code=%lu vk=0x%02X scan=0x%02X flags=0x%08lX msg=0x%llX\n",
					label == 2 ? "SCRIPTED" : "ORDINARY",
					(unsigned long)nCode, (unsigned)key->vkCode,
					(unsigned)key->scanCode, (unsigned long)key->flags,
					(unsigned long long)wParam);
				sLoggedEvents++;
			}
		}
		return CallNextHookEx(sControlKeyboardHook, nCode, wParam, lParam);
	}

	static bool IsLoggedWindowMessage(UINT message)
	{
		return (message >= WM_KEYFIRST && message <= WM_KEYLAST) ||
			(message >= WM_MOUSEFIRST && message <= WM_MOUSELAST) ||
			message == WM_INPUT;
	}

	static LRESULT CALLBACK ControlWindowProc(HWND window, UINT message,
		WPARAM wParam, LPARAM lParam)
	{
		if (IsLoggedWindowMessage(message)) {
			static LONG sLoggedLabel = 0;
			static LONG sLoggedMessages = 0;
			const LONG label = InterlockedCompareExchange(&sControlProbeLabel, 0, 0);
			if (label != sLoggedLabel) { sLoggedLabel = label; sLoggedMessages = 0; }
			if (label != 0 && sLoggedMessages < 120) {
				LogInfo("VRPose control probe: window proc label=%s hwnd=%p "
					"msg=0x%04X wParam=0x%llX lParam=0x%llX\n",
					label == 2 ? "SCRIPTED" : "ORDINARY", window,
					(unsigned)message, (unsigned long long)(ULONG_PTR)wParam,
					(unsigned long long)(ULONG_PTR)lParam);
				sLoggedMessages++;
			}
		}
		return sControlOriginalWndProc
			? CallWindowProcA(sControlOriginalWndProc, window, message, wParam, lParam)
			: DefWindowProcA(window, message, wParam, lParam);
	}

	static BOOL CALLBACK FindMetroWindow(HWND window, LPARAM)
	{
		DWORD processId = 0;
		GetWindowThreadProcessId(window, &processId);
		if (processId == GetCurrentProcessId() && IsWindowVisible(window) &&
			GetWindow(window, GW_OWNER) == NULL) {
			sControlWindow = window;
			return FALSE;
		}
		return TRUE;
	}

	static void InstallControlWindowProbe()
	{
		if (!kReverseEngineeringDiagnosticsEnabled)
			return;
		static LONG sDone = 0;
		if (InterlockedCompareExchange(&sDone, 1, 0) != 0)
			return;
		EnumWindows(FindMetroWindow, 0);
		if (!sControlWindow) {
			InterlockedExchange(&sDone, 0);
			LogInfo("VRPose control probe: game window not found for subclass\n");
			return;
		}
		SetLastError(0);
		sControlOriginalWndProc = (WNDPROC)SetWindowLongPtrA(sControlWindow,
			GWLP_WNDPROC, (LONG_PTR)ControlWindowProc);
		LogInfo("VRPose control probe: window subclass %s hwnd=%p original=%p error=%lu\n",
			sControlOriginalWndProc ? "installed" : "FAILED", sControlWindow,
			sControlOriginalWndProc, (unsigned long)GetLastError());
		if (!sControlOriginalWndProc)
			InterlockedExchange(&sDone, 0);
	}
	typedef BOOL (WINAPI *tPeekMessageA)(LPMSG, HWND, UINT, UINT, UINT);
	typedef LRESULT (WINAPI *tDispatchMessageA)(const MSG *);
	static tPeekMessageA trampoline_PeekMessageA = NULL;
	static tDispatchMessageA trampoline_DispatchMessageA = NULL;
	typedef HRESULT (WINAPI *tDirectInput8Create)(HINSTANCE, DWORD, REFIID, LPVOID *, LPUNKNOWN);
	static tDirectInput8Create trampoline_DirectInput8Create = NULL;
	typedef HRESULT (STDMETHODCALLTYPE *tDirectInputGetDeviceState)(LPVOID, DWORD, LPVOID);
	typedef HRESULT (STDMETHODCALLTYPE *tDirectInputGetDeviceData)(LPVOID, DWORD,
		LPVOID, LPDWORD, DWORD);
	struct DirectInputDeviceHook {
		LPVOID object;
		uintptr_t vtable;
		tDirectInputGetDeviceState original;
		tDirectInputGetDeviceData dataOriginal;
	};
	static DirectInputDeviceHook sDirectInputDeviceHooks[16] = {};
	static volatile LONG sDirectInputDeviceHookCount = 0;

	static HRESULT STDMETHODCALLTYPE Hooked_DirectInputGetDeviceState(
		LPVOID object, DWORD cbData, LPVOID data)
	{
		tDirectInputGetDeviceState original = NULL;
		const LONG count = InterlockedCompareExchange(&sDirectInputDeviceHookCount, 0, 0);
		for (LONG i = 0; i < count && i < (LONG)_countof(sDirectInputDeviceHooks); ++i) {
			if (sDirectInputDeviceHooks[i].object == object) {
				original = sDirectInputDeviceHooks[i].original;
				break;
			}
		}
		const HRESULT hr = original ? original(object, cbData, data) : E_FAIL;

		static LONG sLoggedLabel = 0;
		static LONG sLoggedPolls = 0;
		const LONG label = InterlockedCompareExchange(&sControlProbeLabel, 0, 0);
		if (label != sLoggedLabel) { sLoggedLabel = label; sLoggedPolls = 0; }
		if (sLoggedPolls < 12 && data && cbData) {
			BYTE sample[16] = {};
			const DWORD copy = cbData < sizeof(sample) ? cbData : (DWORD)sizeof(sample);
			memcpy(sample, data, copy);
			LogInfo("VRPose control probe: DirectInput GetDeviceState label=%s "
				"object=%p cbData=%lu hr=0x%08lX bytes="
				"%02X %02X %02X %02X %02X %02X %02X %02X "
				"%02X %02X %02X %02X %02X %02X %02X %02X\n",
				label == 2 ? "SCRIPTED" : (label == 1 ? "ORDINARY" : "UNLABELED"),
				object, (unsigned long)cbData, (unsigned long)hr,
				sample[0], sample[1], sample[2], sample[3], sample[4], sample[5], sample[6], sample[7],
				sample[8], sample[9], sample[10], sample[11], sample[12], sample[13], sample[14], sample[15]);
			sLoggedPolls++;
		}
		return hr;
	}

	static HRESULT STDMETHODCALLTYPE Hooked_DirectInputGetDeviceData(
		LPVOID object, DWORD cbObjectData, LPVOID rgdod, LPDWORD pdwInOut,
		DWORD flags)
	{
		tDirectInputGetDeviceData original = NULL;
		const LONG count = InterlockedCompareExchange(&sDirectInputDeviceHookCount, 0, 0);
		for (LONG i = 0; i < count && i < (LONG)_countof(sDirectInputDeviceHooks); ++i) {
			if (sDirectInputDeviceHooks[i].object == object) {
				original = sDirectInputDeviceHooks[i].dataOriginal;
				break;
			}
		}
		const DWORD requested = pdwInOut ? *pdwInOut : 0;
		const HRESULT hr = original
			? original(object, cbObjectData, rgdod, pdwInOut, flags) : E_FAIL;
		const DWORD returned = pdwInOut ? *pdwInOut : 0;
		static LONG sLogged = 0;
		if (sLogged < 32) {
			LogInfo("VRPose control probe: DirectInput GetDeviceData object=%p "
				"cbObjectData=%lu requested=%lu returned=%lu flags=0x%08lX hr=0x%08lX\n",
				object, (unsigned long)cbObjectData, (unsigned long)requested,
				(unsigned long)returned, (unsigned long)flags, (unsigned long)hr);
			sLogged++;
		}
		return hr;
	}

	static bool PatchMetroImport(const char *dllName, const char *functionName,
		void *replacement, void **original)
	{
		BYTE *base = (BYTE *)GetModuleHandleA(NULL);
		if (!base)
			return false;
		const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)base;
		if (dos->e_magic != IMAGE_DOS_SIGNATURE)
			return false;
		const IMAGE_NT_HEADERS64 *nt =
			(const IMAGE_NT_HEADERS64 *)(base + dos->e_lfanew);
		if (nt->Signature != IMAGE_NT_SIGNATURE)
			return false;
		const IMAGE_DATA_DIRECTORY &dir =
			nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
		if (!dir.VirtualAddress || !dir.Size)
			return false;

		IMAGE_IMPORT_DESCRIPTOR *imports =
			(IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress);
		for (; imports->Name; ++imports) {
			const char *importDll = (const char *)(base + imports->Name);
			if (_stricmp(importDll, dllName) != 0)
				continue;
			IMAGE_THUNK_DATA64 *names = (IMAGE_THUNK_DATA64 *)
				(base + (imports->OriginalFirstThunk ? imports->OriginalFirstThunk
					: imports->FirstThunk));
			IMAGE_THUNK_DATA64 *iat =
				(IMAGE_THUNK_DATA64 *)(base + imports->FirstThunk);
			for (; names->u1.AddressOfData; ++names, ++iat) {
				if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal))
					continue;
				const IMAGE_IMPORT_BY_NAME *name =
					(const IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData);
				if (strcmp((const char *)name->Name, functionName) != 0)
					continue;
				if (original)
					*original = (void *)(uintptr_t)iat->u1.Function;
				DWORD oldProtect = 0;
				if (!VirtualProtect(&iat->u1.Function, sizeof(iat->u1.Function),
					PAGE_READWRITE, &oldProtect))
					return false;
				iat->u1.Function = (ULONGLONG)(uintptr_t)replacement;
				VirtualProtect(&iat->u1.Function, sizeof(iat->u1.Function),
					oldProtect, &oldProtect);
				return true;
			}
		}
		return false;
	}

	static SHORT WINAPI Hooked_GetAsyncKeyState(int vKey)
	{
		const SHORT result = trampoline_GetAsyncKeyState
			? trampoline_GetAsyncKeyState(vKey) : 0;
		static LONG sLoggedLabel = 0;
		static unsigned char sSeen[256] = {};
		const LONG label = InterlockedCompareExchange(&sControlProbeLabel, 0, 0);
		if (label != sLoggedLabel) {
			sLoggedLabel = label;
			memset(sSeen, 0, sizeof(sSeen));
		}
		if (label != 0 && vKey >= 0 && vKey < 256 && !sSeen[vKey]) {
			sSeen[vKey] = 1;
			LogInfo("VRPose control probe: GetAsyncKeyState label=%s vk=0x%02X down=%d\n",
				label == 2 ? "SCRIPTED" : "ORDINARY", vKey,
				(result & 0x8000) != 0);
		}
		return result;
	}

	static bool IsControlWindowMessage(UINT message)
	{
		return (message >= WM_KEYFIRST && message <= WM_KEYLAST) ||
			(message >= WM_MOUSEFIRST && message <= WM_MOUSELAST) ||
			message == WM_INPUT;
	}

	static void LogControlWindowMessage(const char *source, const MSG *msg)
	{
		if (!msg || !IsControlWindowMessage(msg->message))
			return;
		static LONG sLoggedLabel = 0;
		static LONG sLoggedMessages = 0;
		const LONG label = InterlockedCompareExchange(&sControlProbeLabel, 0, 0);
		if (label != sLoggedLabel) { sLoggedLabel = label; sLoggedMessages = 0; }
		if (label != 0 && sLoggedMessages < 100) {
			LogInfo("VRPose control probe: %s label=%s msg=0x%04X wParam=0x%llX "
				"lParam=0x%llX hwnd=%p\n", source,
				label == 2 ? "SCRIPTED" : "ORDINARY", (unsigned)msg->message,
				(unsigned long long)(ULONG_PTR)msg->wParam,
				(unsigned long long)(ULONG_PTR)msg->lParam, msg->hwnd);
			sLoggedMessages++;
		}
	}

	static BOOL WINAPI Hooked_PeekMessageA(LPMSG msg, HWND hwnd, UINT minFilter,
		UINT maxFilter, UINT removeMsg)
	{
		const BOOL result = trampoline_PeekMessageA
			? trampoline_PeekMessageA(msg, hwnd, minFilter, maxFilter, removeMsg) : FALSE;
		if (result)
			LogControlWindowMessage("PeekMessageA", msg);
		return result;
	}

	static LRESULT WINAPI Hooked_DispatchMessageA(const MSG *msg)
	{
		LogControlWindowMessage("DispatchMessageA", msg);
		return trampoline_DispatchMessageA ? trampoline_DispatchMessageA(msg) : 0;
	}

	static void InstallKeyboardInputProbe()
	{
		if (!kReverseEngineeringDiagnosticsEnabled)
			return;
		static bool sDone = false;
		if (sDone)
			return;
		HINSTANCE h = GetModuleHandleA("user32.dll");
		if (!h)
			return;
		sDone = true;
		const int fail = InstallHookLate(h, "GetAsyncKeyState",
			(void **)&trampoline_GetAsyncKeyState, Hooked_GetAsyncKeyState);
		const int failPeek = InstallHookLate(h, "PeekMessageA",
			(void **)&trampoline_PeekMessageA, Hooked_PeekMessageA);
		const int failDispatch = InstallHookLate(h, "DispatchMessageA",
			(void **)&trampoline_DispatchMessageA, Hooked_DispatchMessageA);
		LogInfo("VRPose control probe: keyboard/message hooks GetAsyncKeyState=%s "
			"PeekMessageA=%s DispatchMessageA=%s\n",
			fail ? "FAILED" : "installed",
			failPeek ? "FAILED" : "installed",
			failDispatch ? "FAILED" : "installed");
		if (!sControlKeyboardHook) {
			tSetWindowsHookExA setHook = (tSetWindowsHookExA)GetProcAddress(h,
				"SetWindowsHookExA");
			if (setHook)
				sControlKeyboardHook = setHook(WH_KEYBOARD_LL,
					(HOOKPROC)ControlKeyboardHookProc, GetModuleHandleA(NULL), 0);
			LogInfo("VRPose control probe: parallel low-level keyboard hook %s\n",
				sControlKeyboardHook ? "installed" : "FAILED");
		}
	}

	static HRESULT WINAPI Hooked_DirectInput8Create(HINSTANCE instance, DWORD version,
		REFIID riid, LPVOID *out, LPUNKNOWN outer)
	{
		const HRESULT hr = trampoline_DirectInput8Create
			? trampoline_DirectInput8Create(instance, version, riid, out, outer)
			: E_FAIL;
		LogInfo("VRPose control probe: DirectInput8Create hr=0x%08lX version=0x%08lX "
			"riid.Data1=%08lX object=%p\n", (unsigned long)hr,
			(unsigned long)version, (unsigned long)riid.Data1,
			out ? *out : NULL);
		return hr;
	}

	static void InstallDirectInputProbe()
	{
		if (!kReverseEngineeringDiagnosticsEnabled)
			return;
		static bool sDone = false;
		if (sDone)
			return;
		HINSTANCE h = GetModuleHandleA("dinput8.dll");
		if (!h)
			return;
		sDone = true;
		const int fail = InstallHookLate(h, "DirectInput8Create",
			(void **)&trampoline_DirectInput8Create, Hooked_DirectInput8Create);
		LogInfo("VRPose control probe: DirectInput8Create hook %s\n",
			fail ? "FAILED" : "installed");
	}

	static void ScanExistingDirectInputDevicesOnce()
	{
		if (!kReverseEngineeringDiagnosticsEnabled)
			return;
		static LONG sStarted = 0;
		if (InterlockedCompareExchange(&sStarted, 1, 0) != 0)
			return;

		// Metro may create DirectInput before our late export hook is installed.
		// Search committed private writable memory for existing COM device objects.
		// This is read-only diagnostics: it does not patch the object or vtable.
		CreateThread(NULL, 0, [](LPVOID) -> DWORD {
			HMODULE dinput = NULL;
			for (int i = 0; i < 100 && !dinput; ++i) {
				dinput = GetModuleHandleA("dinput8.dll");
				if (!dinput)
					Sleep(100);
			}
			if (!dinput) {
				LogInfo("VRPose control probe: existing DirectInput scan skipped (dinput8.dll not loaded)\n");
				return 0;
			}

			const BYTE *image = (const BYTE *)dinput;
			const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)image;
			if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
				LogInfo("VRPose control probe: existing DirectInput scan skipped (bad image)\n");
				return 0;
			}
			const IMAGE_NT_HEADERS64 *nt =
				(const IMAGE_NT_HEADERS64 *)(image + dos->e_lfanew);
			if (nt->Signature != IMAGE_NT_SIGNATURE) {
				LogInfo("VRPose control probe: existing DirectInput scan skipped (bad PE)\n");
				return 0;
			}
			const uintptr_t imageBegin = (uintptr_t)image;
			const uintptr_t imageEnd = imageBegin + nt->OptionalHeader.SizeOfImage;
			const SIZE_T ptrSize = sizeof(uintptr_t);
			uintptr_t address = 0;
			SIZE_T regions = 0;
			SIZE_T candidates = 0;

			while (address < (uintptr_t)0x00007FFFFFFFFFFFULL) {
				MEMORY_BASIC_INFORMATION mbi = {};
				if (VirtualQuery((const void *)address, &mbi, sizeof(mbi)) != sizeof(mbi))
					break;
				const uintptr_t next = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
				if (next <= address)
					break;
				address = next;
				if (mbi.State != MEM_COMMIT ||
					!(mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY |
						PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) ||
					(mbi.Protect & PAGE_GUARD))
					continue;

				++regions;
				const uintptr_t begin = (uintptr_t)mbi.BaseAddress;
				const uintptr_t end = begin + mbi.RegionSize;
				for (uintptr_t object = (begin + ptrSize - 1) & ~(ptrSize - 1);
					 object + ptrSize * 10 <= end; object += ptrSize) {
					uintptr_t vtable = 0;
					SIZE_T got = 0;
					if (!ReadProcessMemory(GetCurrentProcess(), (const void *)object,
						&vtable, sizeof(vtable), &got) || got != sizeof(vtable) ||
						vtable < imageBegin || vtable >= imageEnd)
						continue;

					uintptr_t getDeviceState = 0;
					if (!ReadProcessMemory(GetCurrentProcess(),
						(const void *)(vtable + ptrSize * 9), &getDeviceState,
						sizeof(getDeviceState), &got) || got != sizeof(getDeviceState) ||
						getDeviceState < imageBegin || getDeviceState >= imageEnd)
						continue;

					++candidates;
					LogInfo("VRPose control probe: DirectInput device candidate "
						"object=%p vtable=%p GetDeviceState=%p\n",
						(const void *)object, (const void *)vtable,
						(const void *)getDeviceState);
					for (int slotIndex = 7; slotIndex <= 15; ++slotIndex) {
						uintptr_t method = 0;
						if (ReadProcessMemory(GetCurrentProcess(),
							(const void *)(vtable + ptrSize * slotIndex), &method,
							sizeof(method), &got) && got == sizeof(method)) {
							LogInfo("VRPose control probe: DirectInput vtable slot=%d method=%p "
								"in_dinput=%s\n", slotIndex, (const void *)method,
								(method >= imageBegin && method < imageEnd) ? "yes" : "no");
						}
					}

					bool alreadyHooked = false;
					const LONG hookCount = InterlockedCompareExchange(
						&sDirectInputDeviceHookCount, 0, 0);
					for (LONG i = 0; i < hookCount && i < (LONG)_countof(sDirectInputDeviceHooks); ++i) {
						if (sDirectInputDeviceHooks[i].object == (LPVOID)object ||
							sDirectInputDeviceHooks[i].vtable == vtable) {
							alreadyHooked = true;
							break;
						}
					}
					if (!alreadyHooked && hookCount < (LONG)_countof(sDirectInputDeviceHooks)) {
						DWORD oldProtect = 0;
						void **slot = (void **)(vtable + ptrSize * 9);
						if (VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &oldProtect)) {
							sDirectInputDeviceHooks[hookCount].object = (LPVOID)object;
							sDirectInputDeviceHooks[hookCount].vtable = vtable;
							sDirectInputDeviceHooks[hookCount].original =
								(tDirectInputGetDeviceState)getDeviceState;
							InterlockedExchangePointer((PVOID *)slot,
								(PVOID)Hooked_DirectInputGetDeviceState);
							VirtualProtect(slot, sizeof(*slot), oldProtect, &oldProtect);
							InterlockedExchange(&sDirectInputDeviceHookCount, hookCount + 1);
							LogInfo("VRPose control probe: DirectInput GetDeviceState hook installed "
								"object=%p vtable=%p\n", (const void *)object,
								(const void *)vtable);
						} else {
							LogInfo("VRPose control probe: DirectInput vtable not writable "
								"object=%p vtable=%p error=%lu\n", (const void *)object,
								(const void *)vtable, (unsigned long)GetLastError());
						}
					}

					LONG hookIndex = -1;
					const LONG currentHooks = InterlockedCompareExchange(
						&sDirectInputDeviceHookCount, 0, 0);
					for (LONG i = 0; i < currentHooks && i < (LONG)_countof(sDirectInputDeviceHooks); ++i) {
						if (sDirectInputDeviceHooks[i].object == (LPVOID)object) {
							hookIndex = i;
							break;
						}
					}
					if (hookIndex >= 0) {
						void **dataSlot = (void **)(vtable + ptrSize * 10);
						uintptr_t dataMethod = 0;
						if (ReadProcessMemory(GetCurrentProcess(), dataSlot, &dataMethod,
							sizeof(dataMethod), &got) && got == sizeof(dataMethod) &&
							dataMethod != 0) {
							LogInfo("VRPose control probe: DirectInput GetDeviceData entry "
								"object=%p vtable=%p method=%p in_dinput=%s\n",
								(const void *)object, (const void *)vtable,
								(const void *)dataMethod,
								(dataMethod >= imageBegin && dataMethod < imageEnd) ? "yes" : "no");
							DWORD oldProtect = 0;
							if (VirtualProtect(dataSlot, sizeof(*dataSlot), PAGE_READWRITE, &oldProtect)) {
								sDirectInputDeviceHooks[hookIndex].dataOriginal =
									(tDirectInputGetDeviceData)dataMethod;
								InterlockedExchangePointer((PVOID *)dataSlot,
									(PVOID)Hooked_DirectInputGetDeviceData);
								VirtualProtect(dataSlot, sizeof(*dataSlot), oldProtect, &oldProtect);
								LogInfo("VRPose control probe: DirectInput GetDeviceData hook installed "
									"object=%p vtable=%p\n", (const void *)object,
									(const void *)vtable);
							} else {
								LogInfo("VRPose control probe: DirectInput GetDeviceData vtable "
									"not writable object=%p error=%lu\n", (const void *)object,
									(unsigned long)GetLastError());
							}
						}
					}
					if (candidates >= 64)
						break;
				}
				if (candidates >= 64)
					break;
			}

			LogInfo("VRPose control probe: existing DirectInput scan complete "
				"regions=%llu candidates=%llu\n",
				(unsigned long long)regions, (unsigned long long)candidates);
			return 0;
		}, NULL, 0, NULL);
	}

	static void ScanMetroInputCallsitesOnce()
	{
		if (!kReverseEngineeringDiagnosticsEnabled)
			return;
		static LONG sStarted = 0;
		if (InterlockedCompareExchange(&sStarted, 1, 0) != 0)
			return;
		CreateThread(NULL, 0, [](LPVOID) -> DWORD {
			HMODULE module = GetModuleHandleA(NULL);
			if (!module)
				return 0;
			BYTE *base = (BYTE *)module;
			IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
			if (dos->e_magic != IMAGE_DOS_SIGNATURE)
				return 0;
			IMAGE_NT_HEADERS64 *nt =
				(IMAGE_NT_HEADERS64 *)(base + dos->e_lfanew);
			if (nt->Signature != IMAGE_NT_SIGNATURE)
				return 0;

			struct ImportTarget { const char *name; uintptr_t address; } targets[8] = {};
			const char *wanted[] = {
				"DispatchMessageA", "PeekMessageA", "GetCursorPos",
				"GetAsyncKeyState", "SetWindowsHookExA", "CallNextHookEx"
			};
			int targetCount = 0;
			const IMAGE_DATA_DIRECTORY &dir =
				nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
			IMAGE_IMPORT_DESCRIPTOR *imports =
				(IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress);
			for (; imports->Name; ++imports) {
				if (_stricmp((const char *)(base + imports->Name), "USER32.dll") != 0)
					continue;
				IMAGE_THUNK_DATA64 *names = (IMAGE_THUNK_DATA64 *)
					(base + (imports->OriginalFirstThunk ? imports->OriginalFirstThunk
						: imports->FirstThunk));
				IMAGE_THUNK_DATA64 *iat =
					(IMAGE_THUNK_DATA64 *)(base + imports->FirstThunk);
				for (; names->u1.AddressOfData; ++names, ++iat) {
					if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal))
						continue;
					const char *name = (const char *)((IMAGE_IMPORT_BY_NAME *)
						(base + names->u1.AddressOfData))->Name;
					for (const char *wantedName : wanted) {
						if (strcmp(name, wantedName) == 0 && targetCount < (int)_countof(targets)) {
							targets[targetCount++] = { wantedName, (uintptr_t)&iat->u1.Function };
						}
					}
				}
			}

			LogInfo("VRPose control probe: live input callsite scan targets=%d\n", targetCount);
			IMAGE_SECTION_HEADER *sections = IMAGE_FIRST_SECTION(nt);
			for (WORD s = 0; s < nt->FileHeader.NumberOfSections; ++s) {
				if (memcmp(sections[s].Name, ".text", 5) != 0)
					continue;
				BYTE *code = base + sections[s].VirtualAddress;
				const SIZE_T size = sections[s].Misc.VirtualSize;
				for (SIZE_T i = 0; i + 7 <= size; ++i) {
					uintptr_t target = 0;
					SIZE_T instructionSize = 0;
					if (code[i] == 0xFF && (code[i + 1] == 0x15 || code[i + 1] == 0x25)) {
						const LONG disp = *(const LONG *)(code + i + 2);
						target = (uintptr_t)(code + i + 6) + disp;
						instructionSize = 6;
					} else if (code[i] == 0x48 && code[i + 1] == 0x8B && code[i + 2] == 0x05) {
						const LONG disp = *(const LONG *)(code + i + 3);
						target = (uintptr_t)(code + i + 7) + disp;
						instructionSize = 7;
					}
					if (!instructionSize)
						continue;
					for (int t = 0; t < targetCount; ++t) {
						if (target == targets[t].address) {
							LogInfo("VRPose control probe: live input callsite %s at %p "
								"opcode=%02X %02X\n", targets[t].name,
								code + i, code[i], code[i + 1]);
							DWORD64 imageBase = 0;
							PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(
								(DWORD64)(code + i), &imageBase, NULL);
							if (function) {
								if (strcmp(targets[t].name, "PeekMessageA") == 0 &&
									function->EndAddress - function->BeginAddress > 0x200) {
									InterlockedExchangePointer(&sLiveInputDispatcherAddress,
										(PVOID)(imageBase + function->BeginAddress));
								}
								LogInfo("VRPose control probe: input callsite function "
									"%s range=%p-%p\n", targets[t].name,
									(const void *)(imageBase + function->BeginAddress),
									(const void *)(imageBase + function->EndAddress));
								const BYTE *functionBytes =
									(const BYTE *)(imageBase + function->BeginAddress);
								BYTE bytes[48] = {};
								memcpy(bytes, functionBytes, sizeof(bytes));
								if (strcmp(targets[t].name, "SetWindowsHookExA") == 0) {
									for (int j = 0; j + 7 <= (int)sizeof(bytes); ++j) {
										if (bytes[j] == 0x48 && bytes[j + 1] == 0x8D &&
											bytes[j + 2] == 0x15) {
											const LONG disp = *(const LONG *)(functionBytes + j + 3);
											PVOID callback = (PVOID)(functionBytes + j + 7 + disp);
											InterlockedExchangePointer(&sLiveKeyboardHookCallbackAddress, callback);
											LogInfo("VRPose control probe: live keyboard hook callback=%p "
												"from SetWindowsHookExA function=%p\n", callback,
												functionBytes);
											BYTE callbackBytes[64] = {};
											memcpy(callbackBytes, callback, sizeof(callbackBytes));
											int gateCount = 0;
											for (int j = 0; j + 6 <= (int)sizeof(callbackBytes); ++j) {
												if (callbackBytes[j] == 0x38 && callbackBytes[j + 1] == 0x0D) {
													const LONG disp = *(const LONG *)(callbackBytes + j + 2);
													PVOID gate = (PVOID)(callbackBytes + j + 6 + disp);
													if (gateCount++ == 0)
														InterlockedExchangePointer(&sLiveKeyboardGateA, gate);
													else if (gateCount == 2)
														InterlockedExchangePointer(&sLiveKeyboardGateB, gate);
												}
											}
											if (gateCount >= 2)
												LogInfo("VRPose control probe: keyboard gate bytes at %p and %p\n",
													InterlockedCompareExchangePointer(&sLiveKeyboardGateA, NULL, NULL),
													InterlockedCompareExchangePointer(&sLiveKeyboardGateB, NULL, NULL));
											for (int row = 0; row < 4; ++row) {
												LogInfo("VRPose control probe: keyboard callback bytes %p +%02X: "
													"%02X %02X %02X %02X %02X %02X %02X %02X "
													"%02X %02X %02X %02X %02X %02X %02X %02X\n",
													callback, row * 16,
													callbackBytes[row * 16 + 0], callbackBytes[row * 16 + 1], callbackBytes[row * 16 + 2], callbackBytes[row * 16 + 3],
													callbackBytes[row * 16 + 4], callbackBytes[row * 16 + 5], callbackBytes[row * 16 + 6], callbackBytes[row * 16 + 7],
													callbackBytes[row * 16 + 8], callbackBytes[row * 16 + 9], callbackBytes[row * 16 + 10], callbackBytes[row * 16 + 11],
													callbackBytes[row * 16 + 12], callbackBytes[row * 16 + 13], callbackBytes[row * 16 + 14], callbackBytes[row * 16 + 15]);
											}
											DWORD64 callbackImageBase = 0;
											PRUNTIME_FUNCTION callbackFunction = RtlLookupFunctionEntry(
												(DWORD64)callback, &callbackImageBase, NULL);
											if (callbackFunction)
												LogInfo("VRPose control probe: keyboard callback function "
													"range=%p-%p\n",
													(const void *)(callbackImageBase + callbackFunction->BeginAddress),
													(const void *)(callbackImageBase + callbackFunction->EndAddress));
											break;
										}
									}
								}
								for (int row = 0; row < 3; ++row) {
									LogInfo("VRPose control probe: function bytes %s "
										"%p +%02X: %02X %02X %02X %02X %02X %02X %02X %02X "
										"%02X %02X %02X %02X %02X %02X %02X %02X\n",
										targets[t].name, functionBytes, row * 16,
										bytes[row * 16 + 0], bytes[row * 16 + 1], bytes[row * 16 + 2], bytes[row * 16 + 3],
										bytes[row * 16 + 4], bytes[row * 16 + 5], bytes[row * 16 + 6], bytes[row * 16 + 7],
										bytes[row * 16 + 8], bytes[row * 16 + 9], bytes[row * 16 + 10], bytes[row * 16 + 11],
										bytes[row * 16 + 12], bytes[row * 16 + 13], bytes[row * 16 + 14], bytes[row * 16 + 15]);
								}
							}
						}
					}
				}
			}
			return 0;
		}, NULL, 0, NULL);
	}

	// The source-writer probe proved that scripted and ordinary camera updates
	// converge at the same final function.  Find the code that calls that
	// function instead: this is the point where the command stream is produced
	// or interpreted, and is therefore a better place to identify ownership.
	static void ScanCameraSourceWriterCallersOnce()
	{
		if (!kReverseEngineeringDiagnosticsEnabled)
			return;
		static LONG sStarted = 0;
		if (InterlockedCompareExchange(&sStarted, 1, 0) != 0)
			return;
		CreateThread(NULL, 0, [](LPVOID) -> DWORD {
			HMODULE module = GetModuleHandleA(NULL);
			if (!module)
				return 0;
			BYTE *base = (BYTE *)module;
			IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
			if (dos->e_magic != IMAGE_DOS_SIGNATURE)
				return 0;
			IMAGE_NT_HEADERS64 *nt =
				(IMAGE_NT_HEADERS64 *)(base + dos->e_lfanew);
			if (nt->Signature != IMAGE_NT_SIGNATURE)
				return 0;

			const uintptr_t writer = (uintptr_t)base + 0x7EEAC6;
			unsigned matches = 0;
			LogInfo("VRPose ownership probe: scanning live .text for callers of "
				"source writer %p (metro.exe+0x7EEAC6)\n", (void *)writer);

			IMAGE_SECTION_HEADER *sections = IMAGE_FIRST_SECTION(nt);
			for (WORD s = 0; s < nt->FileHeader.NumberOfSections; ++s) {
				if (memcmp(sections[s].Name, ".text", 5) != 0)
					continue;
				BYTE *code = base + sections[s].VirtualAddress;
				const SIZE_T size = sections[s].Misc.VirtualSize;
				for (SIZE_T i = 0; i + 5 <= size; ++i) {
					if (code[i] != 0xE8)
						continue;
					const LONG disp = *(const LONG *)(code + i + 1);
					const uintptr_t target = (uintptr_t)(code + i + 5) + disp;
					if (target != writer)
						continue;

					char callWhere[MAX_PATH + 64];
					DescribeCodeAddress((DWORD64)(code + i), callWhere, sizeof(callWhere));
					DWORD64 imageBase = 0;
					PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(
						(DWORD64)(code + i), &imageBase, NULL);
					if (function) {
						LogInfo("VRPose ownership probe: source-writer CALL %s "
							"function=%p-%p\n", callWhere,
							(const void *)(imageBase + function->BeginAddress),
							(const void *)(imageBase + function->EndAddress));
					} else {
						LogInfo("VRPose ownership probe: source-writer CALL %s "
							"function=unresolved\n", callWhere);
					}
					if (++matches >= 32)
						break;
				}
				if (matches >= 32)
					break;
			}
			LogInfo("VRPose ownership probe: source-writer caller scan complete "
				"matches=%u\n", matches);
			return 0;
		}, NULL, 0, NULL);
	}

	typedef void (*tFlashlightInputDispatch)(void *manager, int button, int extra);
	static tFlashlightInputDispatch trampoline_FlashlightInputDispatch = NULL;

	static void Hooked_FlashlightInputDispatch(void *manager, int button, int extra)
	{
		if (manager && button == 0x10D) {
			int action = -1;
			unsigned listenerCount = 0;
			void *listener = NULL;
			void *handler = NULL;
			__try {
				action = *(int *)((BYTE *)manager + 0x2C + button * 4);
				listenerCount = *(WORD *)((BYTE *)manager + 0x5DA);
				void *listenerArray = *(void **)((BYTE *)manager + 0x5D0);
				if (listenerArray && listenerCount) {
					listener = *(void **)((BYTE *)listenerArray + (listenerCount - 1) * 16);
					if (listener)
						handler = (*(void ***)listener)[0];
				}
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				LogInfo("VRPose gas-mask dispatch: failed to read X action/listener state\n");
			}

			char handlerDescription[160] = {};
			DescribeCodeAddress((DWORD64)handler, handlerDescription,
				sizeof(handlerDescription));
			LogInfo("VRPose gas-mask dispatch: manager=%p button=0x%X extra=%d "
				"action=%d (0x%X) listeners=%u listener=%p handler=%p %s\n",
				manager, button, extra, action, action, listenerCount, listener,
				handler, handlerDescription);

			static LONG dumped = 0;
			if (handler && InterlockedCompareExchange(&dumped, 1, 0) == 0) {
				HMODULE module = GetModuleHandleA(NULL);
				DWORD64 imageBase = 0;
				PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(
					(DWORD64)handler, &imageBase, NULL);
				if (module && function && imageBase == (DWORD64)module) {
					const DWORD size = function->EndAddress - function->BeginAddress;
					char filename[96] = {};
					sprintf_s(filename, "gas_mask_native_handler_%08X_%08X.bin",
						function->BeginAddress, function->EndAddress);
					FILE *file = NULL;
					if (size && size <= 0x10000
						&& fopen_s(&file, filename, "wb") == 0 && file) {
						fwrite((BYTE *)module + function->BeginAddress, 1, size, file);
						fclose(file);
						LogInfo("VRPose gas-mask dispatch: dumped handler +0x%X-0x%X to %s\n",
							function->BeginAddress, function->EndAddress, filename);
					}
				}
			}
		}
		trampoline_FlashlightInputDispatch(manager, button, extra);
	}

	static void InstallFlashlightInputDispatchProbe()
	{
		if (!kFlashlightInputDispatchProbeEnabled)
			return;
		static bool tried = false;
		if (tried)
			return;
		tried = true;
		const BYTE *base = (const BYTE *)GetModuleHandleA(NULL);
		if (!base)
			return;
		static const BYTE expected[] = {
			0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10,
			0x48, 0x89, 0x74, 0x24, 0x18, 0x57
		};
		void *function = (void *)(base + 0x8F7CD0);
		if (memcmp(function, expected, sizeof(expected)) != 0) {
			LogInfo("VRPose gas-mask dispatch: hook refused - unexpected bytes at +0x8F7CD0\n");
			return;
		}
		SIZE_T hookId = 0;
		const DWORD error = cHookMgr.Hook(&hookId,
			(LPVOID *)&trampoline_FlashlightInputDispatch, function,
			Hooked_FlashlightInputDispatch);
		if (error || !trampoline_FlashlightInputDispatch)
			LogInfo("VRPose gas-mask dispatch: hook FAILED at +0x8F7CD0 (err=0x%x)\n",
				error);
		else
			LogInfo("VRPose gas-mask dispatch: X action-listener probe installed at +0x8F7CD0\n");
	}

	// Metro's player-action queue is downstream of XInput, bindings, and the
	// equipment wheel. Hooking it gives the gesture a live player object and
	// lets us enqueue wpn_light (native action 70) without synthesising either
	// controller or keyboard input.
	typedef void (*tMetroPlayerActionQueue)(void *playerActions, int action,
		int sourceButton, int flags, int alternate);
	static tMetroPlayerActionQueue trampoline_MetroPlayerActionQueue = NULL;
	static volatile PVOID sMetroPlayerActions = NULL;
	static volatile LONG sMetroPlayerActionFlags = 0;
	static volatile PVOID sMetroInputManager = NULL;
	static volatile LONG sPendingNativeFlashlightAction = -1;
	static volatile LONG sPendingNativeFilterAction = 0;
	static volatile LONG sPendingNativeChargerAction = 0;
	static volatile LONG sPendingNativePneumaticAction = 0;
	static volatile PVOID sLivePneumaticWeapon = NULL;
	static volatile LONG sLivePneumaticWeaponTick = 0;
	static const unsigned kPneumaticWeaponRegistrySize = 32;
	static volatile PVOID sPneumaticWeaponRegistry[kPneumaticWeaponRegistrySize] = {};
	static volatile LONG sPneumaticWeaponRegistryCursor = 0;
	typedef void *(*tMetroPneumaticConstructor)(void *weapon);
	static tMetroPneumaticConstructor trampoline_MetroPneumaticConstructor = NULL;
	typedef void (*tMetroPneumaticUpdateSsss)(void *weapon);
	static tMetroPneumaticUpdateSsss trampoline_MetroPneumaticUpdateSsss = NULL;
	static volatile LONG sPneumaticQueueTraceDeadlineTick = 0;
	static volatile PVOID sNativePneumaticActionOwner = NULL;
	static volatile PVOID sPneumaticTranslatedActionDispatcher = NULL;
	static volatile LONG sPneumaticTranslatedReplayCountdown = 0;
	typedef void (*tMetroTranslatedActionDispatch)(void *dispatcher, int action,
		int sourceButton, int flags, int alternate);
	static tMetroTranslatedActionDispatch trampoline_MetroTranslatedActionDispatch = NULL;
	typedef void (*tMetroPneumaticPlayerAction)(void *playerActionOwner);
	static tMetroPneumaticPlayerAction trampoline_MetroPneumaticPlayerAction = NULL;
	typedef int (*tMetroActionListenerDispatch)(void *dispatcher, int action,
		int flags, int alternate, int extraA, void *extraB);
	static tMetroActionListenerDispatch trampoline_MetroActionListenerDispatch = NULL;
	typedef int (*tReplaceGasMaskFilter)(void *filterOwner, int forced);
	static tReplaceGasMaskFilter trampoline_ReplaceGasMaskFilter = NULL;
	static volatile PVOID sNativeFilterOwner = NULL;
	typedef void (*tNativeGasMaskDirection)(void *player, int putOn,
		int takeOff);
	static tNativeGasMaskDirection trampoline_NativeGasMaskDirection = NULL;
	typedef int (*tNativeGasMaskEquipmentAction)(void *equipment, int pressed);
	static tNativeGasMaskEquipmentAction trampoline_NativeGasMaskEquipmentAction = NULL;
	static volatile LONG sGasMaskGestureDispatchCountdown = 0;
	static volatile LONG sGasMaskMotionTraceCountdown = 0;
	static volatile LONG sGasMaskWornStateKnown = 0;
	static volatile LONG sGasMaskAssumedWorn = 0;
	static volatile PVOID sGasMaskStatePlayer = NULL;
	static volatile LONG sGasMaskVisualProbePhase = 0;
	static volatile LONG sGasMaskVisualProbeCountdown = 0;
	static volatile LONG sGasMaskStableProbePhase = 0;
	static volatile LONG sGasMaskStableProbeCountdown = 0;
	static volatile PVOID sGasMaskStableProbePlayer = NULL;
	static volatile PVOID sGasMaskStableProbeObject = NULL;
	static unsigned char sGasMaskStableOffPlayer[0x1800] = {};
	static unsigned char sGasMaskStableWornPlayer[0x1800] = {};
	static unsigned char sGasMaskStableOffObject[0x900] = {};
	static unsigned char sGasMaskStableWornObject[0x900] = {};
	static volatile LONG sGasMaskUnknownProbeSequence = 0;
	static volatile PVOID sGasMaskUnknownPostActionPlayer = NULL;
	static volatile LONG sGasMaskUnknownPostActionCountdown = 0;

	unsigned GasMaskVisualProbePhase()
	{
		return (unsigned)InterlockedCompareExchange(
			&sGasMaskVisualProbePhase, 0, 0);
	}

	bool GasMaskAssumedWornForDiagnostics()
	{
		return InterlockedCompareExchange(&sGasMaskWornStateKnown, 0, 0) != 0
			&& InterlockedCompareExchange(&sGasMaskAssumedWorn, 0, 0) != 0;
	}
	static unsigned char sGasMaskPlayerBefore[0x1800] = {};
	static unsigned char sGasMaskObjectBefore[0x900] = {};
	static volatile PVOID sGasMaskDiffPlayer = NULL;
	static volatile PVOID sGasMaskDiffObject = NULL;
	static volatile LONG sGasMaskDiffCountdown = 0;
	static volatile LONG sGasMaskDiffSequence = 0;
	struct GasMaskQueueAccessRecord {
		void *instruction;
		void *address;
		void *consumerObject;
		void *dispatchTarget;
		void *stack[16];
		ULONG_PTR accessType;
		DWORD threadId;
	};
	static GasMaskQueueAccessRecord sGasMaskQueueAccessRecords[512] = {};
	static volatile LONG sGasMaskQueueAccessCount = 0;
	static volatile LONG sGasMaskQueueGuardActive = 0;
	static void *sGasMaskQueueGuardPage = NULL;
	static SIZE_T sGasMaskQueueGuardPageSize = 0;
	static DWORD sGasMaskQueueGuardProtection = 0;
	static BYTE *sGasMaskQueueWatchBegin = NULL;
	static BYTE *sGasMaskQueueWatchEnd = NULL;
	static BYTE *sGasMaskQueueEntryBase = NULL;
	static PVOID sGasMaskQueueVeh = NULL;
	static BYTE *sGasMaskQueueCopyDestination = NULL;
	static void *sGasMaskQueueCopyInstruction = NULL;
	static BYTE *sGasMaskMsvcrBase = NULL;
	static BYTE *sGasMaskMetroBase = NULL;
	static volatile LONG sGasMaskQueueFollowingCopy = 0;
	static __declspec(thread) bool sGasMaskQueueSingleStepPending = false;

	static LONG CALLBACK GasMaskQueueAccessHandler(EXCEPTION_POINTERS *info)
	{
		if (!info || !info->ExceptionRecord || !info->ContextRecord)
			return EXCEPTION_CONTINUE_SEARCH;
		const DWORD code = info->ExceptionRecord->ExceptionCode;
		if (code == STATUS_GUARD_PAGE_VIOLATION
			&& InterlockedCompareExchange(&sGasMaskQueueGuardActive, 0, 0)) {
			BYTE *address = (BYTE *)info->ExceptionRecord->ExceptionInformation[1];
			if (address >= sGasMaskQueueWatchBegin && address < sGasMaskQueueWatchEnd) {
				const LONG index = InterlockedIncrement(&sGasMaskQueueAccessCount) - 1;
				if (index >= 0 && index < ARRAYSIZE(sGasMaskQueueAccessRecords)) {
					GasMaskQueueAccessRecord &record = sGasMaskQueueAccessRecords[index];
					record.instruction = info->ExceptionRecord->ExceptionAddress;
					record.address = address;
					record.consumerObject = NULL;
					record.dispatchTarget = NULL;
					memset(record.stack, 0, sizeof(record.stack));
					CONTEXT unwind = *info->ContextRecord;
					for (unsigned frame = 0; frame < ARRAYSIZE(record.stack)
						&& unwind.Rip; ++frame) {
						record.stack[frame] = (void *)unwind.Rip;
						DWORD64 imageBase = 0;
						PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(
							unwind.Rip, &imageBase, NULL);
						if (function) {
							PVOID handlerData = NULL;
							DWORD64 establisherFrame = 0;
							KNONVOLATILE_CONTEXT_POINTERS nonVolatile = {};
							RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase,
								unwind.Rip, function, &unwind, &handlerData,
								&establisherFrame, &nonVolatile);
						} else {
							DWORD64 returnAddress = 0;
							__try { returnAddress = *(DWORD64 *)unwind.Rsp; }
							__except (EXCEPTION_EXECUTE_HANDLER) { returnAddress = 0; }
							unwind.Rsp += sizeof(DWORD64);
							unwind.Rip = returnAddress;
						}
					}
					if (sGasMaskMetroBase
						&& (BYTE *)record.instruction >= sGasMaskMetroBase + 0x20A1BE
						&& (BYTE *)record.instruction < sGasMaskMetroBase + 0x20A27F) {
						record.consumerObject = (void *)info->ContextRecord->Rbx;
						__try {
							void **vtable = *(void ***)record.consumerObject;
							record.dispatchTarget = vtable ? vtable[2] : NULL;
						}
						__except (EXCEPTION_EXECUTE_HANDLER) {}
					}
					record.accessType = info->ExceptionRecord->ExceptionInformation[0];
					record.threadId = GetCurrentThreadId();
				}
				const bool msvcrCopy = sGasMaskMsvcrBase
					&& info->ExceptionRecord->ExceptionAddress
						== sGasMaskMsvcrBase + 0x3C3B9;
				const bool metroCopy = sGasMaskMetroBase
					&& info->ExceptionRecord->ExceptionAddress
						== sGasMaskMetroBase + 0x1A40;
				BYTE *copySource = msvcrCopy
					? (BYTE *)info->ContextRecord->Rsi
					: (metroCopy ? (BYTE *)info->ContextRecord->Rdx : NULL);
				BYTE *copyDestination = msvcrCopy
					? (BYTE *)info->ContextRecord->Rdi
					: (metroCopy ? (BYTE *)info->ContextRecord->R9 : NULL);
				if (copySource >= sGasMaskQueueWatchBegin
					&& copySource < sGasMaskQueueWatchEnd) {
					const SIZE_T sourceOffset = copySource - sGasMaskQueueWatchBegin;
					sGasMaskQueueCopyDestination = copyDestination - sourceOffset;
					sGasMaskQueueCopyInstruction =
						info->ExceptionRecord->ExceptionAddress;
					InterlockedExchange(&sGasMaskQueueFollowingCopy, 1);
				}
			}
			sGasMaskQueueSingleStepPending = true;
			info->ContextRecord->EFlags |= 0x100;
			return EXCEPTION_CONTINUE_EXECUTION;
		}
		if (code == EXCEPTION_SINGLE_STEP && sGasMaskQueueSingleStepPending) {
			sGasMaskQueueSingleStepPending = false;
			if (InterlockedCompareExchange(&sGasMaskQueueFollowingCopy, 0, 0)) {
				if ((void *)info->ContextRecord->Rip == sGasMaskQueueCopyInstruction
					&& info->ContextRecord->Rcx) {
					sGasMaskQueueSingleStepPending = true;
					info->ContextRecord->EFlags |= 0x100;
					return EXCEPTION_CONTINUE_EXECUTION;
				}
				BYTE *destination = sGasMaskQueueCopyDestination;
				SYSTEM_INFO systemInfo = {};
				GetSystemInfo(&systemInfo);
				const SIZE_T pageSize = systemInfo.dwPageSize;
				void *page = (void *)((ULONG_PTR)destination & ~(pageSize - 1));
				MEMORY_BASIC_INFORMATION memory = {};
				if (destination && VirtualQuery(page, &memory, sizeof(memory))) {
					sGasMaskQueueGuardPage = page;
					sGasMaskQueueGuardPageSize = pageSize;
					sGasMaskQueueGuardProtection = memory.Protect & ~PAGE_GUARD;
					sGasMaskQueueEntryBase = destination;
					sGasMaskQueueWatchBegin = destination;
					sGasMaskQueueWatchEnd = destination + 0x14;
					DWORD oldProtection = 0;
					VirtualProtect(page, pageSize,
						sGasMaskQueueGuardProtection | PAGE_GUARD, &oldProtection);
				}
				InterlockedExchange(&sGasMaskQueueFollowingCopy, 0);
				info->ContextRecord->EFlags &= ~0x100;
				return EXCEPTION_CONTINUE_EXECUTION;
			}
			if (InterlockedCompareExchange(&sGasMaskQueueGuardActive, 0, 0)) {
				DWORD oldProtection = 0;
				VirtualProtect(sGasMaskQueueGuardPage, sGasMaskQueueGuardPageSize,
					sGasMaskQueueGuardProtection | PAGE_GUARD, &oldProtection);
			}
			return EXCEPTION_CONTINUE_EXECUTION;
		}
		return EXCEPTION_CONTINUE_SEARCH;
	}

	static void ArmGasMaskQueueAccessTrace(void *queue)
	{
		if (!queue)
			return;
		BYTE *entries = NULL;
		unsigned entryCount = 0;
		__try {
			entries = *(BYTE **)((BYTE *)queue + 0x18);
			entryCount = *(unsigned short *)((BYTE *)queue + 0x22);
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			return;
		}
		if (!entries || !entryCount)
			return;
		sGasMaskMsvcrBase = (BYTE *)GetModuleHandleA("MSVCR110.dll");
		sGasMaskMetroBase = (BYTE *)GetModuleHandleA(NULL);
		if (!sGasMaskQueueVeh)
			sGasMaskQueueVeh = AddVectoredExceptionHandler(1,
				GasMaskQueueAccessHandler);
		if (!sGasMaskQueueVeh)
			return;
		SYSTEM_INFO systemInfo = {};
		GetSystemInfo(&systemInfo);
		const SIZE_T pageSize = systemInfo.dwPageSize;
		BYTE *watch = entries + (entryCount - 1) * 0x14;
		void *page = (void *)((ULONG_PTR)watch & ~(pageSize - 1));
		MEMORY_BASIC_INFORMATION memory = {};
		if (!VirtualQuery(page, &memory, sizeof(memory)))
			return;
		sGasMaskQueueGuardPage = page;
		sGasMaskQueueGuardPageSize = pageSize;
		sGasMaskQueueGuardProtection = memory.Protect & ~PAGE_GUARD;
		sGasMaskQueueEntryBase = watch;
		sGasMaskQueueWatchBegin = watch;
		sGasMaskQueueWatchEnd = watch + 0x14;
		memset(sGasMaskQueueAccessRecords, 0,
			sizeof(sGasMaskQueueAccessRecords));
		InterlockedExchange(&sGasMaskQueueAccessCount, 0);
		InterlockedExchange(&sGasMaskQueueFollowingCopy, 0);
		InterlockedExchange(&sGasMaskQueueGuardActive, 1);
		DWORD oldProtection = 0;
		if (!VirtualProtect(page, pageSize,
			sGasMaskQueueGuardProtection | PAGE_GUARD, &oldProtection))
			InterlockedExchange(&sGasMaskQueueGuardActive, 0);
	}

	static void ArmGasMaskMotionAccessTrace(void *player)
	{
		if (!kGasMaskStateAccessProbeEnabled || !player || InterlockedCompareExchange(
			&sGasMaskMotionTraceCountdown, 0, 0) > 0)
			return;
		if (!sGasMaskQueueVeh)
			sGasMaskQueueVeh = AddVectoredExceptionHandler(1,
				GasMaskQueueAccessHandler);
		if (!sGasMaskQueueVeh)
			return;
		SYSTEM_INFO systemInfo = {};
		GetSystemInfo(&systemInfo);
		const SIZE_T pageSize = systemInfo.dwPageSize;
		BYTE *watch = (BYTE *)player + 0x1420;
		void *page = (void *)((ULONG_PTR)watch & ~(pageSize - 1));
		MEMORY_BASIC_INFORMATION memory = {};
		if (!VirtualQuery(page, &memory, sizeof(memory)))
			return;
		sGasMaskMetroBase = (BYTE *)GetModuleHandleA(NULL);
		sGasMaskMsvcrBase = (BYTE *)GetModuleHandleA("MSVCR110.dll");
		sGasMaskQueueGuardPage = page;
		sGasMaskQueueGuardPageSize = pageSize;
		sGasMaskQueueGuardProtection = memory.Protect & ~PAGE_GUARD;
		sGasMaskQueueEntryBase = watch;
		sGasMaskQueueWatchBegin = watch;
		sGasMaskQueueWatchEnd = (BYTE *)player + 0x1440;
		memset(sGasMaskQueueAccessRecords, 0,
			sizeof(sGasMaskQueueAccessRecords));
		InterlockedExchange(&sGasMaskQueueAccessCount, 0);
		InterlockedExchange(&sGasMaskQueueFollowingCopy, 0);
		InterlockedExchange(&sGasMaskQueueGuardActive, 1);
		DWORD oldProtection = 0;
		if (VirtualProtect(page, pageSize,
			sGasMaskQueueGuardProtection | PAGE_GUARD, &oldProtection)) {
			InterlockedExchange(&sGasMaskMotionTraceCountdown, 180);
			LogInfo("VRPose gas-mask state trace: armed player=%p range=+0x1420-0x1440\n",
				player);
		} else {
			InterlockedExchange(&sGasMaskQueueGuardActive, 0);
		}
	}

	static bool ArmPneumaticModeWriteTrace(void *player)
	{
		if (!player)
			return false;
		void *modeObject = NULL;
		__try {
			modeObject = *(void **)((BYTE *)player + 0x18);
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
		if (!modeObject)
			return false;
		if (!sGasMaskQueueVeh)
			sGasMaskQueueVeh = AddVectoredExceptionHandler(1,
				GasMaskQueueAccessHandler);
		if (!sGasMaskQueueVeh)
			return false;
		SYSTEM_INFO systemInfo = {};
		GetSystemInfo(&systemInfo);
		const SIZE_T pageSize = systemInfo.dwPageSize;
		// The object-graph diff around a genuine equipment-menu transition found
		// one clean persistent state change: *(player + 0x18) + 0xEC changes
		// from zero to one. Trace that field to find the native mode owner/writer.
		BYTE *watch = (BYTE *)modeObject + 0xEC;
		void *page = (void *)((ULONG_PTR)watch & ~(pageSize - 1));
		MEMORY_BASIC_INFORMATION memory = {};
		if (!VirtualQuery(page, &memory, sizeof(memory)))
			return false;
		sGasMaskMsvcrBase = (BYTE *)GetModuleHandleA("MSVCR110.dll");
		sGasMaskMetroBase = (BYTE *)GetModuleHandleA(NULL);
		sGasMaskQueueGuardPage = page;
		sGasMaskQueueGuardPageSize = pageSize;
		sGasMaskQueueGuardProtection = memory.Protect & ~PAGE_GUARD;
		sGasMaskQueueEntryBase = watch;
		sGasMaskQueueWatchBegin = watch;
		sGasMaskQueueWatchEnd = watch + 4;
		memset(sGasMaskQueueAccessRecords, 0,
			sizeof(sGasMaskQueueAccessRecords));
		InterlockedExchange(&sGasMaskQueueAccessCount, 0);
		InterlockedExchange(&sGasMaskQueueFollowingCopy, 0);
		InterlockedExchange(&sGasMaskQueueGuardActive, 1);
		DWORD oldProtection = 0;
		if (!VirtualProtect(page, pageSize,
			sGasMaskQueueGuardProtection | PAGE_GUARD, &oldProtection)) {
			InterlockedExchange(&sGasMaskQueueGuardActive, 0);
			return false;
		}
		LogInfo("VRPose pneumatic mode write trace: armed player=%p object=%p "
			"field=+0xEC\n", player, modeObject);
		return true;
	}

	static void WriteGasMaskQueueAccessTrace()
	{
		if (!sGasMaskQueueGuardPage)
			return;
		InterlockedExchange(&sGasMaskQueueGuardActive, 0);
		DWORD oldProtection = 0;
		VirtualProtect(sGasMaskQueueGuardPage, sGasMaskQueueGuardPageSize,
			sGasMaskQueueGuardProtection, &oldProtection);
		const LONG count = min((LONG)ARRAYSIZE(sGasMaskQueueAccessRecords),
			InterlockedCompareExchange(&sGasMaskQueueAccessCount, 0, 0));
		FILE *file = NULL;
		if (fopen_s(&file, "gas_mask_queue_access_trace.txt", "w") != 0 || !file)
			return;
		BYTE *base = (BYTE *)GetModuleHandleA(NULL);
		for (LONG i = 0; i < count; ++i) {
			const GasMaskQueueAccessRecord &record = sGasMaskQueueAccessRecords[i];
			char description[160] = {};
			DescribeCodeAddress((DWORD64)record.instruction, description,
				sizeof(description));
			char dispatchDescription[160] = {};
			if (record.dispatchTarget)
				DescribeCodeAddress((DWORD64)record.dispatchTarget,
					dispatchDescription, sizeof(dispatchDescription));
			fprintf(file, "%ld instruction=%p %s address=%p entryOffset=0x%llX "
				"type=%llu thread=%lu consumer=%p dispatch=%p %s\n", i,
				record.instruction, description,
				record.address, (unsigned long long)((BYTE *)record.address
					- sGasMaskQueueEntryBase),
				(unsigned long long)record.accessType, record.threadId,
				record.consumerObject, record.dispatchTarget, dispatchDescription);
			for (unsigned frame = 0; frame < ARRAYSIZE(record.stack)
				&& record.stack[frame]; ++frame) {
				char stackDescription[160] = {};
				DescribeCodeAddress((DWORD64)record.stack[frame], stackDescription,
					sizeof(stackDescription));
				fprintf(file, "  stack[%u]=%p %s\n", frame,
					record.stack[frame], stackDescription);
			}
			DWORD64 imageBase = 0;
			PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(
				(DWORD64)record.instruction, &imageBase, NULL);
			if (base && function && imageBase == (DWORD64)base) {
				char filename[96] = {};
				sprintf_s(filename, "gas_mask_queue_consumer_%08X_%08X.bin",
					function->BeginAddress, function->EndAddress);
				FILE *dump = NULL;
				const DWORD bytes = function->EndAddress - function->BeginAddress;
				if (bytes && bytes <= 0x10000
					&& fopen_s(&dump, filename, "wb") == 0 && dump) {
					fwrite(base + function->BeginAddress, 1, bytes, dump);
					fclose(dump);
				}
			} else if (base && (BYTE *)record.instruction >= base) {
				const ULONG_PTR rva = (BYTE *)record.instruction - base;
				const ULONG_PTR startRva = rva > 0x40 ? rva - 0x40 : 0;
				MEMORY_BASIC_INFORMATION memory = {};
				if (VirtualQuery(base + startRva, &memory, sizeof(memory))
					&& memory.State == MEM_COMMIT) {
					char filename[96] = {};
					sprintf_s(filename, "gas_mask_queue_consumer_raw_%08llX.bin",
						(unsigned long long)startRva);
					FILE *dump = NULL;
					if (fopen_s(&dump, filename, "wb") == 0 && dump) {
						fwrite(base + startRva, 1, 0x100, dump);
						fclose(dump);
					}
				}
			}
			if (record.dispatchTarget) {
				DWORD64 dispatchImageBase = 0;
				PRUNTIME_FUNCTION dispatchFunction = RtlLookupFunctionEntry(
					(DWORD64)record.dispatchTarget, &dispatchImageBase, NULL);
				if (base && dispatchFunction && dispatchImageBase == (DWORD64)base) {
					char filename[96] = {};
					sprintf_s(filename, "gas_mask_native_dispatch_%08X_%08X.bin",
						dispatchFunction->BeginAddress, dispatchFunction->EndAddress);
					FILE *dump = NULL;
					const DWORD bytes = dispatchFunction->EndAddress
						- dispatchFunction->BeginAddress;
					if (bytes && bytes <= 0x10000
						&& fopen_s(&dump, filename, "wb") == 0 && dump) {
						fwrite(base + dispatchFunction->BeginAddress, 1, bytes, dump);
						fclose(dump);
					}
				}
			}
		}
		fclose(file);
		LogInfo("VRPose gas-mask queue access: wrote %ld records\n", count);
	}

	static DWORD WINAPI PneumaticQueueTraceFlushThread(void *)
	{
		Sleep(3000);
		WriteGasMaskQueueAccessTrace();
		InterlockedExchange(&sPneumaticQueueTraceDeadlineTick, 0);
		LogInfo("VRPose pneumatic queue trace: flushed from diagnostic worker\n");
		return 0;
	}

	static void DumpGasMaskPlayerObjectCandidatesOnce(void *player)
	{
		static LONG dumped = 0;
		if (!player || InterlockedCompareExchange(&dumped, 1, 0) != 0)
			return;
		BYTE *base = (BYTE *)GetModuleHandleA(NULL);
		if (!base)
			return;
		IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
		IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(base + dos->e_lfanew);
		const BYTE *imageEnd = base + nt->OptionalHeader.SizeOfImage;
		FILE *file = NULL;
		if (fopen_s(&file, "gas_mask_player_object_candidates.txt", "w") != 0
			|| !file)
			return;
		fprintf(file, "player=%p module=%p\n", player, base);
		void *reported[256] = {};
		unsigned reportedCount = 0;
		for (unsigned offset = 0; offset + sizeof(void *) <= 0x1800;
			offset += sizeof(void *)) {
			void *field = NULL;
			__try { field = *(void **)((BYTE *)player + offset); }
			__except (EXCEPTION_EXECUTE_HANDLER) { continue; }
			if (!field)
				continue;
			void *objects[2] = { field, NULL };
			__try {
				void *embedded = *(void **)field;
				if (embedded)
					objects[1] = (BYTE *)embedded - 0xA0;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {}
			for (unsigned form = 0; form < ARRAYSIZE(objects); ++form) {
				void *object = objects[form];
				if (!object)
					continue;
				bool duplicate = false;
				for (unsigned i = 0; i < reportedCount; ++i)
					if (reported[i] == object) duplicate = true;
				if (duplicate)
					continue;
				void **vtable = NULL;
				__try { vtable = *(void ***)object; }
				__except (EXCEPTION_EXECUTE_HANDLER) { continue; }
				if ((BYTE *)vtable < base || (BYTE *)vtable >= imageEnd)
					continue;
				unsigned gasMaskMethods = 0;
				__try {
					for (unsigned slot = 0; slot < 512; ++slot) {
						BYTE *target = (BYTE *)vtable[slot];
						if (target >= base + 0x33B000 && target < base + 0x346000)
							++gasMaskMethods;
					}
				}
				__except (EXCEPTION_EXECUTE_HANDLER) { gasMaskMethods = 0; }
				if (!gasMaskMethods)
					continue;
				if (reportedCount < ARRAYSIZE(reported))
					reported[reportedCount++] = object;
				fprintf(file, "+0x%04X form=%s field=%p object=%p vtable=%p "
					"gasMaskMethods=%u\n", offset,
					form == 0 ? "direct" : "reference", field, object, vtable,
					gasMaskMethods);
				for (unsigned slot = 0; slot < 512; ++slot) {
					void *target = NULL;
					__try { target = vtable[slot]; }
					__except (EXCEPTION_EXECUTE_HANDLER) { break; }
					if ((BYTE *)target < base + 0x33B000
						|| (BYTE *)target >= base + 0x346000)
						continue;
					char description[160] = {};
					DescribeCodeAddress((DWORD64)target, description,
						sizeof(description));
					fprintf(file, "  slot=%u +0x%X %s\n", slot, slot * 8,
						description);
				}
			}
		}
		fclose(file);
		LogInfo("VRPose gas-mask object scan: wrote %u candidates\n",
			reportedCount);
	}

	typedef bool (*tMetroButtonStateDispatch)(void *manager, int button,
		int down, int repeat, int extra);
	static tMetroButtonStateDispatch trampoline_MetroButtonStateDispatch = NULL;

	static bool Hooked_MetroButtonStateDispatch(void *manager, int button,
		int down, int repeat, int extra)
	{
		if (manager)
			InterlockedExchangePointer(&sMetroInputManager, manager);
		if (button == 0x10D && down) {
			void *actions = InterlockedCompareExchangePointer(
				&sMetroPlayerActions, NULL, NULL);
			if (actions)
				ArmGasMaskMotionAccessTrace((BYTE *)actions - 0xE20);
		}
		return trampoline_MetroButtonStateDispatch(manager, button, down, repeat,
			extra);
	}

	static void Hooked_MetroPlayerActionQueue(void *playerActions, int action,
		int sourceButton, int flags, int alternate)
	{
		if (playerActions) {
			InterlockedExchangePointer(&sMetroPlayerActions, playerActions);
			InterlockedExchange(&sMetroPlayerActionFlags, flags);
		}
		if (action == 35 && sourceButton == 0x10D) {
			void *actionPlayer = playerActions
				? (BYTE *)playerActions - 0xE20 : NULL;
			unsigned nativeState = 0;
			if (actionPlayer) {
				__try { nativeState = *(BYTE *)((BYTE *)actionPlayer + 0xAE0); }
				__except (EXCEPTION_EXECUTE_HANDLER) { nativeState = 0; }
			}
			const bool nativeStateUnknown = actionPlayer
				&& nativeState != 0x10 && nativeState != 0x20;
			if (actionPlayer) {
				void *previousPlayer = InterlockedExchangePointer(
					&sGasMaskStatePlayer, actionPlayer);
				if (previousPlayer && previousPlayer != actionPlayer) {
					InterlockedExchange(&sGasMaskWornStateKnown, 0);
					InterlockedExchange(&sGasMaskAssumedWorn, 0);
				}
			}
			const bool cachedStateKnown = InterlockedCompareExchange(
				&sGasMaskWornStateKnown, 0, 0) != 0;
			if (nativeStateUnknown && !cachedStateKnown) {
				InterlockedExchange(&sGasMaskAssumedWorn, 0);
				InterlockedExchange(&sGasMaskWornStateKnown, 0);
			} else {
				const LONG worn = InterlockedCompareExchange(
					&sGasMaskAssumedWorn, 0, 0);
				InterlockedExchange(&sGasMaskAssumedWorn, worn ? 0 : 1);
				InterlockedExchange(&sGasMaskWornStateKnown, 1);
			}
		}
		trampoline_MetroPlayerActionQueue(playerActions, action, sourceButton,
			flags, alternate);
	}

	static void InstallNativeFlashlightActionHook()
	{
		static bool installedOrFailed = false;
		if (installedOrFailed)
			return;
		const BYTE *base = (const BYTE *)GetModuleHandleA(NULL);
		if (!base)
			return;
		static const BYTE expected[] = {
			0x40, 0x53, 0x48, 0x83, 0xEC, 0x40, 0x83, 0x7C, 0x24, 0x70, 0x00
		};
		void *function = (void *)(base + 0x3E8340);
		if (memcmp(function, expected, sizeof(expected)) != 0) {
			// Metro decrypts this part of .text after the first VR input frames.
			// Leave the attempt live and retry next frame instead of permanently
			// giving up while the startup bytes are still encrypted.
			static bool waitingLogged = false;
			if (!waitingLogged) {
				waitingLogged = true;
				LogInfo("VRPose flashlight gesture: waiting for native action code "
					"at +0x3E8340 to become live\n");
			}
			return;
		}
		installedOrFailed = true;
		SIZE_T hookId = 0;
		const DWORD error = cHookMgr.Hook(&hookId,
			(LPVOID *)&trampoline_MetroPlayerActionQueue, function,
			Hooked_MetroPlayerActionQueue);
		if (error || !trampoline_MetroPlayerActionQueue)
			LogInfo("VRPose flashlight gesture: native action hook FAILED at "
				"+0x3E8340 (err=0x%x)\n", error);
		else
			LogInfo("VRPose flashlight gesture: native player-action hook installed "
				"at +0x3E8340\n");
	}

	static void Hooked_MetroPneumaticPlayerAction(void *playerActionOwner)
	{
		InterlockedExchangePointer(&sNativePneumaticActionOwner,
			playerActionOwner);
		void *queue = InterlockedCompareExchangePointer(&sMetroPlayerActions,
			NULL, NULL);
		const ptrdiff_t delta = queue
			? (BYTE *)playerActionOwner - (BYTE *)queue : 0;
		LogInfo("VRPose pneumatic native probe: owner=%p queue=%p delta=%lld\n",
			playerActionOwner, queue, (long long)delta);
		trampoline_MetroPneumaticPlayerAction(playerActionOwner);
	}

	static void InstallNativePneumaticActionProbe()
	{
		static bool installedOrFailed = false;
		if (installedOrFailed || !trampoline_MetroPlayerActionQueue)
			return;
		BYTE *base = (BYTE *)GetModuleHandleA(NULL);
		BYTE *function = base ? base + 0x20F7D0 : NULL;
		MEMORY_BASIC_INFORMATION memory = {};
		if (!function
			|| VirtualQuery(function, &memory, sizeof(memory)) != sizeof(memory)
			|| (memory.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ
				| PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) == 0)
			return;
		installedOrFailed = true;
		SIZE_T hookId = 0;
		const DWORD error = cHookMgr.Hook(&hookId,
			(LPVOID *)&trampoline_MetroPneumaticPlayerAction, function,
			Hooked_MetroPneumaticPlayerAction);
		if (error || !trampoline_MetroPneumaticPlayerAction)
			LogInfo("VRPose pneumatic native probe: hook FAILED at +0x20F7D0 "
				"(err=0x%x)\n", error);
		else
			LogInfo("VRPose pneumatic native probe: hook installed at +0x20F7D0\n");
	}

	static void CapturePneumaticPlayerPointerCandidates(void *player)
	{
		sPneumaticPointerCandidates.clear();
		if (!player)
			return;
		std::unordered_set<void *> seen;
		for (SIZE_T offset = 0; offset + sizeof(void *) <=
			sizeof(sPneumaticPlayerBefore); offset += sizeof(void *)) {
			void *candidate = NULL;
			memcpy(&candidate, sPneumaticPlayerBefore + offset,
				sizeof(candidate));
			if (!candidate || candidate == player
				|| ((BYTE *)candidate >= (BYTE *)player
					&& (BYTE *)candidate < (BYTE *)player
						+ sizeof(sPneumaticPlayerBefore))
				|| !seen.insert(candidate).second)
				continue;
			MEMORY_BASIC_INFORMATION memory = {};
			if (VirtualQuery(candidate, &memory, sizeof(memory)) != sizeof(memory)
				|| memory.State != MEM_COMMIT
				|| (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS
					| PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE
					| PAGE_EXECUTE_WRITECOPY)) != 0)
				continue;
			const SIZE_T available = (BYTE *)memory.BaseAddress + memory.RegionSize
				- (BYTE *)candidate;
			const SIZE_T bytes = min((SIZE_T)0x1000, available);
			if (bytes < 0x100)
				continue;
			PneumaticPointerCandidate captured = {};
			captured.playerOffset = offset;
			captured.object = candidate;
			captured.before.resize(bytes);
			SIZE_T got = 0;
			if (!ReadProcessMemory(GetCurrentProcess(), candidate,
				captured.before.data(), bytes, &got) || got != bytes)
				continue;
			sPneumaticPointerCandidates.push_back(std::move(captured));
			if (sPneumaticPointerCandidates.size() >= 256)
				break;
		}
		LogInfo("VRPose pneumatic pointer diff: captured %u player references\n",
			(unsigned)sPneumaticPointerCandidates.size());
	}

	static DWORD WINAPI PneumaticStateDiffThread(void *)
	{
		unsigned char weaponMid[0x3000] = {};
		unsigned char weaponLate[0x3000] = {};
		unsigned char serviceMid[0x1000] = {};
		unsigned char serviceLate[0x1000] = {};
		unsigned char playerMid[0x4000] = {};
		unsigned char playerLate[0x4000] = {};
		void *weapon = InterlockedCompareExchangePointer(&sPneumaticDiffWeapon,
			NULL, NULL);
		void *service = InterlockedCompareExchangePointer(&sPneumaticDiffService,
			NULL, NULL);
		void *player = InterlockedCompareExchangePointer(&sPneumaticDiffPlayer,
			NULL, NULL);
		std::vector<std::vector<unsigned char>> pointerMid(
			sPneumaticPointerCandidates.size());
		std::vector<std::vector<unsigned char>> pointerLate(
			sPneumaticPointerCandidates.size());
		Sleep(250);
		if (weapon)
			ReadProcessMemory(GetCurrentProcess(), weapon, weaponMid,
				sizeof(weaponMid), NULL);
		if (service)
			ReadProcessMemory(GetCurrentProcess(), service, serviceMid,
				sizeof(serviceMid), NULL);
		if (player)
			ReadProcessMemory(GetCurrentProcess(), player, playerMid,
				sizeof(playerMid), NULL);
		for (SIZE_T index = 0; index < sPneumaticPointerCandidates.size(); ++index) {
			const PneumaticPointerCandidate &candidate =
				sPneumaticPointerCandidates[index];
			pointerMid[index].resize(candidate.before.size());
			ReadProcessMemory(GetCurrentProcess(), candidate.object,
				pointerMid[index].data(), pointerMid[index].size(), NULL);
		}
		Sleep(1000);
		if (weapon)
			ReadProcessMemory(GetCurrentProcess(), weapon, weaponLate,
				sizeof(weaponLate), NULL);
		if (service)
			ReadProcessMemory(GetCurrentProcess(), service, serviceLate,
				sizeof(serviceLate), NULL);
		if (player)
			ReadProcessMemory(GetCurrentProcess(), player, playerLate,
				sizeof(playerLate), NULL);
		for (SIZE_T index = 0; index < sPneumaticPointerCandidates.size(); ++index) {
			const PneumaticPointerCandidate &candidate =
				sPneumaticPointerCandidates[index];
			pointerLate[index].resize(candidate.before.size());
			ReadProcessMemory(GetCurrentProcess(), candidate.object,
				pointerLate[index].data(), pointerLate[index].size(), NULL);
		}
		FILE *file = NULL;
		if (fopen_s(&file, "pneumatic_state_diff.txt", "w") != 0 || !file)
			return 0;
		struct Region {
			const char *name;
			const unsigned char *before;
			const unsigned char *mid;
			const unsigned char *late;
			SIZE_T bytes;
			void *object;
		};
		const Region regions[] = {
			{ "weapon", sPneumaticWeaponBefore, weaponMid, weaponLate,
				sizeof(weaponMid), weapon },
			{ "service", sPneumaticServiceBefore, serviceMid, serviceLate,
				sizeof(serviceMid), service },
			{ "player", sPneumaticPlayerBefore, playerMid, playerLate,
				sizeof(playerMid), player }
		};
		for (const Region &region : regions) {
			fprintf(file, "%s object=%p bytes=0x%llX\n", region.name,
				region.object, (unsigned long long)region.bytes);
			unsigned changed = 0;
			for (SIZE_T offset = 0; offset + 4 <= region.bytes; offset += 4) {
				if (memcmp(region.before + offset, region.mid + offset, 4) == 0
					&& memcmp(region.before + offset, region.late + offset, 4) == 0)
					continue;
				unsigned before = 0, mid = 0, late = 0;
				memcpy(&before, region.before + offset, 4);
				memcpy(&mid, region.mid + offset, 4);
				memcpy(&late, region.late + offset, 4);
				if (changed++ < 1024)
					fprintf(file, "  +0x%04llX %08X -> %08X -> %08X\n",
						(unsigned long long)offset, before, mid, late);
			}
			fprintf(file, "changedBlocks=%u\n", changed);
		}
		fprintf(file, "pointerCandidates=%u\n",
			(unsigned)sPneumaticPointerCandidates.size());
		unsigned changedObjects = 0;
		for (SIZE_T index = 0; index < sPneumaticPointerCandidates.size(); ++index) {
			const PneumaticPointerCandidate &candidate =
				sPneumaticPointerCandidates[index];
			unsigned changed = 0;
			for (SIZE_T offset = 0; offset + 4 <= candidate.before.size();
				offset += 4) {
				if (memcmp(candidate.before.data() + offset,
					pointerMid[index].data() + offset, 4) != 0
					|| memcmp(candidate.before.data() + offset,
						pointerLate[index].data() + offset, 4) != 0)
					++changed;
			}
			if (!changed)
				continue;
			++changedObjects;
			fprintf(file, "candidate player+0x%04llX object=%p bytes=0x%llX "
				"changedBlocks=%u\n", (unsigned long long)candidate.playerOffset,
				candidate.object, (unsigned long long)candidate.before.size(), changed);
			unsigned logged = 0;
			for (SIZE_T offset = 0; offset + 4 <= candidate.before.size();
				offset += 4) {
				if (memcmp(candidate.before.data() + offset,
					pointerMid[index].data() + offset, 4) == 0
					&& memcmp(candidate.before.data() + offset,
						pointerLate[index].data() + offset, 4) == 0)
					continue;
				unsigned before = 0, mid = 0, late = 0;
				memcpy(&before, candidate.before.data() + offset, 4);
				memcpy(&mid, pointerMid[index].data() + offset, 4);
				memcpy(&late, pointerLate[index].data() + offset, 4);
				if (logged++ < 96)
					fprintf(file, "  +0x%04llX %08X -> %08X -> %08X\n",
						(unsigned long long)offset, before, mid, late);
			}
		}
		fprintf(file, "changedObjects=%u\n", changedObjects);
		fclose(file);
		LogInfo("VRPose pneumatic state diff: wrote weapon=%p service=%p\n",
			weapon, service);
		return 0;
	}

	static void Hooked_MetroTranslatedActionDispatch(void *dispatcher,
		int action, int sourceButton, int flags, int alternate)
	{
		if ((action == 45 && sourceButton == 0x105)
			|| (action == 13 && sourceButton == 0x10C)) {
			InterlockedExchangePointer(&sPneumaticTranslatedActionDispatcher,
				dispatcher);
			void *inputManager = InterlockedCompareExchangePointer(
				&sMetroInputManager, NULL, NULL);
			const ptrdiff_t delta = inputManager
				? (BYTE *)dispatcher - (BYTE *)inputManager : 0;
			LogInfo("VRPose pneumatic translated action: captured dispatcher=%p "
				"inputManager=%p delta=%lld action=%d source=0x%X flags=0x%X "
				"alternate=%d\n", dispatcher, inputManager, (long long)delta,
				action, sourceButton, flags, alternate);
			void *central = NULL;
			__try {
				void *owner = *(void **)((BYTE *)dispatcher + 0x30);
				void **ownerVtable = owner ? *(void ***)owner : NULL;
				typedef void *(*tGetCentralActionDispatcher)(void *owner);
				if (ownerVtable && ownerVtable[0xDA])
					central = ((tGetCentralActionDispatcher)ownerVtable[0xDA])(owner);
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				central = NULL;
			}
			unsigned centralCount = 0;
			void **centralListeners = NULL;
			__try {
				centralListeners = central
					? *(void ***)((BYTE *)central + 0x2A570) : NULL;
				centralCount = central
					? *(unsigned short *)((BYTE *)central + 0x2A57A) : 0;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				centralListeners = NULL;
				centralCount = 0;
			}
			LogInfo("VRPose pneumatic central listeners: central=%p count=%u\n",
				central, centralCount);
			for (unsigned index = 0; centralListeners
				&& index < min(centralCount, 256u); ++index) {
				void *listener = NULL;
				void *callback = NULL;
				__try {
					listener = centralListeners[index];
					void **vtable = listener ? *(void ***)listener : NULL;
					callback = vtable ? vtable[0] : NULL;
				}
				__except (EXCEPTION_EXECUTE_HANDLER) {
					listener = NULL;
					callback = NULL;
				}
				char callbackDescription[160] = {};
				DescribeCodeAddress((DWORD64)callback, callbackDescription,
					sizeof(callbackDescription));
				LogInfo("VRPose pneumatic central listener: index=%u listener=%p "
					"callback=%p %s\n", index, listener, callback,
					callbackDescription);
			}
			static LONG scannedGlobals = 0;
			if (InterlockedCompareExchange(&scannedGlobals, 1, 0) == 0) {
				BYTE *base = (BYTE *)GetModuleHandleA(NULL);
				__try {
					PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)base;
					PIMAGE_NT_HEADERS64 nt = (PIMAGE_NT_HEADERS64)(
						base + dos->e_lfanew);
					PIMAGE_SECTION_HEADER section = IMAGE_FIRST_SECTION(nt);
					unsigned matches = 0;
					for (unsigned index = 0;
						index < nt->FileHeader.NumberOfSections; ++index, ++section) {
						if ((section->Characteristics & IMAGE_SCN_MEM_WRITE) == 0)
							continue;
						BYTE *begin = base + section->VirtualAddress;
						const SIZE_T bytes = section->Misc.VirtualSize;
						for (SIZE_T offset = 0; offset + sizeof(void *) <= bytes;
							offset += sizeof(void *)) {
							if (*(void **)(begin + offset) != dispatcher)
								continue;
							LogInfo("VRPose pneumatic translated action: global ref "
								"module+0x%llX\n", (unsigned long long)(
								begin + offset - base));
							if (++matches >= 32)
								break;
						}
						if (matches >= 32)
							break;
					}
					LogInfo("VRPose pneumatic translated action: global ref count=%u\n",
						matches);
				}
				__except (EXCEPTION_EXECUTE_HANDLER) {
					LogInfo("VRPose pneumatic translated action: global scan faulted\n");
				}
			}
		}
		trampoline_MetroTranslatedActionDispatch(dispatcher, action,
			sourceButton, flags, alternate);
	}

	static void InstallPneumaticTranslatedActionHook()
	{
		static bool installedOrFailed = false;
		if (installedOrFailed || !trampoline_MetroPlayerActionQueue)
			return;
		BYTE *base = (BYTE *)GetModuleHandleA(NULL);
		BYTE *function = base ? base + 0x3E7620 : NULL;
		static const BYTE expected[] = {
			0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x41, 0x56, 0x41, 0x57,
			0x48, 0x83, 0xEC, 0x30
		};
		if (!function || memcmp(function, expected, sizeof(expected)) != 0)
			return;
		installedOrFailed = true;
		SIZE_T hookId = 0;
		const DWORD error = cHookMgr.Hook(&hookId,
			(LPVOID *)&trampoline_MetroTranslatedActionDispatch, function,
			Hooked_MetroTranslatedActionDispatch);
		if (error || !trampoline_MetroTranslatedActionDispatch)
			LogInfo("VRPose pneumatic translated action hook FAILED at +0x3E7620 "
				"(err=0x%x)\n", error);
		else
			LogInfo("VRPose pneumatic translated action hook installed at +0x3E7620\n");
	}

	typedef bool (*tMetroPlayerActionListenerDispatch)(void *actions,
		int action, int dispatchFlags, int value);
	static tMetroPlayerActionListenerDispatch
		trampoline_MetroPlayerActionListenerDispatch = NULL;
	typedef bool (*tMetroPlayerActionListener)(void *listener, int action,
		int value);

	static bool Hooked_MetroPlayerActionListenerDispatch(void *actions,
		int action, int dispatchFlags, int value)
	{
		// Earlier captures mapped pneumatic entry to action 45, but another
		// save/weapon reached the visible pumping pose with action 13 instead.
		// Trace both native routes and let the listener return values identify
		// the real handler rather than assuming one globally stable action ID.
		if ((action != 45 && action != 13) || !actions)
			return trampoline_MetroPlayerActionListenerDispatch(actions, action,
				dispatchFlags, value);
		void **listeners = NULL;
		unsigned count = 0;
		__try {
			listeners = *(void ***)((BYTE *)actions + 0x200);
			count = *(unsigned short *)((BYTE *)actions + 0x20A);
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			return trampoline_MetroPlayerActionListenerDispatch(actions, action,
				dispatchFlags, value);
		}
		LogInfo("VRPose pneumatic player listeners: actions=%p count=%u "
			"dispatchFlags=0x%X value=0x%X\n", actions, count, dispatchFlags,
			value);
		bool anyHandled = false;
		for (unsigned index = 0; listeners && index < count; ++index) {
			void *listener = listeners[index];
			void *callback = NULL;
			bool handled = false;
			__try {
				void **vtable = listener ? *(void ***)listener : NULL;
				callback = vtable ? vtable[0] : NULL;
				if (callback)
					handled = ((tMetroPlayerActionListener)callback)(listener,
						action, value);
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				LogInfo("VRPose pneumatic player listener: index=%u faulted\n",
					index);
			}
			char description[160] = {};
			DescribeCodeAddress((DWORD64)callback, description,
				sizeof(description));
			LogInfo("VRPose pneumatic player listener: index=%u listener=%p "
				"callback=%p %s handled=%d\n", index, listener, callback,
				description, handled ? 1 : 0);
			if (handled)
				anyHandled = true;
		}
		LogInfo("VRPose pneumatic player listeners: aggregate handled=%d\n",
			anyHandled ? 1 : 0);
		return anyHandled;
	}

	static void InstallPneumaticPlayerListenerProbe()
	{
		static bool installedOrFailed = false;
		if (installedOrFailed || !trampoline_MetroPlayerActionQueue)
			return;
		BYTE *base = (BYTE *)GetModuleHandleA(NULL);
		BYTE *function = base ? base + 0x20F9D0 : NULL;
		static const BYTE expected[] = {
			0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x6C, 0x24, 0x20,
			0x56, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x20
		};
		if (!function || memcmp(function, expected, sizeof(expected)) != 0)
			return;
		installedOrFailed = true;
		SIZE_T hookId = 0;
		const DWORD error = cHookMgr.Hook(&hookId,
			(LPVOID *)&trampoline_MetroPlayerActionListenerDispatch, function,
			Hooked_MetroPlayerActionListenerDispatch);
		if (error || !trampoline_MetroPlayerActionListenerDispatch)
			LogInfo("VRPose pneumatic player-listener probe FAILED at +0x20F9D0 "
				"(err=0x%x)\n", error);
		else
			LogInfo("VRPose pneumatic player-listener probe installed at "
				"+0x20F9D0\n");
	}

	static int Hooked_MetroActionListenerDispatch(void *dispatcher, int action,
		int flags, int alternate, int extraA, void *extraB)
	{
		if (action == 45 && dispatcher) {
			unsigned count = 0;
			void **listeners = NULL;
			__try {
				count = *(unsigned short *)((BYTE *)dispatcher + 0x2A57A);
				listeners = *(void ***)((BYTE *)dispatcher + 0x2A570);
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				count = 0;
				listeners = NULL;
			}
			LogInfo("VRPose pneumatic listener probe: dispatcher=%p action=%d "
				"flags=0x%X alternate=%d count=%u\n", dispatcher, action, flags,
				alternate, count);
			const unsigned reportCount = min(count, 128u);
			for (unsigned index = 0; listeners && index < reportCount; ++index) {
				void *listener = NULL;
				void *callback = NULL;
				__try {
					listener = listeners[index];
					void **vtable = listener ? *(void ***)listener : NULL;
					callback = vtable ? vtable[0] : NULL;
				}
				__except (EXCEPTION_EXECUTE_HANDLER) {
					listener = NULL;
					callback = NULL;
				}
				char description[160] = {};
				DescribeCodeAddress((DWORD64)callback, description,
					sizeof(description));
				LogInfo("VRPose pneumatic listener probe: index=%u listener=%p "
					"callback=%p %s\n", index, listener, callback, description);
			}
		}
		const int handled = trampoline_MetroActionListenerDispatch(dispatcher,
			action, flags, alternate, extraA, extraB);
		if (action == 45)
			LogInfo("VRPose pneumatic listener probe: aggregate handled=%d\n",
				handled);
		return handled;
	}

	static void InstallPneumaticListenerDispatchProbe()
	{
		static bool installedOrFailed = false;
		if (installedOrFailed || !trampoline_MetroPlayerActionQueue)
			return;
		BYTE *base = (BYTE *)GetModuleHandleA(NULL);
		BYTE *function = base ? base + 0x3E7A00 : NULL;
		static const BYTE expected[] = {
			0x4C, 0x8B, 0xDC, 0x45, 0x89, 0x4B, 0x20, 0x89, 0x54, 0x24,
			0x10, 0x49, 0x89, 0x4B, 0x08, 0x53, 0x41, 0x57, 0x48, 0x83,
			0xEC, 0x78
		};
		if (!function || memcmp(function, expected, sizeof(expected)) != 0)
			return;
		installedOrFailed = true;
		SIZE_T hookId = 0;
		const DWORD error = cHookMgr.Hook(&hookId,
			(LPVOID *)&trampoline_MetroActionListenerDispatch, function,
			Hooked_MetroActionListenerDispatch);
		if (error || !trampoline_MetroActionListenerDispatch)
			LogInfo("VRPose pneumatic listener probe: hook FAILED at +0x3E7A00 "
				"(err=0x%x)\n", error);
		else
			LogInfo("VRPose pneumatic listener probe: hook installed at +0x3E7A00\n");
	}

	typedef bool (*tMetroNativeActionDispatch)(void *actions, int action,
		int sourceButton, int flags);
	static tMetroNativeActionDispatch trampoline_MetroNativeActionDispatch = NULL;

	static bool Hooked_MetroNativeActionDispatch(void *actions, int action,
		int sourceButton, int flags)
	{
		if (kEquipmentActionTraceEnabled && action == 45) {
			char caller[160] = {};
			DescribeCodeAddress((DWORD64)_ReturnAddress(), caller, sizeof(caller));
			LogInfo("VRPose pneumatic dispatch trace: actions=%p action=%d "
				"source=0x%X flags=0x%X caller=%s\n", actions, action,
				sourceButton, flags, caller);
			const bool handled = trampoline_MetroNativeActionDispatch(actions,
				action, sourceButton, flags);
			LogInfo("VRPose pneumatic dispatch trace: handled=%d\n",
				handled ? 1 : 0);
			return handled;
		}
		if (action != 35)
			return trampoline_MetroNativeActionDispatch(actions, action,
				sourceButton, flags);
		const bool gesture = InterlockedExchange(
			&sGasMaskGestureDispatchCountdown, 0) > 0;
		float gate = 0.0f;
		__try { gate = *(float *)((BYTE *)actions - 0xB18); }
		__except (EXCEPTION_EXECUTE_HANDLER) {}
		char caller[160] = {};
		DescribeCodeAddress((DWORD64)_ReturnAddress(), caller, sizeof(caller));
		LogInfo("VRPose gas-mask dispatch: origin=%s actions=%p action=%d "
			"source=0x%X flags=0x%X gate=%g caller=%s\n",
			gesture ? "gesture" : "normal", actions, action, sourceButton,
			flags, gate, caller);
		static LONG dumped = 0;
		BYTE *base = (BYTE *)GetModuleHandleA(NULL);
		if (base && InterlockedCompareExchange(&dumped, 1, 0) == 0) {
			FILE *file = NULL;
			if (fopen_s(&file, "gas_mask_native_dispatch_0028B0F0_0028B6F0.bin",
				"wb") == 0 && file) {
				fwrite(base + 0x28B0F0, 1, 0x600, file);
				fclose(file);
			}
		}
		const bool handled = trampoline_MetroNativeActionDispatch(actions,
			action, sourceButton, flags);
		LogInfo("VRPose gas-mask dispatch: origin=%s handled=%d gate=%g\n",
			gesture ? "gesture" : "normal", handled ? 1 : 0, gate);
		return handled;
	}

	static void InstallGasMaskNativeDispatchHook()
	{
		static bool installedOrFailed = false;
		if (installedOrFailed)
			return;
		const BYTE *base = (const BYTE *)GetModuleHandleA(NULL);
		if (!base)
			return;
		static const BYTE expected[] = {
			0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18,
			0x48, 0x89, 0x7C, 0x24, 0x20, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x40
		};
		void *function = (void *)(base + 0x28B0F0);
		if (memcmp(function, expected, sizeof(expected)) != 0)
			return;
		installedOrFailed = true;
		SIZE_T hookId = 0;
		const DWORD error = cHookMgr.Hook(&hookId,
			(LPVOID *)&trampoline_MetroNativeActionDispatch, function,
			Hooked_MetroNativeActionDispatch);
		if (error || !trampoline_MetroNativeActionDispatch)
			LogInfo("VRPose gas-mask dispatch hook FAILED at +0x28B0F0 "
				"(err=0x%x)\n", error);
		else
			LogInfo("VRPose gas-mask dispatch hook installed at +0x28B0F0\n");
	}

	static void InstallNativeFlashlightInputManagerHook()
	{
		static bool installedOrFailed = false;
		if (installedOrFailed)
			return;
		const BYTE *base = (const BYTE *)GetModuleHandleA(NULL);
		if (!base)
			return;
		static const BYTE expected[] = {
			0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10,
			0x48, 0x89, 0x74, 0x24, 0x18, 0x48, 0x89, 0x7C, 0x24, 0x20
		};
		void *function = (void *)(base + 0x8F7E50);
		if (memcmp(function, expected, sizeof(expected)) != 0)
			return;
		installedOrFailed = true;
		SIZE_T hookId = 0;
		const DWORD error = cHookMgr.Hook(&hookId,
			(LPVOID *)&trampoline_MetroButtonStateDispatch, function,
			Hooked_MetroButtonStateDispatch);
		if (error || !trampoline_MetroButtonStateDispatch)
			LogInfo("VRPose flashlight gesture: input-manager hook FAILED at "
				"+0x8F7E50 (err=0x%x)\n", error);
		else
			LogInfo("VRPose flashlight gesture: native input-manager hook installed "
				"at +0x8F7E50\n");
	}

	static int Hooked_ReplaceGasMaskFilter(void *filterOwner, int forced)
	{
		InterlockedExchangePointer(&sNativeFilterOwner, filterOwner);
		return trampoline_ReplaceGasMaskFilter(filterOwner, forced);
	}

	static void InstallNativeFilterReplacementHook()
	{
		static bool installedOrFailed = false;
		if (installedOrFailed)
			return;
		BYTE *base = (BYTE *)GetModuleHandleA(NULL);
		if (!base)
			return;
		static const BYTE expected[] = {
			0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10,
			0x57, 0x48, 0x83, 0xEC, 0x30
		};
		void *function = base + 0x3415E0;
		if (memcmp(function, expected, sizeof(expected)) != 0)
			return;
		installedOrFailed = true;
		SIZE_T hookId = 0;
		const DWORD error = cHookMgr.Hook(&hookId,
			(LPVOID *)&trampoline_ReplaceGasMaskFilter, function,
			Hooked_ReplaceGasMaskFilter);
		if (error || !trampoline_ReplaceGasMaskFilter)
			LogInfo("VRPose filter native trace: replacement hook FAILED at "
				"+0x3415E0 (err=0x%x)\n", error);
		else
			LogInfo("VRPose filter native trace: replacement hook installed at "
				"+0x3415E0\n");
	}

	static void LogNativeGasMaskEntityAtDirection(const char *phase,
		void *player, int putOn, int takeOff)
	{
		void *reference = NULL;
		void *entity = NULL;
		unsigned entityState = 0;
		BYTE *base = (BYTE *)GetModuleHandleA(NULL);
		__try {
			reference = player ? *(void **)((BYTE *)player + 0x10A8) : NULL;
			if (base && player) {
				typedef void *(*tResolveLiveEntity)(void *referenceAddress);
				entity = ((tResolveLiveEntity)(base + 0x61680))(
					(BYTE *)player + 0x10A8);
			}
			if (entity)
				entityState = *(BYTE *)((BYTE *)entity + 0xAA);
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			entity = NULL;
		}
		LogInfo("VRPose gas-mask direction probe: %s player=%p putOn=%d "
			"takeOff=%d reference=%p entity=%p entityState=0x%02X\n",
			phase, player, putOn, takeOff, reference, entity, entityState);
	}

	static void Hooked_NativeGasMaskDirection(void *player, int putOn,
		int takeOff)
	{
		LogNativeGasMaskEntityAtDirection("before", player, putOn, takeOff);
		trampoline_NativeGasMaskDirection(player, putOn, takeOff);
		LogNativeGasMaskEntityAtDirection("after", player, putOn, takeOff);
	}

	static void InstallNativeGasMaskDirectionProbe()
	{
		static bool installedOrFailed = false;
		if (installedOrFailed)
			return;
		BYTE *base = (BYTE *)GetModuleHandleA(NULL);
		if (!base)
			return;
		static const BYTE expected[] = {
			0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10,
			0x48, 0x89, 0x74, 0x24, 0x18, 0x57
		};
		void *function = base + 0x1F93D0;
		if (memcmp(function, expected, sizeof(expected)) != 0)
			return;
		installedOrFailed = true;
		SIZE_T hookId = 0;
		const DWORD error = cHookMgr.Hook(&hookId,
			(LPVOID *)&trampoline_NativeGasMaskDirection, function,
			Hooked_NativeGasMaskDirection);
		if (error || !trampoline_NativeGasMaskDirection)
			LogInfo("VRPose gas-mask direction probe: hook FAILED at +0x1F93D0 "
				"(err=0x%x)\n", error);
		else
			LogInfo("VRPose gas-mask direction probe: hook installed at +0x1F93D0\n");
	}

	static void DumpNativeGasMaskEquipmentServiceOnce()
	{
		static LONG dumped = 0;
		if (InterlockedCompareExchange(&dumped, 1, 0) != 0)
			return;
		void *player = NULL;
		if (!ResolveMetroPlayer(&player) || !player) {
			InterlockedExchange(&dumped, 0);
			return;
		}
		BYTE *base = (BYTE *)GetModuleHandleA(NULL);
		void *equipment = NULL;
		void **vtable = NULL;
		void *actionMethod = NULL;
		__try {
			equipment = *(void **)((BYTE *)player + 0xB70);
			vtable = equipment ? *(void ***)equipment : NULL;
			actionMethod = vtable ? vtable[0x48 / sizeof(void *)] : NULL;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			actionMethod = NULL;
		}
		if (!base || !actionMethod) {
			InterlockedExchange(&dumped, 0);
			return;
		}
		char actionDescription[160] = {};
		DescribeCodeAddress((DWORD64)actionMethod, actionDescription,
			sizeof(actionDescription));
		LogInfo("VRPose gas-mask equipment service: player=%p equipment=%p "
			"vtable=%p action+0x48=%s\n", player, equipment, vtable,
			actionDescription);

		DWORD64 imageBase = 0;
		PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(
			(DWORD64)actionMethod, &imageBase, NULL);
		if (function && imageBase == (DWORD64)base) {
			const DWORD size = function->EndAddress - function->BeginAddress;
			char filename[96] = {};
			sprintf_s(filename, "gas_mask_equipment_action_%08X_%08X.bin",
				function->BeginAddress, function->EndAddress);
			FILE *file = NULL;
			if (size && size <= 0x10000
				&& fopen_s(&file, filename, "wb") == 0 && file) {
				fwrite(base + function->BeginAddress, 1, size, file);
				fclose(file);
				LogInfo("VRPose gas-mask equipment service: dumped action method "
					"+0x%X-0x%X to %s\n", function->BeginAddress,
					function->EndAddress, filename);
			}
		}
		FILE *gasMaskHandlerFile = NULL;
		if (fopen_s(&gasMaskHandlerFile,
			"gas_mask_equipment_handler_004F9F30_004FA730.bin", "wb") == 0
			&& gasMaskHandlerFile) {
			fwrite(base + 0x4F9F30, 1, 0x800, gasMaskHandlerFile);
			fclose(gasMaskHandlerFile);
			LogInfo("VRPose gas-mask equipment service: dumped action-0x2A "
				"handler +0x4F9F30 size=0x800\n");
		}
		FILE *vtableFile = NULL;
		if (fopen_s(&vtableFile, "gas_mask_equipment_service_vtable.txt", "w") == 0
			&& vtableFile) {
			for (unsigned slot = 0; slot < 128; ++slot) {
				void *target = NULL;
				__try { target = vtable[slot]; }
				__except (EXCEPTION_EXECUTE_HANDLER) { break; }
				char description[160] = {};
				DescribeCodeAddress((DWORD64)target, description, sizeof(description));
				fprintf(vtableFile, "%03u +0x%03X %p %s\n", slot,
					slot * (unsigned)sizeof(void *), target, description);
			}
			fclose(vtableFile);
		}
	}

	static void ReadNativeGasMaskEquipmentState(void *equipment,
		unsigned *slotOut, unsigned *stateOut, unsigned *transitionOut)
	{
		if (slotOut) *slotOut = 0;
		if (stateOut) *stateOut = 0;
		if (transitionOut) *transitionOut = 0;
		__try {
			const unsigned slot = *(WORD *)((BYTE *)equipment + 0x6A0);
			const BYTE *slotBase = (BYTE *)equipment
				+ (slot ? slot - 1 : 0) * 0x1E0;
			if (slotOut) *slotOut = slot;
			if (stateOut) *stateOut = *(BYTE *)(slotBase + 0x118);
			if (transitionOut)
				*transitionOut = *(BYTE *)((BYTE *)equipment + 0x6C8);
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {}
	}

	static int Hooked_NativeGasMaskEquipmentAction(void *equipment, int pressed)
	{
		unsigned beforeSlot = 0, beforeState = 0, beforeTransition = 0;
		ReadNativeGasMaskEquipmentState(equipment, &beforeSlot, &beforeState,
			&beforeTransition);
		const int result = trampoline_NativeGasMaskEquipmentAction(equipment, pressed);
		unsigned afterSlot = 0, afterState = 0, afterTransition = 0;
		ReadNativeGasMaskEquipmentState(equipment, &afterSlot, &afterState,
			&afterTransition);
		LogInfo("VRPose gas-mask equipment state probe: pressed=%d result=%d "
			"slot=%u state=0x%02X transition=0x%02X -> slot=%u state=0x%02X "
			"transition=0x%02X\n", pressed, result, beforeSlot, beforeState,
			beforeTransition, afterSlot, afterState, afterTransition);
		return result;
	}

	static void InstallNativeGasMaskEquipmentStateProbe()
	{
		static bool installedOrFailed = false;
		if (installedOrFailed)
			return;
		BYTE *base = (BYTE *)GetModuleHandleA(NULL);
		if (!base)
			return;
		static const BYTE expected[] = {
			0x40, 0x56, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xF1,
			0x85, 0xD2
		};
		void *function = base + 0x4F9F30;
		if (memcmp(function, expected, sizeof(expected)) != 0)
			return;
		installedOrFailed = true;
		SIZE_T hookId = 0;
		const DWORD error = cHookMgr.Hook(&hookId,
			(LPVOID *)&trampoline_NativeGasMaskEquipmentAction, function,
			Hooked_NativeGasMaskEquipmentAction);
		if (error || !trampoline_NativeGasMaskEquipmentAction)
			LogInfo("VRPose gas-mask equipment state probe: hook FAILED at "
				"+0x4F9F30 (err=0x%x)\n", error);
		else
			LogInfo("VRPose gas-mask equipment state probe: hook installed at "
				"+0x4F9F30\n");
	}

	static void Hooked_MetroPneumaticUpdateSsss(void *weapon)
	{
		// weapon_pneumo::update_ssss receives the concrete live pneumatic
		// weapon as rcx. Capturing it here avoids guessing through cplayer and
		// equipment layouts and naturally follows level/save changes.
		InterlockedExchangePointer(&sLivePneumaticWeapon, weapon);
		InterlockedExchange(&sLivePneumaticWeaponTick, (LONG)GetTickCount());
		trampoline_MetroPneumaticUpdateSsss(weapon);
	}

	static void RegisterPneumaticWeapon(void *weapon)
	{
		if (!weapon)
			return;
		InterlockedExchangePointer(&sLivePneumaticWeapon, weapon);
		InterlockedExchange(&sLivePneumaticWeaponTick, (LONG)GetTickCount());
		const LONG sequence = InterlockedIncrement(&sPneumaticWeaponRegistryCursor);
		const unsigned slot = (unsigned)(sequence - 1)
			% kPneumaticWeaponRegistrySize;
		InterlockedExchangePointer(&sPneumaticWeaponRegistry[slot], weapon);
	}

	static void *Hooked_MetroPneumaticConstructor(void *weapon)
	{
		void *result = trampoline_MetroPneumaticConstructor(weapon);
		RegisterPneumaticWeapon(result ? result : weapon);
		return result;
	}

	static void InstallNativePneumaticConstructorHook()
	{
		static bool installedOrFailed = false;
		static bool mismatchLogged = false;
		if (installedOrFailed)
			return;
		BYTE *base = (BYTE *)GetModuleHandleA(NULL);
		if (!base)
			return;
		static const BYTE expected[] = {
			0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0xE8
		};
		void *function = base + 0x2CAF90;
		if (memcmp(function, expected, sizeof(expected)) != 0) {
			if (!mismatchLogged) {
				mismatchLogged = true;
				LogInfo("VRPose pneumatic weapon: constructor page not ready; "
					"will retry\n");
			}
			return;
		}
		installedOrFailed = true;
		SIZE_T hookId = 0;
		const DWORD error = cHookMgr.Hook(&hookId,
			(LPVOID *)&trampoline_MetroPneumaticConstructor, function,
			Hooked_MetroPneumaticConstructor);
		if (error || !trampoline_MetroPneumaticConstructor)
			LogInfo("VRPose pneumatic weapon: constructor hook FAILED at "
				"+0x2CAF90 (err=0x%x)\n", error);
		else
			LogInfo("VRPose pneumatic weapon: constructor registry installed at "
				"+0x2CAF90\n");
	}

	static void InstallNativePneumaticWeaponHook()
	{
		static bool installedOrFailed = false;
		static bool mismatchLogged = false;
		if (installedOrFailed)
			return;
		BYTE *base = (BYTE *)GetModuleHandleA(NULL);
		if (!base)
			return;
		static const BYTE expected[] = {
			0x53, 0x48, 0x83, 0xEC, 0x50, 0x48, 0x8B, 0xD9, 0xE8
		};
		void *function = base + 0x2CCCB0;
		if (memcmp(function, expected, sizeof(expected)) != 0) {
			// Metro's protected code pages are still encrypted when the renderer
			// first starts. Retry after the weapon class is used and its page has
			// been unpacked instead of permanently disabling this hook.
			if (!mismatchLogged) {
				mismatchLogged = true;
				LogInfo("VRPose pneumatic weapon: update_ssss page not ready; "
					"will retry\n");
			}
			return;
		}
		installedOrFailed = true;
		SIZE_T hookId = 0;
		const DWORD error = cHookMgr.Hook(&hookId,
			(LPVOID *)&trampoline_MetroPneumaticUpdateSsss, function,
			Hooked_MetroPneumaticUpdateSsss);
		if (error || !trampoline_MetroPneumaticUpdateSsss)
			LogInfo("VRPose pneumatic weapon: update_ssss hook FAILED at "
				"+0x2CCCB0 (err=0x%x)\n", error);
		else
			LogInfo("VRPose pneumatic weapon: update_ssss hook installed at "
				"+0x2CCCB0\n");
	}

	static bool IsConcretePneumaticWeapon(void *weapon, BYTE *base)
	{
		if (!weapon || !base)
			return false;
		void *vtable = NULL;
		__try { vtable = *(void **)weapon; }
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		// The concrete Tihar/Helsing classes replace weapon_pneumo's primary
		// vtable. These are all constructor-proven descendants in this build.
		const ptrdiff_t knownVtables[] = {
			0xAE4690, 0x9D8590, 0xAE6A20, 0xAE89E0, 0xB0FD80
		};
		for (unsigned i = 0; i < ARRAYSIZE(knownVtables); ++i) {
			if (vtable == base + knownVtables[i])
				return true;
		}
		return false;
	}

	static bool InvokeNativePneumaticModeAction(void **weaponOut)
	{
		if (weaponOut)
			*weaponOut = NULL;
		BYTE *base = (BYTE *)GetModuleHandleA(NULL);
		if (!base)
			return false;
		void *candidates[kPneumaticWeaponRegistrySize + 2] = {};
		unsigned candidateCount = 0;
		candidates[candidateCount++] = InterlockedCompareExchangePointer(
			&sLivePneumaticWeapon, NULL, NULL);
		candidates[candidateCount++] = InterlockedCompareExchangePointer(
			&sADSProbeWeapon, NULL, NULL);
		const LONG cursor = InterlockedCompareExchange(
			&sPneumaticWeaponRegistryCursor, 0, 0);
		for (unsigned age = 0; age < kPneumaticWeaponRegistrySize; ++age) {
			const unsigned slot = (unsigned)(cursor - 1 - (LONG)age)
				% kPneumaticWeaponRegistrySize;
			candidates[candidateCount++] = InterlockedCompareExchangePointer(
				&sPneumaticWeaponRegistry[slot], NULL, NULL);
		}
		unsigned valid = 0;
		for (unsigned i = 0; i < candidateCount; ++i) {
			void *weapon = candidates[i];
			if (!IsConcretePneumaticWeapon(weapon, base))
				continue;
			bool duplicate = false;
			for (unsigned prior = 0; prior < i; ++prior)
				duplicate |= candidates[prior] == weapon;
			if (duplicate)
				continue;
			++valid;
			bool invoked = false;
			__try {
			// This is the complete native body of weapon_pneumo's protected
			// +0x2CCA20 mode-entry routine: can_pump() followed by a transition
			// of its embedded pump state machine to state 0x43.
			typedef int (*tCanPump)(void *state);
			void *canPumpTarget = *(void **)(
				*(BYTE **)((BYTE *)weapon + 0x430) + 0x2B0);
			if (!canPumpTarget
				|| !((tCanPump)canPumpTarget)((BYTE *)weapon + 0x430))
				continue;
			typedef void (*tSetPumpState)(void *state, BYTE stateId,
				unsigned immediate);
			void *state = (BYTE *)weapon + 0x700;
			void *setStateTarget = *(void **)(*(BYTE **)state + 0x28);
			if (!setStateTarget)
				continue;
			((tSetPumpState)setStateTarget)(state, 0x43, 1);
			invoked = true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {}
			if (invoked) {
				if (weaponOut)
					*weaponOut = weapon;
				LogInfo("VRPose pneumatic resolver: registered=%ld valid=%u "
					"weapon=%p\n", cursor, valid, weapon);
				return true;
			}
		}
		LogInfo("VRPose pneumatic resolver: registered=%ld valid=%u weapon=0\n",
			cursor, valid);
		return false;
	}

	static void DispatchPendingNativeFlashlightAction()
	{
		const int action = (int)InterlockedExchange(
			&sPendingNativeFlashlightAction, -1);
		void *playerActions = InterlockedCompareExchangePointer(
			&sMetroPlayerActions, NULL, NULL);
		if (!trampoline_MetroPlayerActionQueue)
			return;
		if (action >= 0 && playerActions) {
			const int flags = (int)InterlockedCompareExchange(
				&sMetroPlayerActionFlags, 0, 0);
			trampoline_MetroPlayerActionQueue(playerActions, action, 0x108, flags, 0);
			LogInfo("VRPose flashlight gesture: input thread dispatched native "
				"wpn_light action=%d (0x%X) flags=0x%X\n", action, action, flags);
		}
		if (InterlockedExchange(&sPendingNativeFilterAction, 0) && playerActions) {
			InvokeNativeFilterReplacement();
		}
		if (InterlockedExchange(&sPendingNativeChargerAction, 0)) {
			void *player = playerActions ? (BYTE *)playerActions - 0xE20 : NULL;
			if (!player)
				ResolveMetroPlayer(&player);
			BYTE *base = (BYTE *)GetModuleHandleA(NULL);
			static const BYTE expected[] = {
				0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20,
				0x48, 0x8B, 0xF9, 0x48, 0x8B, 0x89, 0x70, 0x0B, 0x00, 0x00
			};
			BYTE *action = base ? base + 0x288EE0 : NULL;
			if (!player || !action || memcmp(action, expected, sizeof(expected)) != 0) {
				LogInfo("VRPose charger gesture: direct cplayer action unavailable "
					"player=%p action=%p\n", player, action);
			} else {
				typedef void (*tMetroChargerPlayerAction)(void *player);
				bool invoked = false;
				__try {
					((tMetroChargerPlayerAction)action)(player);
					invoked = true;
				}
				__except (EXCEPTION_EXECUTE_HANDLER) {}
				LogInfo("VRPose charger gesture: direct cplayer:action_charger "
					"invoked=%d player=%p\n", invoked ? 1 : 0, player);
			}
		}
	}

	static bool ResolveMetroPlayer(void **playerOut)
	{
		if (playerOut) *playerOut = NULL;
		BYTE *base = (BYTE *)GetModuleHandleA(NULL);
		if (!base)
			return false;
		void *player = NULL;
		__try {
			void *root = *(void **)(base + 0xD01EA8);
			void *holder = root ? *(void **)((BYTE *)root + 0x38) : NULL;
			void *reference = holder ? *(void **)((BYTE *)holder + 0xA0) : NULL;
			if (reference) {
				typedef void *(*tResolvePlayer)(void **reference);
				tResolvePlayer resolvePlayer = (tResolvePlayer)(base + 0x257F0);
				player = resolvePlayer(&reference);
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
		if (playerOut) *playerOut = player;
		return player != NULL;
	}

	// Current and previous front-end mode bytes (+0x138 / +0x139 of the state
	// object behind the game manager; 1 = main-menu level, 0 = gameplay), with
	// the same executable guard as IsNativeMainMenuActive. -1 when unreadable.
	static bool ReadNativeMenuMode(int *mode, int *previous)
	{
		if (mode) *mode = -1;
		if (previous) *previous = -1;
		BYTE *base = (BYTE *)GetModuleHandleA(NULL);
		if (!base)
			return false;
		__try {
			void *manager = *(void **)(base + 0xD01E50);
			void **vtable = manager ? *(void ***)manager : NULL;
			if (!vtable || vtable[0x128 / sizeof(void *)] != base + 0x21ACF0)
				return false;
			BYTE *menuState = *(BYTE **)((BYTE *)manager + 0x8);
			if (!menuState)
				return false;
			if (mode) *mode = menuState[0x138];
			if (previous) *previous = menuState[0x139];
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	bool IsNativeMainMenuActive()
	{
		BYTE *base = (BYTE *)GetModuleHandleA(NULL);
		if (!base)
			return false;
		__try {
			void *manager = *(void **)(base + 0xD01E50);
			void **vtable = manager ? *(void ***)manager : NULL;
			// The verified menu-mode setter is game-manager vtable slot 0x128.
			// Refuse the field read on an unsupported executable instead of
			// interpreting an unrelated object's byte as front-end ownership.
			if (!vtable || vtable[0x128 / sizeof(void *)] != base + 0x21ACF0)
				return false;
			// +0x21ACF0 loads manager->field_8 and passes that pointed-to state
			// object to +0x29C5B0. The latter stores current mode at +0x138
			// and previous mode at +0x139.
			BYTE *menuState = *(BYTE **)((BYTE *)manager + 0x8);
			return menuState && menuState[0x138] == 1;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	static bool QueryNativeGasMaskWorn(void *player, bool *wornOut,
		unsigned *equipmentStateOut)
	{
		if (wornOut) *wornOut = false;
		if (equipmentStateOut) *equipmentStateOut = 0;
		if (!player || !wornOut)
			return false;
		unsigned equipmentState = 0;
		__try {
			// Metro's action-0x2A handler at +0x4F9F30 reads this exact active
			// equipment-slot state before selecting the gas-mask transition.
			// Its native branches prove 0xFF=off and 0x01=worn. Any other value
			// is transitional/unsupported and must not be guessed.
			void *equipment = *(void **)((BYTE *)player + 0xB70);
			if (!equipment)
				return false;
			const unsigned slot = *(WORD *)((BYTE *)equipment + 0x6A0);
			if (!slot)
				return false;
			const BYTE *slotBase = (BYTE *)equipment + (slot - 1) * 0x1E0;
			equipmentState = *(BYTE *)(slotBase + 0x118);
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
		if (equipmentStateOut) *equipmentStateOut = equipmentState;
		if (equipmentState == 0x01) {
			*wornOut = true;
			return true;
		}
		if (equipmentState == 0xFF) {
			*wornOut = false;
			return true;
		}
		return false;
	}

	static bool QueryNativeChargerOut(void *player, bool *out,
		unsigned *equipmentStateOut)
	{
		if (out) *out = false;
		if (equipmentStateOut) *equipmentStateOut = 0;
		if (!player || !out)
			return false;
		unsigned equipmentState = 0;
		__try {
			// Metro's action-0x4C handler at +0x4FA730 reads this exact byte
			// from the active equipment slot.  The native handler treats 0xFF
			// as put away and every other value as already drawn/active.
			void *equipment = *(void **)((BYTE *)player + 0xB70);
			if (!equipment)
				return false;
			const unsigned slot = *(WORD *)((BYTE *)equipment + 0x6A0);
			if (!slot)
				return false;
			const BYTE *slotBase = (BYTE *)equipment + (slot - 1) * 0x1E0;
			equipmentState = *(BYTE *)(slotBase + 0x11A);
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
		if (equipmentStateOut) *equipmentStateOut = equipmentState;
		*out = equipmentState != 0xFF;
		return true;
	}

	bool GetChargerOutForDiagnostics(bool *out)
	{
		static unsigned cachedFrame = 0xFFFFFFFF;
		static bool cachedKnown = false;
		static bool cachedOut = false;
		if (out) *out = false;
		if (!out)
			return false;
		if (cachedFrame != G->frame_no) {
			cachedFrame = G->frame_no;
			void *player = NULL;
			cachedKnown = ResolveMetroPlayer(&player) && player
				&& QueryNativeChargerOut(player, &cachedOut);
		}
		if (cachedKnown)
			*out = cachedOut;
		return cachedKnown;
	}

	static bool InvokeNativeFilterReplacement()
	{
		void *player = NULL;
		void *filterOwner = NULL;
		if (ResolveMetroPlayer(&player) && player) {
			__try {
				void *equipment = *(void **)((BYTE *)player + 0xB70);
				void **equipmentVtable = equipment ? *(void ***)equipment : NULL;
				typedef void *(*tFindEquipment)(void *, int, int);
				tFindEquipment findEquipment = equipmentVtable
					? (tFindEquipment)equipmentVtable[0xC8 / sizeof(void *)] : NULL;
				void *filterEquipment = findEquipment
					? findEquipment(equipment, 4, 1) : NULL;
				void **filterVtable = filterEquipment
					? *(void ***)filterEquipment : NULL;
				typedef void *(*tGetFilterOwner)(void *);
				tGetFilterOwner getFilterOwner = filterVtable
					? (tGetFilterOwner)filterVtable[0x430 / sizeof(void *)] : NULL;
				filterOwner = getFilterOwner
					? getFilterOwner(filterEquipment) : NULL;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				filterOwner = NULL;
			}
		}
		if (filterOwner)
			InterlockedExchangePointer(&sNativeFilterOwner, filterOwner);
		else
			filterOwner = InterlockedCompareExchangePointer(
				&sNativeFilterOwner, NULL, NULL);
		if (!trampoline_ReplaceGasMaskFilter || !filterOwner) {
			LogInfo("VRPose filter gesture: native replacement unavailable - "
				"filter owner could not be resolved\n");
			return false;
		}
		float beforeCurrent = 0.0f;
		float beforeCapacity = 0.0f;
		__try {
			beforeCurrent = *(float *)((BYTE *)filterOwner + 0x44C);
			beforeCapacity = *(float *)((BYTE *)filterOwner + 0x454);
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
		int replaced = 0;
		float afterCurrent = beforeCurrent;
		float afterCapacity = beforeCapacity;
		__try {
			replaced = trampoline_ReplaceGasMaskFilter(filterOwner, 1);
			afterCurrent = *(float *)((BYTE *)filterOwner + 0x44C);
			afterCapacity = *(float *)((BYTE *)filterOwner + 0x454);
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			LogInfo("VRPose filter gesture: native replacement raised an exception\n");
			return false;
		}
		LogInfo("VRPose filter gesture: direct native replacement result=%d "
			"filter=%.2f/%.2f -> %.2f/%.2f owner=%p\n", replaced,
			beforeCurrent, beforeCapacity, afterCurrent, afterCapacity, filterOwner);
		return replaced != 0;
	}

	static bool ResolveMetroPlayerTorch(void **playerOut, void **torchOut)
	{
		if (playerOut) *playerOut = NULL;
		if (torchOut) *torchOut = NULL;
		void *player = NULL;
		if (!ResolveMetroPlayer(&player))
			return false;
		void *torch = NULL;
		__try {
			void *torchReference = *(void **)((BYTE *)player + 0x1758);
			if (torchReference) {
				void *embeddedEntity = *(void **)torchReference;
				if (embeddedEntity)
					torch = (BYTE *)embeddedEntity - 0xA0;
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
		if (playerOut) *playerOut = player;
		if (torchOut) *torchOut = torch;
		return player && torch;
	}

	static bool SyncGasMaskStateFromPlayer(void *player, const char *reason)
	{
		if (!player)
			return false;
		bool worn = false;
		unsigned equipmentState = 0;
		if (!QueryNativeGasMaskWorn(player, &worn, &equipmentState)) {
			LogInfo("VRPose gas-mask native state: authoritative query failed (%s)\n",
				reason ? reason : "unknown");
			return false;
		}
		InterlockedExchangePointer(&sGasMaskStatePlayer, player);
		InterlockedExchange(&sGasMaskAssumedWorn, worn ? 1 : 0);
		InterlockedExchange(&sGasMaskWornStateKnown, 1);
		LogInfo("VRPose gas-mask native state: authoritative equipment state=0x%02X "
			"worn=%d (%s)\n", equipmentState, worn ? 1 : 0,
			reason ? reason : "unknown");
		return true;
	}

	static void *ResolveGasMaskObjectFromPlayer(void *player)
	{
		void *object = NULL;
		__try {
			void *reference = player
				? *(void **)((BYTE *)player + 0x10A8) : NULL;
			void *embedded = reference ? *(void **)reference : NULL;
			object = embedded ? (BYTE *)embedded - 0xA0 : NULL;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { object = NULL; }
		return object;
	}

	static void DumpUnknownGasMaskState(void *player)
	{
		if (!player)
			return;
		const LONG sequence = InterlockedIncrement(&sGasMaskUnknownProbeSequence);
		void *object = ResolveGasMaskObjectFromPlayer(player);
		char playerName[80] = {};
		char objectName[80] = {};
		sprintf_s(playerName, "gas_mask_unknown_probe_%ld_player.bin", sequence);
		sprintf_s(objectName, "gas_mask_unknown_probe_%ld_object.bin", sequence);
		FILE *file = NULL;
		if (fopen_s(&file, playerName, "wb") == 0 && file) {
			__try { fwrite(player, 1, 0x1800, file); }
			__except (EXCEPTION_EXECUTE_HANDLER) {}
			fclose(file);
		}
		if (object && fopen_s(&file, objectName, "wb") == 0 && file) {
			__try { fwrite(object, 1, 0x900, file); }
			__except (EXCEPTION_EXECUTE_HANDLER) {}
			fclose(file);
		}
		LogInfo("VRPose gas-mask unknown probe %ld: player=%p object=%p\n",
			sequence, player, object);
	}

	static void PollGasMaskUnknownPostActionCapture()
	{
		if (InterlockedCompareExchange(
			&sGasMaskUnknownPostActionCountdown, 0, 0) <= 0
			|| InterlockedDecrement(&sGasMaskUnknownPostActionCountdown) != 0)
			return;
		void *player = InterlockedExchangePointer(
			&sGasMaskUnknownPostActionPlayer, NULL);
		DumpUnknownGasMaskState(player);
		LogInfo("VRPose gas-mask unknown probe: automatic post-action capture complete\n");
	}

	static void BeginGasMaskStableReversibleProbe(void *player)
	{
		if (!kGasMaskStableReversibleProbeEnabled || !player
			|| InterlockedCompareExchange(
			&sGasMaskStableProbePhase, 0, 0) != 0)
			return;
		void *object = ResolveGasMaskObjectFromPlayer(player);
		if (!ReadProcessMemory(GetCurrentProcess(), player,
			sGasMaskStableOffPlayer, sizeof(sGasMaskStableOffPlayer), NULL))
			return;
		if (object && !ReadProcessMemory(GetCurrentProcess(), object,
			sGasMaskStableOffObject, sizeof(sGasMaskStableOffObject), NULL))
			object = NULL;
		InterlockedExchangePointer(&sGasMaskStableProbePlayer, player);
		InterlockedExchangePointer(&sGasMaskStableProbeObject, object);
		InterlockedExchange(&sGasMaskStableProbePhase, 1);
		InterlockedExchange(&sGasMaskStableProbeCountdown, 240);
		LogInfo("VRPose gas-mask stable probe: captured OFF player=%p object=%p\n",
			player, object);
	}

	static void ArmGasMaskStableRemovedCapture()
	{
		if (InterlockedCompareExchange(&sGasMaskStableProbePhase, 0, 0) != 2)
			return;
		InterlockedExchange(&sGasMaskStableProbePhase, 3);
		InterlockedExchange(&sGasMaskStableProbeCountdown, 240);
		LogInfo("VRPose gas-mask stable probe: removal capture armed\n");
	}

	static void PollGasMaskStableReversibleProbe()
	{
		const LONG phase = InterlockedCompareExchange(
			&sGasMaskStableProbePhase, 0, 0);
		if ((phase != 1 && phase != 3)
			|| InterlockedCompareExchange(&sGasMaskStableProbeCountdown, 0, 0) <= 0
			|| InterlockedDecrement(&sGasMaskStableProbeCountdown) != 0)
			return;
		void *player = InterlockedCompareExchangePointer(
			&sGasMaskStableProbePlayer, NULL, NULL);
		void *object = InterlockedCompareExchangePointer(
			&sGasMaskStableProbeObject, NULL, NULL);
		if (!player)
			return;
		if (phase == 1) {
			if (!ReadProcessMemory(GetCurrentProcess(), player,
				sGasMaskStableWornPlayer, sizeof(sGasMaskStableWornPlayer), NULL))
				return;
			if (object && !ReadProcessMemory(GetCurrentProcess(), object,
				sGasMaskStableWornObject, sizeof(sGasMaskStableWornObject), NULL))
				InterlockedExchangePointer(&sGasMaskStableProbeObject, NULL);
			InterlockedExchange(&sGasMaskStableProbePhase, 2);
			LogInfo("VRPose gas-mask stable probe: captured WORN player=%p object=%p\n",
				player, object);
			return;
		}

		unsigned char removedPlayer[sizeof(sGasMaskStableOffPlayer)] = {};
		unsigned char removedObject[sizeof(sGasMaskStableOffObject)] = {};
		if (!ReadProcessMemory(GetCurrentProcess(), player, removedPlayer,
			sizeof(removedPlayer), NULL))
			return;
		const bool objectValid = object && ReadProcessMemory(GetCurrentProcess(),
			object, removedObject, sizeof(removedObject), NULL);
		FILE *file = NULL;
		if (fopen_s(&file, "gas_mask_stable_reversible_diff.txt", "w") != 0
			|| !file)
			return;
		unsigned playerCandidates = 0, objectCandidates = 0;
		fprintf(file, "player=%p object=%p\n[player reversible bytes]\n",
			player, objectValid ? object : NULL);
		for (unsigned i = 0; i < sizeof(removedPlayer); ++i) {
			if (sGasMaskStableOffPlayer[i] == removedPlayer[i]
				&& sGasMaskStableOffPlayer[i] != sGasMaskStableWornPlayer[i]) {
				fprintf(file, "+0x%04X off=%02X worn=%02X removed=%02X\n", i,
					sGasMaskStableOffPlayer[i], sGasMaskStableWornPlayer[i],
					removedPlayer[i]);
				++playerCandidates;
			}
		}
		fprintf(file, "[object reversible bytes]\n");
		if (objectValid) {
			for (unsigned i = 0; i < sizeof(removedObject); ++i) {
				if (sGasMaskStableOffObject[i] == removedObject[i]
					&& sGasMaskStableOffObject[i] != sGasMaskStableWornObject[i]) {
					fprintf(file, "+0x%04X off=%02X worn=%02X removed=%02X\n", i,
						sGasMaskStableOffObject[i], sGasMaskStableWornObject[i],
						removedObject[i]);
					++objectCandidates;
				}
			}
		}
		fclose(file);
		InterlockedExchange(&sGasMaskStableProbePhase, 4);
		LogInfo("VRPose gas-mask stable probe: wrote reversible diff "
			"player=%u object=%u\n", playerCandidates, objectCandidates);
	}

	static void PollGasMaskActionStateDiff()
	{
		LONG countdown = InterlockedCompareExchange(&sGasMaskDiffCountdown, 0, 0);
		if (countdown <= 0 || InterlockedDecrement(&sGasMaskDiffCountdown) != 0)
			return;
		void *player = InterlockedCompareExchangePointer(&sGasMaskDiffPlayer,
			NULL, NULL);
		void *mask = InterlockedCompareExchangePointer(&sGasMaskDiffObject,
			NULL, NULL);
		if (!player) {
			WriteGasMaskQueueAccessTrace();
			return;
		}

		unsigned char playerAfter[sizeof(sGasMaskPlayerBefore)] = {};
		unsigned char maskAfter[sizeof(sGasMaskObjectBefore)] = {};
		if (!ReadProcessMemory(GetCurrentProcess(), player, playerAfter,
			sizeof(playerAfter), NULL)
			|| (mask && !ReadProcessMemory(GetCurrentProcess(), mask, maskAfter,
					sizeof(maskAfter), NULL))) {
			LogInfo("VRPose gas-mask diff: final snapshot failed\n");
			WriteGasMaskQueueAccessTrace();
			return;
		}

		const LONG sequence = InterlockedIncrement(&sGasMaskDiffSequence);
		char filename[64] = {};
		sprintf_s(filename, "gas_mask_action35_diff_%ld.txt", sequence);
		FILE *file = NULL;
		if (fopen_s(&file, filename, "w") != 0 || !file) {
			WriteGasMaskQueueAccessTrace();
			return;
		}
		unsigned playerChanges = 0, maskChanges = 0;
		fprintf(file, "player=%p mask=%p\n", player, mask);
		fprintf(file, "[player changes]\n");
		for (unsigned i = 0; i < sizeof(playerAfter); ++i) {
			if (sGasMaskPlayerBefore[i] == playerAfter[i])
				continue;
			fprintf(file, "+0x%04X %02X -> %02X\n", i,
				sGasMaskPlayerBefore[i], playerAfter[i]);
			++playerChanges;
		}
		fprintf(file, "[mask changes]\n");
		for (unsigned i = 0; mask && i < sizeof(maskAfter); ++i) {
			if (sGasMaskObjectBefore[i] == maskAfter[i])
				continue;
			fprintf(file, "+0x%04X %02X -> %02X\n", i,
				sGasMaskObjectBefore[i], maskAfter[i]);
			++maskChanges;
		}
		fclose(file);
		LogInfo("VRPose gas-mask diff: wrote %s playerChanges=%u maskChanges=%u\n",
			filename, playerChanges, maskChanges);
		WriteGasMaskQueueAccessTrace();
	}

	static void DumpResolvedMetroTorchOnce(void *player, void *torch)
	{
		static LONG dumped = 0;
		if (!player || !torch || InterlockedCompareExchange(&dumped, 1, 0) != 0)
			return;
		void **vtable = NULL;
		__try { vtable = *(void ***)torch; }
		__except (EXCEPTION_EXECUTE_HANDLER) {}
		LogInfo("VRPose flashlight torch: resolved player=%p torch=%p vtable=%p\n",
			player, torch, vtable);
		if (!vtable)
			return;
		FILE *file = NULL;
		if (fopen_s(&file, "flashlight_torch_vtable.txt", "w") != 0 || !file)
			return;
		for (unsigned slot = 0; slot < 128; ++slot) {
			void *target = NULL;
			__try { target = vtable[slot]; }
			__except (EXCEPTION_EXECUTE_HANDLER) { break; }
			char description[160] = {};
			DescribeCodeAddress((DWORD64)target, description, sizeof(description));
			fprintf(file, "%03u +0x%03X %p %s\n", slot, slot * 8,
				target, description);
		}
		fclose(file);
		FILE *objectFile = NULL;
		if (fopen_s(&objectFile, "flashlight_torch_object.bin", "wb") == 0
			&& objectFile) {
			__try { fwrite(torch, 1, 0x400, objectFile); }
			__except (EXCEPTION_EXECUTE_HANDLER) {}
			fclose(objectFile);
		}
	}

	static void UpdateNativeFlashlightGesture(bool leftGrip)
	{
		static bool wasHeld = false;
		const bool pressed = leftGrip && !wasHeld;
		wasHeld = leftGrip;
		if (!pressed)
			return;
		const bool menuOpen = VRMenu::IsOpen();
		const bool gamepadMode = VRMenu::GetSettings().gamepadMode;
		const bool gameplayActive = InterlockedCompareExchange(
			&sGameplayModeActive, 0, 0) != 0;
		const bool nearHead = IsLeftHandBesideHead();
		const bool nearWeapon = IsLeftHandNearWeapon();
		LogInfo("VRPose flashlight gesture start: head=%d weapon=%d gameplay=%d "
			"menu=%d gamepad=%d\n", nearHead ? 1 : 0, nearWeapon ? 1 : 0,
			gameplayActive ? 1 : 0, menuOpen ? 1 : 0, gamepadMode ? 1 : 0);
		if (menuOpen || gamepadMode || !gameplayActive || !nearHead || nearWeapon)
			return;

		void *player = NULL;
		void *torch = NULL;
		if (!ResolveMetroPlayerTorch(&player, &torch)) {
			LogInfo("VRPose flashlight gesture: ignored - no live player torch\n");
			return;
		}

		// The player torch overrides the engine's native enabled-state virtual at
		// vtable +0xE58.  Calling that boundary performs the complete torch update
		// (light state, audio, animation, and notifications) without synthesising
		// a keyboard/controller input or requiring the equipment-menu context.
		typedef int (*tSetMetroTorchEnabled)(void *torch, int enabled, int silent);
		int wasEnabled = 0;
		int changed = 0;
		void *setterAddress = NULL;
		__try {
			void **vtable = *(void ***)torch;
			setterAddress = vtable ? vtable[0xE58 / sizeof(void *)] : NULL;
			wasEnabled = (*(BYTE *)((BYTE *)torch + 0x4D9) & 0x02) != 0;
			if (setterAddress) {
				tSetMetroTorchEnabled setEnabled =
					(tSetMetroTorchEnabled)setterAddress;
				changed = setEnabled(torch, !wasEnabled, 0);
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			LogInfo("VRPose flashlight gesture: native torch setter raised an exception\n");
			return;
		}
		if (!setterAddress) {
			LogInfo("VRPose flashlight gesture: native torch setter is unavailable\n");
			return;
		}
		char setterDescription[160] = {};
		DescribeCodeAddress((DWORD64)setterAddress, setterDescription,
			sizeof(setterDescription));
		LogInfo("VRPose flashlight gesture: native torch %s -> %s changed=%d "
			"setter=%s\n", wasEnabled ? "on" : "off",
			wasEnabled ? "off" : "on", changed, setterDescription);
	}

	static bool UpdateNativeChargerGesture(bool leftGrip)
	{
		enum GestureState { Idle, PullingOut, Completed };
		static GestureState state = Idle;
		static bool wasHeld = false;
		static bool ownsPress = false;
		static float pullStartLocal[3] = {};
		const bool pressed = leftGrip && !wasHeld;
		const bool released = !leftGrip && wasHeld;
		wasHeld = leftGrip;
		if (released) {
			if (state == PullingOut)
				LogInfo("VRPose charger gesture: pull cancelled before threshold\n");
			state = Idle;
			ownsPress = false;
			return false;
		}
		if (!leftGrip)
			return false;

		if (state == Idle) {
			if (!pressed)
				return false;
			const bool nearFrontWaist = IsLeftHandNearFrontWaist();
			float waistLocal[3] = {};
			const bool haveWaistLocal = GetLeftHandBodyLocalMeters(waistLocal);
			const bool nearWeapon = IsLeftHandNearWeapon();
			const bool menuOpen = VRMenu::IsOpen();
			const bool gamepadMode = VRMenu::GetSettings().gamepadMode;
			const bool gameplayActive = InterlockedCompareExchange(
				&sGameplayModeActive, 0, 0) != 0;
			void *player = NULL;
			bool chargerOut = false;
			unsigned equipmentState = 0;
			const bool playerKnown = ResolveMetroPlayer(&player) && player;
			const bool chargerStateKnown = playerKnown && QueryNativeChargerOut(
				player, &chargerOut, &equipmentState);
			LogInfo("VRPose charger gesture start: frontWaist=%d weapon=%d "
				"gameplay=%d menu=%d gamepad=%d local=(%.3f %.3f %.3f) "
				"valid=%d stateKnown=%d chargerOut=%d equipmentState=0x%02X\n",
				nearFrontWaist ? 1 : 0, nearWeapon ? 1 : 0,
				gameplayActive ? 1 : 0, menuOpen ? 1 : 0,
				gamepadMode ? 1 : 0, waistLocal[0], waistLocal[1], waistLocal[2],
				haveWaistLocal ? 1 : 0, chargerStateKnown ? 1 : 0,
				chargerOut ? 1 : 0, equipmentState);
			ownsPress = nearFrontWaist && !nearWeapon && gameplayActive
				&& !menuOpen && !gamepadMode && haveWaistLocal
				&& chargerStateKnown;
			if (!ownsPress)
				return false;
			if (chargerOut) {
				InterlockedExchange(&sPendingNativeChargerAction, 1);
				state = Completed;
				LogInfo("VRPose charger gesture: authoritative state is out; "
					"front-waist click queued native holster action\n");
			} else {
				memcpy(pullStartLocal, waistLocal, sizeof(pullStartLocal));
				state = PullingOut;
				LogInfo("VRPose charger gesture: authoritative state is away; "
					"pull armed at (%.3f %.3f %.3f)\n",
					pullStartLocal[0], pullStartLocal[1], pullStartLocal[2]);
			}
		}

		if (state == PullingOut) {
			float currentLocal[3] = {};
			if (!GetLeftHandBodyLocalMeters(currentLocal))
				return ownsPress;
			const float dx = currentLocal[0] - pullStartLocal[0];
			const float dy = currentLocal[1] - pullStartLocal[1];
			const float dz = currentLocal[2] - pullStartLocal[2];
			const float distance = sqrtf(dx * dx + dy * dy + dz * dz);
			const bool deliberateDirection = dy >= 0.10f || dz >= 0.10f;
			if (distance >= 0.18f && deliberateDirection) {
				InterlockedExchange(&sPendingNativeChargerAction, 1);
				state = Completed;
				LogInfo("VRPose charger gesture: pull completed distance=%.3f "
					"delta=(%.3f %.3f %.3f); queued native draw action\n",
					distance, dx, dy, dz);
			}
		}
		return ownsPress;
	}

	static bool UpdateNativeFilterGesture(bool leftGrip)
	{
		enum GestureState { Idle, CarryingToMouth, TwistingAtMouth, Completed };
		static GestureState state = Idle;
		static bool wasHeld = false;
		static float startRotation[9] = {};
		static float grabLocalY = 0.0f;
		const bool pressed = leftGrip && !wasHeld;
		const bool released = !leftGrip && wasHeld;
		wasHeld = leftGrip;

		if (released) {
			if (state == CarryingToMouth || state == TwistingAtMouth)
				LogInfo("VRPose filter gesture: cancelled before twist completed\n");
			state = Idle;
			return false;
		}
		if (!leftGrip)
			return false;

		if (state == Idle) {
			if (!pressed)
				return false;
			void *livePlayer = NULL;
			bool nativeMaskStateKnown = false;
			bool maskWorn = false;
			if (ResolveMetroPlayer(&livePlayer) && livePlayer) {
				nativeMaskStateKnown = QueryNativeGasMaskWorn(
					livePlayer, &maskWorn);
				if (nativeMaskStateKnown) {
					InterlockedExchangePointer(&sGasMaskStatePlayer, livePlayer);
					InterlockedExchange(&sGasMaskAssumedWorn, maskWorn ? 1 : 0);
					InterlockedExchange(&sGasMaskWornStateKnown, 1);
				}
			}
			const bool menuOpen = VRMenu::IsOpen();
			const bool gamepadMode = VRMenu::GetSettings().gamepadMode;
			const bool gameplayActive = InterlockedCompareExchange(
				&sGameplayModeActive, 0, 0) != 0;
			const bool nearChest = IsLeftHandNearChest();
			const bool nearWeapon = IsLeftHandNearWeapon();
			float local[3] = {};
			const bool haveLocal = GetLeftHandBodyLocalMeters(local);
			LogInfo("VRPose filter gesture start: chest=%d mask=%d weapon=%d "
				"gameplay=%d menu=%d gamepad=%d local=(%.3f %.3f %.3f) valid=%d\n",
				nearChest ? 1 : 0,
				maskWorn ? 1 : 0, nearWeapon ? 1 : 0,
				gameplayActive ? 1 : 0, menuOpen ? 1 : 0, gamepadMode ? 1 : 0,
				local[0], local[1], local[2], haveLocal ? 1 : 0);
			if (!nearChest || !nativeMaskStateKnown || !maskWorn
				|| nearWeapon || !gameplayActive
				|| menuOpen || gamepadMode)
				return false;
			state = CarryingToMouth;
			grabLocalY = haveLocal ? local[1] : -0.20f;
			LogInfo("VRPose filter gesture: virtual filter grabbed at chest\n");
		}

		if (state == CarryingToMouth) {
			float local[3] = {};
			const bool mouthReached = GetLeftHandBodyLocalMeters(local)
				&& IsLeftHandNearMouth() && local[1] >= grabLocalY + 0.06f;
			if (mouthReached
				&& GetLeftHandControllerRotationRelativeToHead(startRotation)) {
				state = TwistingAtMouth;
				LogInfo("VRPose filter gesture: mouth reached; twist armed "
					"local=(%.3f %.3f %.3f) rise=%.3f\n", local[0], local[1],
					local[2], local[1] - grabLocalY);
			}
		}
		if (state == CarryingToMouth) {
			static unsigned carryTrace = 0;
			if ((++carryTrace % 30) == 0) {
				float local[3] = {};
				if (GetLeftHandHeadLocalMeters(local))
					LogInfo("VRPose filter gesture: carrying local=(%.3f %.3f %.3f) "
						"mouth=%d\n", local[0], local[1], local[2],
						IsLeftHandNearMouth() ? 1 : 0);
			}
		}
		if (state == TwistingAtMouth && IsLeftHandNearMouth()) {
			float currentRotation[9] = {};
			if (GetLeftHandControllerRotationRelativeToHead(currentRotation)) {
				float relativeTrace = 0.0f;
				for (int i = 0; i < 9; ++i)
					relativeTrace += currentRotation[i] * startRotation[i];
				float cosine = (relativeTrace - 1.0f) * 0.5f;
				cosine = max(-1.0f, min(1.0f, cosine));
				const float angle = acosf(cosine);
				static unsigned twistTrace = 0;
				if ((++twistTrace % 15) == 0) {
					float local[3] = {};
					GetLeftHandHeadLocalMeters(local);
					LogInfo("VRPose filter gesture: twisting angle=%.1f deg "
						"local=(%.3f %.3f %.3f)\n", angle * 57.2957795f,
						local[0], local[1], local[2]);
				}
				static const float kFilterTwistRadians = 0.349065850f; // 20 degrees
				if (angle >= kFilterTwistRadians) {
					InterlockedExchange(&sPendingNativeFilterAction, 1);
					state = Completed;
					LogInfo("VRPose filter gesture: twist completed angle=%.1f deg; "
						"native action queued\n", angle * 57.2957795f);
				}
			}
		}
		return state != Idle;
	}

	typedef void (*tMetroGasMaskTransition)(void *player, int mode,
		int alternateAnimation);
	static tMetroGasMaskTransition trampoline_MetroGasMaskTransition = NULL;

	static void LogGasMaskTransitionState(const char *phase, void *player,
		int mode, int alternateAnimation, void *caller)
	{
		void *maskReference = NULL;
		void *embeddedMask = NULL;
		void *mask = NULL;
		void *maskVtable = NULL;
		unsigned state10B0 = 0, state10B3 = 0, state10B6 = 0;
		__try {
			maskReference = player ? *(void **)((BYTE *)player + 0x10A8) : NULL;
			embeddedMask = maskReference ? *(void **)maskReference : NULL;
			mask = embeddedMask ? (BYTE *)embeddedMask - 0xA0 : NULL;
			maskVtable = mask ? *(void **)mask : NULL;
			if (player) {
				state10B0 = *(BYTE *)((BYTE *)player + 0x10B0);
				state10B3 = *(BYTE *)((BYTE *)player + 0x10B3);
				state10B6 = *(BYTE *)((BYTE *)player + 0x10B6);
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {}
		char callerDescription[160] = {};
		DescribeCodeAddress((DWORD64)caller, callerDescription,
			sizeof(callerDescription));
		LogInfo("VRPose gas-mask transition %s: player=%p mode=%d alternate=%d "
			"maskRef=%p mask=%p vtable=%p state10B0=%u state10B3=%u "
			"state10B6=%u caller=%s\n", phase, player, mode,
			alternateAnimation, maskReference, mask, maskVtable, state10B0,
			state10B3, state10B6, callerDescription);
	}

	static void Hooked_MetroGasMaskTransition(void *player, int mode,
		int alternateAnimation)
	{
		void *caller = _ReturnAddress();
		LogGasMaskTransitionState("before", player, mode, alternateAnimation,
			caller);
		trampoline_MetroGasMaskTransition(player, mode, alternateAnimation);
		LogGasMaskTransitionState("after", player, mode, alternateAnimation,
			caller);
	}

	static void InstallGasMaskTransitionProbe()
	{
		if (!kGasMaskTransitionProbeEnabled)
			return;
		static bool installedOrFailed = false;
		if (installedOrFailed)
			return;
		BYTE *base = (BYTE *)GetModuleHandleA(NULL);
		if (!base)
			return;
		static const BYTE expected[] = {
			0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18,
			0x55, 0x57, 0x41, 0x54, 0x41, 0x56, 0x41, 0x57
		};
		void *function = base + 0x1F9570;
		if (memcmp(function, expected, sizeof(expected)) != 0)
			return;
		installedOrFailed = true;
		SIZE_T hookId = 0;
		const DWORD error = cHookMgr.Hook(&hookId,
			(LPVOID *)&trampoline_MetroGasMaskTransition, function,
			Hooked_MetroGasMaskTransition);
		if (error || !trampoline_MetroGasMaskTransition)
			LogInfo("VRPose gas-mask transition probe FAILED (err=0x%x)\n", error);
		else
			LogInfo("VRPose gas-mask transition probe installed at +0x1F9570\n");
	}

	typedef void (*tGasMaskClassMethod)(void *object, void *argument);
	static tGasMaskClassMethod trampoline_GasMaskReplace = NULL;
	static tGasMaskClassMethod trampoline_GasMaskAttach = NULL;
	static tGasMaskClassMethod trampoline_GasMaskDetach = NULL;
	static tGasMaskClassMethod trampoline_GasMaskState = NULL;

	static void LogGasMaskClassCall(const char *name, void *object, void *argument,
		void *returnAddress)
	{
		char caller[160] = {};
		DescribeCodeAddress((DWORD64)returnAddress, caller, sizeof(caller));
		LogInfo("VRPose gas-mask class: %s object=%p argument=%p caller=%s\n",
			name, object, argument, caller);
	}

	static void Hooked_GasMaskReplace(void *object, void *argument)
	{
		LogGasMaskClassCall("replace +0x340E20", object, argument, _ReturnAddress());
		trampoline_GasMaskReplace(object, argument);
	}

	static void Hooked_GasMaskAttach(void *object, void *argument)
	{
		LogGasMaskClassCall("attach +0x340FE0", object, argument, _ReturnAddress());
		trampoline_GasMaskAttach(object, argument);
	}

	static void Hooked_GasMaskDetach(void *object, void *argument)
	{
		LogGasMaskClassCall("detach +0x3410D0", object, argument, _ReturnAddress());
		trampoline_GasMaskDetach(object, argument);
	}

	static void Hooked_GasMaskState(void *object, void *argument)
	{
		LogGasMaskClassCall("state +0x341760", object, argument, _ReturnAddress());
		trampoline_GasMaskState(object, argument);
	}

	static void InstallGasMaskClassProbe()
	{
		if (!kGasMaskClassProbeEnabled)
			return;
		static bool installedOrFailed = false;
		if (installedOrFailed)
			return;
		BYTE *base = (BYTE *)GetModuleHandleA(NULL);
		if (!base)
			return;
		static const BYTE expected[] = {
			0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20
		};
		const DWORD rvas[] = { 0x340E20, 0x340FE0, 0x3410D0, 0x341760 };
		for (DWORD rva : rvas)
			if (memcmp(base + rva, expected, sizeof(expected)) != 0)
				return;
		installedOrFailed = true;
		struct HookSpec { DWORD rva; tGasMaskClassMethod *trampoline;
			tGasMaskClassMethod hook; const char *name; } hooks[] = {
			{ 0x340E20, &trampoline_GasMaskReplace, Hooked_GasMaskReplace, "replace" },
			{ 0x340FE0, &trampoline_GasMaskAttach, Hooked_GasMaskAttach, "attach" },
			{ 0x3410D0, &trampoline_GasMaskDetach, Hooked_GasMaskDetach, "detach" },
			{ 0x341760, &trampoline_GasMaskState, Hooked_GasMaskState, "state" }
		};
		for (const HookSpec &hook : hooks) {
			SIZE_T hookId = 0;
			const DWORD error = cHookMgr.Hook(&hookId,
				(LPVOID *)hook.trampoline, base + hook.rva, hook.hook);
			if (error || !*hook.trampoline)
				LogInfo("VRPose gas-mask class probe: %s FAILED err=0x%x\n",
					hook.name, error);
			else
				LogInfo("VRPose gas-mask class probe: %s installed +0x%X\n",
					hook.name, hook.rva);
		}
	}

	typedef void (*tMetroGasMaskToggleCallback)(void *controller);
	static tMetroGasMaskToggleCallback trampoline_MetroGasMaskToggleCallback = NULL;
	static volatile PVOID sMetroGasMaskController = NULL;

	static void Hooked_MetroGasMaskToggleCallback(void *controller)
	{
		if (controller)
			InterlockedExchangePointer(&sMetroGasMaskController, controller);
		void *actions = InterlockedCompareExchangePointer(
			&sMetroPlayerActions, NULL, NULL);
		BYTE *player = actions ? (BYTE *)actions - 0xE20 : NULL;
		LONGLONG delta = player && controller
			? (BYTE *)controller - player : 0;
		int pointerOffset = -1;
		if (player && controller) {
			__try {
				for (int offset = 0; offset <= 0x17F8; offset += 8) {
					if (*(void **)(player + offset) == controller) {
						pointerOffset = offset;
						break;
					}
				}
			}
			__except (EXCEPTION_EXECUTE_HANDLER) { pointerOffset = -1; }
		}
		char caller[160] = {};
		DescribeCodeAddress((DWORD64)_ReturnAddress(), caller, sizeof(caller));
		LogInfo("VRPose gas-mask toggle callback: controller=%p player=%p "
			"delta=%lld pointerOffset=0x%X caller=%s\n", controller, player,
			delta, pointerOffset, caller);
		trampoline_MetroGasMaskToggleCallback(controller);
	}

	static void InstallGasMaskToggleCallbackHook()
	{
		static bool installedOrFailed = false;
		if (installedOrFailed)
			return;
		const BYTE *base = (const BYTE *)GetModuleHandleA(NULL);
		if (!base)
			return;
		static const BYTE expected[] = {
			0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x8B, 0x05
		};
		void *function = (void *)(base + 0x341710);
		if (memcmp(function, expected, sizeof(expected)) != 0)
			return;
		installedOrFailed = true;
		SIZE_T hookId = 0;
		const DWORD error = cHookMgr.Hook(&hookId,
			(LPVOID *)&trampoline_MetroGasMaskToggleCallback, function,
			Hooked_MetroGasMaskToggleCallback);
		if (error || !trampoline_MetroGasMaskToggleCallback)
			LogInfo("VRPose gas-mask toggle hook FAILED at +0x341710 "
				"(err=0x%x)\n", error);
		else
			LogInfo("VRPose gas-mask toggle hook installed at +0x341710\n");
	}

	// Script registration for "cplayer:action_gasmask" points at +0x288FA0.
	// Keep six machine-word arguments so a diagnostic detour preserves any
	// stack arguments in addition to the four x64 register arguments.
	typedef uintptr_t (*tMetroGasMaskPlayerAction)(uintptr_t, uintptr_t,
		uintptr_t, uintptr_t, uintptr_t, uintptr_t);
	static tMetroGasMaskPlayerAction trampoline_MetroGasMaskPlayerAction = NULL;
	static uintptr_t Hooked_MetroGasMaskPlayerAction(uintptr_t a1, uintptr_t a2,
		uintptr_t a3, uintptr_t a4, uintptr_t a5, uintptr_t a6)
	{
		return trampoline_MetroGasMaskPlayerAction(a1, a2, a3, a4, a5, a6);
	}

	static void InstallGasMaskPlayerActionHook()
	{
		static bool installedOrFailed = false;
		if (installedOrFailed)
			return;
		BYTE *base = (BYTE *)GetModuleHandleA(NULL);
		if (!base)
			return;
		BYTE *function = base + 0x288FA0;
		MEMORY_BASIC_INFORMATION memory = {};
		if (!VirtualQuery(function, &memory, sizeof(memory))
			|| memory.State != MEM_COMMIT
			|| (memory.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ
				| PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) == 0)
			return;
		installedOrFailed = true;
		SIZE_T hookId = 0;
		const DWORD error = cHookMgr.Hook(&hookId,
			(LPVOID *)&trampoline_MetroGasMaskPlayerAction, function,
			Hooked_MetroGasMaskPlayerAction);
		if (error || !trampoline_MetroGasMaskPlayerAction)
			LogInfo("VRPose gas-mask player-action hook FAILED at +0x288FA0 "
				"(err=0x%x)\n", error);
		else
			LogInfo("VRPose gas-mask player-action hook installed at +0x288FA0\n");
	}

	static bool InvokeNativeGasMaskPlayerAction(const char *gesturePhase)
	{
		void *playerActions = InterlockedCompareExchangePointer(
			&sMetroPlayerActions, NULL, NULL);
		void *player = playerActions ? (BYTE *)playerActions - 0xE20 : NULL;
		if (!player) {
			void *torch = NULL;
			ResolveMetroPlayerTorch(&player, &torch);
		}
		if (!player) {
			LogInfo("VRPose gas-mask gesture: %s has no live player object\n",
				gesturePhase);
			return false;
		}
		if (!trampoline_MetroGasMaskPlayerAction) {
			LogInfo("VRPose gas-mask gesture: native player action unavailable\n");
			return false;
		}
		bool invoked = false;
		__try {
			trampoline_MetroGasMaskPlayerAction((uintptr_t)player, 0, 0, 0, 0, 0);
			InterlockedExchangePointer(&sGasMaskStatePlayer, player);
			invoked = true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			LogInfo("VRPose gas-mask gesture: %s native transition raised an exception\n",
				gesturePhase);
		}
		LogInfo("VRPose gas-mask gesture: %s native toggle invoked=%d\n",
			gesturePhase, invoked ? 1 : 0);
		return invoked;
	}

	// Pressing grip at the waist arms a virtual mask grab; reaching the face
	// while held invokes cplayer:action_gasmask. Once the mask is known to be
	// worn, pressing at the face and pulling away invokes the same native toggle
	// to remove it. Releasing either gesture before its destination cancels.
	static bool UpdateNativeGasMaskGesture(bool leftGrip)
	{
		enum GestureState { Idle, CarryingToFace, PullingFromFace, Completed };
		static GestureState state = Idle;
		static bool wasHeld = false;
		static bool loadingSeen = false;
		const LONG loadingFrame = InterlockedCompareExchange(
			&sLoadingScreenFrame, -1000, -1000);
		const unsigned loadingAge = loadingFrame >= 0
			? G->frame_no - (unsigned)loadingFrame : 0xFFFFFFFFu;
		if (loadingAge <= 1) {
			if (!loadingSeen) {
				loadingSeen = true;
				state = Idle;
				InterlockedExchange(&sGasMaskWornStateKnown, 1);
				InterlockedExchange(&sGasMaskAssumedWorn, 0);
				InterlockedExchangePointer(&sGasMaskStatePlayer, NULL);
				InterlockedExchange(&sGasMaskVisualProbePhase, 0);
				LogInfo("VRPose gas-mask gesture: loading screen reset cached mask state\n");
			}
			wasHeld = leftGrip;
			return false;
		}
		if (loadingSeen && loadingAge > 4)
			loadingSeen = false;
		if (InterlockedCompareExchange(&sGasMaskVisualProbePhase, 0, 0) == 2) {
			const LONG countdown = InterlockedCompareExchange(
				&sGasMaskVisualProbeCountdown, 0, 0);
			if (countdown > 0
				&& InterlockedDecrement(&sGasMaskVisualProbeCountdown) == 0)
				InterlockedExchange(&sGasMaskVisualProbePhase, 0);
		}
		const bool pressed = leftGrip && !wasHeld;
		const bool released = !leftGrip && wasHeld;
		wasHeld = leftGrip;

		if (released) {
			if (state == CarryingToFace) {
				LogInfo("VRPose gas-mask gesture: waist grab cancelled\n");
				InterlockedExchange(&sGasMaskVisualProbePhase, 0);
			} else if (state == PullingFromFace) {
				LogInfo("VRPose gas-mask gesture: face removal cancelled\n");
				InterlockedExchange(&sGasMaskVisualProbePhase, 0);
			}
			state = Idle;
			return false;
		}
		if (!leftGrip)
			return false;

		if (state == Idle) {
			if (!pressed)
				return false;
			void *livePlayer = NULL;
			bool wornStateKnown = false;
			bool knownWorn = false;
			unsigned nativeEquipmentState = 0;
			if (ResolveMetroPlayer(&livePlayer) && livePlayer) {
				void *statePlayer = InterlockedCompareExchangePointer(
					&sGasMaskStatePlayer, NULL, NULL);
				if (statePlayer != livePlayer) {
					InterlockedExchangePointer(&sGasMaskStatePlayer, livePlayer);
					InterlockedExchange(&sGasMaskWornStateKnown, 0);
					LogInfo("VRPose gas-mask gesture: live player changed %p -> %p; "
						"resolving native mask state\n", statePlayer, livePlayer);
				}
				wornStateKnown = QueryNativeGasMaskWorn(livePlayer,
					&knownWorn, &nativeEquipmentState);
				if (wornStateKnown) {
					InterlockedExchangePointer(&sGasMaskStatePlayer, livePlayer);
					InterlockedExchange(&sGasMaskAssumedWorn, knownWorn ? 1 : 0);
					InterlockedExchange(&sGasMaskWornStateKnown, 1);
				}
			}
			const bool menuOpen = VRMenu::IsOpen();
			const bool gamepadMode = VRMenu::GetSettings().gamepadMode;
			const bool gameplayActive = InterlockedCompareExchange(
				&sGameplayModeActive, 0, 0) != 0;
			const bool nearWeapon = IsLeftHandNearWeapon();
			const bool nearHead = IsLeftHandNearHead();
			const bool nearChest = IsLeftHandNearChest();
			const bool nearWaist = IsLeftHandNearWaist();
			LogInfo("VRPose gas-mask gesture start: nativeKnown=%d worn=%d "
				"equipmentState=0x%02X waist=%d chest=%d head=%d weapon=%d gameplay=%d "
				"menu=%d gamepad=%d\n",
				wornStateKnown ? 1 : 0, knownWorn ? 1 : 0,
				nativeEquipmentState, nearWaist ? 1 : 0, nearChest ? 1 : 0,
				nearHead ? 1 : 0,
				nearWeapon ? 1 : 0,
				gameplayActive ? 1 : 0, menuOpen ? 1 : 0, gamepadMode ? 1 : 0);
			if (!wornStateKnown && nearHead)
				DumpUnknownGasMaskState(livePlayer);
			if (menuOpen || gamepadMode || !gameplayActive || nearWeapon
				|| !wornStateKnown)
				return false;
			// The broad head sphere can overlap the upper chest, particularly for
			// seated players. A press in the explicit chest volume belongs to the
			// filter gesture; mask removal must begin at the face outside it.
			// The mask's broad head sphere includes the left temple. Reserve the
			// explicit left-temple box for the flashlight even while a mask is
			// worn; mask removal begins in front of the face, outside both the
			// chest/filter and temple/flashlight volumes.
			if (knownWorn && nearHead && !nearChest
				&& !IsLeftHandBesideHead()) {
				state = PullingFromFace;
				LogInfo("VRPose gas-mask gesture: virtual mask grabbed at face\n");
			} else if (!knownWorn && nearWaist) {
				state = CarryingToFace;
				InterlockedExchange(&sGasMaskVisualProbeCountdown, 0);
				InterlockedExchange(&sGasMaskVisualProbePhase, 1);
				LogInfo("VRPose gas-mask gesture: virtual mask grabbed at waist\n");
			} else {
				return false;
			}
		}

		if (state == CarryingToFace && IsLeftHandNearHead()) {
			InterlockedExchange(&sGasMaskVisualProbeCountdown, 180);
			InterlockedExchange(&sGasMaskVisualProbePhase, 2);
			void *probePlayer = InterlockedCompareExchangePointer(
				&sGasMaskStatePlayer, NULL, NULL);
			BeginGasMaskStableReversibleProbe(probePlayer);
			if (InvokeNativeGasMaskPlayerAction("face reached")) {
				InterlockedExchange(&sGasMaskAssumedWorn, 1);
				InterlockedExchange(&sGasMaskWornStateKnown, 1);
			}
			state = Completed;
		} else if (state == PullingFromFace && IsLeftHandPulledAwayFromHead()) {
			if (InvokeNativeGasMaskPlayerAction("pulled from face")) {
				InterlockedExchange(&sGasMaskAssumedWorn, 0);
				InterlockedExchange(&sGasMaskWornStateKnown, 1);
				ArmGasMaskStableRemovedCapture();
			}
			state = Completed;
		}
		return state != Idle;
	}

	static void ScanFlashlightActionStringXrefsOnce()
	{
		if (!kFlashlightActionXrefProbeEnabled)
			return;
		static LONG started = 0;
		if (InterlockedCompareExchange(&started, 1, 0) != 0)
			return;
		HMODULE module = GetModuleHandleA(NULL);
		if (!module) return;
		BYTE *base = (BYTE *)module;
		IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
		IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(base + dos->e_lfanew);
		struct FixedDump { const char *name; DWORD rva; DWORD size; } fixed[] = {
			{ "filter_action_queue_003E7000_003E9000.bin", 0x3E7000, 0x2000 },
			{ "gas_mask_player_action_00289E00_0028AA00.bin", 0x289E00, 0xC00 },
			{ "gas_mask_player_replace_001F5C00_001F6100.bin", 0x1F5C00, 0x500 },
			{ "gas_mask_player_toggle_001F9200_001F9900.bin", 0x1F9200, 0x700 },
			{ "gas_mask_state_callback_00341500_00341B00.bin", 0x341500, 0x600 },
			{ "gas_mask_class_0033B000_00346000.bin", 0x33B000, 0xB000 }
		};
		for (const FixedDump &dump : fixed) {
			FILE *file = NULL;
			if (fopen_s(&file, dump.name, "wb") == 0 && file) {
				fwrite(base + dump.rva, 1, dump.size, file);
				fclose(file);
				LogInfo("VRPose gas-mask xref: fixed dump +0x%X size=0x%X to %s\n",
					dump.rva, dump.size, dump.name);
			}
		}
		const DWORD targets[] = {
			0xACB6B0, // cplayer:action_charger
			0xB43900, // charger_track
			0xB8CDE8, // triggers/player/charger
			0xB900E8  // actions/engine/discharge
		};
		IMAGE_SECTION_HEADER *sections = IMAGE_FIRST_SECTION(nt);
		unsigned hits = 0;
		for (WORD s = 0; s < nt->FileHeader.NumberOfSections; ++s) {
			if (memcmp(sections[s].Name, ".text", 5) != 0) continue;
			BYTE *code = base + sections[s].VirtualAddress;
			const SIZE_T size = sections[s].Misc.VirtualSize;
			for (SIZE_T i = 0; i + 7 <= size; ++i) {
				if ((code[i] != 0x48 && code[i] != 0x4C)
					|| (code[i + 1] != 0x8D && code[i + 1] != 0x8B)
					|| (code[i + 2] & 0xC7) != 0x05)
					continue;
				const LONG disp = *(const LONG *)(code + i + 3);
				const DWORD targetRva = (DWORD)((code + i + 7 + disp) - base);
				bool targetMatch = false;
				for (DWORD target : targets) {
					if (targetRva == target) {
						targetMatch = true;
						break;
					}
				}
				if (!targetMatch) continue;
				DWORD64 imageBase = 0;
				PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry((DWORD64)(code + i), &imageBase, NULL);
				LogInfo("VRPose gas-mask xref: string +0x%X referenced at +0x%llX\n",
					targetRva, (unsigned long long)((code + i) - base));
				if (fn && imageBase == (DWORD64)base) {
					const DWORD bytes = fn->EndAddress - fn->BeginAddress;
					char filename[96] = {};
					sprintf_s(filename, "gas_mask_xref_%08X_%08X.bin",
						fn->BeginAddress, fn->EndAddress);
					FILE *file = NULL;
					if (bytes && bytes <= 0x10000
						&& fopen_s(&file, filename, "wb") == 0 && file) {
						fwrite(base + fn->BeginAddress, 1, bytes, file);
						fclose(file);
						LogInfo("VRPose gas-mask xref: dumped +0x%X-0x%X to %s\n",
							fn->BeginAddress, fn->EndAddress, filename);
					}
				}
				if (++hits >= 32) break;
			}
			if (hits >= 32) break;
		}
		LogInfo("VRPose gas-mask xref: scan complete hits=%u\n", hits);
	}

	static void TraceFlashlightXInputConsumer(WORD buttons, const char *source)
	{
		if (!kFlashlightConsumerTraceEnabled)
			return;
		static WORD previousPhysical = 0;
		static WORD previousSynthetic = 0;
		WORD &previous = strcmp(source, "physical") == 0
			? previousPhysical : previousSynthetic;
		const WORD rising = (buttons & ~previous) & XINPUT_GAMEPAD_Y;
		previous = buttons;
		if (!rising)
			return;
		void *frames[24] = {};
		const USHORT count = CaptureStackBackTrace(0, ARRAYSIZE(frames), frames, NULL);
		LogInfo("VRPose flashlight consumer: source=%s buttons=%04X rising=%04X stack=%u\n",
			source, buttons, rising, (unsigned)count);
		// The XInput consumer is split across adjacent unwind ranges. Dump the
		// full contiguous body so the Y-bit test and action dispatch after the
		// first range boundary can be disassembled offline.
		HMODULE metroModule = GetModuleHandleA(NULL);
		if (metroModule) {
			FILE *wide = NULL;
			if (fopen_s(&wide, "flashlight_consumer_008F8A10_008F9210.bin", "wb") == 0
				&& wide) {
				fwrite((BYTE *)metroModule + 0x8F8A10, 1, 0x800, wide);
				fclose(wide);
				LogInfo("VRPose flashlight consumer: dumped wide XInput body +0x8F8A10 size=0x800\n");
			}
			FILE *dispatcher = NULL;
			if (fopen_s(&dispatcher, "flashlight_dispatcher_008F7E50_008F8250.bin", "wb") == 0
				&& dispatcher) {
				fwrite((BYTE *)metroModule + 0x8F7E50, 1, 0x400, dispatcher);
				fclose(dispatcher);
				LogInfo("VRPose flashlight consumer: dumped button dispatcher +0x8F7E50 size=0x400\n");
			}
		}
		for (USHORT i = 0; i < count; ++i) {
			char desc[192] = {};
			DescribeCodeAddress((DWORD64)frames[i], desc, sizeof(desc));
			LogInfo("VRPose flashlight consumer: stack[%u] %s\n", (unsigned)i, desc);
			DWORD64 imageBase = 0;
			PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(
				(DWORD64)frames[i], &imageBase, NULL);
			HMODULE metro = GetModuleHandleA(NULL);
			if (function && imageBase == (DWORD64)metro) {
				const DWORD size = function->EndAddress - function->BeginAddress;
				if (size > 0 && size <= 0x10000) {
					char filename[96] = {};
					sprintf_s(filename, "flashlight_consumer_%08X_%08X.bin",
						function->BeginAddress, function->EndAddress);
					FILE *file = NULL;
					if (fopen_s(&file, filename, "wb") == 0 && file) {
						fwrite((BYTE *)metro + function->BeginAddress, 1, size, file);
						fclose(file);
						LogInfo("VRPose flashlight consumer: dumped metro.exe+0x%X-0x%X to %s\n",
							function->BeginAddress, function->EndAddress, filename);
					}
				}
			}
		}
	}

	static DWORD WINAPI Hooked_XInputGetState(DWORD dwUserIndex, XINPUT_STATE *pState)
	{
		static LONG sLoggedLabel = 0;
		static LONG sLoggedPolls = 0;
		static WORD sPreviousPhysicalMaskButtons = 0;
		static WORD sPreviousSyntheticMaskButtons = 0;
		if (dwUserIndex != 0 || !pState) {
			if (trampoline_XInputGetState)
				return trampoline_XInputGetState(dwUserIndex, pState);
			return ERROR_DEVICE_NOT_CONNECTED;
		}
		// In Gamepad Mode the physical XInput device must pass through
		// untouched. The VR-generated pad state is only for motion controls.
		if (VRMenu::GetSettings().gamepadMode && !VRMenu::IsOpen() && trampoline_XInputGetState) {
			const DWORD result = trampoline_XInputGetState(dwUserIndex, pState);
			const LONG label = InterlockedCompareExchange(&sControlProbeLabel, 0, 0);
			if (label != sLoggedLabel) { sLoggedLabel = label; sLoggedPolls = 0; }
			if (label != 0 && sLoggedPolls < 8) {
				LogInfo("VRPose control probe: physical XInput label=%s result=%lu buttons=%04X LX=%d LY=%d RX=%d RY=%d LT=%u RT=%u\n",
					label == 2 ? "SCRIPTED" : "ORDINARY", (unsigned long)result,
					result == ERROR_SUCCESS ? pState->Gamepad.wButtons : 0,
					result == ERROR_SUCCESS ? pState->Gamepad.sThumbLX : 0,
					result == ERROR_SUCCESS ? pState->Gamepad.sThumbLY : 0,
					result == ERROR_SUCCESS ? pState->Gamepad.sThumbRX : 0,
					result == ERROR_SUCCESS ? pState->Gamepad.sThumbRY : 0,
					result == ERROR_SUCCESS ? pState->Gamepad.bLeftTrigger : 0,
					result == ERROR_SUCCESS ? pState->Gamepad.bRightTrigger : 0);
				sLoggedPolls++;
			}
			if (result == ERROR_SUCCESS) {
				const WORD currentButtons = pState->Gamepad.wButtons;
				if ((currentButtons & XINPUT_GAMEPAD_X)
					&& !(sPreviousPhysicalMaskButtons & XINPUT_GAMEPAD_X)) {
					void *actions = InterlockedCompareExchangePointer(
						&sMetroPlayerActions, NULL, NULL);
					if (actions)
						ArmGasMaskMotionAccessTrace((BYTE *)actions - 0xE20);
				}
				sPreviousPhysicalMaskButtons = currentButtons;
				TraceFlashlightXInputConsumer(pState->Gamepad.wButtons, "physical");
				InterlockedExchange(&sGamepadAiming,
					pState->Gamepad.bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD ? 1 : 0);
				if (VRMenu::GetSettings().disableGamepadRightY)
					pState->Gamepad.sThumbRY = 0;
			} else {
				InterlockedExchange(&sGamepadAiming, 0);
			}
			DispatchPendingNativeFlashlightAction();
			return result;
		}
		InterlockedExchange(&sGamepadAiming, 0);

		const WORD buttons = (WORD)InterlockedCompareExchange(&sPadButtons, 0, 0);
		if ((buttons & XINPUT_GAMEPAD_X)
			&& !(sPreviousSyntheticMaskButtons & XINPUT_GAMEPAD_X)) {
			void *actions = InterlockedCompareExchangePointer(
				&sMetroPlayerActions, NULL, NULL);
			if (actions)
				ArmGasMaskMotionAccessTrace((BYTE *)actions - 0xE20);
		}
		sPreviousSyntheticMaskButtons = buttons;
		TraceFlashlightXInputConsumer(buttons, "synthetic");
		SHORT lx = (SHORT)InterlockedCompareExchange(&sPadLX, 0, 0);
		SHORT ly = (SHORT)InterlockedCompareExchange(&sPadLY, 0, 0);
		const SHORT pulseRX = InterlockedExchange(&sPadNeutralDispatchPulse, 0) ? 1 : 0;
		const SHORT followRX = (SHORT)InterlockedCompareExchange(&sPadFollowRX, 0, 0);
		const SHORT ry = (SHORT)InterlockedCompareExchange(&sPadFollowRY, 0, 0);
		const SHORT rx = pulseRX ? pulseRX : followRX;
		const DWORD axisTick = (DWORD)InterlockedCompareExchange(&sPadAxisUpdateTick, 0, 0);
		// If VR polling/rendering stalls or stops, never leave Metro holding the
		// last non-zero movement sample. GetTickCount subtraction is wrap-safe.
		if ((DWORD)(GetTickCount() - axisTick) > 100) {
			lx = 0;
			ly = 0;
		}

		memset(pState, 0, sizeof(*pState));
		pState->Gamepad.wButtons = buttons;
		pState->Gamepad.sThumbLX = lx;
		pState->Gamepad.sThumbLY = ly;
		pState->Gamepad.sThumbRX = rx;
		pState->Gamepad.sThumbRY = ry;
		const LONG label = InterlockedCompareExchange(&sControlProbeLabel, 0, 0);
		if (label != sLoggedLabel) { sLoggedLabel = label; sLoggedPolls = 0; }
		if (label != 0 && sLoggedPolls < 8) {
			LogInfo("VRPose control probe: synthetic XInput label=%s buttons=%04X LX=%d LY=%d\n",
				label == 2 ? "SCRIPTED" : "ORDINARY", buttons, lx, ly);
			sLoggedPolls++;
		}

		// dwPacketNumber must only change when the state does - engines use
		// it to skip re-reading an unchanged pad, and incrementing every
		// call would look like constant activity, which is the sort of
		// thing that makes a game decide the pad is the active device and
		// stop listening to the mouse.
		static DWORD sPacket = 0;
		static WORD sLastButtons = 0;
		static SHORT sLastLX = 0, sLastLY = 0, sLastRX = 0, sLastRY = 0;
		if (buttons != sLastButtons || lx != sLastLX || ly != sLastLY || rx != sLastRX || ry != sLastRY) {
			const bool axisReleased = (sLastLX != 0 || sLastLY != 0)
				&& lx == 0 && ly == 0;
			sLastButtons = buttons;
			sLastLX = lx;
			sLastLY = ly;
			sLastRX = rx;
			sLastRY = ry;
			sPacket++;
			if (axisReleased) {
				static LONG sHookReleaseLogs = 0;
				if (InterlockedIncrement(&sHookReleaseLogs) <= 24)
					LogInfo("VRPose pad: XInput delivered neutral packet=%lu RX=%d "
						"(sub-deadzone dispatch sentinel)\n",
						(unsigned long)sPacket, (int)rx);
			}
		}
		// FORCE THE PAD TO READ AS THE ACTIVE DEVICE.
		//
		// The follow injects synthetic MOUSE movement, so Metro keeps deciding
		// the player switched to mouse: keyboard prompts, and the first press of
		// a button spent switching device instead of acting on it. Raising the
		// injection deadzone fixed that while standing still, but the moment the
		// head moves the mouse events resume and the prompts flip back.
		//
		// The comment above notes that incrementing the packet number every call
		// "would look like constant activity, which is the sort of thing that
		// makes a game decide the pad is the active device" - written there as a
		// hazard, but it is exactly the behaviour wanted here. So do it on
		// purpose: report fresh pad activity every poll, plus a 1-unit jitter on
		// an axis nothing reads at that magnitude, so engines comparing STATE
		// rather than the packet number also see change.
		//
		// The unproven half of that original comment is "and stop listening to
		// the mouse". If Metro arbitrates one active device for INPUT and not
		// merely for prompt icons, the follow's look injection stops landing and
		// culling, interaction and the laser/muzzle-flash attachment break just
		// as they did in the pitch-follow disable test - an unmistakable signal,
		// and one flag to revert.
		// TESTED AND REVERTED 2026-08-18. Did NOT keep the prompts on
		// controller - they still flipped to keyboard while walking - and it
		// broke other things. So Metro does not decide the active device from
		// pad-activity freshness, and feeding it fake activity only costs.
		// The original comment's fear was half right and half wrong: the
		// "stop listening to the mouse" side bit, the "pad becomes active
		// device" side did not. Do not retry via the input layer.
		static const bool kForceControllerActive = false;
		if (kForceControllerActive) {
			static int jitter = 0;
			pState->Gamepad.sThumbRX = (jitter ^= 1) ? 1 : -1;
			sPacket++;
		}
		pState->dwPacketNumber = sPacket;
		DispatchPendingNativeFlashlightAction();
		return ERROR_SUCCESS;
	}

	static DWORD WINAPI Hooked_XInputGetCapabilities(DWORD dwUserIndex, DWORD dwFlags,
		XINPUT_CAPABILITIES *pCaps)
	{
		if (dwUserIndex != 0 || !pCaps) {
			if (trampoline_XInputGetCapabilities)
				return trampoline_XInputGetCapabilities(dwUserIndex, dwFlags, pCaps);
			return ERROR_DEVICE_NOT_CONNECTED;
		}
		memset(pCaps, 0, sizeof(*pCaps));
		pCaps->Type = XINPUT_DEVTYPE_GAMEPAD;
		pCaps->SubType = XINPUT_DEVSUBTYPE_GAMEPAD;
		pCaps->Gamepad.wButtons = 0xF3FF;
		pCaps->Gamepad.bLeftTrigger = 0xFF;
		pCaps->Gamepad.bRightTrigger = 0xFF;
		pCaps->Gamepad.sThumbLX = 0x7FFF;
		pCaps->Gamepad.sThumbLY = 0x7FFF;
		return ERROR_SUCCESS;
	}

	// Retried until it takes: XINPUT1_3.dll is a static import of metro.exe
	// so it is normally loaded well before us, but "normally" is not a
	// guarantee worth a crash on startup.
	static void InstallXInputHooks()
	{
		static bool sDone = false;
		if (sDone)
			return;
		HINSTANCE h = GetModuleHandleA("XINPUT1_3.dll");
		if (!h)
			return;
		sDone = true;
		const int fail =
			InstallHookLate(h, "XInputGetState", (void **)&trampoline_XInputGetState,
				Hooked_XInputGetState) |
			InstallHookLate(h, "XInputGetCapabilities", (void **)&trampoline_XInputGetCapabilities,
				Hooked_XInputGetCapabilities);
		LogInfo("VRPose pad: XInput hooks %s\n", fail ? "FAILED" : "installed");
	}

	// The controller layout, in one place.
	//
	// Left trigger is a MODIFIER here and is never forwarded to the game -
	// aiming is the gun-to-face gesture now (IsWeaponNearFace), which is
	// what freed it up. While it is held the right controller changes
	// meaning wholesale: the stick becomes the d-pad, and the face buttons
	// become Journal/Menu.
	static void PublishPadButtons(const vr::VRControllerState_t &left, bool haveLeft,
		const vr::VRControllerState_t &right, bool haveRight, float stickDead)
	{
		const bool physicalWeaponInventory = haveLeft
			&& (left.ulButtonPressed
				& vr::ButtonMaskFromId(vr::k_EButton_ApplicationMenu)) != 0;
		InterlockedExchange(&sWeaponInventoryHeld, physicalWeaponInventory ? 1 : 0);
		// Vertical right-stick D-pad actions are deliberately hold-to-activate.
		// A short accidental upward/downward nudge should not pull out the
		// lighter or consume a medkit. Horizontal D-pad actions remain immediate.
		static int sRightDpadUpHeld = 0;
		static int sRightDpadDownHeld = 0;
		static const int kRightDpadHoldFrames = 72; // ~1 second at 72 Hz
		static int sModifierOnlyHeld = 0;
		static JournalButtonPulse sJournalButton;
		static const int kEquipmentHoldFrames = 36; // ~0.5 second at 72 Hz
		WORD out = 0;
		if (VRMenu::IsOpen()) {
			sRightDpadUpHeld = 0;
			sRightDpadDownHeld = 0;
			sModifierOnlyHeld = 0;
			sJournalButton.Cancel();
			InterlockedExchange(&sPadButtons, 0);
			return;
		}
		const bool mod = haveLeft && left.rAxis[1].x > 0.5f;
		const bool leftStickClick = haveLeft &&
			(left.ulButtonPressed & vr::ButtonMaskFromId(vr::k_EButton_SteamVR_Touchpad)) != 0;
		const bool rightStickClick = haveRight &&
			(right.ulButtonPressed & vr::ButtonMaskFromId(vr::k_EButton_SteamVR_Touchpad)) != 0;
		const bool journalChord = mod && leftStickClick;
		const bool journalActive = sJournalButton.Update(mod, leftStickClick,
			IsJournalPresented());
		const bool menuChord = mod && rightStickClick;
		if (mod && !journalActive && !journalChord && !menuChord)
			sModifierOnlyHeld = min(kEquipmentHoldFrames, sModifierOnlyHeld + 1);
		else
			sModifierOnlyHeld = 0;
		const bool gameMenuNavigation =
			!InterlockedCompareExchange(&sGameplayModeActive, 0, 0)
			|| InterlockedCompareExchange(&sPauseMenuExpected, 0, 0);
		const float rightX = haveRight ? right.rAxis[0].x : 0.0f;
		const float rightY = haveRight ? right.rAxis[0].y : 0.0f;
		const bool rightVertical = fabsf(rightY) > stickDead
			&& fabsf(rightY) >= fabsf(rightX);
		const bool rightDpadUp = rightVertical && rightY > 0.0f;
		const bool rightDpadDown = rightVertical && rightY < 0.0f;
		if (physicalWeaponInventory) {
			// The weapon wheel owns this stick while Y is held. Do not let menu
			// selection charge the ordinary lighter/medkit hold shortcuts.
			sRightDpadUpHeld = 0;
			sRightDpadDownHeld = 0;
		} else if (rightDpadUp) {
			sRightDpadUpHeld = min(kRightDpadHoldFrames, sRightDpadUpHeld + 1);
			sRightDpadDownHeld = 0;
		} else if (rightDpadDown) {
			sRightDpadDownHeld = min(kRightDpadHoldFrames, sRightDpadDownHeld + 1);
			sRightDpadUpHeld = 0;
		} else {
			sRightDpadUpHeld = 0;
			sRightDpadDownHeld = 0;
		}
		const bool rightDpadUpReady = sRightDpadUpHeld >= kRightDpadHoldFrames;
		const bool rightDpadDownReady = sRightDpadDownHeld >= kRightDpadHoldFrames;

		if (haveLeft) {
			const uint64_t b = left.ulButtonPressed;
			// Left trigger remains the modifier for D-pad/menu actions, and
			// also takes over the former left-grip Equipment Inventory action.
			// Modifier-only hold opens Equipment Inventory. Delaying it prevents
			// LT from opening the radial before an LT+L3/LT+R3 chord is complete.
			if (sModifierOnlyHeld >= kEquipmentHoldFrames)
				out |= XINPUT_GAMEPAD_LEFT_SHOULDER;    // Equipment Inventory
			if (b & vr::ButtonMaskFromId(vr::k_EButton_A))
				out |= XINPUT_GAMEPAD_X;                // Use / Reload / Interact
			if (b & vr::ButtonMaskFromId(vr::k_EButton_ApplicationMenu))
				out |= XINPUT_GAMEPAD_Y;                // Weapons Inventory
			// Stick click is Sprint unmodified; with the modifier it is the
			// stereo-mode toggle instead, handled in UpdateControllerInput.
			if (!mod && (b & vr::ButtonMaskFromId(vr::k_EButton_SteamVR_Touchpad)))
				out |= XINPUT_GAMEPAD_LEFT_THUMB;       // Sprint

			// Menus behave more predictably with discrete D-pad navigation than
			// with a continuously changing analog axis. Dominant axis only avoids
			// stepping both a row and a column on diagonal input.
			if (gameMenuNavigation && !mod) {
				const float x = left.rAxis[0].x, y = left.rAxis[0].y;
				if (fabsf(x) > stickDead || fabsf(y) > stickDead) {
					if (fabsf(y) >= fabsf(x))
						out |= y > 0.0f ? XINPUT_GAMEPAD_DPAD_UP
							: XINPUT_GAMEPAD_DPAD_DOWN;
					else
						out |= x > 0.0f ? XINPUT_GAMEPAD_DPAD_RIGHT
							: XINPUT_GAMEPAD_DPAD_LEFT;
				}
			}
		}

		if (haveRight) {
			const uint64_t b = right.ulButtonPressed;
			const bool aDown = (b & vr::ButtonMaskFromId(vr::k_EButton_A)) != 0;
			const bool bDown = (b & vr::ButtonMaskFromId(vr::k_EButton_ApplicationMenu)) != 0;
			if (b & vr::ButtonMaskFromId(vr::k_EButton_Grip))
				out |= XINPUT_GAMEPAD_RIGHT_SHOULDER;   // Secondary

			if (physicalWeaponInventory) {
				// Metro's weapon wheel uses D-pad directions. Translate the right
				// thumbstick immediately while Y holds the wheel open; dominant-axis
				// selection prevents diagonals from choosing two slots at once.
				if (fabsf(rightX) > stickDead || fabsf(rightY) > stickDead) {
					if (fabsf(rightY) >= fabsf(rightX))
						out |= rightY > 0.0f ? XINPUT_GAMEPAD_DPAD_UP
							: XINPUT_GAMEPAD_DPAD_DOWN;
					else
						out |= rightX > 0.0f ? XINPUT_GAMEPAD_DPAD_RIGHT
							: XINPUT_GAMEPAD_DPAD_LEFT;
				}
			} else if (mod) {
				// The equipment wheel stays open only while the modifier is held.
				// Keep the face buttons live in this branch so Metro can activate
				// the wheel entries labelled A and B without closing the wheel.
				if (aDown) out |= XINPUT_GAMEPAD_A;
				if (bDown) out |= XINPUT_GAMEPAD_B;
				if (rightStickClick) out |= XINPUT_GAMEPAD_START; // Menu

				// Right stick becomes the d-pad. Dominant axis only, so a
				// diagonal push cannot fire two actions at once - these are
				// discrete items (light, lighter, filter, medkit) where
				// firing the wrong one costs a real resource.
				const float x = rightX, y = rightY;
				if (fabsf(x) > stickDead || fabsf(y) > stickDead) {
					if (fabsf(y) >= fabsf(x)) {
						if (rightDpadUpReady)
							out |= XINPUT_GAMEPAD_DPAD_UP;    // Light
						if (rightDpadDownReady)
							out |= XINPUT_GAMEPAD_DPAD_DOWN;  // Medkit
					}
					else
						out |= (x > 0) ? XINPUT_GAMEPAD_DPAD_RIGHT // Swap Filter
						               : XINPUT_GAMEPAD_DPAD_LEFT; // Lighter
				}
			} else {
				if (aDown) out |= XINPUT_GAMEPAD_A;       // Jump
				if (bDown) out |= XINPUT_GAMEPAD_B;       // Melee Attack
				if (b & vr::ButtonMaskFromId(vr::k_EButton_SteamVR_Touchpad))
					out |= XINPUT_GAMEPAD_RIGHT_THUMB;    // Crouch Toggle
				if (!physicalWeaponInventory && rightDpadUpReady)
					out |= XINPUT_GAMEPAD_DPAD_UP;         // Lighter
				if (!physicalWeaponInventory && rightDpadDownReady)
					out |= XINPUT_GAMEPAD_DPAD_DOWN;       // Medkit
			}
		}
		if (journalActive) {
			out |= XINPUT_GAMEPAD_BACK;             // Journal (clean timed pulse)
		}

		// We generate Start ourselves (modifier + right menu button), so its
		// rising edge is a dependable pause-menu toggle. While paused movement
		// must stay cardinal so up/down does not become a compensated diagonal.
		static WORD sPreviousOut = 0;
		const bool confirmRise = (out & XINPUT_GAMEPAD_A)
			&& !(sPreviousOut & XINPUT_GAMEPAD_A);
		const bool startRise = (out & XINPUT_GAMEPAD_START)
			&& !(sPreviousOut & XINPUT_GAMEPAD_START);
		const bool journalRise = (out & XINPUT_GAMEPAD_BACK)
			&& !(sPreviousOut & XINPUT_GAMEPAD_BACK);
		const bool backOutOfMenu = (out & XINPUT_GAMEPAD_B)
			&& !(sPreviousOut & XINPUT_GAMEPAD_B)
			&& InterlockedCompareExchange(&sPauseMenuExpected, 0, 0);
		if (startRise) {
			const LONG wasPaused = InterlockedCompareExchange(&sPauseMenuExpected, 0, 0);
			InterlockedExchange(&sPauseMenuExpected, wasPaused ? 0 : 1);
		} else if (backOutOfMenu) {
			InterlockedExchange(&sPauseMenuExpected, 0);
		}
		if (journalRise)
			NotifyJournalButton(G->frame_no);
		const bool lighterRise = !physicalWeaponInventory && !gameMenuNavigation
			&& ((mod && (out & XINPUT_GAMEPAD_DPAD_LEFT)
				&& !(sPreviousOut & XINPUT_GAMEPAD_DPAD_LEFT))
				|| (!mod && (out & XINPUT_GAMEPAD_DPAD_UP)
					&& !(sPreviousOut & XINPUT_GAMEPAD_DPAD_UP)));
		if (lighterRise)
			NotifyLighterButton(G->frame_no);
		if (confirmRise
			&& InterlockedCompareExchange(&sWorldPlacedUIEverSeen, 0, 0)
			&& !InterlockedCompareExchange(&sGameplayModeActive, 0, 0))
			InterlockedExchange(&sStartupExitArmed, 1);
		// Chapter Select and Load Last Save can dismiss the menu without a
		// later Start/B edge. Clear the pause latch on the confirm edge so the
		// first gameplay input is not suppressed until an unrelated action such
		// as crouch toggles the state.
		if (confirmRise && gameMenuNavigation)
			InterlockedExchange(&sPauseMenuExpected, 0);
		sPreviousOut = out;

		InterlockedExchange(&sPadButtons, (LONG)out);
	}

	static void ProcessOpenVRRecenterEvents()
	{
		vr::VREvent_t event = {};
		bool recentered = false;
		uint32_t lastRecenterEvent = 0;
		while (sVRSystem && sVRSystem->PollNextEvent(&event, sizeof(event))) {
			if (event.eventType != vr::VREvent_SeatedZeroPoseReset
				&& event.eventType != vr::VREvent_StandingZeroPoseReset
				&& event.eventType != vr::VREvent_ChaperoneUniverseHasChanged)
				continue;
			recentered = true;
			lastRecenterEvent = event.eventType;
		}
		if (recentered) {
			LogInfo("VRPose aim: OpenVR tracking origin recentred (last event=%u); "
				"re-anchoring view and weapon once\n", lastRecenterEvent);
			RecenterTrackingOrigin();
		}
	}

	void UpdateControllerInput()
	{
		// All early returns (lost controllers, menus, real-gamepad mode) revoke
		// an unconsumed turn request. They must not leave a deferred body turn.
		NativeBodyTurnInputScope nativeBodyTurnInput;
		static const bool kControllerInputEnabled = true;
		if (!kControllerInputEnabled || !sVRSystem)
			return;
		ProcessOpenVRRecenterEvents();
		InstallNativeCameraOwnershipObserver();
		InstallNativeFlashlightActionHook();
		InstallNativeFilterReplacementHook();
		InstallGasMaskPlayerActionHook();
		LONG gasMaskDispatchCountdown = InterlockedCompareExchange(
			&sGasMaskGestureDispatchCountdown, 0, 0);
		if (gasMaskDispatchCountdown > 0)
			InterlockedDecrement(&sGasMaskGestureDispatchCountdown);

		vr::VRControllerState_t left, right;
		bool haveLeft = GetControllerState(vr::TrackedControllerRole_LeftHand, &left);
		bool haveRight = GetControllerState(vr::TrackedControllerRole_RightHand, &right);
		const bool gamepadMode = VRMenu::GetSettings().gamepadMode;
		if (!haveLeft && !haveRight && !gamepadMode)
			return;
		XINPUT_STATE gamepad = {};
		bool haveGamepad = false;
		// Use the physical pad as the menu-toggle source whenever Gamepad Mode
		// is active. If no motion controllers are present, also allow the pad to
		// reopen the menu after the setting has been turned back off.
		const bool physicalMenuInput = gamepadMode || (!haveLeft && !haveRight);
		if (physicalMenuInput && trampoline_XInputGetState) {
			if (trampoline_XInputGetState(0, &gamepad) == ERROR_SUCCESS) {
				haveGamepad = true;
				const WORD buttons = gamepad.Gamepad.wButtons;
				VRMenu::UpdateToggleInput(
					(buttons & XINPUT_GAMEPAD_LEFT_THUMB) != 0,
					(buttons & XINPUT_GAMEPAD_RIGHT_THUMB) != 0,
					false, false);
			}
		}

		const bool leftStickClick = haveLeft &&
			(left.ulButtonPressed & vr::ButtonMaskFromId(vr::k_EButton_SteamVR_Touchpad)) != 0;
		const bool rightStickClick = haveRight &&
			(right.ulButtonPressed & vr::ButtonMaskFromId(vr::k_EButton_SteamVR_Touchpad)) != 0;
		const bool leftGrip = haveLeft &&
			(left.ulButtonPressed & vr::ButtonMaskFromId(vr::k_EButton_Grip)) != 0;
		const bool rightGrip = haveRight &&
			(right.ulButtonPressed & vr::ButtonMaskFromId(vr::k_EButton_Grip)) != 0;
		static bool wasLeftGripForWeaponAcquire = false;
		const bool leftGripPressedForWeapon = leftGrip
			&& !wasLeftGripForWeaponAcquire;
		if (leftGripPressedForWeapon) {
			InterlockedExchange(&sTwoHandGripAcquireAuthorized,
				IsLeftHandNearWeapon() ? 1 : 0);
		} else if (!leftGrip) {
			InterlockedExchange(&sTwoHandGripAcquireAuthorized, 0);
		}
		wasLeftGripForWeaponAcquire = leftGrip;
		// The explicit central front-waist volume belongs to the charger. The
		// left-side hip remains the gas mask, and the higher chest remains the
		// filter, so each press is routed to one piece of equipment only.
		const bool chargerGripConsumed = UpdateNativeChargerGesture(leftGrip);
		const bool gasMaskGripConsumed = chargerGripConsumed
			? false : UpdateNativeGasMaskGesture(leftGrip);
		const bool filterGripConsumed = chargerGripConsumed || gasMaskGripConsumed
			? false : UpdateNativeFilterGesture(leftGrip);
		if (!chargerGripConsumed && !filterGripConsumed && !gasMaskGripConsumed)
			UpdateNativeFlashlightGesture(leftGrip);
		if (!haveGamepad)
			VRMenu::UpdateToggleInput(leftStickClick, rightStickClick, leftGrip, rightGrip);
		const bool menuConfirm = haveRight &&
			(right.ulButtonPressed & vr::ButtonMaskFromId(vr::k_EButton_A)) != 0;
		const bool menuCancel = haveRight &&
			(right.ulButtonPressed & vr::ButtonMaskFromId(vr::k_EButton_ApplicationMenu)) != 0;
		// SteamVR bindings normally expose the thumbstick/trackpad as axis 0,
		// but some controller profiles place it on a later non-trigger axis.
		// Keep axis 0 authoritative and use a later axis only when axis 0 is
		// neutral, so trigger pressure cannot become menu navigation.
		float menuLeftY = haveLeft ? left.rAxis[0].y : 0.0f;
		float menuRightX = haveRight ? right.rAxis[0].x : 0.0f;
		if (fabsf(menuRightX) < 0.55f && haveRight) {
			for (int axis = 2; axis < 5; ++axis) {
				if (fabsf(right.rAxis[axis].x) > fabsf(menuRightX))
					menuRightX = right.rAxis[axis].x;
			}
		}
		if (!haveGamepad)
			VRMenu::UpdateNavigation(haveLeft ? left.rAxis[0].x : 0.0f,
				menuLeftY,
				menuRightX,
				menuConfirm, menuCancel);
		static KeyBinding kW = { SC_W, false }, kA = { SC_A, false };
		static KeyBinding kS = { SC_S, false }, kD = { SC_D, false };
		if (VRMenu::IsOpen()) {
			if (haveGamepad) {
					const WORD buttons = gamepad.Gamepad.wButtons;
					VRMenu::UpdateTabNavigation(
						(buttons & XINPUT_GAMEPAD_LEFT_SHOULDER) != 0,
						(buttons & XINPUT_GAMEPAD_RIGHT_SHOULDER) != 0);
					VRMenu::UpdateNavigation(
						gamepad.Gamepad.sThumbLX / 32767.0f,
						gamepad.Gamepad.sThumbLY / 32767.0f,
						gamepad.Gamepad.sThumbRX / 32767.0f,
						(buttons & XINPUT_GAMEPAD_A) != 0,
						(buttons & XINPUT_GAMEPAD_B) != 0);
			}
			// The menu owns both sticks, grips, face buttons and triggers while
			// visible. Release any gameplay action that was already held before
			// returning so the player cannot fire, turn, walk, ADS, or interact
			// through the menu.
			PublishPadLeftStick(0.0f, 0.0f);
			// This return occurs before PublishPadButtons(), so clearing only the
			// keyboard/mouse paths left the last synthetic XInput button mask
			// latched in Metro. In particular, holding the menu chord could leave
			// its melee-bound button repeating throughout weapon positioning.
			// Publish an explicit all-buttons-up state for the XInput hook and keep
			// it neutral on every frame while the menu owns input.
			InterlockedExchange(&sPadButtons, 0);
			InterlockedExchange(&sWeaponInventoryHeld, 0);
			InterlockedExchange(&sPadFollowRX, 0);
			InterlockedExchange(&sPadFollowRY, 0);
			SetKeyState(&kW, false); SetKeyState(&kS, false);
			SetKeyState(&kD, false); SetKeyState(&kA, false);
			SendMouseButton(false, false);
			InterlockedExchange(&sFiringNow, 0);
			InterlockedExchange(&sAimingNow, 0);
			return;
		}
		if (VRMenu::GetSettings().gamepadMode) {
			// Keep the XInput hook installed so the real pad can pass through,
			// but stop all VR-controller gameplay synthesis and mouse actions.
			InstallXInputHooks();
			PublishPadLeftStick(0.0f, 0.0f);
			InterlockedExchange(&sPadButtons, 0);
			InterlockedExchange(&sFiringNow, 0);
			InterlockedExchange(&sAimingNow, 0);
			SendMouseButton(false, false);
			return;
		}

		static bool sLogged = false;
		if (!sLogged) {
			LogInfo("VRPose input: VR controller input active (left=%d right=%d)\n", haveLeft, haveRight);
			sLogged = true;
		}

		// Movement only. Everything else that used to be synthesised as a
		// keystroke here now goes out through the virtual pad instead
		// (PublishPadButtons), so the game applies its own preset-3 binds
		// and reaches actions like Equipment Inventory and Lighter that have
		// no keyboard bind to send in the first place.
		const float kStickDead = 0.35f;
		const VRMenu::Settings &controlMenu = VRMenu::GetSettings();
		const bool modHeld = haveLeft && left.rAxis[1].x > 0.5f;
		static const bool kNonWeaponCalibrationEnabled = false;
		if (!kNonWeaponCalibrationEnabled) {
			InterlockedExchange(&sJournalAdjustMode, 0);
			InterlockedExchange(&sLeftHandAdjustMode, 0);
			InterlockedExchange(&sWatchAdjustMode, 0);
			InterlockedExchange(&sWatchTimeAdjustMode, 0);
			InterlockedExchange(&sLighterAdjustMode, 0);
			InterlockedExchange(&sLighterFlameAdjustMode, 0);
		}

		// JOURNAL PAGE CALIBRATION. The renderer, not an input guess, tells us
		// when the objective glyph draw is actually live. LT + right grip held
		// for one second toggles this mode while the journal is visible. This is
		// the watch-calibration chord too, but the confirmed journal draw takes
		// precedence, so the two calibrations cannot change together.
		const LONG journalFrame = InterlockedCompareExchange(&sJournalObjectiveFrame, 0, 0);
		const LONG frameDelta = (LONG)G->frame_no - journalFrame;
		const bool journalVisible = frameDelta >= 0 && frameDelta <= 3;
		// The journal lighter deliberately reuses LT + right grip for its own
		// body calibration. Its exact body draw wins that chord while present;
		// otherwise the established journal-page calibration remains available.
		const bool journalCombo = kNonWeaponCalibrationEnabled
			&& journalVisible && modHeld && rightGrip
			&& !IsJournalLighterRecentlyActive();
		static int sJournalHeld = 0;
		if (journalCombo) {
			if (++sJournalHeld == 72) {
				LoadJournalAdjust();
				const LONG next = InterlockedCompareExchange(&sJournalAdjustMode, 0, 0) ? 0 : 1;
				InterlockedExchange(&sJournalAdjustMode, next);
				if (!next)
					SaveJournalAdjust();
				LogInfo("VRPose journal: adjustment mode %s - left=position, left grip=depth, "
					"right=rotation, right grip=roll/scale\n", next ? "ON" : "OFF (saved)");
			}
		} else {
			sJournalHeld = 0;
		}

		const bool journalMode = InterlockedCompareExchange(&sJournalAdjustMode, 0, 0) != 0;
		if ((journalMode && journalVisible) || journalCombo) {
			LoadJournalAdjust();
			const float lx = haveLeft ? left.rAxis[0].x : 0.0f;
			const float ly = haveLeft ? left.rAxis[0].y : 0.0f;
			const float rx = haveRight ? right.rAxis[0].x : 0.0f;
			const float ry = haveRight ? right.rAxis[0].y : 0.0f;
			const float kPositionRate = 0.0015f;
			const float kRotationRate = 0.0100f;
			const float kScaleRate = 0.0060f;
			bool changed = false;
			if (fabsf(lx) > kStickDead) {
				sJournalAdjustPosition[0] += lx * kPositionRate;
				changed = true;
			}
			if (fabsf(ly) > kStickDead) {
				sJournalAdjustPosition[leftGrip ? 2 : 1] += ly * kPositionRate;
				changed = true;
			}
			if (rightGrip) {
				if (fabsf(rx) > kStickDead) {
					sJournalAdjustRotation[2] += rx * kRotationRate;
					changed = true;
				}
				if (fabsf(ry) > kStickDead) {
					sJournalAdjustScale += ry * kScaleRate;
					changed = true;
				}
			} else {
				if (fabsf(rx) > kStickDead) {
					sJournalAdjustRotation[1] -= rx * kRotationRate;
					changed = true;
				}
				if (fabsf(ry) > kStickDead) {
					sJournalAdjustRotation[0] += ry * kRotationRate;
					changed = true;
				}
			}
			for (int i = 0; i < 3; ++i) {
				sJournalAdjustPosition[i] = max(-1.0f, min(1.0f, sJournalAdjustPosition[i]));
				sJournalAdjustRotation[i] = max(-3.14159f, min(3.14159f, sJournalAdjustRotation[i]));
			}
			sJournalAdjustScale = max(0.25f, min(3.0f, sJournalAdjustScale));
			static unsigned sJournalAdjustLog = 0;
			if (changed && (sJournalAdjustLog++ % 24) == 0)
				LogInfo("VRPose journal: live pos=(%.3f %.3f %.3f) rot=(%.1f %.1f %.1f) scale=%.3f\n",
					sJournalAdjustPosition[0], sJournalAdjustPosition[1], sJournalAdjustPosition[2],
					sJournalAdjustRotation[0] * 57.29578f, sJournalAdjustRotation[1] * 57.29578f,
					sJournalAdjustRotation[2] * 57.29578f, sJournalAdjustScale);

			// Calibration owns all gameplay input while the page is visible.
			InstallXInputHooks();
			PublishPadLeftStick(0.0f, 0.0f);
			InterlockedExchange(&sPadButtons, 0);
			SetKeyState(&kW, false); SetKeyState(&kS, false);
			SetKeyState(&kD, false); SetKeyState(&kA, false);
			SendMouseButton(false, false);
			InterlockedExchange(&sFiringNow, 0);
			InterlockedExchange(&sAimingNow, 0);
			return;
		}

		InstallXInputHooks();
		PublishPadButtons(left, haveLeft, right, haveRight, kStickDead);
		if (!haveLeft)
			PublishPadLeftStick(0.0f, 0.0f);

		// The hand, physical-watch, and watch-time adjustment controls were
		// temporary setup tools. Their final headset-verified values are embedded,
		// so release input must not expose those modes or steal the weapon
		// calibration chord. Keep the underlying loaders/getters for compatibility
		// with existing calibration files and for easy future development.
		static const bool kHandWatchCalibrationEnabled = kNonWeaponCalibrationEnabled;

		// NATIVE WATCH-TIME CALIBRATION. Hold LT + both grips for about one
		// second to toggle. This is deliberately distinct from LT + left grip
		// (hand) and LT + right grip (whole physical watch), so calibrating the
		// readout can never move either mesh. Values save only when toggled off.
		const bool watchTimeCombo = kHandWatchCalibrationEnabled
			&& !journalVisible && modHeld && leftGrip && rightGrip;
		static int sWatchTimeHeld = 0;
		if (watchTimeCombo) {
			if (++sWatchTimeHeld == 72) {
				LoadWatchTimeAdjust();
				const LONG next = InterlockedCompareExchange(
					&sWatchTimeAdjustMode, 0, 0) ? 0 : 1;
				InterlockedExchange(&sWatchTimeAdjustMode, next);
				if (next) {
					InterlockedExchange(&sLeftHandAdjustMode, 0);
					InterlockedExchange(&sWatchAdjustMode, 0);
				} else {
					SaveWatchTimeAdjust();
				}
				LogInfo("VRPose watch time: adjustment mode %s - "
					"left=position, left grip=depth, right=rotation, "
					"right grip=roll/scale\n", next ? "ON" : "OFF (saved)");
			}
		} else {
			sWatchTimeHeld = 0;
		}

		// True while the left trigger is held - the modifier. Kept here as
		// well as inside PublishPadButtons because the mod's OWN controls
		// (recentre, stereo toggle) hang off the same modifier, and because
		// turning has to be suppressed while it is held or pushing the
		// stick right would turn the player as well as swapping a filter.
		static int sHandHeld = 0;
		const bool handCombo = kHandWatchCalibrationEnabled
			&& !watchTimeCombo && modHeld && haveLeft
			&& (left.ulButtonPressed & vr::ButtonMaskFromId(vr::k_EButton_Grip)) != 0;
		if (handCombo) {
			if (++sHandHeld == 72) {
				const LONG next = InterlockedCompareExchange(&sLeftHandAdjustMode, 0, 0) ? 0 : 1;
				if (next)
					sLeftHandAdjustingPrologue = IsPrologueLeftHandRecentlyActive();
				InterlockedExchange(&sLeftHandAdjustMode, next);
				if (!next) {
					if (sLeftHandAdjustingPrologue) SavePrologueLeftHandAdjust();
					else SaveLeftHandAdjust();
				}
				if (sLeftHandAdjustingPrologue) LoadPrologueLeftHandAdjust();
				else LoadLeftHandAdjust();
				const float *activeHand = sLeftHandAdjustingPrologue
					? sPrologueLeftHandAdjust : sLeftHandAdjust;
				LogInfo("VRPose %shand: adjustment mode %s (%.3f %.3f %.3f)\n",
					sLeftHandAdjustingPrologue ? "prologue " : "",
					next ? "ON" : "OFF", activeHand[0], activeHand[1], activeHand[2]);
			}
		} else {
			sHandHeld = 0;
		}

		// Watch positioning is deliberately separate from hand positioning.
		// Hold LT + RIGHT grip for about one second to toggle it. This prevents
		// watch calibration from ever changing the saved hand transform again.
		static int sWatchHeld = 0;
		const bool watchCombo = kHandWatchCalibrationEnabled
			&& !watchTimeCombo && modHeld && haveRight
			&& (right.ulButtonPressed & vr::ButtonMaskFromId(vr::k_EButton_Grip)) != 0;
		if (watchCombo && !handCombo) {
			if (++sWatchHeld == 72) {
				const LONG next = InterlockedCompareExchange(&sWatchAdjustMode, 0, 0) ? 0 : 1;
				if (next)
					sWatchAdjustingPrologue = IsPrologueLeftHandRecentlyActive();
				if (sWatchAdjustingPrologue) LoadPrologueWatchAdjust();
				else LoadWatchAdjust();
				InterlockedExchange(&sWatchAdjustMode, next);
				if (next) {
					InterlockedExchange(&sLeftHandAdjustMode, 0);
					InterlockedExchange(&sWatchTimeAdjustMode, 0);
				} else {
					if (sWatchAdjustingPrologue) SavePrologueWatchAdjust();
					else SaveWatchAdjust();
				}
				LogInfo("VRPose %swatch assembly: adjustment mode %s - "
					"left=position, left grip=depth, right=rotation, "
					"left grip+right X=roll\n",
					sWatchAdjustingPrologue ? "prologue " : "",
					next ? "ON" : "OFF (saved)");
			}
		} else {
			sWatchHeld = 0;
		}

		// Left stick drives movement. Axis 0 is the joystick/touchpad under
		// SteamVR's legacy input emulation.
		if (haveLeft || (controlMenu.swapAnalogSticks && haveRight)) {
			const vr::VRControllerState_t &moveController =
				controlMenu.swapAnalogSticks ? right : left;
			float mx = moveController.rAxis[0].x, my = moveController.rAxis[0].y;
			// Some OpenVR legacy controller profiles retain the last Axis0
			// coordinates after the player releases the physical stick/touchpad.
			// Learn whether this controller exposes capacitive Axis0 touch before
			// relying on it, so controllers with no touch sensor keep working.
			// Once observed, no touch is an authoritative neutral sample and must
			// override any stale coordinates returned by the runtime.
			static bool sMovementAxisTouchSupported = false;
			const bool movementAxisTouched =
				(moveController.ulButtonTouched &
				 vr::ButtonMaskFromId(vr::k_EButton_SteamVR_Touchpad)) != 0;
			if (movementAxisTouched)
				sMovementAxisTouchSupported = true;
			if (sMovementAxisTouchSupported && !movementAxisTouched) {
				mx = 0.0f;
				my = 0.0f;
			}
			uint64_t b = left.ulButtonPressed;

			// Flame assembly calibration is deliberately separate from the lighter
			// body placement. Hold LT + both grips for one second while the lighter
			// is visible to toggle. This exact chord excludes the body and weapon
			// calibration chords below.
			// Both body and particle assemblies are now locked after the standalone
			// flame was calibrated and verified across a process restart against its
			// deterministic per-variant anchor. Keep the implementation for future
			// development, but expose no lighter setup chord in release input.
			// Main-game regular and journal values are release-locked. Only the two
			// newly isolated prologue profiles expose calibration controls.
			static const bool kPrologueLighterCalibrationEnabled = false;

			// CHARGER-ONLY CALIBRATION. The exact 14544-index charger body marks
			// this state from the render path. While it is live, LT+left grip is
			// never allowed to reach weapon calibration. The right-hand target is
			// a second independent mode/file, so neither target can modify guns or
			// the general-purpose right-hand placement.
			static const bool kChargerCalibrationEnabled = false;
			if (!kChargerCalibrationEnabled) {
				InterlockedExchange(&sChargerAdjustMode, 0);
				InterlockedExchange(&sChargerRightHandAdjustMode, 0);
			}
			const bool chargerVisible = IsChargerVisualActive();
			const bool chargerWasAdjusting = InterlockedCompareExchange(
				&sChargerAdjustMode, 0, 0) != 0;
			const bool chargerRightWasAdjusting = InterlockedCompareExchange(
				&sChargerRightHandAdjustMode, 0, 0) != 0;
			const bool chargerCombo = kChargerCalibrationEnabled
				&& modHeld && leftGrip && !rightGrip
				&& !chargerRightWasAdjusting
				&& (chargerVisible || chargerWasAdjusting);
			const bool chargerRightCombo = kChargerCalibrationEnabled
				&& modHeld && rightGrip && !leftGrip
				&& !chargerWasAdjusting
				&& (chargerVisible || chargerRightWasAdjusting);
			static int sChargerHeld = 0;
			static int sChargerRightHeld = 0;
			if (chargerCombo) {
				if (++sChargerHeld == 72) {
					const LONG next = chargerWasAdjusting ? 0 : 1;
					LoadChargerAdjust();
					InterlockedExchange(&sChargerAdjustMode, next);
					InterlockedExchange(&sWeaponAdjustMode, 0);
					if (!next)
						SaveChargerAdjust();
					LogInfo("VRPose charger: assembly adjustment mode %s - "
						"left=position, left grip=depth, right=rotation, "
						"right grip=roll\n", next ? "ON" : "OFF (saved)");
				}
			} else {
				sChargerHeld = 0;
			}
			if (chargerRightCombo) {
				if (++sChargerRightHeld == 72) {
					const LONG next = chargerRightWasAdjusting ? 0 : 1;
					LoadChargerRightHandAdjust();
					InterlockedExchange(&sChargerRightHandAdjustMode, next);
					InterlockedExchange(&sWeaponAdjustMode, 0);
					if (!next)
						SaveChargerRightHandAdjust();
					LogInfo("VRPose charger: right-hand adjustment mode %s - "
						"left=position, left grip=depth, right=rotation, "
						"right grip=roll\n", next ? "ON" : "OFF (saved)");
				}
			} else {
				sChargerRightHeld = 0;
			}

			static int sLighterFlameHeld = 0;
			const bool flameWasAdjusting = InterlockedCompareExchange(
				&sLighterFlameAdjustMode, 0, 0) != 0;
			const bool lighterFlameCombo =
				kPrologueLighterCalibrationEnabled
				&& modHeld && leftGrip
				&& rightGrip
				&& (flameWasAdjusting || (IsLighterBodyRecentlyActive()
					&& IsPrologueLighterRecentlyActive()));
			if (lighterFlameCombo) {
				if (++sLighterFlameHeld == 72) {
					const LONG next = flameWasAdjusting ? 0 : 1;
					if (next)
						sLighterFlameAdjustingVariant = ActiveLighterVariant();
					LoadLighterVariantFlameAdjust(
						sLighterFlameAdjustingVariant);
					InterlockedExchange(&sLighterFlameAdjustMode, next);
					if (next) {
						InterlockedExchange(&sLighterAdjustMode, 0);
						InterlockedExchange(&sWeaponAdjustMode, 0);
						InterlockedExchange(&sJournalAdjustMode, 0);
					} else {
						SaveLighterVariantFlameAdjust(
							sLighterFlameAdjustingVariant);
					}
					LogInfo("VRPose %s lighter flame: adjustment mode %s - "
						"left=group position/depth, right up/down=scale, "
						"right left/right=secondary X, right grip+right=secondary Y/Z\n",
						LighterVariantName(sLighterFlameAdjustingVariant),
						next ? "ON" : "OFF (saved)");
				}
			} else {
				sLighterFlameHeld = 0;
			}

			// Temporary lighter-only placement. Its exact renderer draw marks the
			// lighter as present, so LT + right grip cannot steal normal controls
			// while it is put away. Once enabled, the same chord remains available
			// to save and leave the mode even if the lighter disappears.
			static int sLighterHeld = 0;
			const bool lighterWasAdjusting =
				InterlockedCompareExchange(&sLighterAdjustMode, 0, 0) != 0;
			const bool lighterCombo = kPrologueLighterCalibrationEnabled
				&& modHeld && rightGrip
				&& !leftGrip && !flameWasAdjusting
				&& ((IsLighterBodyRecentlyActive()
					&& IsPrologueLighterRecentlyActive()) || lighterWasAdjusting);
			if (lighterCombo) {
				if (++sLighterHeld == 72) {
					const LONG next = lighterWasAdjusting ? 0 : 1;
					if (next)
						sLighterAdjustingVariant = ActiveLighterVariant();
					LoadLighterVariantAdjust(sLighterAdjustingVariant);
					InterlockedExchange(&sLighterAdjustMode, next);
					if (next) {
						InterlockedExchange(&sWeaponAdjustMode, 0);
						InterlockedExchange(&sJournalAdjustMode, 0);
					} else {
						SaveLighterVariantAdjust(sLighterAdjustingVariant);
					}
					LogInfo("VRPose %s lighter: adjustment mode %s - left=position, "
						"left grip=depth, right=rotation, right grip=roll\n",
						LighterVariantName(sLighterAdjustingVariant),
						next ? "ON" : "OFF (saved)");
				}
			} else {
				sLighterHeld = 0;
			}

			// WEAPON POSITION ADJUSTMENT.
			//
			// Hold the left stick CLICK for about a second to toggle it. A hold
			// rather than a tap because that click is sprint, and a tap has to
			// keep working as sprint.
			//
			// While adjusting, the stick moves the gun instead of the player:
			//   stick up/down    - raise / LOWER
			//   stick left/right - left / RIGHT
			//   left grip + up/down - FORWARD / back
			//
			// Two axes on one stick needs the grip as a shift key; height and
			// lateral are unmodified because those were the ones asked for.
			// Movement keys are suppressed so the player does not walk off
			// while lining the weapon up.
			//
			// Disabled for now - every currently-owned weapon (SMG,
			// Revolver) is calibrated, so this control has nothing left to
			// do until a new weapon is unlocked, and leaving it live just
			// means an accidental long hold on the sprint stick could open
			// it during normal play. Flip back to true to calibrate the
			// next one; the rest of this block is unchanged.
			// Re-enabled 2026-08-09 for SIGHT ZEROING. Live report: bullets
			// land where the flat game's reticle sits, left of where the gun
			// visually points - which is exactly the fixed offset between the
			// weapon model's axis and the direction the aim is steered in,
			// i.e. what mode 2 below exists to dial out.
			//
			// Moved off the plain left stick click, which is Sprint now, onto
			// MODIFIER + left grip. Equipment Inventory still fires on that
			// grip unmodified, so nothing is lost.
			// The temporary left-hand setup mode previously owned this chord. Its
			// final transform is now embedded and its input is disabled, so restore
			// the established weapon calibration control for release.
			static const bool kWeaponCalibrationEnabled = true;
			if (kWeaponCalibrationEnabled
				&& !chargerVisible
				&& !chargerWasAdjusting && !chargerRightWasAdjusting
				&& !chargerCombo && !chargerRightCombo
				&& InterlockedCompareExchange(&sLighterAdjustMode, 0, 0) == 0
				&& InterlockedCompareExchange(&sLighterFlameAdjustMode, 0, 0) == 0
				&& !lighterCombo && !lighterFlameCombo) {
				static int sHeld = 0;
				const bool click = modHeld &&
					(b & vr::ButtonMaskFromId(vr::k_EButton_Grip)) != 0;
				if (click) {
					if (++sHeld == 72) {     // ~1s at 72 Hz
						// One mode: left stick translates and right stick rotates.
						const LONG cur = InterlockedCompareExchange(&sWeaponAdjustMode, 0, 0);
						const LONG next = cur ? 0 : 1;
						InterlockedExchange(&sWeaponAdjustMode, next);
						// Physical calibration owns the live values from this point.
						// Discard any unsaved VR-menu preview lock immediately.
						sWeaponMenuPreviewDirty = false;
						sWeaponMenuPreviewKey = -1;
						sWeaponMenuPreviewMeshes.clear();
						const char *name = next ? "ON (left stick position, right stick rotation)" : "OFF (saved)";
						LogInfo("VRPose weapon: adjust mode %s - offset (%.3f %.3f %.3f) "
							"rotation pitch=%.2f yaw=%.2f deg\n", name,
							sWeaponAdjust[0], sWeaponAdjust[1], sWeaponAdjust[2],
							sShotAimBias[0] * 57.29578f, sShotAimBias[1] * 57.29578f);

						if (next == 0) {
							SaveCurrentWeaponAdjust();
						} else {
							sWeaponAdjustObservedMeshes.clear();
							sWeaponCalibrationBodyMeshes.clear();
						}
					}
				} else {
					sHeld = 0;
				}
			}

			const LONG mode = InterlockedCompareExchange(&sWeaponAdjustMode, 0, 0);
			const LONG handMode = InterlockedCompareExchange(&sLeftHandAdjustMode, 0, 0);
			const LONG watchMode = InterlockedCompareExchange(&sWatchAdjustMode, 0, 0);
			const LONG watchTimeMode = InterlockedCompareExchange(
				&sWatchTimeAdjustMode, 0, 0);
			const LONG lighterMode = InterlockedCompareExchange(
				&sLighterAdjustMode, 0, 0);
			const LONG lighterFlameMode = InterlockedCompareExchange(
				&sLighterFlameAdjustMode, 0, 0);
			const LONG chargerMode = InterlockedCompareExchange(
				&sChargerAdjustMode, 0, 0);
			const LONG chargerRightMode = InterlockedCompareExchange(
				&sChargerRightHandAdjustMode, 0, 0);
			if (chargerMode || chargerRightMode) {
				PublishPadLeftStick(0.0f, 0.0f);
				InterlockedExchange(&sPadButtons, 0);
				float *activePosition = chargerRightMode
					? sChargerRightHandAdjust : sChargerAdjust;
				float *activeRotation = chargerRightMode
					? sChargerRightHandRotationAdjust : sChargerRotationAdjust;
				if (chargerRightMode)
					LoadChargerRightHandAdjust();
				else
					LoadChargerAdjust();
				const float rx = haveRight ? right.rAxis[0].x : 0.0f;
				const float ry = haveRight ? right.rAxis[0].y : 0.0f;
				const float kPositionRate = 0.0015f;
				const float kRotationRate = 0.0100f;
				bool changed = false;
				if (fabsf(mx) > kStickDead) {
					activePosition[0] += mx * kPositionRate;
					changed = true;
				}
				if (fabsf(my) > kStickDead) {
					activePosition[leftGrip ? 2 : 1] += my * kPositionRate;
					changed = true;
				}
				if (rightGrip) {
					if (fabsf(rx) > kStickDead) {
						activeRotation[2] += rx * kRotationRate;
						changed = true;
					}
				} else {
					if (fabsf(rx) > kStickDead) {
						activeRotation[1] -= rx * kRotationRate;
						changed = true;
					}
					if (fabsf(ry) > kStickDead) {
						activeRotation[0] += ry * kRotationRate;
						changed = true;
					}
				}
				for (unsigned i = 0; i < 3; ++i) {
					activePosition[i] = max(-0.75f, min(0.75f,
						activePosition[i]));
					activeRotation[i] = max(-3.14159f, min(3.14159f,
						activeRotation[i]));
				}
				static unsigned chargerAdjustLog = 0;
				if (changed && (chargerAdjustLog++ % 12) == 0)
					LogInfo("VRPose charger: live %s pos=(%.3f %.3f %.3f) "
						"rot=(%.1f %.1f %.1f deg)\n",
						chargerRightMode ? "right hand" : "assembly",
						activePosition[0], activePosition[1], activePosition[2],
						activeRotation[0] * 57.29578f,
						activeRotation[1] * 57.29578f,
						activeRotation[2] * 57.29578f);
				SetKeyState(&kW, false); SetKeyState(&kS, false);
				SetKeyState(&kD, false); SetKeyState(&kA, false);
			} else if (lighterFlameMode) {
				PublishPadLeftStick(0.0f, 0.0f);
				float *activeFlame = NULL, *activeSecondary = NULL;
				float *activeScalePointer = NULL;
				GetLighterVariantFlameAdjust(sLighterFlameAdjustingVariant,
					activeFlame, activeSecondary, activeScalePointer);
				float &activeScale = *activeScalePointer;
				const float rx = haveRight ? right.rAxis[0].x : 0.0f;
				const float ry = haveRight ? right.rAxis[0].y : 0.0f;
				const float kPositionRate = 0.0010f;
				const float kScaleRate = 0.0100f;
				bool changed = false;
				if (fabsf(mx) > kStickDead) {
					activeFlame[0] += mx * kPositionRate;
					changed = true;
				}
				if (fabsf(my) > kStickDead) {
					activeFlame[leftGrip ? 2 : 1] += my * kPositionRate;
					changed = true;
				}
				if (rightGrip) {
					if (fabsf(rx) > kStickDead) {
						activeSecondary[2] += rx * kPositionRate;
						changed = true;
					}
					if (fabsf(ry) > kStickDead) {
						activeSecondary[1] += ry * kPositionRate;
						changed = true;
					}
				} else {
					if (fabsf(rx) > kStickDead) {
						activeSecondary[0] += rx * kPositionRate;
						changed = true;
					}
					if (fabsf(ry) > kStickDead) {
						activeScale += ry * kScaleRate;
						changed = true;
					}
				}
				for (int i = 0; i < 3; ++i) {
					activeFlame[i] = max(-0.5f,
						min(0.5f, activeFlame[i]));
					activeSecondary[i] = max(-0.5f,
						min(0.5f, activeSecondary[i]));
				}
				const float maxScale = (sLighterFlameAdjustingVariant & 1u)
					? 6.0f : 3.0f;
				activeScale = max(0.25f, min(maxScale, activeScale));
				static unsigned flameAdjustLog = 0;
				if (changed && (flameAdjustLog++ % 12) == 0)
					LogInfo("VRPose %s lighter flame: live group=(%.3f %.3f %.3f) "
						"secondary=(%.3f %.3f %.3f) scale=%.3f\n",
						LighterVariantName(sLighterFlameAdjustingVariant),
						activeFlame[0], activeFlame[1], activeFlame[2],
						activeSecondary[0], activeSecondary[1],
						activeSecondary[2], activeScale);
				SetKeyState(&kW, false); SetKeyState(&kS, false);
				SetKeyState(&kD, false); SetKeyState(&kA, false);
			} else if (lighterMode) {
				PublishPadLeftStick(0.0f, 0.0f);
				float *activeLighter = NULL, *activeRotation = NULL;
				GetLighterVariantAdjust(sLighterAdjustingVariant,
					activeLighter, activeRotation);
				const float rx = haveRight ? right.rAxis[0].x : 0.0f;
				const float ry = haveRight ? right.rAxis[0].y : 0.0f;
				const float kPositionRate = 0.0020f;
				const float kRotationRate = 0.0100f;
				bool changed = false;
				if (fabsf(mx) > kStickDead) {
					activeLighter[0] += mx * kPositionRate;
					changed = true;
				}
				if (fabsf(my) > kStickDead) {
					activeLighter[leftGrip ? 2 : 1] += my * kPositionRate;
					changed = true;
				}
				if (rightGrip) {
					if (fabsf(rx) > kStickDead) {
						activeRotation[2] += rx * kRotationRate;
						changed = true;
					}
				} else {
					if (fabsf(rx) > kStickDead) {
						activeRotation[1] -= rx * kRotationRate;
						changed = true;
					}
					if (fabsf(ry) > kStickDead) {
						activeRotation[0] += ry * kRotationRate;
						changed = true;
					}
				}
				for (int i = 0; i < 3; ++i) {
					activeLighter[i] = max(-1.5f, min(1.5f, activeLighter[i]));
					activeRotation[i] = max(-3.14159f,
						min(3.14159f, activeRotation[i]));
				}
				static unsigned lighterAdjustLog = 0;
				if (changed && (lighterAdjustLog++ % 12) == 0)
					LogInfo("VRPose %s lighter: live pos=(%.3f %.3f %.3f) "
						"rot=(%.1f %.1f %.1f deg)\n",
						LighterVariantName(sLighterAdjustingVariant),
						activeLighter[0], activeLighter[1], activeLighter[2],
						activeRotation[0] * 57.29578f,
						activeRotation[1] * 57.29578f,
						activeRotation[2] * 57.29578f);
				SetKeyState(&kW, false); SetKeyState(&kS, false);
				SetKeyState(&kD, false); SetKeyState(&kA, false);
			} else if (watchTimeMode) {
				PublishPadLeftStick(0.0f, 0.0f);
				LoadWatchTimeAdjust();
				const float rx = haveRight ? right.rAxis[0].x : 0.0f;
				const float ry = haveRight ? right.rAxis[0].y : 0.0f;
				const float kPositionRate = 0.0015f;
				const float kRotationRate = 0.0100f;
				// The original 0.006/frame changed scale so slowly that a deliberate
				// up/down test returned from 1.022 to 1.002 without a perceptible
				// size difference. Make this control visibly responsive while retaining
				// continuous fine adjustment and the existing 0.1..4.0 clamp.
				const float kScaleRate = 0.0150f;
				bool changed = false;
				if (fabsf(mx) > kStickDead) {
					sWatchTimeAdjustPosition[0] += mx * kPositionRate;
					changed = true;
				}
				if (fabsf(my) > kStickDead) {
					sWatchTimeAdjustPosition[leftGrip ? 2 : 1]
						+= my * kPositionRate;
					changed = true;
				}
				if (rightGrip) {
					if (fabsf(rx) > kStickDead) {
						sWatchTimeAdjustRotation[2] += rx * kRotationRate;
						changed = true;
					}
					if (fabsf(ry) > kStickDead) {
						sWatchTimeAdjustScale += ry * kScaleRate;
						changed = true;
					}
				} else {
					if (fabsf(rx) > kStickDead) {
						sWatchTimeAdjustRotation[1] -= rx * kRotationRate;
						changed = true;
					}
					if (fabsf(ry) > kStickDead) {
						sWatchTimeAdjustRotation[0] += ry * kRotationRate;
						changed = true;
					}
				}
				for (int i = 0; i < 3; ++i) {
					sWatchTimeAdjustPosition[i] = max(-1.5f,
						min(1.5f, sWatchTimeAdjustPosition[i]));
					sWatchTimeAdjustRotation[i] = max(-3.14159f,
						min(3.14159f, sWatchTimeAdjustRotation[i]));
				}
				sWatchTimeAdjustScale = max(0.1f,
					min(4.0f, sWatchTimeAdjustScale));
				static unsigned watchTimeAdjustLog = 0;
				if (changed && (watchTimeAdjustLog++ % 20) == 0)
					LogInfo("VRPose watch time: live pos=(%.3f %.3f %.3f) "
						"rot=(%.1f %.1f %.1f) scale=%.3f\n",
						sWatchTimeAdjustPosition[0], sWatchTimeAdjustPosition[1],
						sWatchTimeAdjustPosition[2],
						sWatchTimeAdjustRotation[0] * 57.29578f,
						sWatchTimeAdjustRotation[1] * 57.29578f,
						sWatchTimeAdjustRotation[2] * 57.29578f,
						sWatchTimeAdjustScale);
				SetKeyState(&kW, false); SetKeyState(&kS, false);
				SetKeyState(&kD, false); SetKeyState(&kA, false);
			} else if (watchMode) {
				PublishPadLeftStick(0.0f, 0.0f);
				if (sWatchAdjustingPrologue) LoadPrologueWatchAdjust();
				else LoadWatchAdjust();
				float *activeWatchAdjust = sWatchAdjustingPrologue
					? sPrologueWatchAdjust : sWatchAdjust;
				float *activeWatchRotation = sWatchAdjustingPrologue
					? sPrologueWatchRotationAdjust : sWatchRotationAdjust;
				const float kRate = 0.0015f;
				const float kRotRate = 0.0100f;
				const bool depth = (b & vr::ButtonMaskFromId(vr::k_EButton_Grip)) != 0;
				if (fabsf(my) > kStickDead)
					activeWatchAdjust[depth ? 2 : 1] += (my > 0 ? kRate : -kRate);
				if (fabsf(mx) > kStickDead)
					activeWatchAdjust[0] += (mx > 0 ? kRate : -kRate);
				const float rx = haveRight ? right.rAxis[0].x : 0.0f;
				const float ry = haveRight ? right.rAxis[0].y : 0.0f;
				bool changed = fabsf(mx) > kStickDead || fabsf(my) > kStickDead;
				if (depth) {
					if (fabsf(rx) > kStickDead) {
						activeWatchRotation[2] += rx > 0 ? kRotRate : -kRotRate;
						changed = true;
					}
				} else {
					if (fabsf(rx) > kStickDead) {
						activeWatchRotation[1] += rx > 0 ? kRotRate : -kRotRate;
						changed = true;
					}
					if (fabsf(ry) > kStickDead) {
						activeWatchRotation[0] += ry > 0 ? kRotRate : -kRotRate;
						changed = true;
					}
				}
				for (int i = 0; i < 3; ++i) {
					activeWatchAdjust[i] = max(-1.5f, min(1.5f, activeWatchAdjust[i]));
					activeWatchRotation[i] = max(-3.14159f,
						min(3.14159f, activeWatchRotation[i]));
				}
				static unsigned watchAssemblyLog = 0;
				if (changed && (watchAssemblyLog++ % 12) == 0)
					LogInfo("VRPose watch assembly: live pos=(%.3f %.3f %.3f) "
						"rot=(%.1f %.1f %.1f deg)\n",
						activeWatchAdjust[0], activeWatchAdjust[1], activeWatchAdjust[2],
						activeWatchRotation[0] * 57.29578f,
						activeWatchRotation[1] * 57.29578f,
						activeWatchRotation[2] * 57.29578f);
				SetKeyState(&kW, false); SetKeyState(&kS, false);
				SetKeyState(&kD, false); SetKeyState(&kA, false);
			} else if (handMode) {
				PublishPadLeftStick(0.0f, 0.0f);
				LoadPrologueLeftHandAdjust();
				float *activeHandAdjust = sLeftHandAdjustingPrologue
					? sPrologueLeftHandAdjust : sLeftHandAdjust;
				float *activeHandRotation = sLeftHandAdjustingPrologue
					? sPrologueLeftHandRotationAdjust : sLeftHandRotationAdjust;
				const float kRate = 0.002f;
				const bool depth = (b & vr::ButtonMaskFromId(vr::k_EButton_Grip)) != 0;
				if (fabsf(my) > kStickDead)
					activeHandAdjust[depth ? 2 : 1] += (my > 0 ? kRate : -kRate);
				if (fabsf(mx) > kStickDead)
					activeHandAdjust[0] += (mx > 0 ? kRate : -kRate);
				// Right stick adjusts the saved hand rotation while this mode is
				// active. The rate is intentionally small: a few seconds of input
				// gives a precise wrist correction rather than a full turn.
				// Axis 0 is the physical thumbstick on this controller. Axis 2 is
				// the analog grip, as confirmed by the live calibration log.
				float rx = 0.0f, ry = 0.0f;
				if (haveRight) {
					rx = right.rAxis[0].x;
					ry = right.rAxis[0].y;
				}
				static bool handAxisTouchSupported = false;
				const bool handAxisTouched = haveRight
					&& (right.ulButtonTouched
						& vr::ButtonMaskFromId(vr::k_EButton_SteamVR_Touchpad)) != 0;
				if (handAxisTouched)
					handAxisTouchSupported = true;
				if (handAxisTouchSupported && !handAxisTouched) {
					rx = 0.0f;
					ry = 0.0f;
				}
				const float kRotRate = 0.0100f;
				const bool wristTwistModifier = haveRight
					&& (right.ulButtonPressed
						& vr::ButtonMaskFromId(vr::k_EButton_Grip)) != 0;
				bool rotationChanged = false;
				if (fabsf(rx) > kStickDead) {
					activeHandRotation[wristTwistModifier ? 2 : 1]
						+= (rx > 0 ? kRotRate : -kRotRate);
					rotationChanged = true;
				}
				if (!wristTwistModifier && fabsf(ry) > kStickDead) {
					activeHandRotation[0] += (ry > 0 ? kRotRate : -kRotRate);
					rotationChanged = true;
				}
				activeHandRotation[0] = max(-1.5f, min(1.5f, activeHandRotation[0]));
				activeHandRotation[1] = max(-1.5f, min(1.5f, activeHandRotation[1]));
				activeHandRotation[2] = max(-1.5f, min(1.5f, activeHandRotation[2]));
				static unsigned handRotationLog = 0;
				if (rotationChanged && (handRotationLog++ % 12) == 0)
					LogInfo("VRPose hand: live rightAxis=%d stick=(%.3f %.3f) "
						"rotation=(%.2f %.2f %.2f deg) twistMod=%d\n",
						0, rx, ry,
						activeHandRotation[0] * 57.29578f,
						activeHandRotation[1] * 57.29578f,
						activeHandRotation[2] * 57.29578f,
						wristTwistModifier ? 1 : 0);
				SetKeyState(&kW, false); SetKeyState(&kS, false);
				SetKeyState(&kD, false); SetKeyState(&kA, false);
			} else if (mode == 1) {
				PublishPadLeftStick(0.0f, 0.0f);
				const float kRate = 0.002f;      // ~14 cm per second held
				const bool depth = (b & vr::ButtonMaskFromId(vr::k_EButton_Grip)) != 0;
				if (fabsf(my) > kStickDead)
					sWeaponAdjust[depth ? 2 : 1] += (my > 0 ? kRate : -kRate);
				if (fabsf(mx) > kStickDead)
					sWeaponAdjust[0] += (mx > 0 ? kRate : -kRate);
				// The right stick rotates the rendered weapon while position is
				// adjusted concurrently. Pitch and yaw are persisted with this gun.
				const float rx = haveRight ? right.rAxis[0].x : 0.0f;
				const float ry = haveRight ? right.rAxis[0].y : 0.0f;
				const float kRotRate = 0.0012f;
				if (fabsf(ry) > kStickDead) sWeaponRotAdjust[0] += (ry > 0 ? kRotRate : -kRotRate);
				if (fabsf(rx) > kStickDead) sWeaponRotAdjust[1] += (rx > 0 ? -kRotRate : kRotRate);

				static unsigned n = 0;
				if ((fabsf(my) > kStickDead || fabsf(mx) > kStickDead) && (n++ % 30) == 0)
					LogInfo("VRPose weapon: adjust (%.3f %.3f %.3f)\n",
						sWeaponAdjust[0], sWeaponAdjust[1], sWeaponAdjust[2]);

				SetKeyState(&kW, false);
				SetKeyState(&kS, false);
				SetKeyState(&kD, false);
				SetKeyState(&kA, false);
			} else if (mode == 2) {
				PublishPadLeftStick(0.0f, 0.0f);
				// Rotate the rendered weapon with the RIGHT stick. These are
				// model-placement adjustments, not bullet-aim adjustments.
				const float rx = haveRight ? right.rAxis[0].x : 0.0f;
				const float ry = haveRight ? right.rAxis[0].y : 0.0f;
				const float kRotRate = 0.0012f;
				if (fabsf(ry) > kStickDead)
					sWeaponRotAdjust[0] += (ry > 0 ? kRotRate : -kRotRate);
				if (fabsf(rx) > kStickDead)
					sWeaponRotAdjust[1] += (rx > 0 ? -kRotRate : kRotRate);

				static unsigned n = 0;
				if ((fabsf(ry) > kStickDead || fabsf(rx) > kStickDead) && (n++ % 30) == 0)
					LogInfo("VRPose weapon: model rotation pitch=%.2f yaw=%.2f deg\n",
						sWeaponRotAdjust[0] * 57.29578f, sWeaponRotAdjust[1] * 57.29578f);

				SetKeyState(&kW, false);
				SetKeyState(&kS, false);
				SetKeyState(&kD, false);
				SetKeyState(&kA, false);
			} else {
				float outX = 0.0f, outY = 0.0f;
				const bool gameMenuNavigation =
					!InterlockedCompareExchange(&sGameplayModeActive, 0, 0)
					|| InterlockedCompareExchange(&sPauseMenuExpected, 0, 0);
				if (!gameMenuNavigation) {
					// Feed locomotion through Metro's native gamepad path so partial
					// deflection gives analog walking speed and diagonals remain analog.
					// Use a radial deadzone and rescale the live range back to 0..1;
					// simply cutting each axis independently makes diagonals uneven.
					const float magnitude = sqrtf(mx * mx + my * my);
					if (magnitude > kStickDead) {
						const float clampedMagnitude = min(magnitude, 1.0f);
						const float outputMagnitude =
							(clampedMagnitude - kStickDead) / (1.0f - kStickDead);
						const float scale = outputMagnitude / magnitude;
						outX = mx * scale;
						outY = my * scale;
					}
					// Metro interprets its native left stick in the unchanged source/body
					// camera frame. Direct HMD follow rotates only the upstream camera
					// construction, so rotate locomotion by the same relative head yaw.
					if (!kNativeStateFollowEnabled &&
						InterlockedCompareExchange(&sDirectMovementHeadingInstalled, 0, 0) != 1)
						ApplyUpstreamDirectLocomotion(&outX, &outY);
				}
				PublishPadLeftStick(outX, outY);
				// Movement used to be synthesised as WASD. Release every key on the
				// analog path so switching builds or modes cannot leave one latched.
				SetKeyState(&kW, false);
				SetKeyState(&kS, false);
				SetKeyState(&kD, false);
				SetKeyState(&kA, false);
			}
			// Sprint (stick click) and Equipment Inventory (grip) both go
			// out through the virtual pad now - see PublishPadButtons.

			// Hold the LEFT GRIP for about two seconds to switch between
			// true stereo and alternate-eye rendering.
			//
			// A real choice, not a compatibility workaround: alternate-eye is
			// currently faster in geometry-heavy areas on ANY hardware,
			// because true stereo transforms every vertex a second time.
			// Until single-pass stereo lands, "better depth" and "better
			// framerate" genuinely point in opposite directions, so the
			// player picks. This belongs in a VR menu eventually; a held
			// button does until there is one.
			//
			// Moved to MODIFIER + left stick click, now that the mapping pass
			// this comment used to anticipate has actually happened. The
			// grip is Equipment Inventory, and a two-second hold on it would
			// have opened that every single time on the way through.
			{
				static int sModeHeld = 0;
				if (modHeld && (b & vr::ButtonMaskFromId(vr::k_EButton_SteamVR_Touchpad))) {
					if (++sModeHeld == 150)   // ~2s at 72fps
						StereoTwin::RequestStereoModeToggle();
				} else {
					sModeHeld = 0;
				}
			}
			// Left X / Y now carry Use-Reload-Interact and Weapons Inventory
			// through the virtual pad, not Light and Crouch as keystrokes.


			// Aim down sights is now the gesture ALONE - raise the weapon to
			// the face.
			//
			// The engine's own ADS already suppresses recoil and sway (see
			// sAimingNow's comment), so the gesture gets that suppression
			// for free, with no need to find or fight the recoil code.
			//
			// The left trigger no longer aims: it is the modifier for the
			// d-pad now, and leaving it wired here would have meant every
			// filter swap and medkit also raised the sights.
			// NO FORCED ADS.
			//
			// Raising the gun no longer presses the game's aim button. The
			// engine's ADS did suppress recoil and sway, but it also drags
			// the weapon to the face through the skinned bone palette,
			// fighting the controller the gun is pinned to - and that
			// movement cannot be cancelled without decoding the palette
			// (the hands were confirmed to be a single mesh, 12132, so
			// there is no cheap way at it). The gesture now drives OUR own
			// recoil control instead; see the recoil measurement in the aim
			// loop.
			//
			// Kept as a flag rather than deleted: flipping this back on is
			// the fastest way to compare the engine's suppression against
			// ours, and the engine's version probably also tightens bullet
			// SPREAD, which camera-side cancellation cannot reproduce.
			// The gun-to-face gesture is the VR ADS control. Forward it to Metro's
			// native ADS action so recoil/sway/spread use the game's own tuned
			// suppression rather than a replacement recoil model.
			// Keep native visual ADS off while its recoil/spread state is being
			// separated from the weapon animation. The gesture remains published
			// through IsAiming for that gameplay-only hook.
			// Native visual ADS remains disabled: its animation pulls the weapon,
			// watch and attachments away from the tracked controller.
			// Native visual ADS is useful for magnifying scopes, but probing an
			// unknown weapon by pressing Metro's aim control is not safe. On the
			// shotgun that control is secondary fire; on ordinary guns it also left
			// the two AER eyes at different animation states and disturbed culling.
			// Only a positively matched rendered configuration may receive native
			// visual ADS. Unknown, iron and reflex sights do nothing here while the
			// gameplay-only recoil suppression below remains available to all guns.
			static bool sEngineADSDown = false;
			bool wantAim = IsWeaponNearFace();

			const std::unordered_set<unsigned> &scopeMeshes = !sActiveWeaponMeshes.empty()
				? sActiveWeaponMeshes : sRuntimeWeaponMeshes;
			// Same-gun before/after capture isolated the six draws added by Metro's
			// 2x optic. Require the complete attachment signature: this is independent
			// of which compatible gun hosts it, and neither total mesh count nor one
			// collision-prone IndexCount can make another attachment false-trigger.
			static const unsigned k2xOpticVariantA[] = { 36, 48, 384, 936, 960, 5808 };
			static const unsigned k2xOpticVariantB[] = { 24, 48, 120, 420, 480, 8928 };
			// The first 4x optic was captured on weapon profile 8 while equipped.
			// Keep this as a complete configuration signature until a matching
			// unscoped capture is available to subtract the host-weapon meshes.
			static const unsigned k4xOpticVariantA[] = {
				12, 27, 42, 60, 144, 273, 288, 360,
				2517, 4647, 10662, 11751, 15222, 21762
			};
			unsigned optic2xMatchesA = 0, optic2xMatchesB = 0;
			unsigned optic4xMatchesA = 0;
			for (unsigned id : k2xOpticVariantA)
				if (scopeMeshes.count(id)) ++optic2xMatchesA;
			for (unsigned id : k2xOpticVariantB)
				if (scopeMeshes.count(id)) ++optic2xMatchesB;
			for (unsigned id : k4xOpticVariantA)
				if (scopeMeshes.count(id)) ++optic4xMatchesA;
			const bool known2x =
				optic2xMatchesA == _countof(k2xOpticVariantA)
				|| optic2xMatchesB == _countof(k2xOpticVariantB);
			const bool known4x =
				optic4xMatchesA == _countof(k4xOpticVariantA);

			// Retain a bounded passive census for validating this signature on other
			// host guns and isolating the later 4x attachment. It sends no input.
			static bool sWasScopeNearFace = false;
			static unsigned sScopeMeshCaptureNumber = 0;
			if (wantAim && !sWasScopeNearFace && sScopeMeshCaptureNumber < 32) {
				std::vector<unsigned> sortedScopeMeshes(scopeMeshes.begin(), scopeMeshes.end());
				std::sort(sortedScopeMeshes.begin(), sortedScopeMeshes.end());
				std::ostringstream meshLine;
				for (unsigned id : sortedScopeMeshes) meshLine << ' ' << id;
				LogInfo("VRPose scope-mesh-set: capture=%u count=%u profile=%d weaponKey=%d "
					"2xA=%u/%u 2xB=%u/%u ids=%s\n",
					++sScopeMeshCaptureNumber, (unsigned)sortedScopeMeshes.size(),
					sSelectedWeaponProfile, sCurrentWeaponKey,
					optic2xMatchesA, (unsigned)_countof(k2xOpticVariantA),
					optic2xMatchesB, (unsigned)_countof(k2xOpticVariantB),
					meshLine.str().c_str());
			}
			sWasScopeNearFace = wantAim;
			// Scope zoom and Metro's visual ADS animation are separate controls.
			// The projection magnification always follows a positively identified
			// 2x/4x optic near the eye. The menu setting controls only whether that
			// same gesture also presses Metro's aim button and pulls the weapon into
			// its native ADS pose.
			const bool wantScopeZoom = IsScopeNearEye() && (known2x || known4x);
			InterlockedExchange(&sRequestedScopeScaleMilli,
				wantScopeZoom ? (known4x ? 3488 : 1744) : 1000);
			const bool wantVisualScopeADS = wantScopeZoom
				&& VRMenu::GetSettings().scopeForcedADS;

			if (wantVisualScopeADS != sEngineADSDown) {
				BeginNativeADSStateDiff(wantVisualScopeADS);
				SendMouseButton(true, wantVisualScopeADS);
				sEngineADSDown = wantVisualScopeADS;
				LogInfo("VRPose scope: visual ADS %s meshes=%u\n",
					wantVisualScopeADS ? "ON" : "OFF",
					(unsigned)scopeMeshes.size());
			}
			// Still published: this is now "the player wants recoil control",
			// which is what the gesture means from here on.
			InterlockedExchange(&sAimingNow, wantAim ? 1 : 0);
			UpdateGameplayADSFlag(wantAim);
			TickNativeADSStateDiff();
		}

		// CLICK THE RIGHT STICK to cycle which viewmodel mesh is hidden.
		//
		// There is no way to tell from here which draws are the arms - they
		// are just more meshes in the weapon cluster - so this lets the
		// player point at them: click, see what vanishes, repeat.
		//
		// The right stick click is the one control on either controller that
		// is not already bound, which matters: the previous two-grip
		// combination fired reload and use instead, and a timed auto-cycle
		// was worse still because it only starts once the weapon is drawn,
		// so the player's stopwatch and the step counter had different zero
		// points and the count came out wrong.
		// DISABLED: the right stick click is Crouch Toggle now. Weapon scale
		// was calibrated and locked in earlier, so this stepper has done its
		// job; kept rather than deleted in case it needs revisiting, which
		// would mean giving it a modifier combination of its own.
		static const bool kWeaponScaleStepperEnabled = false;
		if (kWeaponScaleStepperEnabled && haveRight) {
			static bool wasDown = false;
			const bool down =
				(right.ulButtonPressed & vr::ButtonMaskFromId(vr::k_EButton_SteamVR_Touchpad)) != 0;
			if (down && !wasDown) {
				// No longer steps the hide selection either: that would hide
				// a piece of the weapon on every click, which is the last
				// thing wanted while judging its size.
				//
				// Now steps the viewmodel SCALE instead of dumping the eye
				// textures.
				//
				// The eye dump answered its question long ago; weapon scale is
				// the live one, and it is the rare quantity here that the
				// player's own eye judges better than any measurement taken
				// from outside the headset - "is this gun the size of a real
				// one in my hand" needs a hand and a gun.
				InterlockedIncrement(&sWeaponScaleStep);
				LogInfo("VRPose weapon: scale step %ld\n", sWeaponScaleStep);
			}
			wasDown = down;
		}

		if (haveRight) {
			uint64_t b = right.ulButtonPressed;
			// Jump (A) and Secondary (grip) go out through the virtual pad.

			// Recentre moved to MODIFIER + right stick click.
			//
			// It used to be the right menu button held, but that button is
			// Melee Attack now. Behind the modifier it cannot be reached by
			// accident at all, which suits it better than the old hold did -
			// and unlike every other modifier combination this one is ours,
			// never forwarded to the game.
			static int sRecenterHeldFrames = 0;
			if (modHeld && (b & vr::ButtonMaskFromId(vr::k_EButton_SteamVR_Touchpad))) {
				if (++sRecenterHeldFrames == 25)     // ~0.4s
					RecenterAiming();
			} else {
				sRecenterHeldFrames = 0;
			}

			// Right trigger fires.
			static bool sFiring = false;
			bool wantFire = right.rAxis[1].x > 0.5f;
			if (wantFire != sFiring) {
				SendMouseButton(false, wantFire);
				sFiring = wantFire;
			}
			InterlockedExchange(&sFiringNow, wantFire ? 1 : 0);

			// Right stick X turns the body. The weapon wheel receives its directions
			// as virtual Xbox D-pad buttons above, so suppress body turning for the
			// entire hold and keep controller glyph ownership intact.
			//
			// Suppressed under the modifier, where the same stick is the
			// d-pad: without this, reaching for Swap Filter would spin the
			// player a quarter turn on the way.
			const bool weaponMenuHeld = IsWeaponInventoryHeld();
			const bool calibrationOwnsRightStick =
				InterlockedCompareExchange(&sChargerAdjustMode, 0, 0) != 0
				|| InterlockedCompareExchange(
					&sChargerRightHandAdjustMode, 0, 0) != 0
				|| InterlockedCompareExchange(&sLighterAdjustMode, 0, 0) != 0
				|| InterlockedCompareExchange(&sLighterFlameAdjustMode, 0, 0) != 0
				|| InterlockedCompareExchange(&sLeftHandAdjustMode, 0, 0) != 0
				|| InterlockedCompareExchange(&sWatchAdjustMode, 0, 0) != 0
				|| InterlockedCompareExchange(&sWatchTimeAdjustMode, 0, 0) != 0
				|| InterlockedCompareExchange(&sWeaponAdjustMode, 0, 0) != 0;
			float tx = (modHeld || weaponMenuHeld || calibrationOwnsRightStick) ? 0.0f
				: (controlMenu.swapAnalogSticks
					? (haveLeft ? left.rAxis[0].x : 0.0f)
					: right.rAxis[0].x);
			static bool sSnapTurnLatched = false;
			const VRMenu::Settings &menu = VRMenu::GetSettings();
			if (fabsf(tx) <= kStickDead)
				sSnapTurnLatched = false;
			int dx = 0;
			if (fabsf(tx) > kStickDead
				&& (menu.turning == VRMenu::TurningMode::Smooth || !sSnapTurnLatched)) {
				if (menu.turning == VRMenu::TurningMode::Smooth) {
					const int kTurnUnitsAtFull = 30;   // ~2.7 deg/frame
					dx = (int)(tx * kTurnUnitsAtFull);
				} else {
					const float radians = menu.snapAngle * 3.14159265358979323846f / 180.0f;
					dx = (int)roundf(radians / 0.00159f);
					if (tx < 0.0f) dx = -dx;
					sSnapTurnLatched = true;
				}
			}
			if (NativeStateFollowSelected()) {
				nativeBodyTurnInput.Publish(dx, fabsf(tx) > kStickDead,
					menu.turning != VRMenu::TurningMode::Smooth);
			}
		}
	}

	// Is the weapon hand physically held up near the face, as if sighting
	// down the barrel?
	//
	// The old test used total hand-to-head distance. That made recoil
	// suppression depend on how far the player held the gun out, even when
	// the gun was clearly raised to eye height. Use the hand's vertical
	// position relative to the HMD instead: horizontal reach is irrelevant
	// to whether the weapon is raised for general recoil control, and controller pitch must not be used
	// because the player may deliberately aim at the floor or ceiling.
	//
	// Hysteresis, not a single threshold: a hand hovering near the cutoff
	// would otherwise chatter the game's aim button on and off several
	// times a second, which reads as the sights snapping in and out.
	// The controller is at the weapon grip, below the optic. Centering this test
	// on the HMD itself made the unforced scope sit too high before zoom engaged.
	// Target the grip eight centimetres below eye height, then interpret the menu
	// value as tolerance around that aligned position. A two-centimetre release
	// margin still prevents tracking chatter without making the 0.01 m minimum
	// behave like a much broader setting after activation.
	static const float kAimGripBelowEyeMeters = 0.08f;
	static const float kAimReleaseHysteresisMeters = 0.02f;
	// Scope zoom needs a second condition that general recoil control does not:
	// the optic must actually be close to the eye. The tracked point is the
	// controller grip, not the optic, so its allowed radius includes the normal
	// grip-to-scope offset. Adding the menu tolerance makes 0.01 m a tight
	// 0.26 m grip radius while retaining the useful 0.30 m default behavior.
	static const float kScopeGripToEyeBaseMeters = 0.25f;
	static bool sWeaponNearFace = false;
	static bool sScopeNearEye = false;

	// A grip press within roughly one forearm's width of the HMD is treated as
	// a head-mounted flashlight gesture. Use full 3-D distance so a hand merely
	// held at head height across the body does not qualify.
	static bool GetLeftHandHeadDistanceSquared(float *distanceSquared)
	{
		if (!sVRSystem || !distanceSquared)
			return false;
		const vr::TrackedDeviceIndex_t leftIdx =
			sVRSystem->GetTrackedDeviceIndexForControllerRole(
				vr::TrackedControllerRole_LeftHand);
		if (leftIdx == vr::k_unTrackedDeviceIndexInvalid)
			return false;

		vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount];
		GetTrackedPosesForFrame(poses);
		const vr::TrackedDevicePose_t &head =
			poses[vr::k_unTrackedDeviceIndex_Hmd];
		const vr::TrackedDevicePose_t &left = poses[leftIdx];
		if (!head.bDeviceIsConnected || !head.bPoseIsValid
			|| !left.bDeviceIsConnected || !left.bPoseIsValid)
			return false;

		const float dx = left.mDeviceToAbsoluteTracking.m[0][3]
			- head.mDeviceToAbsoluteTracking.m[0][3];
		const float dy = left.mDeviceToAbsoluteTracking.m[1][3]
			- head.mDeviceToAbsoluteTracking.m[1][3];
		const float dz = left.mDeviceToAbsoluteTracking.m[2][3]
			- head.mDeviceToAbsoluteTracking.m[2][3];
		*distanceSquared = dx * dx + dy * dy + dz * dz;
		return true;
	}

	static bool IsLeftHandNearHead()
	{
		float distanceSquared = 0.0f;
		if (!GetLeftHandHeadDistanceSquared(&distanceSquared))
			return false;
		static const float kFlashlightHeadRadiusMeters = 0.32f;
		return distanceSquared
			<= kFlashlightHeadRadiusMeters * kFlashlightHeadRadiusMeters;
	}

	// Keep the flashlight gesture away from the mask interaction directly in
	// front of the face.  Measure in the headset's local frame so "left side"
	// follows head yaw, then require the controller to sit beside the left
	// temple rather than anywhere inside the broader mask/head sphere.
	static bool IsLeftHandBesideHead()
	{
		if (!sVRSystem)
			return false;
		const vr::TrackedDeviceIndex_t leftIdx =
			sVRSystem->GetTrackedDeviceIndexForControllerRole(
				vr::TrackedControllerRole_LeftHand);
		if (leftIdx == vr::k_unTrackedDeviceIndexInvalid)
			return false;
		vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount];
		GetTrackedPosesForFrame(poses);
		const vr::TrackedDevicePose_t &head = poses[vr::k_unTrackedDeviceIndex_Hmd];
		const vr::TrackedDevicePose_t &left = poses[leftIdx];
		if (!head.bDeviceIsConnected || !head.bPoseIsValid
			|| !left.bDeviceIsConnected || !left.bPoseIsValid)
			return false;
		const float worldDelta[3] = {
			left.mDeviceToAbsoluteTracking.m[0][3]
				- head.mDeviceToAbsoluteTracking.m[0][3],
			left.mDeviceToAbsoluteTracking.m[1][3]
				- head.mDeviceToAbsoluteTracking.m[1][3],
			left.mDeviceToAbsoluteTracking.m[2][3]
				- head.mDeviceToAbsoluteTracking.m[2][3] };
		float local[3] = {};
		for (int axis = 0; axis < 3; ++axis) {
			local[axis] = head.mDeviceToAbsoluteTracking.m[0][axis] * worldDelta[0]
				+ head.mDeviceToAbsoluteTracking.m[1][axis] * worldDelta[1]
				+ head.mDeviceToAbsoluteTracking.m[2][axis] * worldDelta[2];
		}
		static const float kMinimumLeftOffsetMeters = 0.12f;
		static const float kMaximumLeftOffsetMeters = 0.34f;
		static const float kMaximumVerticalOffsetMeters = 0.24f;
		static const float kMaximumForeAftOffsetMeters = 0.22f;
		return local[0] <= -kMinimumLeftOffsetMeters
			&& local[0] >= -kMaximumLeftOffsetMeters
			&& fabsf(local[1]) <= kMaximumVerticalOffsetMeters
			&& fabsf(local[2]) <= kMaximumForeAftOffsetMeters;
	}

	static bool IsLeftHandPulledAwayFromHead()
	{
		float distanceSquared = 0.0f;
		if (!GetLeftHandHeadDistanceSquared(&distanceSquared))
			return false;
		// Starting requires <= 32 cm. Requiring 52 cm to finish gives the
		// removal gesture a deliberate 20 cm pull and useful hysteresis.
		static const float kMaskRemovalDistanceMeters = 0.52f;
		return distanceSquared >= kMaskRemovalDistanceMeters
			* kMaskRemovalDistanceMeters;
	}

	static bool GetLeftHandHeadLocalMeters(float out[3])
	{
		if (!out || !sVRSystem)
			return false;
		const vr::TrackedDeviceIndex_t leftIdx =
			sVRSystem->GetTrackedDeviceIndexForControllerRole(
				vr::TrackedControllerRole_LeftHand);
		if (leftIdx == vr::k_unTrackedDeviceIndexInvalid)
			return false;
		vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount];
		GetTrackedPosesForFrame(poses);
		const vr::TrackedDevicePose_t &head = poses[vr::k_unTrackedDeviceIndex_Hmd];
		const vr::TrackedDevicePose_t &left = poses[leftIdx];
		if (!head.bDeviceIsConnected || !head.bPoseIsValid
			|| !left.bDeviceIsConnected || !left.bPoseIsValid)
			return false;
		const float worldDelta[3] = {
			left.mDeviceToAbsoluteTracking.m[0][3]
				- head.mDeviceToAbsoluteTracking.m[0][3],
			left.mDeviceToAbsoluteTracking.m[1][3]
				- head.mDeviceToAbsoluteTracking.m[1][3],
			left.mDeviceToAbsoluteTracking.m[2][3]
				- head.mDeviceToAbsoluteTracking.m[2][3] };
		for (int axis = 0; axis < 3; ++axis) {
			out[axis] = head.mDeviceToAbsoluteTracking.m[0][axis] * worldDelta[0]
				+ head.mDeviceToAbsoluteTracking.m[1][axis] * worldDelta[1]
				+ head.mDeviceToAbsoluteTracking.m[2][axis] * worldDelta[2];
		}
		return true;
	}

	// Central front-waist volume for the persistent charger. Requiring a point
	// in front of the torso keeps it separate from the left-side gas-mask hip
	// zone and from the higher filter grab.
	// HMD-position-relative body frame. Horizontal axes follow headset yaw so
	// physically turning still turns the equipment layout, while raw tracking
	// height and a flattened horizontal basis make looking up/down or tilting the
	// head unable to move chest/waist acquisition zones.
	static bool GetLeftHandBodyLocalMeters(float out[3])
	{
		if (!out || !sVRSystem)
			return false;
		const vr::TrackedDeviceIndex_t leftIdx =
			sVRSystem->GetTrackedDeviceIndexForControllerRole(
				vr::TrackedControllerRole_LeftHand);
		if (leftIdx == vr::k_unTrackedDeviceIndexInvalid)
			return false;
		vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount];
		GetTrackedPosesForFrame(poses);
		const vr::TrackedDevicePose_t &head = poses[vr::k_unTrackedDeviceIndex_Hmd];
		const vr::TrackedDevicePose_t &left = poses[leftIdx];
		if (!head.bDeviceIsConnected || !head.bPoseIsValid
			|| !left.bDeviceIsConnected || !left.bPoseIsValid)
			return false;
		const float dx = left.mDeviceToAbsoluteTracking.m[0][3]
			- head.mDeviceToAbsoluteTracking.m[0][3];
		const float dy = left.mDeviceToAbsoluteTracking.m[1][3]
			- head.mDeviceToAbsoluteTracking.m[1][3];
		const float dz = left.mDeviceToAbsoluteTracking.m[2][3]
			- head.mDeviceToAbsoluteTracking.m[2][3];
		float rightX = head.mDeviceToAbsoluteTracking.m[0][0];
		float rightZ = head.mDeviceToAbsoluteTracking.m[2][0];
		const float rightLength = sqrtf(rightX * rightX + rightZ * rightZ);
		if (rightLength < 1e-5f)
			return false;
		rightX /= rightLength;
		rightZ /= rightLength;
		out[0] = dx * rightX + dz * rightZ;
		out[1] = dy;
		out[2] = dx * rightZ - dz * rightX;
		return true;
	}

	static bool IsLeftHandNearFrontWaist()
	{
		float local[3] = {};
		if (!GetLeftHandBodyLocalMeters(local))
			return false;
		// Keep a deliberate horizontal gap before the mask zone, whose inner
		// left edge begins at -0.18 m.
		return local[0] >= -0.12f && local[0] <= 0.22f
			&& local[1] >= -1.00f && local[1] <= -0.32f
			&& local[2] >= 0.00f && local[2] <= 0.55f;
	}

	// Central chest grab volume. It is HMD-relative so seated and standing play
	// use the same reach, and it stops above the front-waist charger zone and
	// the left-hip mask zone.
	static bool IsLeftHandNearChest()
	{
		float local[3] = {};
		if (!GetLeftHandBodyLocalMeters(local))
			return false;
		return local[0] >= -0.38f && local[0] <= 0.22f
			&& local[1] >= -0.34f && local[1] <= -0.10f
			&& local[2] >= -0.58f && local[2] <= 0.12f;
	}

	// Mouth/filter volume: centered in front of the lower face and deliberately
	// below the temple flashlight zone.
	static bool IsLeftHandNearMouth()
	{
		float local[3] = {};
		if (!GetLeftHandHeadLocalMeters(local))
			return false;
		return fabsf(local[0]) <= 0.22f
			&& local[1] >= -0.17f && local[1] <= 0.03f
			&& local[2] >= -0.42f && local[2] <= -0.08f;
	}

	// Left-hip grab volume relative to the HMD. Keep it vertically broad enough
	// for standing and seated play, but require the hand to be distinctly left
	// of the torso so the front waist remains available for other equipment.
	// Horizontal axes follow headset yaw rather than the tracking-space axes.
	static bool IsLeftHandNearWaist()
	{
		if (!sVRSystem)
			return false;
		const vr::TrackedDeviceIndex_t leftIdx =
			sVRSystem->GetTrackedDeviceIndexForControllerRole(
				vr::TrackedControllerRole_LeftHand);
		if (leftIdx == vr::k_unTrackedDeviceIndexInvalid)
			return false;

		vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount];
		GetTrackedPosesForFrame(poses);
		const vr::TrackedDevicePose_t &head =
			poses[vr::k_unTrackedDeviceIndex_Hmd];
		const vr::TrackedDevicePose_t &left = poses[leftIdx];
		if (!head.bDeviceIsConnected || !head.bPoseIsValid
			|| !left.bDeviceIsConnected || !left.bPoseIsValid)
			return false;

		const float dx = left.mDeviceToAbsoluteTracking.m[0][3]
			- head.mDeviceToAbsoluteTracking.m[0][3];
		const float dy = left.mDeviceToAbsoluteTracking.m[1][3]
			- head.mDeviceToAbsoluteTracking.m[1][3];
		const float dz = left.mDeviceToAbsoluteTracking.m[2][3]
			- head.mDeviceToAbsoluteTracking.m[2][3];
		float rightX = head.mDeviceToAbsoluteTracking.m[0][0];
		float rightZ = head.mDeviceToAbsoluteTracking.m[2][0];
		const float rightLength = sqrtf(rightX * rightX + rightZ * rightZ);
		if (rightLength < 1e-5f)
			return false;
		rightX /= rightLength;
		rightZ /= rightLength;
		const float localRight = dx * rightX + dz * rightZ;
		const float localForward = dx * rightZ - dz * rightX;
		static const float kWaistTopMeters = -0.45f;
		static const float kWaistBottomMeters = -1.00f;
		static const float kHipInnerLeftMeters = -0.18f;
		static const float kHipOuterLeftMeters = -0.60f;
		static const float kHipFrontBackHalfDepthMeters = 0.32f;
		return dy <= kWaistTopMeters && dy >= kWaistBottomMeters
			&& localRight <= kHipInnerLeftMeters
			&& localRight >= kHipOuterLeftMeters
			&& fabsf(localForward) <= kHipFrontBackHalfDepthMeters;
	}

	static bool IsWeaponNearFace()
	{
		if (VRMenu::GetSettings().gamepadMode) {
			sWeaponNearFace = false;
			sScopeNearEye = false;
			return false;
		}
		if (!sVRSystem || !sLastPolledPoseValid) {
			sWeaponNearFace = false;
			sScopeNearEye = false;
			return false;
		}

		vr::TrackedDeviceIndex_t idx =
			sVRSystem->GetTrackedDeviceIndexForControllerRole(WeaponControllerRole());
		if (idx == vr::k_unTrackedDeviceIndexInvalid) {
			sWeaponNearFace = false;
			sScopeNearEye = false;
			return false;
		}

		vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount];
		GetTrackedPosesForFrame(poses);
		if (!poses[idx].bDeviceIsConnected || !poses[idx].bPoseIsValid) {
			// Tracking dropped - hold the last verdicts rather than yanking
			// the sights away mid-shot on a momentary occlusion.
			return sWeaponNearFace;
		}

		const vr::HmdMatrix34_t &c = poses[idx].mDeviceToAbsoluteTracking;
		const vr::HmdMatrix34_t &h = sLastPolledPose;
		const float dx = c.m[0][3] - h.m[0][3];
		const float dy = c.m[1][3] - h.m[1][3];
		const float dz = c.m[2][3] - h.m[2][3];
		const float height = fabsf(dy + kAimGripBelowEyeMeters);
		const float engageHeight = VRMenu::GetSettings().scopeSensitivityMeters;
		const float releaseHeight = engageHeight + kAimReleaseHysteresisMeters;
		const float scopeDistance = sqrtf(dx * dx + height * height + dz * dz);
		const float engageScopeDistance = kScopeGripToEyeBaseMeters + engageHeight;
		const float releaseScopeDistance = engageScopeDistance + kAimReleaseHysteresisMeters;

		if (sWeaponNearFace)
			sWeaponNearFace = (height < releaseHeight);
		else
			sWeaponNearFace = (height < engageHeight);

		if (sScopeNearEye)
			sScopeNearEye = (height < releaseHeight
				&& scopeDistance < releaseScopeDistance);
		else
			sScopeNearEye = (height < engageHeight
				&& scopeDistance < engageScopeDistance);

		return sWeaponNearFace;
	}

	static bool IsScopeNearEye()
	{
		return sScopeNearEye;
	}

	// Whether the left controller is close enough to be touching the weapon
	// held by the right controller.  We do not have a collision mesh on the
	// CPU, so use an oriented rectangular volume around the rendered gun's
	// approximate barrel: narrow across the gun, but extended toward its
	// fore-end.  A sphere here made a hand beside the gun qualify from too far
	// away.
	//
	// This volume is used only to acquire the support grip.  Its rear edge is
	// deliberately forward of the right-controller grip so the stock/rear of
	// the weapon cannot consume left grip when reaching for body gestures.
	// Once acquired, sTwoHandActive latches the support grip until left grip is
	// released (or tracking/weapon state forces the existing reset path).
	static const float kTwoHandEngageLongMinMeters = 0.08f;
	static const float kTwoHandEngageLongMaxMeters = 0.42f;
	static const float kTwoHandEngageSideHalfMeters = 0.10f;
	static const float kTwoHandEngageUpHalfMeters = 0.10f;

	static bool IsLeftHandNearWeapon()
	{
		if (VRMenu::GetSettings().gamepadMode)
			return false;
		if (!sVRSystem || !sLastPolledPoseValid)
			return false;

		const vr::TrackedDeviceIndex_t rightIdx =
			sVRSystem->GetTrackedDeviceIndexForControllerRole(
				vr::TrackedControllerRole_RightHand);
		const vr::TrackedDeviceIndex_t leftIdx =
			sVRSystem->GetTrackedDeviceIndexForControllerRole(
				vr::TrackedControllerRole_LeftHand);
		if (rightIdx == vr::k_unTrackedDeviceIndexInvalid
			|| leftIdx == vr::k_unTrackedDeviceIndexInvalid)
			return false;

		vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount];
		GetTrackedPosesForFrame(poses);
		if (!poses[rightIdx].bDeviceIsConnected || !poses[rightIdx].bPoseIsValid
			|| !poses[leftIdx].bDeviceIsConnected || !poses[leftIdx].bPoseIsValid)
			return false;

		const vr::HmdMatrix34_t &right = poses[rightIdx].mDeviceToAbsoluteTracking;
		const vr::HmdMatrix34_t &left = poses[leftIdx].mDeviceToAbsoluteTracking;
		const float worldDelta[3] = {
			left.m[0][3] - right.m[0][3],
			left.m[1][3] - right.m[1][3],
			left.m[2][3] - right.m[2][3] };
		// This is the same calibrated barrel direction used by the weapon aim
		// path, expressed in right-controller-local game coordinates.  Using it
		// makes the box follow the gun rather than the controller's grip axis.
		static const float kControllerAimTilt = 64.6f * 0.0174532925f;
		static const float kBarrelPitchBias = -0.06720f;
		static const float kBarrelYawBias = -0.46304f;
		const float tilt = kControllerAimTilt + kBarrelPitchBias;
		const float yaw = kBarrelYawBias;
		const float tc = cosf(tilt), ts = sinf(tilt);
		const float yc = cosf(yaw), ys = sinf(yaw);
		const float barrel[3] = { -ys, -ts * yc, tc * yc };
		const float local[3] = {
			right.m[0][0] * worldDelta[0] + right.m[1][0] * worldDelta[1]
				+ right.m[2][0] * worldDelta[2],
			right.m[0][1] * worldDelta[0] + right.m[1][1] * worldDelta[1]
				+ right.m[2][1] * worldDelta[2],
			-(right.m[0][2] * worldDelta[0] + right.m[1][2] * worldDelta[1]
				+ right.m[2][2] * worldDelta[2]) };
		// Build two perpendicular axes around the barrel from controller-local
		// up.  The resulting coordinates are a true rectangular prism test.
		const float upDot = barrel[1];
		float up[3] = { -upDot * barrel[0], 1.0f - upDot * barrel[1],
			-upDot * barrel[2] };
		const float upLength = sqrtf(up[0] * up[0] + up[1] * up[1] + up[2] * up[2]);
		if (upLength < 0.001f)
			return false;
		for (int axis = 0; axis < 3; ++axis)
			up[axis] /= upLength;
		const float side[3] = {
			up[1] * barrel[2] - up[2] * barrel[1],
			up[2] * barrel[0] - up[0] * barrel[2],
			up[0] * barrel[1] - up[1] * barrel[0] };
		const float longitudinal = local[0] * barrel[0] + local[1] * barrel[1]
			+ local[2] * barrel[2];
		const float lateral = local[0] * side[0] + local[1] * side[1]
			+ local[2] * side[2];
		const float vertical = local[0] * up[0] + local[1] * up[1]
			+ local[2] * up[2];
		const bool nearWeapon = longitudinal >= kTwoHandEngageLongMinMeters
			&& longitudinal <= kTwoHandEngageLongMaxMeters
			&& fabsf(lateral) <= kTwoHandEngageSideHalfMeters
			&& fabsf(vertical) <= kTwoHandEngageUpHalfMeters;

		static const bool kTwoHandDistanceTrace = false;
		static unsigned traceFrame = 0;
		if (kTwoHandDistanceTrace && (traceFrame++ % 30) == 0)
			LogInfo("VRPose weapon: left support box local=(%.3f %.3f %.3f) near=%d\n",
				longitudinal, lateral, vertical, nearWeapon ? 1 : 0);

		return nearWeapon;
	}

	// Right controller's aim direction, expressed as yaw/pitch in the same
	// frame the head reference was captured in. Returns false when no
	// right-hand controller is tracking.
	bool GetControllerAim(float *outYaw, float *outPitch)
	{
		if (VRMenu::GetSettings().gamepadMode)
			return false;
		if (!sVRSystem || !G->vrReferenceRotationCaptured)
			return false;

		vr::TrackedDeviceIndex_t idx =
			sVRSystem->GetTrackedDeviceIndexForControllerRole(WeaponControllerRole());
		if (idx == vr::k_unTrackedDeviceIndexInvalid)
			return false;

		vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount];
		GetTrackedPosesForFrame(poses);
		if (!poses[idx].bDeviceIsConnected || !poses[idx].bPoseIsValid)
			return false;

		const vr::HmdMatrix34_t &m = poses[idx].mDeviceToAbsoluteTracking;

		// The controller's -Z is the GRIP axis, not the barrel.
		//
		// Measured: held naturally to aim, the controller's -Z points 64.6
		// degrees UPWARD. That is the handle running up through the fist, and
		// treating it as the aiming direction has two consequences, both of
		// which showed in the shot log:
		//
		//   - yaw compresses by cos(pitch). At 64 degrees that is 0.44, and
		//     the measured yaw response was 0.49, 0.36, 0.35 across the range.
		//   - the missing rotation reappears as PITCH. 75 degrees of wrist yaw
		//     produced 17.6 degrees of aim pitch while the wrist barely
		//     pitched at all. That is the diagonal.
		//
		// The anchor hides the constant part, which is why this looked like a
		// zeroing problem rather than a wrong axis.
		//
		// So tilt the grip axis down onto the barrel: rotate -Z toward -Y, the
		// controller's own "down", by the measured angle. Then the aim axis is
		// near horizontal in normal use, yaw maps roughly one to one, and the
		// coupling goes away.
		//
		// This is a property of the controller, not of the game - if a
		// different controller aims differently, this is the number to change.
		static const float kControllerAimTilt = 64.6f * 0.0174532925f;

		// THE ZEROING LIVES HERE, IN THE CONTROLLER'S OWN FRAME.
		//
		// It used to be added to the Euler yaw/pitch after this vector had
		// been converted to angles. That is not the same thing: the real
		// error is a fixed misalignment between the barrel axis we assume and
		// the actual one, which is a rotation of THIS vector, fixed in the
		// controller's frame. An offset added in angle space instead matches
		// at one elevation and drifts either side of it - reported live as
		// the shot being off "in opposite ways" pointing at the ceiling
		// versus the floor, which is the signature of exactly that.
		//
		// Applied here, the correction rotates with the hand, so it holds at
		// any elevation and any roll. kBarrelPitchBias is really a correction
		// to the 64.6 degree tilt itself - the two add - which is the honest
		// reading of what that constant is: a measurement of how this player
		// holds the controller.
		// CLEARED 2026-08-09, deliberately.
		//
		// The previous values (-7.77 pitch, +27.09 yaw) were measured against
		// geometry that was wrong in two separate ways at the time: the shot
		// left the EYE rather than the muzzle, and the bias was applied in
		// Euler space rather than as a rotation of the barrel axis. They were
		// cancelling those defects, not measuring the hand - which is why 27
		// degrees was needed at all, and why re-measuring kept landing
		// somewhere new.
		//
		// The ray is now the barrel line end to end (confirmed: 40 of 40
		// shots came back KEPT, with the origin 0.6 m off the camera), so
		// whatever is left IS the genuine misalignment between the assumed
		// barrel axis and the real one - a physical property of how this
		// player holds the controller. Measure it fresh from zero; a small
		// number now would be the sign it is finally the real thing.
		// Measured 2026-08-09 against the corrected geometry: the ray now
		// leaves the muzzle (confirmed KEPT on 40 of 40 shots) and the bias
		// rotates the barrel axis in the controller's own frame.
		//
		// The magnitude is worth recording: ~29 degrees of yaw, close to the
		// 27 measured under the old scheme and opposite in sign only because
		// the rotation axis and the stick convention both changed since. Two
		// independent measurements agreeing on ~28 degrees says the 64.6
		// degree grip tilt above is genuinely wrong for this player by about
		// that much - the bias is cancelling a bad constant, not correcting a
		// small physical quirk. Fixing kControllerAimTilt itself would be the
		// cause-level change; this is the working one.
		// Re-measured 2026-08-11 for the SMG, after the single-pass, UI and
		// deferred-space work (Notes/21) shifted where the gun sits relative
		// to the aim. sShotAimBias stacks on these, so the logged nudge is a
		// delta: +11.28/-29.08 plus -24.48/+11.28 gives the values below.
		//
		// The yaw magnitude dropped from 29 degrees to 18, which is a mild
		// sign this is converging on something physical rather than
		// cancelling an unrelated defect - the 29 was measured while the
		// renderer still had known bugs in it.
		//
		// Note this constant is only stable while the transform chain feeding
		// it is. If the weapon or view path changes again, expect to
		// re-measure. Correcting kControllerAimTilt above - the grip-axis
		// assumption these values keep compensating for - is the change that
		// would stop that recurring.
		// Third pass, 2026-08-11: -13.20/-17.80 plus the +9.35/-8.73 nudge.
		//
		// These are NOT converging. Across three passes the totals have gone
		// pitch +11.28 -> -13.20 -> -3.85 and yaw -29.08 -> -17.80 -> -26.53:
		// oscillating around roughly -2 and -24 rather than settling. The
		// transplant is exact (the nudge adds to this same constant in this
		// same space), so the bake is not losing anything - something differs
		// between sessions.
		//
		// Most likely the 64.6 degree kControllerAimTilt above: it assumes one
		// wrist posture, and a correction this large means small differences
		// in how the controller is held change what is needed. A constant
		// cannot absorb a posture-dependent error. Measuring the barrel axis
		// objectively - model orientation against aim direction, in one frame -
		// would end the re-calibration cycle; eyeballing impacts cannot.
		static const float kBarrelPitchBias = -0.06720f;   // -3.85 deg
		static const float kBarrelYawBias   = -0.46304f;   // -26.53 deg

		const float tilt = kControllerAimTilt + kBarrelPitchBias + sShotAimBias[0];
		const float yawB = kBarrelYawBias + sShotAimBias[1];
		const float tc = cosf(tilt), ts = sinf(tilt);
		const float yc = cosf(yawB), ysn = sinf(yawB);

		// Barrel axis in controller space: -Z tilted toward -Y by `tilt`,
		// then swung about the BARREL's own up axis by `yawB`.
		//
		// The swing used to be about the CONTROLLER's up axis, which is wrong
		// once the tilt has moved the barrel away from it: the two are no
		// longer perpendicular, so rotating about it moves the barrel
		// diagonally rather than sideways, and the size of the sideways
		// component varies with elevation. Live symptoms, both explained by
		// it: the calibration stick moved the impact "diagonal instead of
		// straight", and the residual flipped sign between shooting up and
		// shooting down - with a correction as large as 27 degrees, the
		// coupling is large too.
		//
		// With f = (0, -sin t, -cos t) the barrel's own up is
		// u = right x f = (0, cos t, -sin t), and u x f = (-1, 0, 0), so
		// rotating f about u by phi gives f cos(phi) + (u x f) sin(phi):
		//
		//     v = (-sin phi, -sin t cos phi, -cos t cos phi)
		//
		// The cos(phi) on the Y term is what the old form was missing - that
		// omission IS the coupling.
		const float vx = -ysn;
		const float vy = -ts * yc;
		const float vz = -tc * yc;

		float fwd[3] = {
			vx * m.m[0][0] + vy * m.m[0][1] + vz * m.m[0][2],
			vx * m.m[1][0] + vy * m.m[1][1] + vz * m.m[1][2],
			vx * m.m[2][0] + vy * m.m[2][1] + vz * m.m[2][2],
		};

		// Into the reference frame - but its YAW ONLY.
		//
		// This used the full reference rotation, which is the head's
		// orientation captured at startup. If the headset was tilted at that
		// moment - on a desk, or simply worn at an angle - the entire frame is
		// pitched, and a level controller comes out pointing steeply upward.
		//
		// Measured: ctrl pitch read +84.45 degrees with the controller held
		// level and forward. At that angle the aim is in GIMBAL LOCK, where
		// yaw is ill-defined, so tiny hand movements swung the reported yaw
		// through 140 and 180 degrees and every axis coupled into every other.
		// That is the diagonal shots, and it is why moving left/right changed
		// elevation.
		//
		// Yaw was never visibly wrong because a constant yaw error is absorbed
		// by the anchor - which is exactly what the old comment here observed,
		// without following it to the conclusion that the pitch error was the
		// same bug rather than a separate one.
		//
		// The room's up axis is already known: tracking space has Y up. So the
		// reference must only ever supply yaw, and the horizon stays level
		// however the headset happened to be sitting.
		const float *refR = G->vrReferenceRotation3x3;
		const float refYaw = atan2f(refR[2], refR[8]);
		const float rc = cosf(-refYaw), rs = sinf(-refYaw);
		float rel[3] = {
			rc * fwd[0] + rs * fwd[2],
			fwd[1],
			-rs * fwd[0] + rc * fwd[2],
		};

		// OpenVR -> game space, as a DIRECTION.
		//
		// Not the same mapping as the rotation conjugation S = diag(-1,-1,1)
		// used for the head pose. That conjugation is correct for rotation
		// MATRICES but wrong applied to a bare direction: it negates Y, which
		// inverts pitch, and it maps OpenVR's forward to game-space backward.
		// (Live test: pointing the controller down aimed the gun up, while
		// yaw was fine - exactly the signature of a flipped Y.)
		//
		// The correct direction mapping takes OpenVR's right/up/forward
		// (+X, +Y, -Z) to the game's right/up/forward (+X, +Y, +Z), i.e.
		// negate Z only. Yaw behaves identically under either mapping - they
		// differ by a constant 180 degrees, which the anchor below absorbs -
		// which is why only pitch was visibly wrong.
		float gx = rel[0], gy = rel[1], gz = -rel[2];

		float clampedY = gy < -1.0f ? -1.0f : (gy > 1.0f ? 1.0f : gy);
		// SIGHT ZEROING, applied to the AIM rather than to the weapon.
		//
		// Bullets go where the game's camera points, and the camera is steered
		// toward this direction - the controller's own forward axis. The gun's
		// barrel is the MODEL's axis, which differs from the controller's by a
		// fixed rotation. Correcting that by rotating the weapon would drag the
		// gun off the controller it is pinned to; correcting the aim instead
		// leaves the weapon exactly where it was calibrated and moves the point
		// of impact onto the sights, which is the direction that should follow
		// the barrel anyway.
		*outYaw = atan2f(gx, gz) + sWeaponRotAdjust[1];
		*outPitch = asinf(clampedY) + sWeaponRotAdjust[0];
		return true;
	}

	float WrapAngle(float a)
	{
		while (a > 3.14159265f) a -= 6.28318531f;
		while (a < -3.14159265f) a += 6.28318531f;
		return a;
	}

	// Cumulative look input we have COMMANDED, in radians. Drives the aim
	// loop's shortfall calculation.
	static float sAppliedOffsetYaw = 0.0f;
	static float sAppliedOffsetPitch = 0.0f;

	// Our best estimate of how much the game's camera ACTUALLY moved because
	// of us - which is what has to be subtracted from the rendered view, and
	// is not the same thing as what we commanded. Two effects separate them:
	//
	//  - Latency. The game applies injected input a frame or two after we
	//    send it, so cancelling on the same frame subtracts rotation that
	//    has not happened yet, and the view lurches until reality catches up.
	//  - Scale. "0.00159 radians per mouse unit" is a measured estimate; if
	//    the game really moves a few percent more, that few percent of every
	//    gun movement leaks into the view.
	//
	// Both are measured rather than assumed - see UpdateLagScaleEstimate.

	// Most recent fit, carried into the next frame so the aim loop can
	// pre-divide its target by the game's real sensitivity.
	// -----------------------------------------------------------------
	// Camera-write code probe.
	//
	// Every value-based search for the camera's orientation has failed the
	// same way: plenty of memory TRACKS the camera, but it is all recomputed
	// copies rather than state the engine reads back. Searching for values
	// can only ever find effects. This finds the CAUSE.
	//
	// The engine has to write its view matrix into the constant buffer we
	// already intercept, and Map hands us the exact address it will write to.
	// So arm a hardware breakpoint on that address. When the game stores the
	// matrix, the CPU traps mid-instruction and we get RIP sitting inside the
	// engine's own camera code, plus every register at that moment - one of
	// which is almost certainly a pointer to the camera or player object.
	//
	// That is the authoritative state, and it is reachable from code in a way
	// it was never reachable from a value scan.
	//
	// Read-only and one-shot: it disarms itself on the first hit, logs, and
	// never fires again.

	static void *sProbeWatchAddress = NULL;
	static volatile LONG sProbeState = 0;      // 0 idle, 1 arming, 2 armed, 3 fired
	static PVOID sProbeVehHandle = NULL;

	// A separate DR1 watch follows one render-queue slot backwards to the code
	// that creates/recycles it.  Keep it independent from the older DR0 camera
	// and fire probes: those are still useful elsewhere and may remain armed.
	static volatile PVOID sQueueEntryWatchAddress = NULL;
	static volatile LONG sQueueEntryWatchState = 0; // 0 idle, 1 pending, 2 armed, 3 hit
	static volatile LONG sQueueEntryWatchReportPending = 0;
	static volatile LONG sQueueEntryWatchReported = 0;
	static CONTEXT sQueueEntryWatchContext = {};
	static DWORD64 sQueueEntryWatchStack[64] = {};
	static const DWORD kQueueArmExceptionCode = 0x51554555; // 'QUEU'
	static const DWORD64 kDr7QueueMask =
		(1ull << 2) | (1ull << 3) | (3ull << 20) | (3ull << 22);
	static const DWORD64 kDr7QueueWrite4 =
		(1ull << 2) | (1ull << 8) | (1ull << 9) | (1ull << 10)
		| (1ull << 20) | (3ull << 22);

	// Temporary native-ADS writer trace. The addresses are deliberately loaded
	// from a file produced after each process launch: private allocations move
	// under ASLR, so embedding addresses from an earlier run would be unsafe.
	// All four debug registers are reserved while this diagnostic is active.
	static const bool kADSWriterDiagnostic = false;
	static const DWORD kADSWriterArmExceptionCode = 0x41445357; // 'ADSW'
	static DWORD64 sADSWriterAddresses[4] = {};
	static volatile LONG sADSWriterState = 0; // 0 waiting, 1 armed
	static volatile LONG sADSWriterCaptured[4] = {};
	static volatile LONG sADSWriterReportMask = 0;
	static CONTEXT sADSWriterContexts[4] = {};
	static DWORD64 sADSWriterStacks[4][32] = {};
	static const DWORD64 kADSWriterDr7 =
		(1ull << 0) | (1ull << 2) | (1ull << 4) | (1ull << 6)
		| (1ull << 8) | (1ull << 9) | (1ull << 10)
		| (1ull << 16) | (1ull << 20) | (1ull << 24) | (1ull << 28);

	// Resolves a code address to "module+RVA", which is what a disassembler
	// needs - the runtime base moves with ASLR and is meaningless on its own.
	void DescribeCodeAddress(DWORD64 addr, char *out, size_t outSize)
	{
		HMODULE mod = NULL;
		if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			(LPCSTR)addr, &mod) && mod) {
			char path[MAX_PATH] = { 0 };
			GetModuleFileNameA(mod, path, sizeof(path));
			const char *name = strrchr(path, '\\');
			name = name ? name + 1 : path;
			_snprintf_s(out, outSize, _TRUNCATE, "%s+0x%llX (base %p)",
				name, addr - (DWORD64)mod, mod);
		} else {
			_snprintf_s(out, outSize, _TRUNCATE, "0x%llX (no module)", addr);
		}
	}

	static const DWORD kGuardPageViolation = 0x80000001;   // STATUS_GUARD_PAGE_VIOLATION
	void CoverageNoteAddress(DWORD_PTR addr);
	static volatile LONG sFireProbeArmed = 0;
	static volatile LONG sFireProbeState = 0;   // 0 idle, 1 armed, 3 done
	static volatile LONG sFireProbeReported = 0;
	void ReportFireCall(DWORD64 objPtr);
	void RedirectShot(DWORD64 objPtr);
	static volatile LONG sShotWriteWatchPending = 0;
	static volatile LONG sShotWriteWatchActive = 0;
	static volatile LONG sShotWriteWatchCount = 0;
	static DWORD64 sShotWriteWatchObject = 0;
	static void ArmNextShotWriteWatch(DWORD64 objPtr);
	static const DWORD64 kDr7ShotWriteMask =
		(1ull << 2) | (3ull << 20) | (3ull << 22);
	static const DWORD64 kDr7ShotWrite4 =
		(1ull << 2) | (1ull << 8) | (1ull << 9) | (1ull << 10)
		| (1ull << 20) | (3ull << 22);
	static int ArmShotWriteWatchOnAllThreads(void *addr);
	static volatile LONG sShotPrepWatchPending = 0;
	static volatile LONG sShotPrepWatchMode = 0; // 1 = execution, 2 = target write
	static DWORD64 sShotPrepWatchObject = 0;
	// The earlier loop-entry probe at +0x3D8305 never fired in the live game.
	// The confirmed call site immediately before the fire wrapper is stable and
	// carries the shot object in RDX.
	static const DWORD_PTR kShotPrepRva = 0x3D85FC;
	static const DWORD64 kDr7ShotPrepExec =
		(1ull << 3) | (1ull << 8) | (1ull << 9) | (1ull << 10);
	static const DWORD64 kDr7ShotPrepWrite4 = kDr7ShotPrepExec
		| (1ull << 24) | (3ull << 26);
	static const DWORD64 kDr7ShotPrepMask =
		(1ull << 3) | (3ull << 24) | (3ull << 26);
	static int ArmShotPrepExecOnAllThreads(void *addr);

	// Set once the inline hook is live. The debug-register path defers to it
	// so a shot is never redirected twice.
	static volatile LONG sInlineFireHookActive = 0;

	// What RedirectShot last wrote into the object's ray fields, so the hook
	// can check after the engine's call whether they survived it.
	static float sLastWrittenOrigin[3] = { 0, 0, 0 };
	static float sLastWrittenDir[3] = { 0, 0, 0 };
	static float sLastCamPosAtShot[3] = { 0, 0, 0 };
	static volatile LONG sRayWritePending = 0;
	// Controller anchor, shared by the fire redirect and the parked aim loop.
	// Weapon rest pose, cleared by RecenterAiming so recentring recalibrates.
	// Where the weapon is drawn, in view space, published by the renderer so
	// the shot can be traced from the gun rather than from the eye.
	static float sWeaponViewPos[3] = { 0, 0, 0 };
	static bool sWeaponViewPosValid = false;

	void SetWeaponViewPosition(const float v[3])
	{
		sWeaponViewPos[0] = v[0];
		sWeaponViewPos[1] = v[1];
		sWeaponViewPos[2] = v[2];
		sWeaponViewPosValid = true;
	}

	bool GetWeaponViewPosition(float out[3])
	{
		if (!sWeaponViewPosValid)
			return false;
		out[0] = sWeaponViewPos[0];
		out[1] = sWeaponViewPos[1];
		out[2] = sWeaponViewPos[2];
		return true;
	}

	static bool sWeaponAnchored = false;
	static bool sWeaponPosAnchored = false;
	static int sWeaponAnchorSettle = 0;
	// Rotation the weapon must have at the anchor for its barrel to retain the
	// controller's calibrated direction. The old anchor always produced
	// identity, returning both the model and shot ray to Metro's native rest
	// direction regardless of where the controller was actually pointing.
	static float sWeaponAnchorOutputRot[9] = { 1,0,0, 0,1,0, 0,0,1 };

	static bool sCtrlAnchored = false;
	static float sAnchorCtrlYaw = 0.0f, sAnchorCtrlPitch = 0.0f;

	bool GetAimOffsetFromController(float *outYaw, float *outPitch);
	static const bool kFireRedirectTest = true;
	// Move the VR aim into Metro's pre-fire trace input. This lets the engine
	// perform its own spread, recoil, collision, and maximum-range handling.
	// The older post-trace rewrite is bypassed while this is enabled.
	static const bool kUpstreamPreFireRedirect = false;
	// Hook Metro's shot-construction routine before it applies native
	// dispersion.  Only replace the centre direction; the engine remains
	// responsible for spread, recoil, collision, range, damage, and effects.
	static const bool kPreSpreadShotRedirect = false;
	// +0x4F2E40 is the authoritative branch selected for shot records whose
	// +0x134 marker is non-zero. It consumes +0xF0 directly before collision
	// and damage. The sibling +0x4F3E60 branch is the non-authoritative visual
	// path that produced the second, non-damaging impact.
	static const bool kAuthoritativeShotRedirect = false;
	// Earliest shared shot producer. +0x3D9360 receives the source descriptor
	// in R9, copies it to the queued record, and only then calls +0x4F2770.
	// Redirecting at entry ensures every consumer starts from the VR barrel;
	// Metro's native dispersion routine still runs afterward.
	static const bool kShotProducerRedirect = false;
	// Actual ballistic collision routine. +0x3D9A20 reads RDX+0xF0 into its
	// collision direction at +0x3D9AD1; visual records are produced only after
	// this routine has completed its hit loop.
	static const bool kBallisticCollisionRedirect = false;
	// +0x3D7FF0 is the projectile-creation boundary. Its RDX and R8 arguments
	// are explicit origin and direction pointers; it passes them to the
	// projectile constructor at +0x3D6970, which copies R8 to projectile+0xF0.
	// Redirect here exactly once instead of reprocessing the projectile during
	// each later collision/update pass.
	static const bool kProjectileCreationRedirect = true;

	// First reverse-engineering pass for native Metro shooting.  Keep the
	// fire hook installed, but do not modify the shot object.  This gives us
	// an unmodified native spread/recoil baseline while still capturing the
	// live decrypted module on the first shot for offline disassembly.
	// Flip back to false only when comparing against the existing VR redirect.
	static const bool kNativeShotDiagnostics = false;
	// Redirect the shot center to the VR barrel while carrying the native
	// direction's yaw/pitch deviation (spread and recoil) onto that center.
	static const bool kPreserveNativeShotDeviation = true;
	// Diagnostic isolation: leave Metro's already-computed hit point intact
	// while testing whether relocating +0x150 causes the vertical inversion.
	static const bool kRewriteNativeHitPoint = true;
	static volatile LONG sNativeShotDiagnosticLogged = 0;

	// Our own exception code, raised purely to get at this thread's context.
	static const DWORD kArmExceptionCode = 0x4D455452;   // 'METR'

	// Self-test state. Two things can make the probe silently never fire -
	// arming failing, or the engine writing from a different thread than the
	// one that mapped the buffer - and they look identical in the log. So
	// prove the mechanism against a write we control before arming for real,
	// and the log distinguishes them outright.
	static volatile LONG sProbeSelfTesting = 0;
	static volatile LONG sProbeSelfTestHit = 0;
	static volatile DWORD64 sProbeSelfTestVar = 0;

	// Set when the probe fires; the dump itself happens from Present, since
	// file I/O inside a vectored exception handler is asking for trouble.
	static volatile LONG sWantModuleDump = 0;

	// Stage 2 runs after stage 1 finishes, so the two never contend for DR0.
	static volatile LONG sStage2State = 0;   // 0 idle, 1 arming/armed, 3 done
	static volatile LONG sStage2Armed = 0;
	static volatile LONG sProbeReported = 0;
	static volatile LONG sStage3State = 0;
	static volatile LONG sStage3Armed = 0;
	static volatile LONG sStage4State = 0;
	static volatile LONG sMainCameraSourceWatchState = 0;
	static const DWORD_PTR kMainCameraSourceMatrixRva = 0xD07730;
	static void *sCmdStreamMatrixAddr = NULL;

	// DR7 configuration for the pending arm. Write watches and execution
	// breakpoints need different RW/LEN encodings, and stage 4 needs both.
	//   write, 4 bytes : RW=01 LEN=11
	//   execute        : RW=00 LEN=00 (and the address must be an
	//                    instruction boundary, not just any byte)
	static const DWORD64 kDr7Common = (1ull << 0) | (1ull << 8) | (1ull << 9) | (1ull << 10);
	static const DWORD64 kDr7Write4 = kDr7Common | (1ull << 16) | (3ull << 18);
	// RW=11 watches reads as well as writes. Used only by the manual
	// ownership diagnostic, never by the shipped camera path.
	static const DWORD64 kDr7ReadWrite4 = kDr7Common | (3ull << 16) | (3ull << 18);
	static const DWORD64 kDr7Exec   = kDr7Common;
	static volatile DWORD64 sProbeDr7 = kDr7Write4;
	static int sDumpIndex = 0;
	static volatile LONG sOwnershipProbeSlot = 0;
	static volatile LONG sOwnershipProbeArmed = 0;
	static volatile LONG sOwnershipProbeLabel = 0; // 1=ordinary, 2=scripted
	static volatile LONG sOwnershipProbeStage = 0; // 1=setter, 2=local copy, 3=payload write
	static const DWORD_PTR kCameraCommandRecorderRva = 0x8373FF;

	// Direct-camera investigation, experiment 1.  This hook is deliberately
	// inert until Shift+F10 is pressed.  It targets the pre-replay camera construction
	// path at +0xD6BE0, not the known-too-late +0x7E09A0 setter.  The selected
	// caller (+0x846F33) is observed every frame and immediately follows
	// it with the matching projection matrix; static analysis makes it the best
	// main-view candidate, but the probe exists to prove or reject that inference
	// in game.  Mouse follow remains completely intact during this experiment.
	static const bool kDirectCameraConstructionProbeEnabled = false;
	static const DWORD_PTR kDirectCameraConstructionRva = 0xD6BE0;
	static const DWORD_PTR kDirectCameraMainViewCallRva = 0x846F2E;
	static volatile LONG sDirectCameraProbeInstalled = 0;
	static volatile LONG sDirectCameraProbeActive = 0;
	static volatile LONG sDirectCameraProbeHitLogged = 0;
	static float sDirectCameraEntryInjectedYaw = 0.0f;
	static float sDirectCameraEntryInjectedPitch = 0.0f;
	static void *sDirectCameraProbeReturnAddress = NULL;
	typedef void (__fastcall *tDirectCameraConstruction)(void *recorder,
		const float *viewMatrix);
	static tDirectCameraConstruction trampoline_DirectCameraConstruction = NULL;

	// Direct-camera investigation, experiment 2.  The v10 writer trace reached
	// +0x69B900, called from the active camera object's virtual +0x1220 update at
	// +0x1F38A0.  Unlike the rejected +0xD6BE0 recorder hook, this routine is
	// executed near the start of Metro's frame callback and constructs the
	// global camera basis/view state at +0xD076F0/+0xD07730.  The active A/B
	// mode applies HMD yaw/pitch changes directly to private copies of the two
	// incoming orientation vectors and suppresses synthetic follow.  Roll stays
	// in the existing rendered-pose path so it is not applied twice.
	static const bool kUpstreamCameraBasisProbeEnabled = true;
	static const DWORD_PTR kUpstreamCameraBasisRva = 0x69B900;
	static const DWORD_PTR kUpstreamCameraBasisCallRva = 0x1F3960;
	static volatile LONG sUpstreamCameraBasisProbeInstalled = 0;
	// Native-only selection; successful installation still gates hook execution.
	static volatile LONG sUpstreamCameraBasisProbeActive = 1;
	bool NativeStateFollowSelected()
	{
		return kNativeStateFollowEnabled &&
			InterlockedCompareExchange(&sNativeStateFollowInstalled, 0, 0) == 1 &&
			InterlockedCompareExchange(&sUpstreamCameraBasisProbeActive, 0, 0) != 0;
	}
	static volatile LONG sUpstreamCameraBasisProbeHitLogged = 0;
	static volatile LONG sUpstreamCameraYieldingToScript = 1;
	static int sUpstreamCameraYieldReleaseFrames = 0;
	static void *sUpstreamCameraBasisReturnAddress = NULL;
	static float sUpstreamHeadReferenceYawPitch[9] = { 1,0,0, 0,1,0, 0,0,1 };
	static float sUpstreamEntryInjectedYaw = 0.0f;
	static float sUpstreamEntryInjectedPitch = 0.0f;
	typedef void (__fastcall *tUpstreamCameraBasis)(void *manager,
		const float *position, const float *axisA, const float *axisB,
		void *arg5, void *arg6);
	static tUpstreamCameraBasis trampoline_UpstreamCameraBasis = NULL;
	// Native camera-effector ownership feeds camera/viewmodel handoff gates.
	// Keep this observer even though its temporary transition trace is retired.
	static const DWORD_PTR kCameraEffectorBlendRva = 0x69C370;
	typedef int (__fastcall *tNativeCameraEffectorBlend)(void *manager, int allowBlend);
	static tNativeCameraEffectorBlend trampoline_NativeCameraEffectorBlend = NULL;
	static volatile LONG sNativeCameraObserverInstalled = 0;
	static volatile LONG sObservedAuthoredCamera = 0;
	static volatile LONG sObservedAuthoredViewmodel = 0;
	static volatile LONG sNativeVendorCameraOwnerInstalled = 0;

	static int __fastcall Hooked_NativeCameraOwnershipObserver(void *manager,
		int allowBlend)
	{
		const int result = trampoline_NativeCameraEffectorBlend
			? trampoline_NativeCameraEffectorBlend(manager, allowBlend) : 0;
		if (!manager)
			return result;

		bool authoredCamera = false;
		bool authoredViewmodel = false;
		__try {
			void **entries = *(void ***)((BYTE *)manager + 0x18);
			unsigned short count = *(unsigned short *)((BYTE *)manager + 0x22);
			if (count > 32)
				count = 32;
			for (unsigned i = 0; i < count; ++i) {
				void *effector = entries ? entries[i] : NULL;
				void *descriptor = effector
					? *(void **)((BYTE *)effector + 8) : NULL;
				const unsigned short flags = descriptor
					? *(unsigned short *)((BYTE *)descriptor + 8) : 0;
				if (flags == 0x0C08 || flags == 0x0E5C
					|| flags == 0x0C3C || flags == 0x063C)
					authoredCamera = true;
				if (flags == 0x0F18 || flags == 0x071C
					|| flags == 0x0718 || flags == 0x0C7C)
					authoredViewmodel = true;
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			authoredCamera = false;
			authoredViewmodel = false;
		}

		const LONG camera = authoredCamera ? 1 : 0;
		const LONG viewmodel = (authoredCamera || authoredViewmodel) ? 1 : 0;
		InterlockedExchange(
			&sObservedAuthoredCamera, camera);
		InterlockedExchange(
			&sObservedAuthoredViewmodel, viewmodel);
		return result;
	}

	static void InstallNativeCameraOwnershipObserver()
	{
		if (InterlockedCompareExchange(&sNativeCameraObserverInstalled, 1, 1))
			return;
		BYTE *base = (BYTE *)GetModuleHandleA(NULL);
		if (!base)
			return;
		void *function = base + kCameraEffectorBlendRva;
		static const BYTE expected[] = { 0x48, 0x8B, 0xC4, 0x88, 0x50, 0x10,
			0x55, 0x53, 0x56, 0x57 };
		if (memcmp(function, expected, sizeof(expected)) != 0) {
			LogInfo("VRPose native ownership observer: signature mismatch at +0x69C370\n");
			InterlockedExchange(&sNativeCameraObserverInstalled, 1);
			return;
		}
		SIZE_T hookId = 0;
		const DWORD error = cHookMgr.Hook(&hookId,
			(LPVOID *)&trampoline_NativeCameraEffectorBlend, function,
			Hooked_NativeCameraOwnershipObserver);
		if (error || !trampoline_NativeCameraEffectorBlend) {
			LogInfo("VRPose native ownership observer: hook failed (err=0x%x)\n", error);
			InterlockedExchange(&sNativeCameraObserverInstalled, 1);
			return;
		}
		InterlockedExchange(&sNativeCameraObserverInstalled, 1);
		LogInfo("VRPose native ownership observer: installed at +0x69C370\n");
	}

#include "NativeVendorCameraOwner.inl"

	static bool NativeAuthoredCameraOwnerActive()
	{
		return InterlockedCompareExchange(&sObservedAuthoredCamera, 0, 0) != 0
			|| NativeVendorCameraOwnerActive();
	}

	// Native-state experiment: alter the local angular result BEFORE Metro's
	// own look/body setters and camera-source update. Never alter movement output,
	// raw stick intent, or the published camera basis in this mode.
	typedef void (__fastcall *tNativeLookUpdate)(void *player, float dt);
	typedef void (__fastcall *tNativeAngularUpdate)(void *controller, float *angle,
		float *pending, float reference, float factor, bool bounded,
		const float *hardLimits, bool softActive, const float *softLimits, float multiplier);
	static tNativeLookUpdate trampoline_NativeLookUpdate = NULL;
	static tNativeAngularUpdate trampoline_NativeAngularUpdate = NULL;
	static __declspec(thread) BYTE *sNativeFollowPlayer = NULL;
	static BYTE *sNativeFollowModule = NULL;

	static bool MetroNativeLookInputAllowed(BYTE *player)
	{
		// These are the actual mouse handler's native predicates. The live input
		// owner's +0x88 implementation (+0x29ABD0) only consumes actions 9/10,
		// never look axes 5..8. Refuse an unverified replacement implementation.
		__try {
			BYTE *base = sNativeFollowModule;
			if ((base[0xD07664] && !base[0xD07665]) ||
				!(*(float *)(player + 0x308) > 0.0f))
				return false;
			BYTE *root = *(BYTE **)(base + 0xD01E50);
			void *manager = root ? *(void **)(root + 0x10) : NULL;
			typedef bool (__fastcall *tAllowed)(void *);
			if (!manager || !((tAllowed)(base + 0x69B170))(manager))
				return false;
			BYTE *input = *(BYTE **)(root + 8);
			if (!input) return false;
			void **vtable = *(void ***)input;
			if (vtable[0x88 / sizeof(void *)] != base + 0x29ABD0 ||
				vtable[0xD0 / sizeof(void *)] != base + 0x29A3E0)
				return false;
			if (((tAllowed)vtable[0xD0 / sizeof(void *)])(input) && input[0xE1])
				return false;
			return true;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	static bool NativeFollowInputAllowed(BYTE *player)
	{
		if (!G || !G->vrHeadRotationValid || !IsGameplayModeActive() ||
			InterlockedCompareExchange(&sPauseMenuExpected, 0, 0) ||
			NativeAuthoredCameraOwnerActive() ||
			ScriptedPlayerCameraOwnerActive())
			return false;
		return MetroNativeLookInputAllowed(player);
	}

	static void __fastcall Hooked_NativeAngularUpdate(void *controller, float *angle,
		float *pending, float reference, float factor, bool bounded,
		const float *hardLimits, bool softActive, const float *softLimits, float multiplier)
	{
		const void *caller = _ReturnAddress();
		// Consume genuine native input exactly once. Its residual, smoothing, and
		// ownership remain Metro's. Never infer HMD credit from this rotation.
		trampoline_NativeAngularUpdate(controller, angle, pending, reference,
			factor, bounded, hardLimits, softActive, softLimits, multiplier);
		if (!sNativeFollowPlayer || !angle || !pending || !G)
			return;
		const bool yaw = caller == sNativeFollowModule + 0x8FE54A &&
			pending == (float *)(sNativeFollowPlayer + 0xF4C);
		const bool pitch = caller == sNativeFollowModule + 0x8FE717 &&
			pending == (float *)(sNativeFollowPlayer + 0xF48);
		if (!yaw && !pitch) return;
		const float *head = G->vrHeadRotation3x3;
		const float before = *angle;
		const float headY = head[5] < -1.0f ? -1.0f : (head[5] > 1.0f ? 1.0f : head[5]);
		const float requested = yaw ? NativeFollowYawError(atan2f(head[2], head[8]))
			: NativeFollowWrap(-asinf(headY) - before);
		const float scale = factor * multiplier;
		if (!isfinite(before) || !isfinite(requested) || !isfinite(scale) ||
			scale <= 0.000001f || fabsf(requested) <= 0.000001f)
			return;
		float privateAngle = before;
		float privatePending = requested / scale;
		if (!isfinite(privatePending)) return;
		// Native hard/soft limits still decide the applied angle. The private
		// residual is discarded; it never enters the player's rate accumulator.
		trampoline_NativeAngularUpdate(controller, &privateAngle, &privatePending,
			reference, factor, bounded, hardLimits, softActive, softLimits, multiplier);
		if (!isfinite(privateAngle)) return;
		*angle = privateAngle;
		if (yaw) CreditNativeFollowYaw(NativeFollowWrap(privateAngle - before));
		else CreditNativeFollowPitch(privateAngle);
	}

	static void __fastcall Hooked_NativeLookUpdate(void *player, float dt)
	{
		BYTE *previous = sNativeFollowPlayer;
		sNativeFollowPlayer = NULL;
		void *local = NULL;
		if (InterlockedCompareExchange(&sNativeStateFollowInstalled, 0, 0) == 1 &&
			InterlockedCompareExchange(&sUpstreamCameraBasisProbeActive, 0, 0) &&
			ResolveMetroPlayer(&local) && local == player) {
			const bool allowed = NativeFollowInputAllowed((BYTE *)player);
			const int bodyTurnUnits = TakeNativeBodyTurnIntent();
			const bool settling = sNativeFollowSettleFrames != 0;
			if (settling) --sNativeFollowSettleFrames;
			const LONG yielding = allowed && !settling ? 0 : 1;
			InterlockedExchange(&sUpstreamCameraYieldingToScript, yielding);
			if (allowed && !settling) {
				if (bodyTurnUnits && sNativeBodyLookInput && !VRMenu::IsOpen() &&
					!VRMenu::GetSettings().gamepadMode) {
					// Same native callback reached by mouse motion, on Metro's
					// update thread and BEFORE its ordinary look/body setters.
					// No SendInput, virtual RX, angle prediction or HMD credit.
					sNativeBodyLookInput((BYTE *)player + 0xE20,
						(float)bodyTurnUnits, 0.0f, 0);
				}
				sNativeFollowPlayer = (BYTE *)player;
			}
		}
		trampoline_NativeLookUpdate(player, dt);
		sNativeFollowPlayer = previous;
	}

#include "NativeCameraCreditAdapter.inl"
#include "NativeVisibilityCoverage.inl"

	static void InstallNativeStateFollow()
	{
		if (InterlockedCompareExchange(&sNativeStateFollowInstalled, 0, 0)) return;
		BYTE *base = (BYTE *)GetModuleHandleA(NULL);
		if (!base) return;
		static const BYTE look[] = { 0x48,0x8B,0xC4,0x55,0x57,0x48,0x8D,0x68,
			0x98,0x48,0x81,0xEC,0x58,0x01,0x00,0x00 };
		static const BYTE angular[] = { 0x48,0x8B,0xC4,0xF3,0x0F,0x11,0x58,0x20,
			0x55,0x53,0x41,0x55,0x48,0x8D,0x68,0xD1 };
		static const BYTE gate[] = { 0x48,0x8B,0xD1,0x83,0xC9,0xFF,0x33,0xC0,
			0xF0,0x0F,0xB1,0x0A,0x75,0xF8 };
		static const BYTE bodyLook[] = { 0x48,0x89,0x74,0x24,0x10,0x57,
			0x48,0x81,0xEC,0xA0,0x00,0x00,0x00 };
		if (memcmp(base + 0x28BFC0, look, sizeof(look)) ||
			memcmp(base + 0x8FE740, angular, sizeof(angular)) ||
			memcmp(base + 0x69B170, gate, sizeof(gate)) ||
			memcmp(base + 0x28B280, bodyLook, sizeof(bodyLook))) return;
		const BYTE *bodyDispatch = base + 0x28B4C2;
		if (bodyDispatch[0] != 0xE8 || bodyDispatch + 5 + *(const INT32 *)(bodyDispatch + 1)
			!= base + 0x28BE80) return;
		// Validate both selected wrapper calls before installing either hook.
		const DWORD_PTR calls[] = { 0x8FE545, 0x8FE712 };
		for (unsigned i = 0; i < 2; ++i) {
			BYTE *call = base + calls[i];
			if (call[0] != 0xE8 || call + 5 + *(INT32 *)(call + 1) != base + 0x8FE740) {
				InterlockedExchange(&sNativeStateFollowInstalled, -1);
				LogInfo("VRPose native-state follow: wrapper signature rejected; native follow unavailable (no mouse fallback)\n");
				return;
			}
		}
		sNativeFollowModule = base;
		SIZE_T angularId = 0, lookId = 0;
		DWORD error = cHookMgr.Hook(&angularId, (LPVOID *)&trampoline_NativeAngularUpdate,
			base + 0x8FE740, Hooked_NativeAngularUpdate);
		if (!error && trampoline_NativeAngularUpdate)
			error = cHookMgr.Hook(&lookId, (LPVOID *)&trampoline_NativeLookUpdate,
				base + 0x28BFC0, Hooked_NativeLookUpdate);
		const bool installed = !error && trampoline_NativeAngularUpdate && trampoline_NativeLookUpdate;
		if (installed) sNativeBodyLookInput = (tNativeBodyLookInput)(base + 0x28B280);
		InterlockedExchange(&sNativeStateFollowInstalled, installed ? 1 : -1);
		if (installed) {
			sNativeFollowSettleFrames = 32;
			ClearNativeBodyTurnIntent();
			ResetNativeCameraCreditEpoch();
			SeedNativeFollowHistory();
		}
		if (installed) InstallNativeCameraCreditAdapter();
		if (installed) InstallNativeVendorCameraOwner();
		if (installed) InstallNativeVisibilityCoverage();
		if (installed) LogInfo("VRPose native body turn: direct +28B280 input delivery ready; native mode emits no turn mouse events\n");
		LogInfo("VRPose native-state follow: %s; native-only, no mouse fallback or mode toggle\n",
			installed ? "installed and enabled" : "install failed");
	}

	static void __fastcall Hooked_DirectCameraConstruction(void *recorder,
		const float *viewMatrix)
	{
		if (!trampoline_DirectCameraConstruction || !viewMatrix)
			return;

		const bool selectedMainViewCall =
			_ReturnAddress() == sDirectCameraProbeReturnAddress;
		if (!selectedMainViewCall ||
			InterlockedCompareExchange(&sDirectCameraProbeActive, 0, 0) == 0 ||
			!G || !G->vrHeadRotationValid) {
			trampoline_DirectCameraConstruction(recorder, viewMatrix);
			return;
		}

		// Build the engine-facing camera from Metro's body/script camera plus the
		// current HMD rotation. First remove the synthetic follow that is already
		// baked into Metro's persistent camera, using the values captured when
		// direct mode was enabled. Then compose the tracked delta exactly as the
		// rendered-camera path does. Recompute translation from the unchanged
		// camera world position; rotating a view matrix while retaining its old
		// translation silently moves the camera and broke shadow selection in the
		// fixed five-degree probe.
		float sourceR[9] = {
			viewMatrix[0], viewMatrix[1], viewMatrix[2],
			viewMatrix[4], viewMatrix[5], viewMatrix[6],
			viewMatrix[8], viewMatrix[9], viewMatrix[10]
		};
		const float sourceT[3] = { viewMatrix[3], viewMatrix[7], viewMatrix[11] };
		float inverseR[9], cameraPos[3];
		ComputeRigidInverse(sourceR, sourceT, inverseR, cameraPos);

		const float cy = cosf(sDirectCameraEntryInjectedYaw);
		const float sy = sinf(sDirectCameraEntryInjectedYaw);
		const float yawCancel[9] = {
			 cy, 0.0f,  sy,
			0.0f, 1.0f, 0.0f,
			-sy, 0.0f,  cy
		};
		const float cp = cosf(sDirectCameraEntryInjectedPitch);
		const float sp = sinf(sDirectCameraEntryInjectedPitch);
		const float pitchCancel[9] = {
			1.0f, 0.0f, 0.0f,
			0.0f,  cp,   sp,
			0.0f, -sp,   cp
		};
		float tempR[9], bodyR[9];
		Multiply3x3(sourceR, yawCancel, tempR);
		Multiply3x3(pitchCancel, tempR, bodyR);

		float headT[9], directR[9];
		Transpose3x3(G->vrHeadRotation3x3, headT);
		Multiply3x3(headT, bodyR, directR);

		__declspec(align(16)) float directView[16];
		memcpy(directView, viewMatrix, sizeof(directView));
		for (unsigned row = 0; row < 3; ++row) {
			directView[row * 4 + 0] = directR[row * 3 + 0];
			directView[row * 4 + 1] = directR[row * 3 + 1];
			directView[row * 4 + 2] = directR[row * 3 + 2];
			directView[row * 4 + 3] = -(directR[row * 3 + 0] * cameraPos[0]
				+ directR[row * 3 + 1] * cameraPos[1]
				+ directR[row * 3 + 2] * cameraPos[2]);
		}

		if (InterlockedExchange(&sDirectCameraProbeHitLogged, 1) == 0)
			LogInfo("VRPose direct-camera follow: selected +0x846F33 hit; "
				"applying HMD rotation with synthetic mouse follow suppressed\n");
		trampoline_DirectCameraConstruction(recorder, directView);
	}

	static void InstallDirectCameraConstructionProbe()
	{
		if (!kDirectCameraConstructionProbeEnabled ||
			InterlockedCompareExchange(&sDirectCameraProbeInstalled, 0, 0) != 0)
			return;
		BYTE *base = (BYTE *)GetModuleHandleA(NULL);
		if (!base)
			return;

		BYTE *function = base + kDirectCameraConstructionRva;
		static const BYTE expected[] = {
			0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x0F, 0x28, 0x02, 0x48, 0x8B, 0xD9
		};
		if (memcmp(function, expected, sizeof(expected)) != 0) {
			// Metro decrypts .text after startup. Retry quietly until this exact
			// signature becomes available instead of ever hooking uncertain code.
			static bool mismatchLogged = false;
			if (!mismatchLogged) {
				mismatchLogged = true;
				LogInfo("VRPose direct-camera probe: waiting for +0xD6BE0 signature; "
					"current bytes=%02X %02X %02X %02X %02X\n",
					function[0], function[1], function[2], function[3], function[4]);
			}
			return;
		}

		BYTE *call = base + kDirectCameraMainViewCallRva;
		const LONG displacement = *(const LONG *)(call + 1);
		if (call[0] != 0xE8 || call + 5 + displacement != function) {
			LogInfo("VRPose direct-camera probe: callsite signature mismatch at "
				"+0x846F33; probe disabled\n");
			InterlockedExchange(&sDirectCameraProbeInstalled, -1);
			return;
		}

		sDirectCameraProbeReturnAddress = call + 5;
		SIZE_T hookId = 0;
		const DWORD error = cHookMgr.Hook(&hookId,
			(LPVOID *)&trampoline_DirectCameraConstruction, function,
			Hooked_DirectCameraConstruction);
		if (error || !trampoline_DirectCameraConstruction) {
			LogInfo("VRPose direct-camera probe: hook failed at +0xD6BE0 "
				"(err=0x%x); probe disabled\n", error);
			InterlockedExchange(&sDirectCameraProbeInstalled, -1);
			return;
		}
		InterlockedExchange(&sDirectCameraProbeInstalled, 1);
		LogInfo("VRPose direct-camera probe: installed inert hook at +0xD6BE0; "
			"Shift+F10 toggles the +0x846F33 five-degree yaw probe\n");
	}

	static void PollDirectCameraConstructionProbeHotkey()
	{
		if (!kDirectCameraConstructionProbeEnabled ||
			InterlockedCompareExchange(&sDirectCameraProbeInstalled, 0, 0) != 1)
			return;
		static bool wasDown = false;
		const bool down = (GetAsyncKeyState(VK_F10) & 0x8000) != 0
			&& (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
		if (down && !wasDown) {
			const LONG active = InterlockedCompareExchange(
				&sDirectCameraProbeActive, 0, 0) ? 0 : 1;
			if (active) {
				sDirectCameraEntryInjectedYaw = GetCullingCancellation();
				sDirectCameraEntryInjectedPitch = GetCullingCancellationPitch();
			}
			InterlockedExchange(&sDirectCameraProbeActive, active);
			InterlockedExchange(&sDirectCameraProbeHitLogged, 0);
			LogInfo("VRPose direct-camera follow: %s (Shift+F10), entry yaw=%+.2f "
				"pitch=%+.2f deg\n", active ? "ACTIVE" : "inactive",
				sDirectCameraEntryInjectedYaw * 57.29578f,
				sDirectCameraEntryInjectedPitch * 57.29578f);
		}
		wasDown = down;
	}

	static void __fastcall Hooked_UpstreamCameraBasis(void *manager,
		const float *position, const float *axisA, const float *axisB,
		void *arg5, void *arg6)
	{
		if (!trampoline_UpstreamCameraBasis)
			return;

		const bool selectedCameraUpdate =
			_ReturnAddress() == sUpstreamCameraBasisReturnAddress;
		if (!selectedCameraUpdate || !position || !axisA || !axisB ||
			!G || !G->vrHeadRotationValid ||
			InterlockedCompareExchange(&sUpstreamCameraBasisProbeActive, 0, 0) == 0) {
			trampoline_UpstreamCameraBasis(manager, position, axisA, axisB, arg5, arg6);
			return;
		}

		// Construct a roll-free HMD basis. Metro's synthetic camera has never
		// contained tracked roll, and the rendered-pose patch still supplies it;
		// putting roll here too would double it. Relative = reference^T * current.
		const float *head = G->vrHeadRotation3x3;
		const float headYaw = atan2f(head[2], head[8]);
		const float headY = head[5] < -1.0f ? -1.0f : (head[5] > 1.0f ? 1.0f : head[5]);
		const float headPitch = asinf(headY);
		const float cy = cosf(headYaw), sy = sinf(headYaw);
		const float cp = cosf(headPitch), sp = sinf(headPitch);
		const float currentHead[9] = {
			cy, -sp * sy, cp * sy,
			0.0f, cp, sp,
			-sy, -sp * cy, cp * cy
		};

		// Forced/authored cameras must own this construction call outright. Keep
		// direct mode selected, but pass Metro's original basis through until its
		// native effector or player-performance owner releases. Re-anchor on the
		// first normal frame back so the handoff cannot replay head motion that
		// occurred during the scene.
		const bool scriptedOwner =
			!IsGameplayModeActive()
			|| InterlockedCompareExchange(&sPauseMenuExpected, 0, 0) != 0
			|| NativeAuthoredCameraOwnerActive()
			|| InterlockedCompareExchange(&sObservedAuthoredViewmodel, 0, 0) != 0
			|| ScriptedPlayerCameraOwnerActive();
		if (scriptedOwner)
			sUpstreamCameraYieldReleaseFrames = 8;
		else if (sUpstreamCameraYieldReleaseFrames > 0)
			--sUpstreamCameraYieldReleaseFrames;
		const bool yielding = scriptedOwner || sUpstreamCameraYieldReleaseFrames > 0;
		const LONG wasYielding = InterlockedExchange(
			&sUpstreamCameraYieldingToScript, yielding ? 1 : 0);
		if (yielding) {
			// Authored/menu ownership uses Metro's complete native source camera.
			// Reset direct-follow cancellation on entry: merely passing the source
			// basis through while cancellation stayed active caused the 100-160 degree
			// forced-view divergence seen in v13.
			if (!wasYielding)
				RestoreUpstreamLegacyCancellation();
			if (!wasYielding)
				LogInfo("VRPose upstream direct camera: native source ownership acquired "
					"for menu/authored camera\n");
			trampoline_UpstreamCameraBasis(manager, position, axisA, axisB, arg5, arg6);
			return;
		}
		if (wasYielding) {
			CaptureUpstreamLegacyCancellation();
			memcpy(sUpstreamHeadReferenceYawPitch, currentHead, sizeof(currentHead));
			LogInfo("VRPose upstream direct camera: native source ownership released; "
				"captured fresh interaction/body state and re-anchored HMD handoff\n");
		}
		// Match PatchMappedVRCameraData's additive, world-level convention.
		// A relative 3D rotation around the source camera retained its pitch
		// after scenes and turned physical yaw around a tilted axis. Keep the
		// source BODY yaw, but supply absolute HMD pitch plus the same authored
		// release easing used by rendering. Position and source memory stay intact.
		const float horizontal = sqrtf(axisA[0]*axisA[0] + axisA[2]*axisA[2]);
		if (horizontal < 1e-5f || !isfinite(horizontal)) {
			trampoline_UpstreamCameraBasis(manager, position, axisA, axisB, arg5, arg6);
			return;
		}
		const float referenceYaw = atan2f(sUpstreamHeadReferenceYawPitch[2],
			sUpstreamHeadReferenceYawPitch[8]);
		const float desiredYaw = atan2f(axisA[0], axisA[2]) + headYaw - referenceYaw;
		float desiredPitch = headPitch + GetScriptedPitchOffset();
		const float kMaxViewPitch = 1.48353f; // same 85-degree limit as rendering
		if (desiredPitch > kMaxViewPitch) desiredPitch = kMaxViewPitch;
		if (desiredPitch < -kMaxViewPitch) desiredPitch = -kMaxViewPitch;
		const float dyc = cosf(desiredYaw), dys = sinf(desiredYaw);
		const float dpc = cosf(desiredPitch), dps = sinf(desiredPitch);
		__declspec(align(16)) float directForward[4] = {
			dpc * dys, dps, dpc * dyc, 0.0f
		};
		__declspec(align(16)) float directUp[4] = {
			-dps * dys, dpc, -dps * dyc, 0.0f
		};

		// The rendered-camera correction cancels the camera-follow component and
		// reapplies the tracked pose. Publish the current direct component without
		// its old two-frame mouse prediction delay so yaw/pitch are not doubled.
		PublishUpstreamDirectCancellation(headYaw, headPitch);

		if (InterlockedExchange(&sUpstreamCameraBasisProbeHitLogged, 1) == 0)
			LogInfo("VRPose upstream camera basis probe: selected +0x1F3965 hit; "
				"applying direct HMD yaw/pitch with synthetic mouse follow suppressed\n");
		trampoline_UpstreamCameraBasis(manager, position, directForward, directUp, arg5, arg6);
	}

	// Native movement evaluates forward/sprint flags before converting its
	// local movement vector to world space. Rotate that result, not the stick,
	// so physical turning cannot turn forward intent into native backward input.
	typedef void (__fastcall *tDirectMovementHeading)(void *, void *, int, void *,
		float *, void *, float, void *, void *, void *, void *, void *, void *);
	static tDirectMovementHeading trampoline_DirectMovementHeading = NULL;
	static void *sDirectMovementReturnAddress = NULL;
	static void __fastcall Hooked_DirectMovementHeading(void *player, void *requested,
		int previous, void *accepted, float *movement, void *a6, float dt,
		void *basis, void *a9, void *a10, void *a11, void *a12, void *a13)
	{
		const bool selected = _ReturnAddress() == sDirectMovementReturnAddress;
		trampoline_DirectMovementHeading(player, requested, previous, accepted,
			movement, a6, dt, basis, a9, a10, a11, a12, a13);
		if (!selected || !movement || !IsGameplayModeActive() ||
			InterlockedCompareExchange(&sPauseMenuExpected, 0, 0) != 0 ||
			NativeAuthoredCameraOwnerActive() ||
			InterlockedCompareExchange(&sObservedAuthoredViewmodel, 0, 0) != 0 ||
			ScriptedPlayerCameraOwnerActive())
			return;
		if (isfinite(movement[0]) && isfinite(movement[2]))
			ApplyUpstreamDirectLocomotion(&movement[0], &movement[2]);
	}

	static void InstallDirectMovementHeading()
	{
		if (InterlockedCompareExchange(&sDirectMovementHeadingInstalled, 0, 0) != 0)
			return;
		BYTE *base = (BYTE *)GetModuleHandleA(NULL);
		if (!base) return;
		BYTE *function = base + 0x28D110;
		BYTE *call = base + 0x1F32C5;
		static const BYTE expected[] = { 0x48,0x8B,0xC4,0x44,0x89,0x40,0x18,
			0x55,0x53,0x57,0x48,0x8D,0x68,0xE1 };
		if (memcmp(function, expected, sizeof(expected)) != 0 || call[0] != 0xE8 ||
			call + 5 + *(INT32 *)(call + 1) != function) {
			InterlockedExchange(&sDirectMovementHeadingInstalled, -1);
			LogInfo("VRPose direct movement: signature rejected; retaining stick correction\n");
			return;
		}
		sDirectMovementReturnAddress = call + 5;
		SIZE_T hookId = 0;
		const DWORD error = cHookMgr.Hook(&hookId,
			(LPVOID *)&trampoline_DirectMovementHeading, function, Hooked_DirectMovementHeading);
		InterlockedExchange(&sDirectMovementHeadingInstalled,
			!error && trampoline_DirectMovementHeading ? 1 : -1);
		LogInfo("VRPose direct movement: %s at +0x28D110; selected local-player +0x1F32CA\n",
			!error && trampoline_DirectMovementHeading ? "installed" : "install failed");
	}

	static void InstallUpstreamCameraBasisProbe()
	{
		if (!kUpstreamCameraBasisProbeEnabled ||
			InterlockedCompareExchange(&sUpstreamCameraBasisProbeInstalled, 0, 0) != 0)
			return;
		BYTE *base = (BYTE *)GetModuleHandleA(NULL);
		if (!base)
			return;

		BYTE *function = base + kUpstreamCameraBasisRva;
		static const BYTE expected[] = {
			0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x74,
			0x24, 0x20, 0x57, 0x48, 0x81, 0xEC, 0xA0, 0x00, 0x00, 0x00
		};
		if (memcmp(function, expected, sizeof(expected)) != 0)
			return; // Metro decrypts .text after startup; retry next frame.

		BYTE *call = base + kUpstreamCameraBasisCallRva;
		const LONG displacement = *(const LONG *)(call + 1);
		if (call[0] != 0xE8 || call + 5 + displacement != function) {
			LogInfo("VRPose upstream camera basis probe: callsite mismatch at +0x1F3960; disabled\n");
			InterlockedExchange(&sUpstreamCameraBasisProbeInstalled, -1);
			return;
		}

		sUpstreamCameraBasisReturnAddress = call + 5;
		SIZE_T hookId = 0;
		const DWORD error = cHookMgr.Hook(&hookId,
			(LPVOID *)&trampoline_UpstreamCameraBasis, function,
			Hooked_UpstreamCameraBasis);
		if (error || !trampoline_UpstreamCameraBasis) {
			LogInfo("VRPose upstream camera basis probe: hook failed at +0x69B900 "
				"(err=0x%x); disabled\n", error);
			InterlockedExchange(&sUpstreamCameraBasisProbeInstalled, -1);
			return;
		}
		InterlockedExchange(&sUpstreamCameraBasisProbeInstalled, 1);
		LogInfo("VRPose upstream camera basis probe: installed inert hook at +0x69B900; "
			"both controller grips toggle direct HMD follow at selected +0x1F3965\n");
	}

	static bool ScriptedPlayerPerformanceActive()
	{
		// These exact player states were validated across Riga and the separate
		// Armory performances. They cover authored first-person animation that
		// does not retain a camera-manager descriptor for its full lifetime.
		static unsigned playerStateFrame = 0xFFFFFFFF;
		static bool playerControlOwned = false;
		if (G && playerStateFrame != G->frame_no) {
			playerStateFrame = G->frame_no;
			void *player = NULL;
			playerControlOwned = false;
			if (IsGameplayModeActive() && ResolveMetroPlayer(&player) && player) {
				__try {
					void *owner = *(void **)((BYTE *)player + 0xF78);
					const WORD sentinel = *(WORD *)((BYTE *)player + 0xF90);
					const WORD playerMode = *(WORD *)((BYTE *)player + 0x15D0);
					const WORD performanceMode = *(WORD *)((BYTE *)player + 0x15D2);
					const bool explicitOwner = owner != NULL && sentinel == 0xFFFF;
					const bool layeredPerformance = playerMode == 1
						&& performanceMode == 1;
					// +0x1850 is not a reliable ownership signal. Continue/save loads
					// and some door transitions can leave it at zero after normal
					// control has returned. Using that stale value preserved native
					// arms in head/body space after the gun had resumed 6DOF.
					playerControlOwned = explicitOwner || layeredPerformance;
				}
				__except (EXCEPTION_EXECUTE_HANDLER) {
					playerControlOwned = false;
				}
			}
		}
		return playerControlOwned;
	}

	static bool ScriptedPlayerCameraOwnerActive()
	{
		// The positive player-performance fields are shared by real forced-view
		// performances and ordinary control in some towns. Metro's own native
		// look gate supplies the missing ownership distinction: a performance is
		// a camera owner only while the engine itself refuses player look. This
		// keeps `(1,1)` available for authored hand routing without letting that
		// persistent town state suppress HMD or right-stick input.
		if (!ScriptedPlayerPerformanceActive())
			return false;
		void *player = NULL;
		return ResolveMetroPlayer(&player) && player
			&& !MetroNativeLookInputAllowed((BYTE *)player);
	}

	bool NativeScriptedViewmodelActive()
	{
		static bool latched = false;
		static unsigned quietFrames = 0;
		static unsigned lastFrame = 0xFFFFFFFF;
		const bool nativeEffectorActive = InterlockedCompareExchange(
			&sObservedAuthoredViewmodel, 0, 0) != 0;
		if (nativeEffectorActive) {
			latched = true;
			quietFrames = 0;
		} else if (latched && G && lastFrame != G->frame_no) {
			if (++quietFrames >= 3)
				latched = false;
		}
		if (G)
			lastFrame = G->frame_no;
		return latched;
	}

	bool ScriptedHandPerformanceActive()
	{
		static bool latched = false;
		static unsigned quietFrames = 0;
		static unsigned lastFrame = 0xFFFFFFFF;
		const bool nativeEffectorActive = NativeScriptedViewmodelActive();
		const bool playerPerformanceActive = ScriptedPlayerPerformanceActive();
		if (nativeEffectorActive || playerPerformanceActive) {
			latched = true;
			quietFrames = 0;
		} else if (latched && G && lastFrame != G->frame_no) {
			if (++quietFrames >= 3)
				latched = false;
		}
		if (G)
			lastFrame = G->frame_no;
		return latched;
	}

	LONG CALLBACK CameraWriteProbeVeh(EXCEPTION_POINTERS *ep)
	{
		CONTEXT *c = ep->ContextRecord;

		if (kADSWriterDiagnostic
		    && ep->ExceptionRecord->ExceptionCode == EXCEPTION_SINGLE_STEP
		    && InterlockedCompareExchange(&sADSWriterState, 1, 1) == 1
		    && (c->Dr6 & 0xFull)) {
			const DWORD64 hitBits = c->Dr6 & 0xFull;
			for (unsigned slot = 0; slot < ARRAYSIZE(sADSWriterAddresses); ++slot) {
				const DWORD64 bit = 1ull << slot;
				if (!(hitBits & bit))
					continue;
				if (InterlockedCompareExchange(&sADSWriterCaptured[slot], 1, 0) == 0) {
					sADSWriterContexts[slot] = *c;
					memset(sADSWriterStacks[slot], 0, sizeof(sADSWriterStacks[slot]));
					for (unsigned i = 0; i < ARRAYSIZE(sADSWriterStacks[slot]); ++i) {
						SIZE_T got = 0;
						ReadProcessMemory(GetCurrentProcess(),
							(const void *)(c->Rsp + i * sizeof(DWORD64)),
							&sADSWriterStacks[slot][i], sizeof(DWORD64), &got);
					}
					MemoryBarrier();
					InterlockedOr(&sADSWriterReportMask, (LONG)bit);
				}

				// Disarm only the slot that fired; the remaining candidates can
				// still identify their writers during the same ADS transition.
				switch (slot) {
				case 0: c->Dr0 = 0; break;
				case 1: c->Dr1 = 0; break;
				case 2: c->Dr2 = 0; break;
				case 3: c->Dr3 = 0; break;
				}
				c->Dr7 &= ~bit;
			}
			c->Dr6 = 0;
			c->ContextFlags |= CONTEXT_DEBUG_REGISTERS;
			return EXCEPTION_CONTINUE_EXECUTION;
		}

		if (kADSWriterDiagnostic
		    && ep->ExceptionRecord->ExceptionCode == kADSWriterArmExceptionCode) {
			c->Dr0 = sADSWriterAddresses[0];
			c->Dr1 = sADSWriterAddresses[1];
			c->Dr2 = sADSWriterAddresses[2];
			c->Dr3 = sADSWriterAddresses[3];
			c->Dr7 = kADSWriterDr7;
			c->Dr6 = 0;
			c->ContextFlags |= CONTEXT_DEBUG_REGISTERS;
			return EXCEPTION_CONTINUE_EXECUTION;
		}

		// DR1 belongs exclusively to the render-queue producer diagnostic. Check
		// it before the older DR0 fire probe, whose broad Dr6 test would otherwise
		// mistake a DR1 hit for a shot.
		if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_SINGLE_STEP
		    && (c->Dr6 & 0x2)) {
			c->Dr1 = 0;
			c->Dr6 &= ~0x2ull;
			c->Dr7 &= ~kDr7QueueMask;
			c->ContextFlags |= CONTEXT_DEBUG_REGISTERS;

			if (InterlockedCompareExchange(&sQueueEntryWatchState, 3, 2) == 2) {
				sQueueEntryWatchContext = *c;
				memset(sQueueEntryWatchStack, 0, sizeof(sQueueEntryWatchStack));
				for (unsigned i = 0; i < ARRAYSIZE(sQueueEntryWatchStack); ++i) {
					SIZE_T got = 0;
					ReadProcessMemory(GetCurrentProcess(),
						(const void *)(c->Rsp + i * sizeof(DWORD64)),
						&sQueueEntryWatchStack[i], sizeof(DWORD64), &got);
				}
				MemoryBarrier();
				InterlockedExchange(&sQueueEntryWatchReportPending, 1);
			}
			return EXCEPTION_CONTINUE_EXECUTION;
		}

		if (ep->ExceptionRecord->ExceptionCode == kQueueArmExceptionCode) {
			c->Dr1 = (DWORD64)sQueueEntryWatchAddress;
			c->Dr7 = (c->Dr7 & ~kDr7QueueMask) | kDr7QueueWrite4;
			c->Dr6 &= ~0x2ull;
			c->ContextFlags |= CONTEXT_DEBUG_REGISTERS;
			return EXCEPTION_CONTINUE_EXECUTION;
		}

		// Ownership probing shares DR0 with the fire probe. Handle it first so
		// the older broad DR0-DR3 test cannot consume this execution hit.
		if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_SINGLE_STEP
			&& InterlockedCompareExchange(&sOwnershipProbeArmed, 1, 1)
			&& (c->Dr6 & 0x1)) {
			const LONG ownershipStage = InterlockedCompareExchange(&sOwnershipProbeStage, 0, 0);
			c->Dr0 = 0;
			c->Dr6 &= ~0x1ull;
			c->Dr7 &= ~0x1ull;
			c->ContextFlags |= CONTEXT_DEBUG_REGISTERS;
			char readWhere[MAX_PATH + 64];
			DescribeCodeAddress(c->Rip, readWhere, sizeof(readWhere));
			const LONG slot = InterlockedCompareExchange(&sOwnershipProbeSlot, 0, 0);
			if (ownershipStage == 3) {
				LogInfo("VRPose ownership probe: command payload WRITE label=%s at %s\n",
					InterlockedCompareExchange(&sOwnershipProbeLabel, 0, 0) == 2 ? "SCRIPTED" : "ORDINARY",
					readWhere);
				LogInfo("VRPose ownership probe: payload setter args RCX=%016llX RDX=%016llX "
					"R8=%016llX R9=%016llX\n",
					(unsigned long long)c->Rcx, (unsigned long long)c->Rdx,
					(unsigned long long)c->R8, (unsigned long long)c->R9);
				InterlockedExchange(&sOwnershipProbeStage, 0);
				InterlockedExchange(&sOwnershipProbeArmed, 0);
				return EXCEPTION_CONTINUE_EXECUTION;
			}
			if (ownershipStage == 2) {
				LogInfo("VRPose ownership probe: source WRITE label=%s at %s\n",
					InterlockedCompareExchange(&sOwnershipProbeLabel, 0, 0) == 2 ? "SCRIPTED" : "ORDINARY",
					readWhere);
				BYTE commandBytes[32] = {};
				SIZE_T commandBytesRead = 0;
				DWORD commandOpcode = 0;
				SIZE_T opcodeBytesRead = 0;
				ReadProcessMemory(GetCurrentProcess(), (const void *)c->Rax,
					commandBytes, sizeof(commandBytes), &commandBytesRead);
				ReadProcessMemory(GetCurrentProcess(), (const void *)(c->Rax - sizeof(DWORD)),
					&commandOpcode, sizeof(commandOpcode), &opcodeBytesRead);
				LogInfo("VRPose ownership probe: writer regs RAX=%016llX RBX=%016llX "
					"RBP=%016llX RSP=%016llX opcode=0x%08X commandBytes=%02X %02X %02X %02X %02X %02X %02X %02X\n",
					(unsigned long long)c->Rax, (unsigned long long)c->Rbx,
					(unsigned long long)c->Rbp, (unsigned long long)c->Rsp,
					opcodeBytesRead == sizeof(DWORD) ? commandOpcode : 0,
					commandBytesRead >= 8 ? commandBytes[0] : 0,
					commandBytesRead >= 8 ? commandBytes[1] : 0,
					commandBytesRead >= 8 ? commandBytes[2] : 0,
					commandBytesRead >= 8 ? commandBytes[3] : 0,
					commandBytesRead >= 8 ? commandBytes[4] : 0,
					commandBytesRead >= 8 ? commandBytes[5] : 0,
					commandBytesRead >= 8 ? commandBytes[6] : 0,
					commandBytesRead >= 8 ? commandBytes[7] : 0);
				for (unsigned i = 0; i < 4; ++i) {
					DWORD64 ret = 0;
					SIZE_T got = 0;
					ReadProcessMemory(GetCurrentProcess(),
						(const void *)(c->Rsp + i * sizeof(DWORD64)), &ret, sizeof(ret), &got);
					if (got == sizeof(ret) && ret) {
						char stackWhere[MAX_PATH + 64];
						DescribeCodeAddress(ret, stackWhere, sizeof(stackWhere));
						LogInfo("VRPose ownership probe: source writer stack[%u]=%s\n", i, stackWhere);
					}
				}
				InterlockedExchange(&sOwnershipProbeStage, 0);
				InterlockedExchange(&sOwnershipProbeArmed, 0);
				return EXCEPTION_CONTINUE_EXECUTION;
			}
			if (ownershipStage == 4) {
				LogInfo("VRPose ownership probe: input dispatcher EXEC label=%s at %s "
					"RIP=%016llX RCX=%016llX RDX=%016llX R8=%016llX R9=%016llX\n",
					InterlockedCompareExchange(&sOwnershipProbeLabel, 0, 0) == 2 ? "SCRIPTED" : "ORDINARY",
					readWhere, (unsigned long long)c->Rip, (unsigned long long)c->Rcx,
					(unsigned long long)c->Rdx, (unsigned long long)c->R8,
					(unsigned long long)c->R9);
				InterlockedExchange(&sOwnershipProbeStage, 0);
				InterlockedExchange(&sOwnershipProbeArmed, 0);
				return EXCEPTION_CONTINUE_EXECUTION;
			}
			if (ownershipStage == 5) {
				LogInfo("VRPose control-state probe: camera command RECORDER "
					"label=%s at %s RIP=%016llX\n",
					InterlockedCompareExchange(&sOwnershipProbeLabel, 0, 0) == 2
						? "SCRIPTED" : "ORDINARY",
					readWhere, (unsigned long long)c->Rip);
				LogInfo("VRPose control-state probe: recorder regs "
					"RAX=%016llX RBX=%016llX RCX=%016llX RDX=%016llX "
					"RSI=%016llX RDI=%016llX RBP=%016llX RSP=%016llX\n",
					(unsigned long long)c->Rax, (unsigned long long)c->Rbx,
					(unsigned long long)c->Rcx, (unsigned long long)c->Rdx,
					(unsigned long long)c->Rsi, (unsigned long long)c->Rdi,
					(unsigned long long)c->Rbp, (unsigned long long)c->Rsp);
				for (unsigned i = 0; i < 8; ++i) {
					DWORD64 value = 0;
					SIZE_T got = 0;
					ReadProcessMemory(GetCurrentProcess(),
						(const void *)(c->Rsp + i * sizeof(DWORD64)),
						&value, sizeof(value), &got);
					if (got == sizeof(value) && value) {
						char stackWhere[MAX_PATH + 64];
						DescribeCodeAddress(value, stackWhere, sizeof(stackWhere));
						LogInfo("VRPose control-state probe: recorder stack[%u]=%s\n",
							i, stackWhere);
					}
				}
				InterlockedExchange(&sOwnershipProbeStage, 0);
				InterlockedExchange(&sOwnershipProbeArmed, 0);
				return EXCEPTION_CONTINUE_EXECUTION;
			}
			if (slot == 3)
				LogInfo("VRPose ownership probe: camera setter EXEC label=%s at %s\n",
					InterlockedCompareExchange(&sOwnershipProbeLabel, 0, 0) == 2 ? "SCRIPTED" : "ORDINARY",
					readWhere);
			else
				LogInfo("VRPose ownership probe: candidate +0x%lX READ at %s\n",
					slot == 1 ? 0xD0FB48L : 0xD0BA08L, readWhere);
			LogInfo("VRPose ownership probe: RIP=%016llX RAX=%016llX RBX=%016llX "
				"RCX=%016llX RDX=%016llX RSI=%016llX RDI=%016llX\n",
				(unsigned long long)c->Rip, (unsigned long long)c->Rax,
				(unsigned long long)c->Rbx, (unsigned long long)c->Rcx,
				(unsigned long long)c->Rdx, (unsigned long long)c->Rsi,
				(unsigned long long)c->Rdi);
			for (unsigned i = 0; i < 8; ++i) {
				DWORD64 ret = 0;
				SIZE_T got = 0;
				ReadProcessMemory(GetCurrentProcess(),
					(const void *)(c->Rsp + i * sizeof(DWORD64)), &ret, sizeof(ret), &got);
				if (got == sizeof(ret) && ret) {
					char stackWhere[MAX_PATH + 64];
					DescribeCodeAddress(ret, stackWhere, sizeof(stackWhere));
					LogInfo("VRPose ownership probe: stack[%u]=%s\n", i, stackWhere);
				}
			}
			float sourceMatrix[16] = {};
			SIZE_T matrixBytes = 0;
			if (ReadProcessMemory(GetCurrentProcess(), (const void *)c->Rdx,
				sourceMatrix, sizeof(sourceMatrix), &matrixBytes)
				&& matrixBytes == sizeof(sourceMatrix)) {
				LogInfo("VRPose ownership probe: source=%016llX rows="
					"[%.5f %.5f %.5f %.5f] [%.5f %.5f %.5f %.5f] "
					"[%.5f %.5f %.5f %.5f] [%.5f %.5f %.5f %.5f]\n",
					(unsigned long long)c->Rdx,
					sourceMatrix[0], sourceMatrix[1], sourceMatrix[2], sourceMatrix[3],
					sourceMatrix[4], sourceMatrix[5], sourceMatrix[6], sourceMatrix[7],
					sourceMatrix[8], sourceMatrix[9], sourceMatrix[10], sourceMatrix[11],
					sourceMatrix[12], sourceMatrix[13], sourceMatrix[14], sourceMatrix[15]);
			}
				// The interpreter has just copied the payload into its local source
				// matrix. Keep the probe alive and watch the payload buffer itself
				// for the upstream writer.
				if (slot == 3) {
					sProbeWatchAddress = (void *)c->Rax;
					sProbeDr7 = kDr7Write4;
					InterlockedExchange(&sOwnershipProbeStage, 3);
				c->Dr0 = (DWORD64)sProbeWatchAddress;
				c->Dr7 = kDr7Write4;
				c->Dr6 = 0;
				c->ContextFlags |= CONTEXT_DEBUG_REGISTERS;
				return EXCEPTION_CONTINUE_EXECUTION;
			}
			InterlockedExchange(&sOwnershipProbeArmed, 0);
			return EXCEPTION_CONTINUE_EXECUTION;
		}

		// Fire-call probe: an execution breakpoint, so the object is still in
		// RDX and nothing has run yet.
		if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_SINGLE_STEP
		    && (c->Dr6 & 0xFull)
		    && InterlockedCompareExchange(&sFireProbeArmed, 1, 1) == 1) {
			c->Dr6 = 0;

			// Report the first one, then steer every shot after it - unless
			// the inline hook is live, which catches every call on every
			// thread and would otherwise redirect the same shot twice.
			if (InterlockedExchange(&sFireProbeReported, 1) == 0)
				ReportFireCall(c->Rdx);
			else if (kFireRedirectTest
			         && InterlockedCompareExchange(&sInlineFireHookActive, 0, 0) == 0)
				RedirectShot(c->Rdx);

			if (kFireRedirectTest) {
				// Stay armed for every subsequent shot. An execution
				// breakpoint would re-trigger on the very instruction we are
				// resuming to, so set the Resume Flag - it suppresses the
				// breakpoint for exactly one instruction, which is what it
				// exists for.
				c->EFlags |= 0x10000;
			} else {
				c->Dr0 = 0;
				c->Dr7 = 0;
				InterlockedExchange(&sFireProbeArmed, 0);
				InterlockedExchange(&sFireProbeState, 3);
			}
			c->ContextFlags |= CONTEXT_DEBUG_REGISTERS;
			return EXCEPTION_CONTINUE_EXECUTION;
		}

		// Coverage sampling. The guard clears itself on the faulting access,
		// so each page costs exactly one fault per sample window. Kept first
		// and deliberately trivial - this runs on every thread the game has.
		if (ep->ExceptionRecord->ExceptionCode == kGuardPageViolation) {
			if (ep->ExceptionRecord->NumberParameters >= 2)
				CoverageNoteAddress((DWORD_PTR)ep->ExceptionRecord->ExceptionInformation[1]);
			return EXCEPTION_CONTINUE_EXECUTION;
		}

		// Arming request. Debug registers are per-thread and cannot be set
		// reliably on a running thread from itself - but the kernel restores
		// the whole context, debug registers included, when a handler returns
		// EXCEPTION_CONTINUE_EXECUTION. So programming them here arms the
		// very thread that asked, synchronously, before it runs another
		// instruction.
		//
		// The first attempt armed from a worker thread that had to suspend
		// the render thread, and lost the race every time: the engine writes
		// the matrix microseconds after Map returns, long before a thread
		// switch could complete. Hence "armed ... waiting" and no hit.
		if (ep->ExceptionRecord->ExceptionCode == kArmExceptionCode) {
			if (InterlockedCompareExchange(&sShotPrepWatchPending, 0, 0) == 1) {
				c->Dr3 = (DWORD64)sProbeWatchAddress;
				c->Dr7 = (c->Dr7 & ~kDr7ShotPrepMask) | kDr7ShotPrepExec;
				c->Dr6 &= ~0x8ull;
				c->ContextFlags |= CONTEXT_DEBUG_REGISTERS;
				InterlockedExchange(&sShotPrepWatchPending, 0);
				InterlockedExchange(&sShotPrepWatchMode, 1);
				return EXCEPTION_CONTINUE_EXECUTION;
			}

			if (InterlockedCompareExchange(&sShotWriteWatchPending, 0, 0) == 1) {
				c->Dr2 = (DWORD64)sProbeWatchAddress;
				c->Dr7 = (c->Dr7 & ~kDr7ShotWriteMask) | kDr7ShotWrite4;
				c->Dr6 &= ~0x4ull;
				c->ContextFlags |= CONTEXT_DEBUG_REGISTERS;
				InterlockedExchange(&sShotWriteWatchPending, 0);
				InterlockedExchange(&sShotWriteWatchActive, 1);
				return EXCEPTION_CONTINUE_EXECUTION;
			}

			c->Dr0 = (DWORD64)sProbeWatchAddress;
			// L0 enabled; bit 10 is reserved and must be set; LE/GE
			// recommended; RW0 = 01 (break on write); LEN0 = 11 (4 bytes).
			c->Dr7 = sProbeDr7;
			c->Dr6 = 0;

			// Without this the debug registers are silently discarded. The
			// context a vectored handler receives describes only what the
			// trap captured - typically CONTEXT_FULL, which does NOT include
			// the Dr fields - and the kernel restores exactly what
			// ContextFlags advertises. Asking for them explicitly is what
			// makes the write above take effect, and its absence is why the
			// first synchronous attempt armed "successfully" and never fired.
			c->ContextFlags |= CONTEXT_DEBUG_REGISTERS;

			InterlockedExchange(&sProbeState, 2);
			return EXCEPTION_CONTINUE_EXECUTION;
		}

		if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP)
			return EXCEPTION_CONTINUE_SEARCH;

		if ((c->Dr6 & 0x8) && InterlockedCompareExchange(&sShotPrepWatchMode, 0, 0) == 1) {
			DWORD marker = 0;
			SIZE_T got = 0;
			ReadProcessMemory(GetCurrentProcess(), (const void *)(c->Rdx + 0x130),
				&marker, sizeof(marker), &got);
			if (got == sizeof(marker) && marker != 0) {
				sShotPrepWatchObject = c->Rdx;
				sProbeWatchAddress = (void *)(c->Rdx + 0x150);
				c->Dr3 = (DWORD64)sProbeWatchAddress;
				c->Dr7 = (c->Dr7 & ~kDr7ShotPrepMask) | kDr7ShotPrepWrite4;
				c->Dr6 &= ~0x8ull;
				c->ContextFlags |= CONTEXT_DEBUG_REGISTERS;
				InterlockedExchange(&sShotPrepWatchMode, 2);
				LogInfo("VRPose fire: shot-call object=%016llX, now watching +0x150 at %p\n",
					(unsigned long long)c->Rdx, sProbeWatchAddress);
				return EXCEPTION_CONTINUE_EXECUTION;
			}
			c->Dr6 &= ~0x8ull;
			c->EFlags |= 0x10000;
			c->ContextFlags |= CONTEXT_DEBUG_REGISTERS;
			return EXCEPTION_CONTINUE_EXECUTION;
		}

		if ((c->Dr6 & 0x8) && InterlockedExchange(&sShotPrepWatchMode, 0) == 2) {
			c->Dr3 = 0;
			c->Dr6 &= ~0x8ull;
			c->Dr7 &= ~kDr7ShotPrepMask;
			c->ContextFlags |= CONTEXT_DEBUG_REGISTERS;
			char where[MAX_PATH + 64];
			DescribeCodeAddress(c->Rip, where, sizeof(where));
			LogInfo("VRPose fire: SHOT TARGET WRITE caught at %s object=%016llX "
				"field=+0x150 address=%p\n", where,
				(unsigned long long)sShotPrepWatchObject, sProbeWatchAddress);
			return EXCEPTION_CONTINUE_EXECUTION;
		}

		if ((c->Dr6 & 0x4) && InterlockedExchange(&sShotWriteWatchActive, 0) == 1) {
			c->Dr2 = 0;
			c->Dr6 &= ~0x4ull;
			c->Dr7 &= ~kDr7ShotWriteMask;
			c->ContextFlags |= CONTEXT_DEBUG_REGISTERS;
			char shotWhere[MAX_PATH + 64];
			DescribeCodeAddress(c->Rip, shotWhere, sizeof(shotWhere));
			LogInfo("VRPose fire: SHOT TARGET WRITE caught at %s object=%016llX "
				"field=+0x150 address=%p\n", shotWhere,
				(unsigned long long)sShotWriteWatchObject, sProbeWatchAddress);
			return EXCEPTION_CONTINUE_EXECUTION;
		}

		if (!(c->Dr6 & 0xFull))
			return EXCEPTION_CONTINUE_SEARCH;

		// Self-test hit: disarm quietly and record that the mechanism works.
		if (sProbeSelfTesting) {
			c->Dr0 = 0;
			c->Dr6 = 0;
			c->Dr7 = 0;
			c->ContextFlags |= CONTEXT_DEBUG_REGISTERS;
			InterlockedExchange(&sProbeSelfTestHit, 1);
			return EXCEPTION_CONTINUE_EXECUTION;
		}

		// Disarm first, so nothing below can retrigger us. A read-watch hit is
		// the direct ownership diagnostic: the RIP is the engine instruction
		// that consumes the candidate state.
		if (InterlockedCompareExchange(&sOwnershipProbeArmed, 1, 1)
			&& (c->Dr6 & 0x1)) {
			c->Dr0 = 0;
			c->Dr6 &= ~0x1ull;
			c->Dr7 &= ~0x1ull;
			c->ContextFlags |= CONTEXT_DEBUG_REGISTERS;
			char readWhere[MAX_PATH + 64];
			DescribeCodeAddress(c->Rip, readWhere, sizeof(readWhere));
			const LONG slot = InterlockedCompareExchange(&sOwnershipProbeSlot, 0, 0);
			if (slot == 3)
				LogInfo("VRPose ownership probe: camera setter EXEC label=%s at %s\n",
					InterlockedCompareExchange(&sOwnershipProbeLabel, 0, 0) == 2 ? "SCRIPTED" : "ORDINARY",
					readWhere);
			else
				LogInfo("VRPose ownership probe: candidate +0x%lX READ at %s\n",
					slot == 1 ? 0xD0FB48L : 0xD0BA08L, readWhere);
			LogInfo("VRPose ownership probe: RIP=%016llX RAX=%016llX RBX=%016llX "
				"RCX=%016llX RDX=%016llX RSI=%016llX RDI=%016llX\n",
				(unsigned long long)c->Rip, (unsigned long long)c->Rax,
				(unsigned long long)c->Rbx, (unsigned long long)c->Rcx,
				(unsigned long long)c->Rdx, (unsigned long long)c->Rsi,
				(unsigned long long)c->Rdi);
			InterlockedExchange(&sOwnershipProbeArmed, 0);
			return EXCEPTION_CONTINUE_EXECUTION;
		}
		c->Dr0 = 0;
		c->Dr6 = 0;
		c->Dr7 = 0;
		c->ContextFlags |= CONTEXT_DEBUG_REGISTERS;

		if (InterlockedExchange(&sShotWriteWatchActive, 0) == 1) {
			char shotWhere[MAX_PATH + 64];
			DescribeCodeAddress(c->Rip, shotWhere, sizeof(shotWhere));
			LogInfo("VRPose fire: SHOT TARGET WRITE caught at %s object=%016llX "
				"field=+0x150 address=%p\n", shotWhere,
				(unsigned long long)sShotWriteWatchObject, sProbeWatchAddress);
			return EXCEPTION_CONTINUE_EXECUTION;
		}

		char where[MAX_PATH + 64];
		DescribeCodeAddress(c->Rip, where, sizeof(where));

		// Stage 4 phase A is an execution breakpoint whose only job is to
		// capture the command-stream address the interpreter is reading
		// from. It reports nothing; phase B does the reporting.
		if (InterlockedCompareExchange(&sStage4State, 1, 1) == 1) {
			sCmdStreamMatrixAddr = (void *)c->Rax;
			InterlockedExchange(&sStage4State, 2);
			return EXCEPTION_CONTINUE_EXECUTION;
		}

		// Armed on every thread, so several can trip nearly at once. Each
		// clears its own registers above; only the first one reports.
		if (InterlockedExchange(&sProbeReported, 1) != 0)
			return EXCEPTION_CONTINUE_EXECUTION;

		const bool mainCameraSource =
			InterlockedCompareExchange(&sMainCameraSourceWatchState, 2, 1) == 1;
		const bool stage3 = InterlockedCompareExchange(&sStage3Armed, 1, 1) == 1;
		const bool stage2 = !stage3 && InterlockedCompareExchange(&sStage2Armed, 1, 1) == 1;
		if (!mainCameraSource)
			LogOverlay(LOG_INFO, "VRPose probe: caught %s\n",
				stage3 ? "the camera UPDATE" : (stage2 ? "the camera code" : "the camera write"));
		LogInfo("========================================================\n");
		LogInfo("VRPose probe: %s\n", mainCameraSource
			? "UPSTREAM SOURCE WRITE CAUGHT - this wrote metro.exe+0xD07730"
			: (stage3 ? "CAMERA UPDATE CAUGHT - this WROTE the camera matrix itself"
				: (stage2 ? "CAMERA CODE CAUGHT - this WROTE the view matrix"
				          : "CAMERA MATRIX UPLOAD CAUGHT")));
		LogInfo("VRPose probe:   RIP = %s\n", where);
		LogInfo("VRPose probe:   (data breakpoints trap AFTER the store, so the\n");
		LogInfo("VRPose probe:    storing instruction ends just before this RVA)\n");
		LogInfo("VRPose probe:   watched address = %p\n", sProbeWatchAddress);
		LogInfo("VRPose probe: registers - one of these should point at the camera:\n");
		LogInfo("VRPose probe:   RAX=%016llX RBX=%016llX RCX=%016llX RDX=%016llX\n",
			c->Rax, c->Rbx, c->Rcx, c->Rdx);
		LogInfo("VRPose probe:   RSI=%016llX RDI=%016llX RBP=%016llX RSP=%016llX\n",
			c->Rsi, c->Rdi, c->Rbp, c->Rsp);
		LogInfo("VRPose probe:   R8 =%016llX R9 =%016llX R10=%016llX R11=%016llX\n",
			c->R8, c->R9, c->R10, c->R11);
		LogInfo("VRPose probe:   R12=%016llX R13=%016llX R14=%016llX R15=%016llX\n",
			c->R12, c->R13, c->R14, c->R15);

		// Return addresses off the stack give the call chain into the camera
		// code, which is usually more informative than the leaf itself.
		LogInfo("VRPose probe: plausible return addresses on the stack:\n");
		DWORD64 *sp = (DWORD64 *)c->Rsp;
		int shown = 0;
		for (int i = 0; i < 64 && shown < 12; i++) {
			DWORD64 v = 0;
			SIZE_T got = 0;
			if (!ReadProcessMemory(GetCurrentProcess(), sp + i, &v, sizeof(v), &got) || got != sizeof(v))
				continue;
			HMODULE mod = NULL;
			if (v > 0x10000 && GetModuleHandleExA(
				GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				(LPCSTR)v, &mod) && mod) {
				char desc[MAX_PATH + 64];
				DescribeCodeAddress(v, desc, sizeof(desc));
				LogInfo("VRPose probe:   [rsp+%03X] %s\n", i * 8, desc);
				shown++;
			}
		}
		LogInfo("========================================================\n");

		if (stage3) {
			InterlockedExchange(&sStage3State, 3);
			// Re-dump: stage 1's image caught .data before the camera
			// existed (every matrix was identity), so the object layout
			// was only confirmable structurally. This one has live values.
			InterlockedExchange(&sWantModuleDump, 1);
		} else if (stage2) {
			InterlockedExchange(&sStage2State, 3);
		} else {
			InterlockedExchange(&sWantModuleDump, 1);
		}
		InterlockedExchange(&sProbeState, 3);
		return EXCEPTION_CONTINUE_EXECUTION;
	}

	// Writes the game's decrypted code image to disk for offline analysis.
	//
	// metro.exe ships with Steam's DRM stub (a `.bind` section, with .text
	// encrypted on disk), so disassembling the file yields ciphertext - the
	// first attempt decoded our target as a nonsensical `movabs al, [imm64]`
	// followed by garbage. The decrypted code only ever exists in this
	// process's memory, and we are already inside it.
	//
	// Dumped as a flat image at virtual addresses, so a file offset IS an
	// RVA - exactly what the probe reports and what the disassembler wants.
	// Read-only with respect to the game; it only copies bytes out.
	void DumpGameModuleIfRequested()
	{
		if (InterlockedExchange(&sWantModuleDump, 0) == 0)
			return;

		HMODULE base = GetModuleHandleA(NULL);
		if (!base) {
			LogInfo("VRPose dump: GetModuleHandle failed\n");
			return;
		}

		const BYTE *img = (const BYTE *)base;
		const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)img;
		if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
			LogInfo("VRPose dump: bad DOS header\n");
			return;
		}
		const IMAGE_NT_HEADERS64 *nt = (const IMAGE_NT_HEADERS64 *)(img + dos->e_lfanew);
		if (nt->Signature != IMAGE_NT_SIGNATURE) {
			LogInfo("VRPose dump: bad NT header\n");
			return;
		}
		const DWORD imageSize = nt->OptionalHeader.SizeOfImage;

		char dumpName[64];
		_snprintf_s(dumpName, sizeof(dumpName), _TRUNCATE, "metro_dumped%d.bin", sDumpIndex++);
		HANDLE f = CreateFileA(dumpName, GENERIC_WRITE, 0, NULL,
			CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
		if (f == INVALID_HANDLE_VALUE) {
			LogInfo("VRPose dump: CreateFile failed (%u)\n", GetLastError());
			return;
		}

		// Page by page, because parts of an image are legitimately
		// unreadable (guard pages, uncommitted padding) and touching them
		// would fault. Those become zeroes rather than a crash.
		const DWORD kPage = 0x1000;
		BYTE zero[kPage];
		memset(zero, 0, sizeof(zero));
		DWORD written = 0, readable = 0, skipped = 0;
		for (DWORD off = 0; off < imageSize; off += kPage) {
			DWORD chunk = min(kPage, imageSize - off);
			BYTE buf[kPage];
			SIZE_T got = 0;
			DWORD wrote = 0;
			if (ReadProcessMemory(GetCurrentProcess(), img + off, buf, chunk, &got)
			    && got == chunk) {
				WriteFile(f, buf, chunk, &wrote, NULL);
				readable++;
			} else {
				WriteFile(f, zero, chunk, &wrote, NULL);
				skipped++;
			}
			written += wrote;
		}
		CloseHandle(f);

		LogInfo("VRPose dump: wrote %s - %u bytes, base %p, "
			"%u pages read, %u unreadable (zero-filled)\n",
			dumpName, written, base, readable, skipped);
		LogInfo("VRPose dump: file offset == RVA, so probe addresses map directly\n");
	}

	// Stage 2: catch whoever COMPUTES the view matrix.
	//
	// Stage 1 trapped the engine's constant-buffer flush loop, which is one
	// step downstream - a generic memcpy that uploads 14 dirty constant
	// buffers. Its disassembly gave us something better than its own code
	// though: it reads each buffer's contents from a CPU-side copy whose
	// pointer sits in a static table.
	//
	//     lea  rsi, [rip+0x53d12d]   ; table of ID3D11Buffer*
	//     lea  r14, [rip+0x53cfde]   ; table of CPU-side source pointers
	//     mov  rbx, [r14-8]          ; the source for this buffer
	//
	// The trapped registers place us at entry 1 of both tables, which is
	// constant buffer slot b1 - cb_main_matrices1, the camera buffer. So the
	// engine's own copy of m_V lives behind the static pointer below.
	//
	// Breaking on a write to THAT is the camera code proper, and whatever
	// object it reads the rotation from is the authoritative player
	// orientation every value scan failed to find.
	static const DWORD_PTR kMatricesSourcePtrRva = 0xD23A98;

	// Arms the watchpoint on EVERY thread in the process, not just this one.
	//
	// Debug registers are per-thread state. Stage 2 armed only the thread
	// that calls Present and never fired - and 4A Engine is heavily
	// multithreaded, so the camera almost certainly updates on a game or
	// simulation thread rather than the render thread. Arming one thread and
	// concluding "the engine doesn't write it" would have been exactly the
	// wrong inference.
	//
	// Other threads are programmed the ordinary way (suspend, set context,
	// resume); this thread cannot be, so it still goes through the exception
	// trick. Deliberately no logging while any thread is suspended - the
	// logger takes locks, and a suspended thread holding one would deadlock
	// the process instantly.
	int ArmWatchpointOnAllThreads(void *addr)
	{
		const DWORD myPid = GetCurrentProcessId();
		const DWORD myTid = GetCurrentThreadId();

		HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
		if (snap == INVALID_HANDLE_VALUE)
			return 0;

		int armed = 0;
		THREADENTRY32 te;
		te.dwSize = sizeof(te);
		if (Thread32First(snap, &te)) {
			do {
				if (te.dwSize < FIELD_OFFSET(THREADENTRY32, th32OwnerProcessID) + sizeof(DWORD))
					continue;
				if (te.th32OwnerProcessID != myPid || te.th32ThreadID == myTid)
					continue;

				HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT,
					FALSE, te.th32ThreadID);
				if (!th)
					continue;

				// CHECK BEFORE SUSPENDING.
				//
				// This runs periodically now (every few seconds, to catch
				// threads the engine creates after the first pass - see
				// ArmFireProbe), and unconditionally suspending every thread
				// in the process on every pass caused a visible hitch each
				// time it ran: ~112 Suspend/Get/Set/Resume round trips, which
				// pauses the game's own render, simulation and audio threads
				// for the walk's duration.
				//
				// A thread that already carries the breakpoint needs nothing
				// done to it, and that is the common case - new threads are
				// the exception. Reading the context without suspending first
				// is only a fast-path filter, not load-bearing for
				// correctness: at worst it misses an already-armed thread and
				// pays for one redundant suspend, which is what the full path
				// below already cost on every thread, every time.
				CONTEXT peek;
				memset(&peek, 0, sizeof(peek));
				peek.ContextFlags = CONTEXT_DEBUG_REGISTERS;
				const bool alreadyArmed = GetThreadContext(th, &peek)
					&& peek.Dr0 == (DWORD64)addr && (peek.Dr7 & 1);

				if (alreadyArmed) {
					armed++;
				} else if (SuspendThread(th) != (DWORD)-1) {
					CONTEXT ctx;
					memset(&ctx, 0, sizeof(ctx));
					ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
					if (GetThreadContext(th, &ctx)) {
						ctx.Dr0 = (DWORD64)addr;
						ctx.Dr7 = sProbeDr7;
						ctx.Dr6 = 0;
						ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
						if (SetThreadContext(th, &ctx))
							armed++;
					}
					ResumeThread(th);
				}
				CloseHandle(th);
			} while (Thread32Next(snap, &te));
		}
		CloseHandle(snap);
		return armed;
	}

	void GetLeftHandOffsetAdjust(float out[3])
	{
		LoadLeftHandAdjust();
		out[0] = sLeftHandAdjust[0];
		out[1] = sLeftHandAdjust[1];
		out[2] = sLeftHandAdjust[2];
	}

	void GetWatchOffsetAdjust(float out[3])
	{
		if (!out) return;
		LoadWatchLegacyAdjust();
		out[0] = sWatchLegacyAdjust[0];
		out[1] = sWatchLegacyAdjust[1];
		out[2] = sWatchLegacyAdjust[2];
	}

	void GetRightHandOffsetAdjust(float out[3])
	{
		if (!out) return;
		LoadRightHandAdjust();
		memcpy(out, sRightHandAdjust, sizeof(sRightHandAdjust));
	}

	void GetPrologueLeftHandOffsetAdjust(float out[3])
	{
		if (!out) return;
		LoadPrologueLeftHandAdjust();
		memcpy(out, sPrologueLeftHandAdjust, sizeof(sPrologueLeftHandAdjust));
	}

	void GetWatchAssemblyCalibration(float position[3], float rotation[3])
	{
		LoadWatchAdjust();
		if (position) {
			position[0] = sWatchAdjust[0];
			position[1] = sWatchAdjust[1];
			position[2] = sWatchAdjust[2];
		}
		if (rotation) {
			rotation[0] = sWatchRotationAdjust[0];
			rotation[1] = sWatchRotationAdjust[1];
			rotation[2] = sWatchRotationAdjust[2];
		}
	}

	void GetPrologueWatchAssemblyCalibration(float position[3], float rotation[3])
	{
		LoadPrologueWatchAdjust();
		if (position)
			memcpy(position, sPrologueWatchAdjust, sizeof(sPrologueWatchAdjust));
		if (rotation)
			memcpy(rotation, sPrologueWatchRotationAdjust,
				sizeof(sPrologueWatchRotationAdjust));
	}

	void GetWatchTimeCalibration(float position[3], float rotation[3], float *scale)
	{
		LoadWatchTimeAdjust();
		if (position)
			memcpy(position, sWatchTimeAdjustPosition,
				sizeof(sWatchTimeAdjustPosition));
		if (rotation)
			memcpy(rotation, sWatchTimeAdjustRotation,
				sizeof(sWatchTimeAdjustRotation));
		if (scale)
			*scale = sWatchTimeAdjustScale;
	}

	void GetLighterCalibration(float position[3], float rotation[3],
		unsigned lighterVariant)
	{
		float *activePosition = NULL, *activeRotation = NULL;
		GetLighterVariantAdjust(lighterVariant, activePosition, activeRotation);
		if (position)
			memcpy(position, activePosition, 3 * sizeof(float));
		if (rotation)
			memcpy(rotation, activeRotation, 3 * sizeof(float));
	}

	void GetChargerCalibration(float position[3], float rotation[3])
	{
		LoadChargerAdjust();
		if (position)
			memcpy(position, sChargerAdjust, sizeof(sChargerAdjust));
		if (rotation)
			memcpy(rotation, sChargerRotationAdjust,
				sizeof(sChargerRotationAdjust));
	}

	void GetChargerRightHandCalibration(float position[3], float rotation[3])
	{
		LoadChargerRightHandAdjust();
		if (position)
			memcpy(position, sChargerRightHandAdjust,
				sizeof(sChargerRightHandAdjust));
		if (rotation)
			memcpy(rotation, sChargerRightHandRotationAdjust,
				sizeof(sChargerRightHandRotationAdjust));
	}

	void GetLighterFlameCalibration(float groupPosition[3],
		float secondaryPosition[3], float *scale, unsigned lighterVariant)
	{
		float *activeGroup = NULL, *activeSecondary = NULL;
		float *activeScale = NULL;
		GetLighterVariantFlameAdjust(lighterVariant, activeGroup,
			activeSecondary, activeScale);
		if (groupPosition)
			memcpy(groupPosition, activeGroup, 3 * sizeof(float));
		if (secondaryPosition)
			memcpy(secondaryPosition, activeSecondary, 3 * sizeof(float));
		if (scale)
			*scale = *activeScale;
	}

	void NotifyJournalObjectiveDraw(unsigned frameNo)
	{
		InterlockedExchange(&sJournalObjectiveFrame, (LONG)frameNo);
	}

	void GetJournalCalibration(float position[3], float rotation[3], float *scale)
	{
		LoadJournalAdjust();
		if (position) {
			position[0] = sJournalAdjustPosition[0];
			position[1] = sJournalAdjustPosition[1];
			position[2] = sJournalAdjustPosition[2];
		}
		if (rotation) {
			rotation[0] = sJournalAdjustRotation[0];
			rotation[1] = sJournalAdjustRotation[1];
			rotation[2] = sJournalAdjustRotation[2];
		}
		if (scale)
			*scale = sJournalAdjustScale;
	}

	static int ArmShotWriteWatchOnAllThreads(void *addr)
	{
		const DWORD myPid = GetCurrentProcessId();
		const DWORD myTid = GetCurrentThreadId();
		HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
		if (snap == INVALID_HANDLE_VALUE)
			return 0;

		int armed = 0;
		THREADENTRY32 te = {};
		te.dwSize = sizeof(te);
		if (Thread32First(snap, &te)) {
			do {
				if (te.th32OwnerProcessID != myPid || te.th32ThreadID == myTid)
					continue;
				HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
					THREAD_SET_CONTEXT, FALSE, te.th32ThreadID);
				if (!th)
					continue;

				CONTEXT peek = {};
				peek.ContextFlags = CONTEXT_DEBUG_REGISTERS;
				const bool already = GetThreadContext(th, &peek)
					&& peek.Dr2 == (DWORD64)addr && (peek.Dr7 & (1ull << 2));
				if (already) {
					armed++;
				} else if (SuspendThread(th) != (DWORD)-1) {
					CONTEXT ctx = {};
					ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
					if (GetThreadContext(th, &ctx)) {
						ctx.Dr2 = (DWORD64)addr;
						ctx.Dr7 = (ctx.Dr7 & ~kDr7ShotWriteMask) | kDr7ShotWrite4;
						ctx.Dr6 &= ~0x4ull;
						ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
						if (SetThreadContext(th, &ctx))
							armed++;
					}
					ResumeThread(th);
				}
				CloseHandle(th);
			} while (Thread32Next(snap, &te));
		}
		CloseHandle(snap);
		return armed;
	}

	static int ArmShotPrepExecOnAllThreads(void *addr)
	{
		const DWORD pid = GetCurrentProcessId();
		const DWORD tid = GetCurrentThreadId();
		HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
		if (snap == INVALID_HANDLE_VALUE)
			return 0;
		int armed = 0;
		THREADENTRY32 te = {};
		te.dwSize = sizeof(te);
		if (Thread32First(snap, &te)) {
			do {
				if (te.th32OwnerProcessID != pid || te.th32ThreadID == tid)
					continue;
				HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
					THREAD_SET_CONTEXT, FALSE, te.th32ThreadID);
				if (!th)
					continue;
				if (SuspendThread(th) != (DWORD)-1) {
					CONTEXT ctx = {};
					ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
					if (GetThreadContext(th, &ctx)) {
						ctx.Dr3 = (DWORD64)addr;
						ctx.Dr7 = (ctx.Dr7 & ~kDr7ShotPrepMask) | kDr7ShotPrepExec;
						ctx.Dr6 &= ~0x8ull;
						ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
						if (SetThreadContext(th, &ctx))
							armed++;
					}
					ResumeThread(th);
				}
				CloseHandle(th);
			} while (Thread32Next(snap, &te));
		}
		CloseHandle(snap);
		return armed;
	}

	static int ArmQueueEntryWatchOnAllThreads(void *addr)
	{
		const DWORD myPid = GetCurrentProcessId();
		const DWORD myTid = GetCurrentThreadId();
		HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
		if (snap == INVALID_HANDLE_VALUE)
			return 0;

		int armed = 0;
		THREADENTRY32 te = {};
		te.dwSize = sizeof(te);
		if (Thread32First(snap, &te)) {
			do {
				if (te.th32OwnerProcessID != myPid || te.th32ThreadID == myTid)
					continue;
				HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
					THREAD_SET_CONTEXT, FALSE, te.th32ThreadID);
				if (!th)
					continue;
				if (SuspendThread(th) != (DWORD)-1) {
					CONTEXT ctx = {};
					ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
					if (GetThreadContext(th, &ctx)) {
						ctx.Dr1 = (DWORD64)addr;
						ctx.Dr7 = (ctx.Dr7 & ~kDr7QueueMask) | kDr7QueueWrite4;
						ctx.Dr6 &= ~0x2ull;
						ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
						if (SetThreadContext(th, &ctx))
							armed++;
					}
					ResumeThread(th);
				}
				CloseHandle(th);
			} while (Thread32Next(snap, &te));
		}
		CloseHandle(snap);
		return armed;
	}

	static void ProcessQueueEntryProducerWatch()
	{
		if (InterlockedExchange(&sQueueEntryWatchReportPending, 0) != 0
		    && InterlockedExchange(&sQueueEntryWatchReported, 1) == 0) {
			CONTEXT c = sQueueEntryWatchContext;
			char where[MAX_PATH + 64];
			DescribeCodeAddress(c.Rip, where, sizeof(where));
			LogInfo("========================================================\n");
			LogInfo("VRPose queue watch: RENDER-QUEUE SLOT WRITE CAUGHT\n");
			LogInfo("VRPose queue watch: watched=%p RIP(after store)=%s\n",
				(PVOID)sQueueEntryWatchAddress, where);
			LogInfo("VRPose queue watch: RAX=%016llX RBX=%016llX RCX=%016llX RDX=%016llX\n",
				c.Rax, c.Rbx, c.Rcx, c.Rdx);
			LogInfo("VRPose queue watch: RSI=%016llX RDI=%016llX RBP=%016llX RSP=%016llX\n",
				c.Rsi, c.Rdi, c.Rbp, c.Rsp);
			LogInfo("VRPose queue watch: R8 =%016llX R9 =%016llX R10=%016llX R11=%016llX\n",
				c.R8, c.R9, c.R10, c.R11);
			LogInfo("VRPose queue watch: R12=%016llX R13=%016llX R14=%016llX R15=%016llX\n",
				c.R12, c.R13, c.R14, c.R15);
			LogInfo("VRPose queue watch: plausible return addresses:\n");
			for (unsigned i = 0, shown = 0;
			     i < ARRAYSIZE(sQueueEntryWatchStack) && shown < 16; ++i) {
				const DWORD64 v = sQueueEntryWatchStack[i];
				HMODULE mod = NULL;
				if (v > 0x10000 && GetModuleHandleExA(
					GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
					GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
					(LPCSTR)v, &mod) && mod) {
					char desc[MAX_PATH + 64];
					DescribeCodeAddress(v, desc, sizeof(desc));
					LogInfo("VRPose queue watch:   [rsp+%03X] %s\n", i * 8, desc);
					shown++;
				}
			}
			LogInfo("========================================================\n");
		}

		if (InterlockedCompareExchange(&sQueueEntryWatchState, 2, 1) != 1)
			return;
		void *addr = (void *)sQueueEntryWatchAddress;
		if (!addr) {
			InterlockedExchange(&sQueueEntryWatchState, 3);
			return;
		}
		if (!sProbeVehHandle) {
			sProbeVehHandle = AddVectoredExceptionHandler(1, CameraWriteProbeVeh);
			if (!sProbeVehHandle) {
				LogInfo("VRPose queue watch: failed to install exception handler\n");
				InterlockedExchange(&sQueueEntryWatchState, 3);
				return;
			}
		}
		const int others = ArmQueueEntryWatchOnAllThreads(addr);
		RaiseException(kQueueArmExceptionCode, 0, 0, NULL);
		LogInfo("VRPose queue watch: armed DR1 on slot %p across this and %d other threads\n",
			addr, others);
	}

	void ArmMatrixSourceProbe()
	{
		if (InterlockedCompareExchange(&sOwnershipProbeArmed, 0, 0) != 0)
			return;
		static const bool kStage2Enabled = true;
		if (!kStage2Enabled)
			return;
		// Stage 1 must have finished, so the two probes never contend for
		// the same debug register.
		if (InterlockedCompareExchange(&sProbeState, 3, 3) != 3)
			return;
		if (InterlockedCompareExchange(&sStage2State, 1, 0) != 0)
			return;

		HMODULE base = GetModuleHandleA(NULL);
		if (!base) {
			InterlockedExchange(&sStage2State, 3);
			return;
		}

		void **slot = (void **)((BYTE *)base + kMatricesSourcePtrRva);
		void *src = NULL;
		SIZE_T got = 0;
		if (!ReadProcessMemory(GetCurrentProcess(), slot, &src, sizeof(src), &got)
		    || got != sizeof(src) || !src) {
			LogInfo("VRPose probe2: source pointer at RVA 0x%llX is not populated yet\n",
				(unsigned long long)kMatricesSourcePtrRva);
			InterlockedExchange(&sStage2State, 0);   // retry next frame
			return;
		}

		LogInfo("VRPose probe2: engine's CPU-side matrices at %p (from RVA 0x%llX)\n",
			src, (unsigned long long)kMatricesSourcePtrRva);

		// m_V's first float is at offset 0 of cb_main_matrices1.
		sProbeWatchAddress = src;
		InterlockedExchange(&sStage2Armed, 1);
		// Stage 1 already consumed the one-shot report guard; stage 2 gets
		// its own, or it would disarm silently and report nothing.
		InterlockedExchange(&sProbeReported, 0);

		int others = ArmWatchpointOnAllThreads(src);
		RaiseException(kArmExceptionCode, 0, 0, NULL);   // this thread

		LogInfo("VRPose probe2: armed on the view matrix across %d other threads "
			"+ this one - next hit is the camera code\n", others);
	}

	// Stage 3: catch whoever sets the CAMERA's own matrix.
	//
	// Stage 2 landed in the code that turns the camera into m_V. Its
	// disassembly reads a 4x4 from [rbx+0x90..0xC0], transposes it with the
	// standard shufps sequence, and stores it into the constant-buffer
	// staging area:
	//
	//     movaps xmm4, [rbx + 0x90]      ; camera matrix, row 0
	//     ...                            ; _MM_TRANSPOSE4_PS
	//     movaps [r11 + rax*8 + 0x1d0], xmm0
	//
	// RBX was 0xD271B0 (an RVA - a static global, and the same address that
	// turned up in stage 1's stack scan). Confirmed from the dump:
	// [obj+0x3EC] = 0x0001000D packs constant-buffer index 1 in its high half
	// and destination row 13 in its low half, and 0xD23C50 + 13*16 is exactly
	// the address that trapped.
	//
	// So the engine's camera matrix is at RVA 0xD271B0 + 0x90. Whoever writes
	// THAT is the camera update, and it reads from the player - which is the
	// direction aim ultimately comes from.
	static const DWORD_PTR kCameraObjectRva = 0xD271B0;
	static const DWORD_PTR kCameraMatrixRva = kCameraObjectRva + 0x90;

	void ArmCameraMatrixProbe()
	{
		if (InterlockedCompareExchange(&sOwnershipProbeArmed, 0, 0) != 0)
			return;
		static const bool kStage3Enabled = true;
		if (!kStage3Enabled)
			return;
		// Only after stage 2 is done, so they never contend for DR0.
		if (InterlockedCompareExchange(&sStage2State, 3, 3) != 3)
			return;
		if (InterlockedCompareExchange(&sStage3State, 1, 0) != 0)
			return;

		HMODULE base = GetModuleHandleA(NULL);
		if (!base) {
			InterlockedExchange(&sStage3State, 3);
			return;
		}

		void *addr = (BYTE *)base + kCameraMatrixRva;
		sProbeWatchAddress = addr;
		InterlockedExchange(&sStage3Armed, 1);
		InterlockedExchange(&sProbeReported, 0);

		int others = ArmWatchpointOnAllThreads(addr);
		RaiseException(kArmExceptionCode, 0, 0, NULL);

		LogInfo("VRPose probe3: armed on the camera matrix at %p (RVA 0x%llX) "
			"across %d other threads + this one\n",
			addr, (unsigned long long)kCameraMatrixRva, others);
	}

	// Stage 4: find who RECORDS the camera matrix, and therefore the player.
	//
	// Stage 3 showed the matrix arrives as literal data inside a replayed
	// command buffer, so climbing the return chain any further only walks the
	// interpreter, never the producer. Instead: break on EXECUTION at the
	// interpreter's read of the stream, which hands us the matrix's address
	// inside that buffer -
	//
	//     0x7EEABB  movaps xmm0, [rax]     ; rax = matrix, in the stream
	//
	// then re-arm as a WRITE watch on that same address. Command buffers are
	// reused frame to frame, so the next frame's recorder writes there and
	// trips it - and the recorder is the code that computes the camera from
	// the player.
	//
	// Why that answers the question that matters: if aim comes from a player
	// entity, writing it gives controller aim while the camera (and culling)
	// keeps following the head - no mismatch, no vanishing props. If aim
	// comes from this camera, view and aim are welded at the source and the
	// fire-swing design is the fallback.
	static const DWORD_PTR kCmdStreamReadRva = 0x7EEABB;

	void ArmCommandStreamProbe()
	{
		if (InterlockedCompareExchange(&sOwnershipProbeArmed, 0, 0) != 0)
			return;
		static const bool kStage4Enabled = true;
		if (!kStage4Enabled)
			return;
		if (InterlockedCompareExchange(&sStage3State, 3, 3) != 3)
			return;   // wait for stage 3

		LONG s = InterlockedCompareExchange(&sStage4State, 0, 0);
		HMODULE base = GetModuleHandleA(NULL);
		if (!base)
			return;

		if (s == 0) {
			// Phase A: execution breakpoint on the interpreter's read.
			void *addr = (BYTE *)base + kCmdStreamReadRva;
			sProbeDr7 = kDr7Exec;
			sProbeWatchAddress = addr;
			InterlockedExchange(&sStage4State, 1);
			InterlockedExchange(&sProbeReported, 0);
			int others = ArmWatchpointOnAllThreads(addr);
			RaiseException(kArmExceptionCode, 0, 0, NULL);
			LogInfo("VRPose probe4: exec breakpoint at RVA 0x%llX across %d threads "
				"- capturing the command-stream address\n",
				(unsigned long long)kCmdStreamReadRva, others);
		} else if (s == 2 && sCmdStreamMatrixAddr) {
			// Phase B: watch the stream slot the matrix was copied from.
			InterlockedExchange(&sStage4State, 3);
			sProbeDr7 = kDr7Write4;
			sProbeWatchAddress = sCmdStreamMatrixAddr;
			InterlockedExchange(&sProbeReported, 0);
			int others = ArmWatchpointOnAllThreads(sCmdStreamMatrixAddr);
			RaiseException(kArmExceptionCode, 0, 0, NULL);
			LogInfo("VRPose probe4: watching command-stream slot %p across %d threads "
				"- next hit is the RECORDER\n", sCmdStreamMatrixAddr, others);
		}
	}

	// -----------------------------------------------------------------
	// Player-state hunt: find memory that is UPSTREAM of the camera.
	//
	// Every earlier value scan failed the same way - it could establish
	// correlation but never causation, so candidates that tracked the camera
	// perfectly could never be confirmed as state the engine reads back. That
	// is what made them expensive and inconclusive.
	//
	// The camera object removes that limitation. It publishes ground truth at
	// a fixed address every frame, so we can test causation directly: write a
	// rotated direction into a candidate and see whether the camera's own
	// forward vector follows. It does -> genuinely upstream. It doesn't ->
	// derived copy, discarded. No statistics, no judgement calls.
	//
	// Deliberately bounded at every stage, and it reports a verdict either
	// way rather than running until someone gives up:
	//   collect  - one sweep, capped
	//   narrow   - cheap read-only rounds against fresh camera values, which
	//              is what kills the vast majority of false positives
	//   confirm  - writes, only to the handful that survive, always restored
	// A hard frame budget ends it regardless.

	// World units, and deliberately wide. At 4.0 exactly one candidate out of
	// 3858 survived four rounds of delta matching - too few for a player object
	// to be among them, which says the bracket excluded it rather than that it
	// does not exist. The delta matching is what discriminates; the bracket only
	// has to avoid sweeping the entire heap.
	static const float kHuntEps = 40.0f;
	static const size_t kHuntMaxCandidates = 400000;
	static const unsigned kHuntFrameBudget = 5400;   // ~90s at 60fps

	// Each candidate carries its last observed value, so narrowing can compare
	// how it MOVED rather than what it equals.
	//
	// Exact-value matching assumed the player entity's X/Z equals the
	// camera's to within a quarter unit. If the engine offsets the camera
	// forward of the player origin, or applies lean or sway, that assumption
	// rejects a perfectly rigid player object - which is what a clean run
	// returning zero survivors out of four candidates looks like.
	//
	// Anything rigidly attached to the player moves by the same delta as the
	// camera regardless of what constant offset it sits at, so matching the
	// motion is both more permissive about offsets and far more selective
	// about coincidences.
	struct HuntCandidate {
		void *addr;
		float last[3];
	};
	static std::vector<HuntCandidate> sHuntCandidates;
	static volatile LONG sHuntPhase = 0;   // 0 idle, 1 collecting, 2 narrow, 3 confirm, 4 done
	static float sHuntCollectFwd[3] = { 0, 0, 0 };
	static unsigned sHuntFrames = 0;
	static int sHuntRounds = 0;

	bool ReadCameraVec(DWORD_PTR offset, float out[3])
	{
		HMODULE base = GetModuleHandleA(NULL);
		if (!base)
			return false;
		SIZE_T got = 0;
		if (!ReadProcessMemory(GetCurrentProcess(),
			(BYTE *)base + kCameraObjectRva + offset, out, sizeof(float) * 3, &got)
		    || got != sizeof(float) * 3)
			return false;
		return isfinite(out[0]) && isfinite(out[1]) && isfinite(out[2]);
	}

	// Position match, X and Z only.
	//
	// The forward-vector hunt came back clean but empty: one match in the
	// whole heap, gone as soon as the camera turned. The orientation is not
	// mirrored as a plain direction - and an exact match was always going to
	// be brittle, because the camera's forward carries head bob and weapon
	// sway that the player's own facing does not.
	//
	// Position is a far better key. A player entity's eye offset is almost
	// entirely vertical, so X and Z should track the camera closely while Y
	// differs by eye height - hence Y is ignored here. World positions are
	// also much more distinctive than direction components, which cluster
	// around a few common values.
	bool VecMatches(const float *p, const float ref[3])
	{
		if (!isfinite(p[0]) || !isfinite(p[1]) || !isfinite(p[2]))
			return false;
		// Deliberately loose. This only has to bracket the player object
		// somewhere near the camera; the delta matching in narrowing is what
		// actually discriminates, and it tolerates any constant offset.
		return fabsf(p[0] - ref[0]) <= kHuntEps && fabsf(p[2] - ref[2]) <= kHuntEps
		    && fabsf(p[1] - ref[1]) <= kHuntEps;
	}

	DWORD WINAPI PlayerHuntCollectThread(LPVOID)
	{
		HMODULE gameBase = GetModuleHandleA(NULL);
		const BYTE *cameraObj = (const BYTE *)gameBase + kCameraObjectRva;

		float ref[3];
		memcpy(ref, sHuntCollectFwd, sizeof(ref));

		std::vector<HuntCandidate> found;
		std::vector<BYTE> buf;
		buf.resize(4 * 1024 * 1024);

		SYSTEM_INFO si;
		GetSystemInfo(&si);
		const BYTE *addr = (const BYTE *)si.lpMinimumApplicationAddress;
		const BYTE *maxAddr = (const BYTE *)si.lpMaximumApplicationAddress;

		while (addr < maxAddr && found.size() < kHuntMaxCandidates) {
			MEMORY_BASIC_INFORMATION mbi;
			if (!VirtualQuery(addr, &mbi, sizeof(mbi)))
				break;
			const BYTE *next = (const BYTE *)mbi.BaseAddress + mbi.RegionSize;

			// Heap only: the player entity is a runtime allocation, and
			// skipping images/mapped files removes most of the address space
			// along with every copy that lives in the module's own data.
			const bool usable = mbi.State == MEM_COMMIT
				&& (mbi.Type == MEM_PRIVATE || mbi.Type == MEM_MAPPED)
				&& !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))
				&& (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY));

			if (usable) {
				SIZE_T remaining = mbi.RegionSize;
				const BYTE *p = (const BYTE *)mbi.BaseAddress;
				while (remaining > 0 && found.size() < kHuntMaxCandidates) {
					SIZE_T chunk = min(remaining, buf.size());
					SIZE_T got = 0;
					if (ReadProcessMemory(GetCurrentProcess(), p, buf.data(), chunk, &got) && got >= 12) {
						const size_t limit = got - 12;
						for (size_t i = 0; i <= limit; i += 4) {
							const float *f = (const float *)(buf.data() + i);
							if (!VecMatches(f, ref))
								continue;
							const BYTE *hit = p + i;
							if (hit >= cameraObj - 0x800 && hit < cameraObj + 0x800)
								continue;   // the camera object's own copies

							// Thread stacks sit far below the heap in this
							// process - heap allocations land around
							// 0x000001xx_xxxxxxxx while stacks are down at
							// 0x000000xx_xxxxxxxx. Stack frames are full of
							// live copies of the camera position that vanish
							// the moment the function returns, and an earlier
							// scan wasted a run locking onto exactly that.
							if ((uintptr_t)hit < 0x0000010000000000ull)
								continue;
							HuntCandidate hc;
							hc.addr = (void *)hit;
							memcpy(hc.last, f, sizeof(hc.last));
							found.push_back(hc);
							if (found.size() >= kHuntMaxCandidates)
								break;
						}
					}
					p += chunk;
					remaining -= chunk;
				}
			}

			if (next <= addr)
				break;
			addr = next;
		}

		EnterCriticalSection(&sScanLock);
		sHuntCandidates.swap(found);
		LeaveCriticalSection(&sScanLock);

		LogInfo("VRPose hunt: collected %zu candidates matching the camera position "
			"(%.3f %.3f %.3f)\n", sHuntCandidates.size(), ref[0], ref[1], ref[2]);
		if (sHuntCandidates.size() >= kHuntMaxCandidates) {
			// A real view direction should be rare in memory. Hitting the cap
			// means the search key was not distinctive, so the set is
			// truncated and no verdict drawn from it can be trusted.
			LogInfo("VRPose hunt: WARNING - hit the candidate cap; the set is truncated "
				"and any verdict from it is unsafe\n");
		}
		InterlockedExchange(&sHuntPhase, 2);
		return 0;
	}

	void RunPlayerStateHunt()
	{
		static const bool kHuntEnabled = false;
		if (!kHuntEnabled)
			return;

		LONG phase = InterlockedCompareExchange(&sHuntPhase, 0, 0);
		if (phase == 4)
			return;

		EnsureScanLock();

		if (phase != 0 && ++sHuntFrames > kHuntFrameBudget) {
			LogOverlay(LOG_INFO, "VRPose hunt: budget reached\n");
			LogInfo("VRPose hunt: VERDICT - frame budget reached with %zu candidates "
				"still unresolved; nothing upstream of the camera was confirmed\n",
				sHuntCandidates.size());
			InterlockedExchange(&sHuntPhase, 4);
			return;
		}

		float fwd[3];
		if (!ReadCameraVec(0x180, fwd))
			return;   // camera not live yet

		if (phase == 0) {
			// The camera must be genuinely live and pointing somewhere
			// distinctive before collecting, or the sweep is worthless.
			//
			// The first run collected against (0, 0, 1) - the identity
			// forward, because the camera had not been set up yet - matched
			// every such constant in the heap, hit the 40000 cap, and then
			// discarded all of them. The verdict looked like "nothing tracks
			// the camera" when it actually meant "we searched for a number
			// that means nothing". A unit-length check passes (0,0,1) quite
			// happily, so length alone is not liveness.
			//
			// Two conditions instead: not axis-aligned (a real view direction
			// almost never is), and demonstrably changing (which only a live
			// camera does).
			// A position at or near the origin is a default, not a place
			// in the level - the same class of mistake as collecting
			// against the identity forward (0,0,1) last time.
			float dist = fabsf(fwd[0]) + fabsf(fwd[1]) + fabsf(fwd[2]);
			if (dist < 1.0f)
				return;

			static float watch[3] = { 0, 0, 0 };
			static int settle = 0;
			float delta = fabsf(fwd[0] - watch[0]) + fabsf(fwd[1] - watch[1]) + fabsf(fwd[2] - watch[2]);
			memcpy(watch, fwd, sizeof(watch));
			if (delta < 0.01f) {
				settle = 0;   // static: could still be a default sitting in memory
				return;
			}
			if (++settle < 30)
				return;       // moving, and has been for a while - now it is real

			memcpy(sHuntCollectFwd, fwd, sizeof(fwd));
			InterlockedExchange(&sHuntPhase, 1);
			LogInfo("VRPose hunt: sweeping heap for the camera position\n");
			HANDLE t = CreateThread(NULL, 0, PlayerHuntCollectThread, NULL, 0, NULL);
			if (t)
				CloseHandle(t);
			else
				InterlockedExchange(&sHuntPhase, 0);
			return;
		}

		if (phase == 1)
			return;   // sweep still running

		if (phase == 2) {
			// Re-baseline before the first comparison.
			//
			// The sweep runs on a background thread and takes seconds, during
			// which the player keeps moving - and it samples candidates at
			// whatever instant it reaches them. So neither the stored
			// candidate values nor sHuntCollectFwd refer to a common moment
			// once it finishes. Comparing against them produced a camera
			// delta of (17.5, 130.0) on the very first round and discarded
			// every candidate, which looked exactly like a clean negative.
			//
			// Reading everything once here puts all candidates and the camera
			// on the same timestamp, so every subsequent delta is meaningful.
			static bool rebased = false;
			if (!rebased) {
				rebased = true;
				EnterCriticalSection(&sScanLock);
				for (HuntCandidate &c : sHuntCandidates) {
					SIZE_T got = 0;
					ReadProcessMemory(GetCurrentProcess(), c.addr, c.last, sizeof(c.last), &got);
				}
				LeaveCriticalSection(&sScanLock);
				memcpy(sHuntCollectFwd, fwd, sizeof(fwd));
				LogInfo("VRPose hunt: re-baselined %zu candidates against the current "
					"camera position\n", sHuntCandidates.size());
				return;
			}

			// Read-only narrowing. Only worth doing once the camera has
			// actually turned - re-testing against an unchanged value proves
			// nothing and would just burn the budget.
			// Narrowing needs the player to have MOVED, not turned - the
			// key is a position now. Walking is what drives this.
			float moved = fabsf(fwd[0] - sHuntCollectFwd[0]) + fabsf(fwd[2] - sHuntCollectFwd[2]);
			if (moved < 1.0f)
				return;

			// How far the camera moved since the last round. Anything rigidly
			// attached to the player must have moved by the same amount.
			const float camDx = fwd[0] - sHuntCollectFwd[0];
			const float camDz = fwd[2] - sHuntCollectFwd[2];
			const float kDeltaTol = 0.20f;

			EnterCriticalSection(&sScanLock);
			std::vector<HuntCandidate> keep;
			keep.reserve(sHuntCandidates.size());
			for (HuntCandidate &c : sHuntCandidates) {
				float v[3];
				SIZE_T got = 0;
				if (!ReadProcessMemory(GetCurrentProcess(), c.addr, v, sizeof(v), &got)
				    || got != sizeof(v))
					continue;
				if (!isfinite(v[0]) || !isfinite(v[2]))
					continue;
				const float dx = v[0] - c.last[0];
				const float dz = v[2] - c.last[2];
				if (fabsf(dx - camDx) > kDeltaTol || fabsf(dz - camDz) > kDeltaTol)
					continue;
				// A value that never changes trivially "matches" a camera
				// that barely moved, so require real motion of its own.
				if (fabsf(dx) + fabsf(dz) < 0.1f)
					continue;
				memcpy(c.last, v, sizeof(c.last));
				keep.push_back(c);
			}
			sHuntCandidates.swap(keep);
			size_t remain = sHuntCandidates.size();
			LeaveCriticalSection(&sScanLock);

			memcpy(sHuntCollectFwd, fwd, sizeof(fwd));
			LogInfo("VRPose hunt: %zu candidates moved with the camera "
				"(delta %.2f, %.2f)\n", remain, camDx, camDz);

			// A survivor that turns out to be a read-only mirror is still a
			// lead worth keeping: engines allocate related objects together,
			// so the player struct is plausibly a short walk from it.
			if (remain > 0 && remain <= 8) {
				EnterCriticalSection(&sScanLock);
				for (HuntCandidate &c : sHuntCandidates)
					LogInfo("VRPose hunt:   survivor %p = (%.3f %.3f %.3f)\n",
						c.addr, c.last[0], c.last[1], c.last[2]);
				LeaveCriticalSection(&sScanLock);
			}

			if (remain == 0) {
				LogOverlay(LOG_INFO, "VRPose hunt: no candidates\n");
				LogInfo("VRPose hunt: VERDICT - nothing in the heap moves with the camera; "
					"the player position is not reachable as a plain float3\n");
				InterlockedExchange(&sHuntPhase, 4);
			} else if (++sHuntRounds >= 4 && remain <= 64) {
				// Several rounds, not one. A single delta match is easy to
				// hit by chance across thousands of candidates; four
				// consecutive ones, each against a different movement, is not.
				LogInfo("VRPose hunt: %zu survived %d rounds - starting write confirmation\n",
					remain, sHuntRounds);
				InterlockedExchange(&sHuntPhase, 3);
			}
			return;
		}

		// Phase 3: causation test. One candidate at a time - write a rotated
		// direction, give the engine a frame to rebuild the camera from it,
		// then look at whether the camera followed. Restored either way.
		static size_t idx = 0;
		static int step = 0;
		static float saved[3] = { 0, 0, 0 };
		static float target[3] = { 0, 0, 0 };
		static void *current = NULL;
		// The camera's position at the instant of the write.
		//
		// This used to reuse the position from the last narrowing round,
		// which could be many metres stale - so the player simply walking
		// toward the test displacement registered as the camera "following"
		// our write. That produced a confident false positive on a stack
		// temporary, reporting a 33-unit response to a 1-unit nudge.
		static float beforePos[3] = { 0, 0, 0 };
		// A genuine hit must reproduce. Coincidence does not.
		static int confirmsNeeded = 0;

		EnterCriticalSection(&sScanLock);
		const size_t total = sHuntCandidates.size();
		if (idx < total)
			current = sHuntCandidates[idx].addr;
		LeaveCriticalSection(&sScanLock);

		if (idx >= total) {
			LogOverlay(LOG_INFO, "VRPose hunt: finished\n");
			LogInfo("VRPose hunt: VERDICT - tested every candidate and NONE drives the "
				"camera. The orientation the engine renders from is not readable back "
				"out of any heap copy, so aim cannot be set that way.\n");
			InterlockedExchange(&sHuntPhase, 4);
			return;
		}

		if (step == 0) {
			SIZE_T got = 0;
			if (!ReadProcessMemory(GetCurrentProcess(), current, saved, sizeof(saved), &got)
			    || got != sizeof(saved)) {
				idx++;
				return;
			}
			// Displace horizontally by 1 unit: big enough for the camera to
			// follow unmistakably, small enough not to shove the player
			// through geometry in the one frame before it is restored.
			target[0] = saved[0] + 1.0f;
			target[1] = saved[1];
			target[2] = saved[2];
			memcpy(beforePos, fwd, sizeof(beforePos));   // camera position right now
			SIZE_T wrote = 0;
			WriteProcessMemory(GetCurrentProcess(), current, target, sizeof(target), &wrote);
			step = 1;
			return;
		}

		if (step == 1) {
			step = 2;   // let the engine consume it
			return;
		}

		// Did the camera follow what we wrote? Measured against where the
		// camera was at the moment of the write, so ordinary walking cannot
		// be mistaken for a response.
		float toTarget = fabsf(beforePos[0] - target[0]) - fabsf(fwd[0] - target[0]);

		SIZE_T wrote = 0;
		WriteProcessMemory(GetCurrentProcess(), current, saved, sizeof(saved), &wrote);

		// A 1-unit nudge cannot move the camera more than about a unit. A
		// larger reading means something other than our write moved it, so
		// treat it as noise rather than a spectacular success.
		if (toTarget > 2.0f) {
			idx++;
			step = 0;
			confirmsNeeded = 0;
			return;
		}

		if (toTarget > 0.3f && confirmsNeeded < 2) {
			// Plausible - repeat it on the same candidate before believing it.
			confirmsNeeded++;
			step = 0;
			LogInfo("VRPose hunt: %p responded (%.3f), confirming (%d/2)\n",
				current, toTarget, confirmsNeeded);
			return;
		}

		if (toTarget > 0.3f) {
			LogOverlay(LOG_INFO, "VRPose hunt: FOUND upstream state\n");
			LogInfo("========================================================\n");
			LogInfo("VRPose hunt: VERDICT - FOUND. Writing %p moved the engine's own "
				"camera toward it (%.4f).\n", current, toTarget);
			LogInfo("VRPose hunt: this is the PLAYER OBJECT - its orientation fields live "
				"nearby in the same struct, which is exactly what aim needs.\n");
			LogInfo("========================================================\n");
			InterlockedExchange(&sHuntPhase, 4);
			return;
		}

		idx++;
		step = 0;
		confirmsNeeded = 0;
		if ((idx % 8) == 0)
			LogInfo("VRPose hunt: %zu/%zu candidates tested\n", idx, total);
	}

	// -----------------------------------------------------------------
	// Ammo hunt: same heap-scan-then-narrow methodology as
	// PlayerHuntCollectThread above, adapted for an int32 magazine-ammo
	// counter instead of a float3 position. Where the player hunt narrows
	// by tracking motion every frame, this narrows by tracking a
	// DECREASE every time a shot is fired (RedirectShot already runs on
	// every shot, so that's the natural trigger - no separate polling).
	//
	// Needs a known starting value to seed the first exact-match pass
	// (unlike the player hunt, which can bootstrap from "moving" alone) -
	// that has to come from reading the number off the HUD once, since
	// this mod has no other way to know what's currently displayed.
	static const size_t kAmmoHuntMaxCandidates = 400000;

	struct AmmoCandidate {
		void *addr;
		int32_t last;
		// The value when this candidate was first collected. Narrowing
		// discriminates on TOTAL drop across the whole run rather than on
		// a per-round strict decrease - see AmmoNarrowThread for why.
		int32_t first;
		bool firstValid;
	};
	static std::vector<AmmoCandidate> sAmmoCandidates;
	static volatile LONG sAmmoHuntPhase = 0;   // 0 idle, 1 collecting, 2 narrowing, 3 done
	// Set from the HUD read in flatscreen mode: reload once on load-in to
	// reach this known magazine count, then fire without reloading again
	// so it only ever decreases from here.
	static int32_t sAmmoHuntSeedValue = 31;
	static int sAmmoHuntRounds = 0;

	// The scan takes long enough to walk the whole heap that several more
	// shots can fire before it finishes - the first live run showed 2-3
	// "redirect skipped" log lines land between "sweeping heap" and
	// "collected N candidates". An exact match against the value read
	// before the scan started misses the real counter entirely once it
	// has moved on, and every survivor from a false start is coincidence
	// (confirmed live: 3 survivors with values 2, 0, 3 - nowhere near a
	// magazine draining from 31). Matching a range instead, rather than
	// one exact value, tolerates that drift.
	static const int32_t kAmmoHuntTolerance = 20;

	DWORD WINAPI AmmoHuntCollectThread(LPVOID param)
	{
		// Housekeeping, not gameplay - must never compete with the game's
		// own threads for a core. Moving this work off the fire hook's
		// thread wasn't enough on its own ("framerate is slightly better
		// but still really bad when shooting") because a background
		// thread at default priority still contends for the same cores;
		// this is the other half of that fix, same lesson already
		// learned once in this file for the fire-hook re-arm thread.
		SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);

		const int32_t seed = (int32_t)(intptr_t)param;

		std::vector<AmmoCandidate> found;
		std::vector<BYTE> buf;
		buf.resize(4 * 1024 * 1024);

		SYSTEM_INFO si;
		GetSystemInfo(&si);
		const BYTE *addr = (const BYTE *)si.lpMinimumApplicationAddress;
		const BYTE *maxAddr = (const BYTE *)si.lpMaximumApplicationAddress;

		while (addr < maxAddr && found.size() < kAmmoHuntMaxCandidates) {
			MEMORY_BASIC_INFORMATION mbi;
			if (!VirtualQuery(addr, &mbi, sizeof(mbi)))
				break;
			const BYTE *next = (const BYTE *)mbi.BaseAddress + mbi.RegionSize;

			// Same filter as the player hunt: heap only, skip images/mapped
			// files, skip guard/no-access pages.
			const bool usable = mbi.State == MEM_COMMIT
				&& (mbi.Type == MEM_PRIVATE || mbi.Type == MEM_MAPPED)
				&& !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))
				&& (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY));

			if (usable) {
				SIZE_T remaining = mbi.RegionSize;
				const BYTE *p = (const BYTE *)mbi.BaseAddress;
				while (remaining > 0 && found.size() < kAmmoHuntMaxCandidates) {
					SIZE_T chunk = min(remaining, buf.size());
					SIZE_T got = 0;
					if (ReadProcessMemory(GetCurrentProcess(), p, buf.data(), chunk, &got) && got >= 4) {
						const size_t limit = got - 4;
						for (size_t i = 0; i <= limit; i += 4) {
							const int32_t *v = (const int32_t *)(buf.data() + i);
							if (*v > seed || *v < seed - kAmmoHuntTolerance)
								continue;
							const BYTE *hit = p + i;
							// Same stack exclusion as the player hunt: heap
							// allocations sit above this boundary in this
							// process, thread stacks below it.
							if ((uintptr_t)hit < 0x0000010000000000ull)
								continue;
							AmmoCandidate ac;
							ac.addr = (void *)hit;
							ac.last = *v;
							// Not treated as the baseline for the total-drop
							// test: this was sampled mid-scan, at whatever
							// moment the sweep happened to reach this address,
							// so it doesn't correspond to any common instant.
							// The first narrowing round re-reads everything at
							// once and sets the real baseline there.
							ac.first = *v;
							ac.firstValid = false;
							found.push_back(ac);
							if (found.size() >= kAmmoHuntMaxCandidates)
								break;
						}
					}
					p += chunk;
					remaining -= chunk;
				}
			}

			if (next <= addr)
				break;
			addr = next;
		}

		EnsureScanLock();
		EnterCriticalSection(&sScanLock);
		sAmmoCandidates.swap(found);
		LeaveCriticalSection(&sScanLock);

		LogInfo("VRPose ammo hunt: collected %zu candidates in range [%d, %d]\n",
			sAmmoCandidates.size(), seed - kAmmoHuntTolerance, seed);
		if (sAmmoCandidates.size() >= kAmmoHuntMaxCandidates) {
			LogInfo("VRPose ammo hunt: WARNING - hit the candidate cap; the range "
				"was not distinctive enough and this set is unreliable\n");
		}
		InterlockedExchange(&sAmmoHuntPhase, 2);
		return 0;
	}

	static volatile LONG sAmmoNarrowRunning = 0;

	// Phase 2: narrow by re-reading survivors. MOVED TO A BACKGROUND
	// THREAD: right after collection this list can hold up to
	// kAmmoHuntMaxCandidates (400,000) entries, each needing its own
	// ReadProcessMemory call - doing that synchronously on whatever
	// thread calls RunAmmoHunt (the fire hook, i.e. the game's own
	// thread) is exactly what froze the game for a second per shot,
	// confirmed live: "the game freezes for a second when I shoot... if
	// I hold down the trigger, it becomes a slideshow."
	//
	// The test is NON-INCREASING plus a total-drop check, not a
	// per-round strict decrease. Strict-decrease was wrong once this
	// moved off the fire thread and repeatedly killed the real answer:
	// the hook fires at the moment the shot is issued, but the engine
	// hasn't necessarily applied the decrement by the time this thread
	// gets scheduled and reads. An unchanged read then failed the
	// "must have decreased" test and discarded the genuine magazine
	// address. Live proof, from three consecutive failed runs: the real
	// address was found and tracked correctly (24 -> 23) and then
	// eliminated on the very next round, ending with zero survivors.
	//
	// Non-increasing alone would be far too weak (static memory passes
	// it forever - that is exactly how the reserve hunt failed), so the
	// discriminator is the TOTAL drop from a common baseline instead:
	// after N shots a real magazine has dropped by about N, while static
	// memory has dropped by zero. That tolerates any per-round timing
	// skew while still being a strong filter, since it only cares about
	// the accumulated total, not when each decrement landed.
	//
	// Also sleeps briefly first, so the common case is simply that the
	// decrement has already landed before anything is read.
	DWORD WINAPI AmmoNarrowThread(LPVOID)
	{
		SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
		Sleep(120);

		EnsureScanLock();
		EnterCriticalSection(&sScanLock);
		const bool baselining = !sAmmoCandidates.empty() && !sAmmoCandidates[0].firstValid;
		std::vector<AmmoCandidate> keep;
		keep.reserve(sAmmoCandidates.size());
		for (AmmoCandidate &c : sAmmoCandidates) {
			int32_t v = 0;
			SIZE_T got = 0;
			if (!ReadProcessMemory(GetCurrentProcess(), c.addr, &v, sizeof(v), &got)
			    || got != sizeof(v))
				continue;
			if (v < 0 || v > c.last)
				continue;   // never increases, stays non-negative
			if (baselining) {
				// First narrowing round: everything is re-read at one
				// instant here, which is the only point where all the
				// candidates share a timestamp. That is the baseline the
				// total-drop test measures against.
				c.first = v;
				c.firstValid = true;
			}
			c.last = v;
			keep.push_back(c);
		}
		sAmmoCandidates.swap(keep);
		size_t remain = sAmmoCandidates.size();
		LeaveCriticalSection(&sScanLock);

		sAmmoHuntRounds++;
		if (baselining) {
			LogInfo("VRPose ammo hunt: %zu candidates after baseline round\n", remain);
			InterlockedExchange(&sAmmoNarrowRunning, 0);
			return 0;
		}

		// Require the accumulated drop to track the shots actually seen
		// since the baseline. Allowing a little slack in both directions:
		// shots can be missed while a round is already in flight (the
		// sAmmoNarrowRunning guard skips them), and the engine may apply a
		// decrement just after a read.
		const int shotsSinceBaseline = sAmmoHuntRounds - 1;
		if (shotsSinceBaseline >= 3) {
			EnterCriticalSection(&sScanLock);
			std::vector<AmmoCandidate> plausible;
			plausible.reserve(sAmmoCandidates.size());
			for (AmmoCandidate &c : sAmmoCandidates) {
				const int drop = c.first - c.last;
				if (drop >= shotsSinceBaseline - 2 && drop <= shotsSinceBaseline + 2)
					plausible.push_back(c);
			}
			// Only accept the filtered set if it kept something - if the
			// drop test wiped everything out, the shot count is more
			// likely wrong than every candidate being wrong, and keeping
			// the unfiltered set lets later rounds still resolve it.
			if (!plausible.empty())
				sAmmoCandidates.swap(plausible);
			remain = sAmmoCandidates.size();
			LeaveCriticalSection(&sScanLock);
		}

		LogInfo("VRPose ammo hunt: %zu candidates after shot #%d (expected drop ~%d)\n",
			remain, sAmmoHuntRounds, shotsSinceBaseline);

		if (remain > 0 && remain <= 20) {
			EnterCriticalSection(&sScanLock);
			for (AmmoCandidate &c : sAmmoCandidates)
				LogInfo("VRPose ammo hunt:   survivor %p = %d\n", c.addr, c.last);
			LeaveCriticalSection(&sScanLock);
		}

		if (remain == 0) {
			// Not re-armed automatically: sAmmoHuntSeedValue is now stale
			// (the magazine has kept draining past it), and re-sweeping
			// against a stale seed would just repeat this failure. A fresh
			// reload puts the magazine back at a value we actually know.
			LogInfo("VRPose ammo hunt: VERDICT - nothing survived %d round(s) against seed "
				"%d; reload to reset to a known value and fire again\n",
				sAmmoHuntRounds, sAmmoHuntSeedValue);
			InterlockedExchange(&sAmmoHuntPhase, 3);
		} else if (remain == 1 && shotsSinceBaseline >= 3) {
			// The shot-count condition matters: a lone survivor after only
			// a round or two is far more likely to be luck than the answer,
			// and the total-drop test hasn't even been applied yet at that
			// point.
			LogInfo("VRPose ammo hunt: VERDICT - unique survivor after %d shots\n", sAmmoHuntRounds);
			InterlockedExchange(&sAmmoHuntPhase, 3);
		} else if (sAmmoHuntRounds >= 8) {
			// More than one survivor, but not necessarily still ambiguous:
			// a real magazine counter is often mirrored (a live run showed
			// exactly this - two addresses both reading 22, tracking a
			// third pair that both read 1 and looked nothing like a
			// magazine - started small, drained to 1 fast). If everything
			// left agrees on the current value, they're plausibly just
			// synchronized copies of the same real counter and any one is
			// as good to read from as another. If they still disagree,
			// that is genuine, unresolved ambiguity - keep narrowing.
			bool allSame = true;
			EnterCriticalSection(&sScanLock);
			for (size_t i = 1; i < sAmmoCandidates.size(); i++) {
				if (sAmmoCandidates[i].last != sAmmoCandidates[0].last) {
					allSame = false;
					break;
				}
			}
			LeaveCriticalSection(&sScanLock);
			if (allSame && remain <= 20) {
				LogInfo("VRPose ammo hunt: VERDICT - %zu survivors after %d shots, all read "
					"%d - treating as mirrored copies, using the first\n",
					remain, sAmmoHuntRounds, sAmmoCandidates[0].last);
				InterlockedExchange(&sAmmoHuntPhase, 3);
			} else if (sAmmoHuntRounds >= 15) {
				LogInfo("VRPose ammo hunt: VERDICT - gave up after %d shots with %zu still-"
					"disagreeing survivors, listed above - not confident enough to pick one\n",
					sAmmoHuntRounds, remain);
				InterlockedExchange(&sAmmoHuntPhase, 3);
			}
			// else: still disagreeing and under the round cap - keep firing.
		}

		InterlockedExchange(&sAmmoNarrowRunning, 0);
		return 0;
	}

	// Called once per shot from RedirectShot. currentAmmo is unknown to us
	// (that is the whole point of the hunt) until the first call, where the
	// caller supplies the value read off the HUD via kAmmoHuntSeedValue.
	void RunAmmoHunt()
	{
		static const bool kAmmoHuntEnabled = true;
		if (!kAmmoHuntEnabled)
			return;

		// Set from outside once, by whatever first calls this - see the
		// seed constant below, near where RedirectShot fires this.
		LONG phase = InterlockedCompareExchange(&sAmmoHuntPhase, 0, 0);
		if (phase == 3)
			return;

		if (phase == 0) {
			if (sAmmoHuntSeedValue <= 0)
				return;   // not seeded yet
			LogInfo("VRPose ammo hunt: sweeping heap for seed value %d\n", sAmmoHuntSeedValue);
			InterlockedExchange(&sAmmoHuntPhase, 1);
			HANDLE t = CreateThread(NULL, 0, AmmoHuntCollectThread,
				(LPVOID)(intptr_t)sAmmoHuntSeedValue, 0, NULL);
			if (t)
				CloseHandle(t);
			else
				InterlockedExchange(&sAmmoHuntPhase, 0);
			return;
		}

		if (phase == 1)
			return;   // sweep still running

		// Phase 2: kick off one narrowing round on a background thread, if
		// one isn't already in flight - see AmmoNarrowThread above for why
		// this can no longer run synchronously here.
		if (InterlockedCompareExchange(&sAmmoNarrowRunning, 1, 0) != 0)
			return;
		HANDLE t = CreateThread(NULL, 0, AmmoNarrowThread, NULL, 0, NULL);
		if (t)
			CloseHandle(t);
		else
			InterlockedExchange(&sAmmoNarrowRunning, 0);
	}

	// For HackerContext's weapon-mounted ammo display: only trust the
	// result when the hunt actually finished (phase 3) with either a
	// unique survivor, or several survivors that all currently agree on
	// the same value (mirrored copies - see the VERDICT branches above).
	// A finished hunt that gave up still disagreeing leaves multiple
	// candidates with different values in the vector, which must NOT be
	// read as ammo - hence checking agreement here too, not just count.
	void *ConfirmedAmmoAddress()
	{
		if (InterlockedCompareExchange(&sAmmoHuntPhase, 0, 0) != 3)
			return NULL;
		EnsureScanLock();
		EnterCriticalSection(&sScanLock);
		void *addr = NULL;
		if (!sAmmoCandidates.empty()) {
			bool allSame = true;
			for (size_t i = 1; i < sAmmoCandidates.size(); i++) {
				if (sAmmoCandidates[i].last != sAmmoCandidates[0].last) {
					allSame = false;
					break;
				}
			}
			if (allSame)
				addr = sAmmoCandidates[0].addr;
		}
		LeaveCriticalSection(&sScanLock);
		return addr;
	}

	// ---------------------------------------------------------------
	// Reserve-ammo: NOT a full heap scan. Two earlier attempts both used
	// the same "narrow by staying exactly the same" technique the
	// magazine hunt uses for "decreased", full-heap - reserve doesn't
	// change on every shot (only on reload, and at a firing range it
	// auto-refills anyway per the user's own report), so "unchanged" was
	// the only signal available to a scan run the same way. Both failed
	// outright: "unchanged" is a far weaker filter than it sounds, since
	// enormous amounts of any real process's memory are simply static
	// forever, scan or no scan. First attempt (wide ±5 tolerance on
	// collection) collected 400,000 candidates and still had 358,472 of
	// them after 43 shots - a background thread re-checking that many
	// addresses every single shot, forever, is what caused a serious
	// live framerate regression. Tightening to an exact-match collection
	// (second attempt) still left 31,847 unresolved even after a hard
	// 15-round cap - proof it was the filter itself, not just its
	// tolerance.
	//
	// This version doesn't scan the heap for reserve at all. It anchors
	// on the CONFIRMED magazine address instead: reserve is very likely
	// a sibling field in the same game-side ammo/weapon struct, so once
	// the magazine hunt has found and confirmed its address, a single
	// read of a small window around it (a few KB, not hundreds of MB)
	// is enough to find an exact match - cheap enough to not need a
	// background thread, narrowing, or a round cap at all.
	static const int32_t kReserveSearchWindow = 0x2000;   // bytes each side of the magazine address
	static const int32_t kReserveExactValue = 34;   // from the same HUD read: "34 rounds in the reserve"

	static void *sReserveAddress = NULL;
	static bool sReserveSearchDone = false;   // one-shot per magazine confirmation, not per shot

	void RunReserveHunt()
	{
		static const bool kReserveHuntEnabled = true;
		if (!kReserveHuntEnabled || sReserveSearchDone)
			return;

		void *magAddr = ConfirmedAmmoAddress();
		if (!magAddr)
			return;   // wait for the magazine hunt to confirm its address first

		sReserveSearchDone = true;

		const BYTE *base = (const BYTE *)magAddr - kReserveSearchWindow;
		const SIZE_T span = (SIZE_T)kReserveSearchWindow * 2;
		std::vector<BYTE> buf(span);
		SIZE_T got = 0;
		if (!ReadProcessMemory(GetCurrentProcess(), base, buf.data(), span, &got) || got < 4) {
			LogInfo("VRPose reserve: couldn't read the window around the magazine address %p\n", magAddr);
			return;
		}

		std::vector<void *> hits;
		const size_t limit = got - 4;
		for (size_t i = 0; i <= limit; i += 4) {
			const int32_t *v = (const int32_t *)(buf.data() + i);
			if (*v == kReserveExactValue)
				hits.push_back((void *)(base + i));
		}

		LogInfo("VRPose reserve: %zu exact match(es) for %d within %d bytes of the magazine "
			"address %p\n", hits.size(), kReserveExactValue, kReserveSearchWindow, magAddr);

		if (hits.empty()) {
			LogInfo("VRPose reserve: VERDICT - no match nearby; reserve may not be adjacent to "
				"the magazine field, or wasn't showing %d\n", kReserveExactValue);
			return;
		}

		// Prefer whichever hit sits closest to the magazine address - the
		// most plausible sibling field in the same struct.
		void *best = hits[0];
		intptr_t bestDist = (intptr_t)hits[0] - (intptr_t)magAddr;
		if (bestDist < 0)
			bestDist = -bestDist;
		for (size_t i = 1; i < hits.size(); i++) {
			intptr_t d = (intptr_t)hits[i] - (intptr_t)magAddr;
			if (d < 0)
				d = -d;
			if (d < bestDist) {
				bestDist = d;
				best = hits[i];
			}
		}

		EnsureScanLock();
		EnterCriticalSection(&sScanLock);
		sReserveAddress = best;
		LeaveCriticalSection(&sScanLock);

		LogInfo("VRPose reserve: VERDICT - using %p (%lld bytes from magazine), closest of %zu "
			"match(es)\n", best, (long long)bestDist, hits.size());
	}

	void *ConfirmedReserveAddress()
	{
		EnsureScanLock();
		EnterCriticalSection(&sScanLock);
		void *addr = sReserveAddress;
		LeaveCriticalSection(&sScanLock);
		return addr;
	}

	// -----------------------------------------------------------------
	// Fire-code coverage differencing.
	//
	// Value scanning asks "where is the aim state?" and failed because the
	// answer is "nowhere writable" - the player's position and orientation
	// are owned by physics or recomputed per frame, so every copy we could
	// find was an output.
	//
	// This asks a different question entirely: which CODE runs when the
	// player shoots, that does not run otherwise? The shot logic has to be in
	// that difference, and behaviour cannot hide the way storage can.
	//
	// Mechanism: mark every executable page PAGE_GUARD. The first instruction
	// fetch from a guarded page faults, we record it, and the guard clears
	// itself - so the cost is one fault per page for the whole sample, not
	// one per execution. Sample while not firing, re-arm, sample while
	// firing, and diff.
	//
	// We inject the fire input ourselves, so the two windows are exact and
	// need no timing from the player.

	static const DWORD_PTR kTextStart = 0x1000;
	static const DWORD_PTR kTextEnd   = 0x9C91D3;
	static const size_t kPageCount = (size_t)((kTextEnd - kTextStart) / 0x1000) + 1;

	// Per page, the RVA of the first instruction fetched from it (plus one,
	// so zero means "never executed"). The guard clears itself on that first
	// fault, so this is exactly the entry point into the page - far more
	// useful than a bit, since it points at the function actually running
	// rather than leaving 4KB of mixed code and data to sift through.
	static std::vector<DWORD> sCovBaseline;   // executed while NOT firing
	static std::vector<DWORD> sCovFiring;     // executed while firing
	static std::vector<DWORD> *sCovActive = NULL;
	static volatile LONG sCovPhase = 0;   // 0 idle, 1 baseline, 2 firing, 3 done
	static unsigned sCovFrames = 0;
	static BYTE *sCovBase = NULL;

	void CoverageNoteAddress(DWORD_PTR addr)
	{
		std::vector<DWORD> *set = sCovActive;
		if (!set || !sCovBase)
			return;
		DWORD_PTR rva = addr - (DWORD_PTR)sCovBase;
		if (rva < kTextStart || rva > kTextEnd)
			return;
		size_t page = (size_t)((rva - kTextStart) / 0x1000);
		if (page < set->size() && (*set)[page] == 0)
			(*set)[page] = (DWORD)rva + 1;   // benign race between threads
	}

	// Re-arms guards across the whole text section. Untouched pages already
	// carry a guard, so this is idempotent for them.
	int ArmCoverageGuards()
	{
		if (!sCovBase)
			return 0;
		int armed = 0;
		for (size_t i = 0; i < kPageCount; i++) {
			BYTE *p = sCovBase + kTextStart + i * 0x1000;
			MEMORY_BASIC_INFORMATION mbi;
			if (!VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT)
				continue;
			DWORD old = 0;
			if (VirtualProtect(p, 0x1000, mbi.Protect | PAGE_GUARD, &old))
				armed++;
		}
		return armed;
	}

	void RunFireCodeCoverage()
	{
		static const bool kCoverageEnabled = false;   // done - gave us 0x4F3E60
		if (!kCoverageEnabled)
			return;
		LONG phase = InterlockedCompareExchange(&sCovPhase, 0, 0);
		if (phase == 3)
			return;

		// Only once the aim system has settled, so we know we are in-world
		// rather than in a menu.
		float live[3];
		if (!sVRSystem || !ReadCameraVec(0x180, live))
			return;
		if (fabsf(live[0]) + fabsf(live[1]) + fabsf(live[2]) < 1.0f)
			return;   // camera still at the origin: menu or loading

		if (phase == 0) {
			sCovBase = (BYTE *)GetModuleHandleA(NULL);
			if (!sCovBase)
				return;
			sCovBaseline.assign(kPageCount, 0);
			sCovFiring.assign(kPageCount, 0);
			sCovActive = &sCovBaseline;
			int armed = ArmCoverageGuards();
			sCovFrames = 0;
			InterlockedExchange(&sCovPhase, 1);
			LogInfo("VRPose coverage: baseline window - guarded %d of %zu text pages, "
				"DO NOT SHOOT for the next few seconds\n", armed, kPageCount);
			LogOverlay(LOG_INFO, "VRPose: coverage baseline - do not shoot\n");
			return;
		}

		if (phase == 1) {
			if (++sCovFrames < 2400)   // the game runs well above 60fps here
				return;
			size_t hit = 0;
			for (DWORD v : sCovBaseline)
				if (v) hit++;
			sCovActive = &sCovFiring;
			int armed = ArmCoverageGuards();
			sCovFrames = 0;
			InterlockedExchange(&sCovPhase, 2);
			LogInfo("VRPose coverage: baseline saw %zu pages execute; re-armed %d. "
				"Now firing.\n", hit, armed);
			LogOverlay(LOG_INFO, "VRPose: coverage - firing now\n");
			return;
		}

		// Phase 2: fire in bursts. Injected rather than asked for, so the
		// window is exact - and bursts rather than a held button so the
		// whole fire/reload/impact path gets exercised.
		if ((sCovFrames % 30) == 0)
			SendMouseButton(false, true);
		else if ((sCovFrames % 30) == 12)
			SendMouseButton(false, false);

		if (++sCovFrames < 2400)
			return;
		SendMouseButton(false, false);

		size_t onlyFiring = 0;
		LogInfo("========================================================\n");
		LogInfo("VRPose coverage: pages that executed ONLY while firing:\n");
		for (size_t i = 0; i < kPageCount; i++) {
			if (sCovFiring[i] && !sCovBaseline[i]) {
				// The exact instruction execution entered on, not just the
				// page - that lands inside the function that actually ran.
				LogInfo("VRPose coverage:   entered at metro.exe+0x%llX (page 0x%llX)\n",
					(unsigned long long)(sCovFiring[i] - 1),
					(unsigned long long)(kTextStart + i * 0x1000));
				onlyFiring++;
			}
		}
		LogInfo("VRPose coverage: %zu pages unique to firing - these contain the shot code\n",
			onlyFiring);
		LogInfo("========================================================\n");
		LogOverlay(LOG_INFO, "VRPose: coverage complete\n");
		InterlockedExchange(&sCovPhase, 3);
		sCovActive = NULL;
	}

	// -----------------------------------------------------------------
	// Fire-call probe.
	//
	// Coverage differencing narrowed the shot path to 11 pages, and
	// metro.exe+0x4F3E60 is the standout: a thin wrapper that unpacks fields
	// from an object in RDX and forwards them -
	//
	//     lea   r8,  [rdx + 0x140]     ; a vector
	//     lea   r10, [rdx + 0x150]     ; another, 16 bytes later
	//     lea   r9,  [rdx + 0x180]
	//     movss xmm2, [rdx + 0x138]
	//     movss xmm3, [rdx + 0x13c]
	//     call  0x4F3EE0
	//
	// Two 16-byte-aligned vectors handed to a callee is the shape of an
	// origin and a direction. Rather than reverse the callee statically, stop
	// at the wrapper and read the object: if one of those vectors matches the
	// camera's position and another matches its forward, this is the shot ray
	// - and writing the direction before the call redirects the bullet
	// without touching the camera at all.
	//
	// That is the whole feature: aim from the controller, view from the head,
	// culling still following the camera. Read-only for now; confirm first.
	static const DWORD_PTR kFireFnRva = 0x4F3E60;

	// Redirects the shot by rewriting its target point.
	//
	// The captured object showed +0x150 holding the camera position walked
	// 9.299 units (the distance at +0x178) along the camera forward - i.e.
	// the point the bullet is aimed at, already computed and sitting in
	// writable memory one instruction before the engine consumes it.
	//
	// Rewriting it steers the shot WITHOUT touching the camera, which is the
	// whole feature: the camera keeps following the player's head so culling
	// stays correct, while the bullet goes where the controller points.
	//
	// This first version writes a deliberate 30-degree offset rather than the
	// controller direction, because a visible, unmistakable deflection proves
	// the mechanism. Aiming for real is the same write with a different
	// vector.
	// How far the controller has moved from its anchor, in radians.
	//
	// Kept independent of UpdateMotionAiming, which is parked: that function
	// carried the whole injection-and-cancellation approach, and none of it
	// is wanted now that the shot can be steered directly. Only the anchoring
	// idea survives - whatever direction the controller was pointing when it
	// settled becomes "straight ahead", so no assumption is needed about how
	// the tracking frame relates to the game's world.
	bool GetAimOffsetFromController(float *outYaw, float *outPitch)
	{
		float cy = 0.0f, cp = 0.0f;
		if (!GetControllerAim(&cy, &cp))
			return false;

		if (!sCtrlAnchored) {
			// The settle is FRAME-based, not call-based.
			//
			// This function is called from exactly one place - RedirectShot -
			// which means once per SHOT. A counter incremented here therefore
			// spent its "short settle" on the player's first thirty ROUNDS,
			// every one of them bailing out unredirected and going wherever
			// the engine sent it. That is the live report that it "takes more
			// than 10 shots for the redirect to start", and the two separate
			// impact clusters in one screenshot: skipped shots at the
			// engine's point, redirected ones at ours.
			//
			// Making it frame-based was not enough on its own: the window
			// still STARTED at the first call, i.e. at the player's first
			// shot, so it simply cost the next ~0.4s of firing instead - two
			// rounds at a normal rate, which is exactly what the next test
			// showed. Anchoring a settle to the act of shooting is the bug,
			// in any unit.
			//
			// So anchor on the first call outright. The stray-pose worry the
			// settle guarded against does not apply here: firing a weapon is
			// itself proof the session is up and the controller has been
			// tracking for a long time, and the aim loop's own anchoring
			// (90 steady frames) has usually claimed sCtrlAnchored before
			// this path is ever reached. If an anchor is ever caught badly,
			// recentring re-takes it deliberately - modifier + right stick
			// click - which is the honest control for that, rather than
			// silently mis-aiming the opening rounds of every session.
			sAnchorCtrlYaw = cy;
			sAnchorCtrlPitch = cp;
			sCtrlAnchored = true;
			LogInfo("VRPose aim: controller anchored at (%.3f, %.3f) rad - "
				"this direction is now straight ahead\n", cy, cp);
		}

		*outYaw = WrapAngle(cy - sAnchorCtrlYaw);
		*outPitch = cp - sAnchorCtrlPitch;
		return true;
	}

	void RedirectShot(DWORD64 objPtr)
	{
		// These hooks are diagnostic-only and must remain active during the
		// real VR redirect path; native-shot diagnostics themselves may be off.
		if (kNativeShotDiagnostics) {
			if (InterlockedExchange(&sNativeShotDiagnosticLogged, 1) == 0) {
				float v0e0[4] = { 0, 0, 0, 0 };
				float v0f0[4] = { 0, 0, 0, 0 };
				float v150[4] = { 0, 0, 0, 0 };
				float dist = 0.0f;
				SIZE_T got = 0;
				ReadProcessMemory(GetCurrentProcess(), (void *)(objPtr + 0x0E0),
					v0e0, sizeof(v0e0), &got);
				ReadProcessMemory(GetCurrentProcess(), (void *)(objPtr + 0x0F0),
					v0f0, sizeof(v0f0), &got);
				ReadProcessMemory(GetCurrentProcess(), (void *)(objPtr + 0x150),
					v150, sizeof(v150), &got);
				ReadProcessMemory(GetCurrentProcess(), (void *)(objPtr + 0x178),
					&dist, sizeof(dist), &got);

				LogInfo("VRPose fire: NATIVE DIAGNOSTIC MODE - no shot fields written\n");
				LogInfo("VRPose fire: native object=%016llX +0x0E0=(%.5f %.5f %.5f %.5f) "
					"+0x0F0=(%.5f %.5f %.5f %.5f) +0x150=(%.5f %.5f %.5f %.5f) dist=%.5f\n",
					(unsigned long long)objPtr,
					v0e0[0], v0e0[1], v0e0[2], v0e0[3],
					v0f0[0], v0f0[1], v0f0[2], v0f0[3],
					v150[0], v150[1], v150[2], v150[3], dist);

				// DumpGameModuleIfRequested performs the file I/O later from the
				// render/present path, not inside this fire hook.
				InterlockedExchange(&sWantModuleDump, 1);
				LogInfo("VRPose fire: requested live decrypted module dump; target callee is "
					"metro.exe+0x4F3EE0\n");
			}
			return;
		}

		// Metro can enter this wrapper twice for one logical trigger event.  The
		// two calls arrive in the same game frame, with different temporary shot
		// objects and different native hit points.  Rewriting both completed hit
		// points creates two visible impacts for one trigger pull.  Keep the first
		// call as the authoritative redirect and leave the duplicate's native
		// result alone.  This is deliberately frame-local: real automatic fire
		// rounds on later frames must still be processed normally.
		if (G) {
			static volatile LONG sLastRedirectFrame = -1;
			const LONG frame = (LONG)G->frame_no;
			const LONG previous = InterlockedExchange(&sLastRedirectFrame, frame);
			if (previous == frame) {
				static volatile LONG sDuplicateRedirects = 0;
				const LONG n = InterlockedIncrement(&sDuplicateRedirects);
				if (n <= 20)
					LogInfo("VRPose fire: duplicate same-frame fire object skipped "
						"(frame=%ld obj=%016llX)\n", frame,
						(unsigned long long)objPtr);
				return;
			}
		}

		// The ammo/reserve heap hunts used to run from here, once per
		// shot. Both are RETIRED: the weapon-mounted ammo counter now
		// redirects the game's own HUD draw (see BeginAmmoCounterCB in
		// HackerContext.cpp) instead of scanning memory for the values,
		// so nothing needs finding. That removes the per-session
		// reload-then-fire calibration, and with it the per-shot
		// rescanning that caused a serious framerate regression.

		// Every early return below used to be silent, so "no shot data" was
		// indistinguishable from "the breakpoint never fired again" - and the
		// first shot is always consumed by the identification pass, so a short
		// burst can produce nothing at all with no indication why.
		static int sBailLog = 0;
		#define REDIRECT_BAIL(why) do { \
			if (sBailLog < 12) { sBailLog++; \
				LogInfo("VRPose fire: redirect skipped - %s\n", why); } \
			return; \
		} while (0)

		float camPos[3], camFwd[3];
		if (!ReadCameraVec(0x180, camPos) || !ReadCameraVec(0x170, camFwd))
			REDIRECT_BAIL("camera vectors unreadable");

		// Shot-basis diagnostic: compare the engine camera heading with the
		// rendered VR basis and the tracked 6DOF displacement at the exact fire
		// call. This is read-only and does not participate in shot composition.
		static const bool kShotBasisDiagnostic = false;
		if (kShotBasisDiagnostic) {
			float rendered[9] = {};
			float headDelta[3] = {};
			const bool renderedValid = GetCurrentRenderedViewBasis(rendered);
			const bool deltaValid = GetHeadTranslationDelta(headDelta);
			static unsigned sBasisLogged = 0;
			if (sBasisLogged < 160) {
				sBasisLogged++;
				const float camYaw = atan2f(camFwd[0], camFwd[2]);
				const float camPitch = asinf(camFwd[1] < -1.0f ? -1.0f
					: (camFwd[1] > 1.0f ? 1.0f : camFwd[1]));
				const float renderYaw = renderedValid ? atan2f(rendered[6], rendered[8]) : 0.0f;
				const float renderPitch = renderedValid ? asinf(rendered[7] < -1.0f ? -1.0f
					: (rendered[7] > 1.0f ? 1.0f : rendered[7])) : 0.0f;
				LogInfo("VRPose fire: BASIS cam=(yaw %+7.2f pitch %+7.2f) rendered=(yaw %+7.2f pitch %+7.2f valid=%d) "
					"delta=(%.4f %.4f %.4f valid=%d)\n",
					camYaw * 57.29578f, camPitch * 57.29578f,
					renderYaw * 57.29578f, renderPitch * 57.29578f,
					renderedValid ? 1 : 0,
					deltaValid ? headDelta[0] : 0.0f,
					deltaValid ? headDelta[1] : 0.0f,
					deltaValid ? headDelta[2] : 0.0f,
					deltaValid ? 1 : 0);
			}
		}

		float nativeDir[3] = { 0, 0, 0 };
		bool nativeDirValid = false;
		float nativeTarget[3] = { 0, 0, 0 };
		float nativeTargetRange = 0.0f;
		{
			SIZE_T nativeGot = 0;
			if (ReadProcessMemory(GetCurrentProcess(), (void *)(objPtr + 0x0F0),
				nativeDir, sizeof(nativeDir), &nativeGot) && nativeGot == sizeof(nativeDir)) {
				const float nl = sqrtf(nativeDir[0] * nativeDir[0]
					+ nativeDir[1] * nativeDir[1] + nativeDir[2] * nativeDir[2]);
				if (isfinite(nl) && nl > 0.5f && nl < 1.5f) {
					nativeDir[0] /= nl;
					nativeDir[1] /= nl;
					nativeDir[2] /= nl;
					nativeDirValid = true;
				}
			}
			if (ReadProcessMemory(GetCurrentProcess(), (void *)(objPtr + 0x150),
				nativeTarget, sizeof(nativeTarget), &nativeGot) && nativeGot == sizeof(nativeTarget)) {
				const float tx = nativeTarget[0] - camPos[0];
				const float ty = nativeTarget[1] - camPos[1];
				const float tz = nativeTarget[2] - camPos[2];
				nativeTargetRange = sqrtf(tx * tx + ty * ty + tz * tz);
				if (!isfinite(nativeTargetRange) || nativeTargetRange < 0.5f
					|| nativeTargetRange > 10000.0f)
					nativeTargetRange = 0.0f;
			}
		}

		float dist = 0.0f;
		SIZE_T got = 0;
		if (!ReadProcessMemory(GetCurrentProcess(), (void *)(objPtr + 0x178),
			&dist, sizeof(dist), &got) || got != sizeof(dist))
			REDIRECT_BAIL("object distance field unreadable");
		if (!isfinite(dist) || dist < 0.5f || dist > 10000.0f)
			dist = 20.0f;

		// RAY ORIGIN PROBE.
		//
		// The engine traces from the camera and we can only move the target,
		// so rounds leave the eye rather than the muzzle - the one remaining
		// structural error, and the reason this needs zeroing at all rather
		// than just pointing the ray down the barrel.
		//
		// +0x120 held the camera position exactly in the first captured
		// object (31.3420, 0.0282, 13.9695 against a camera at 31.4295,
		// 1.8392, 13.8983 - X and Z match, Y does not, so it may be a
		// muzzle/chest point rather than the eye). If it TRACKS the camera
		// as the player moves, it is the ray origin, and writing the muzzle
		// there makes the traced ray exactly the barrel line at every range.
		//
		// Log the candidate against the camera before writing anything: a
		// field that merely happened to look right once is exactly the kind
		// of thing that has cost test cycles here already.
		{
			// Disassembly narrowed WHERE to look. The wrapper's 5th argument
			// is &obj+0x150 - the target, which we already control - and it
			// is that vector the descriptor picks up. So the origin is not on
			// that path at all. But the same call also passes &obj+0x80, and
			// a run of fields from there is copied into the descriptor, and
			// obj+0x80 is a region no probe has ever looked at: the earlier
			// field dump only covered 0x100..0x190.
			//
			// Sweep it against the live camera. A vector that tracks the
			// camera through this block is the ray origin, and unlike the
			// callee's locals it sits in the object, one instruction before
			// the engine consumes it - i.e. writable from right here.
			// WIDE sweep: 0x00..0x300, not the 0x60..0x110 window used before.
			//
			// +0x0E0 tracked the camera and +0x0F0 was a unit vector beside
			// it - a textbook origin/direction pair - but writing them moved
			// nothing across 424 shots, so they are not what the trace
			// consumes. The pair may simply be a copy, with the real one
			// living outside the window anyone has looked at. Cheap to rule
			// out, and the same method that found the last pair.
			//
			// Only two shots' worth is logged: this is 48 lines per shot and
			// the point is the pattern, not the volume.
			static int sOriginProbeLogged = 0;
			if (sOriginProbeLogged < 2) {
				sOriginProbeLogged++;
				LogInfo("VRPose fire:   SWEEP cam=(%.3f %.3f %.3f) camFwd=(%.3f %.3f %.3f)\n",
					camPos[0], camPos[1], camPos[2], camFwd[0], camFwd[1], camFwd[2]);
				for (DWORD_PTR off = 0x00; off < 0x300; off += 0x10) {
					float v[4] = { 0, 0, 0, 0 };
					SIZE_T got = 0;
					if (!ReadProcessMemory(GetCurrentProcess(), (void *)(objPtr + off),
							v, sizeof(v), &got) || got != sizeof(v))
						continue;
					const float d = fabsf(v[0] - camPos[0]) + fabsf(v[1] - camPos[1])
						+ fabsf(v[2] - camPos[2]);
					const float len = sqrtf(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
					// Also flag anything pointing along the camera's forward:
					// the direction half of an origin/direction pair is what
					// the trace steers by, and a unit vector that AGREES with
					// camFwd is a far stronger candidate than one that merely
					// has length 1.
					const float dotF = (len > 1e-4f)
						? (v[0]*camFwd[0] + v[1]*camFwd[1] + v[2]*camFwd[2]) / len : 0.0f;
					const char *note = (d < 0.6f) ? " <== TRACKS CAMERA"
						: (fabsf(len - 1.0f) < 0.02f && dotF > 0.99f) ? " <== CAMERA FORWARD"
						: (fabsf(len - 1.0f) < 0.02f) ? " <== unit vector" : "";
					LogInfo("VRPose fire:     +0x%03llX = %10.3f %10.3f %10.3f %10.3f  "
						"dCam=%8.3f%s\n",
						(unsigned long long)off, v[0], v[1], v[2], v[3], d, note);
				}
			}
		}

		// Aim where the controller points.
		//
		// The camera's own direction is the player's BODY heading - it moves
		// when they turn with the thumbstick, and never because of the head,
		// since the head rotation is applied only to what we render and the
		// engine never learns about it. So offsetting it by how far the
		// controller has moved from its anchor gives the direction the gun is
		// pointing in world space.
		float dYaw = 0.0f, dPitch = 0.0f;
		if (!GetAimOffsetFromController(&dYaw, &dPitch))
			REDIRECT_BAIL("no controller aim (settling, or reference not captured)");

		// The CAMERA heading - the frame the weapon is actually drawn in.
		//
		// Subtracting the culling injection to get the body heading has been
		// tried twice and is wrong both times: the weapon's transform carries
		// the game's view, so the gun points along the camera, and the aim
		// has to be expressed in the same frame or the two disagree by the
		// injection (~7 degrees, and it moves with the head).
		// Take the injection back out, so the aim is in the frame the gun is
		// DRAWN in rather than the frame the engine's camera is pointing in.
		//
		// The camera carries the culling injection - the yaw and pitch we
		// continuously feed the engine so its frustum follows the player's
		// head. The rendered view has that cancelled, and since the view
		// correction is folded into the projection the weapon is drawn in the
		// corrected frame too. So the gun points along (body + controller)
		// while this was aiming along (body + injection + controller): the two
		// differ by the injection, which tracks the head and lags it.
		//
		// Symptom that identified it: the zero holds at one heading and drifts
		// as the player turns to shoot, then comes back on returning. A
		// constant zeroing offset cannot do that; a head-tracking term can.
		//
		// This was tried twice before and reverted, but that predates folding
		// the correction into the projection - at the time the weapon really
		// did carry the game's view, and subtracting here was wrong. The frame
		// the gun lives in has changed since.
		static const bool kAimInBodyFrame = true;
		const float injYaw = kAimInBodyFrame ? GetCullingCancellation() : 0.0f;
		const float injPitch = kAimInBodyFrame ? GetCullingCancellationPitch() : 0.0f;

		const float baseYaw = atan2f(camFwd[0], camFwd[2]) - injYaw;
		const float clampedY = camFwd[1] < -1.0f ? -1.0f : (camFwd[1] > 1.0f ? 1.0f : camFwd[1]);
		const float basePitch = asinf(clampedY) - injPitch;

		// Fixed correction for where the barrel sits relative to the
		// controller you are holding.
		//
		// Sign established live rather than derived: +0.026 was tried on the
		// assumption that positive yaw aims right, and shots moved further
		// LEFT. So positive is left here, and correcting a leftward error
		// needs a negative value.
		// Both zero now that the shot traces from the gun: the errors these
		// were compensating for were parallax, which the geometry above
		// handles correctly at every range. Any residual after testing is a
		// genuine barrel-to-controller offset and belongs here.
		// BAKED-IN SIGHT ZEROING, measured in the headset 2026-08-09.
		//
		// Dialled in with the rotation calibration mode until impacts sat on
		// the sights, then read from its log line: pitch -13.06 deg, yaw
		// +27.09 deg. Equivalent to the live nudge, since sWeaponRotAdjust is
		// added to ctrlYaw/ctrlPitch inside GetControllerAim and these are
		// added to the same sum here - so putting it here makes it permanent
		// while leaving the calibration mode free for future tuning.
		//
		// This is only meaningful because the aim finally has a FIXED
		// reference: absolute pitch and yaw rather than a per-session anchor,
		// and the barrel-line target removing the range-dependent parallax
		// term. Measured before those, the same procedure gave 10.7, -1.2 and
		// 25.6 degrees on three attempts, because it was chasing a moving
		// target.
		//
		// The size is worth noting rather than glossing: 27 degrees of yaw is
		// not a small residual. It says the barrel axis we derive from the
		// controller - the hard-coded 64.6 degree grip tilt in
		// GetControllerAim - is itself off by a lot. This cancels the error
		// where it lands; correcting that tilt would remove the cause. Worth
		// revisiting, but not at the price of a working aim.
		// Refined once in a second pass: the calibration mode stacks ON TOP of
		// these, so its logged value is the remaining delta. That pass gave
		// pitch +5.29, yaw 0.00 - so yaw was already right (a good sign the
		// 27.09 is a real constant rather than a lucky fit) and pitch moved
		// to -13.06 + 5.29 = -7.77.
		// Zero now: the measured zeroing moved to the controller frame, where
		// it belongs (kBarrelPitchBias/kBarrelYawBias in GetControllerAim).
		// Kept as a hook for any correction that genuinely IS in world angle
		// space, should one ever turn up.
		const float kAimBiasYaw = 0.0f;
		const float kAimBiasPitch = 0.0f;

		// COMPOSE THE ROTATIONS. Do not add Euler angles.
		//
		// This used to be aimYaw = baseYaw + dYaw, aimPitch = basePitch +
		// dPitch, with the deltas coming from GetAimOffsetFromController as
		// plain subtractions of yaw and pitch. Rotations do not compose by
		// adding their Euler components: the result is exact only when the
		// offset is zero and the error grows with it.
		//
		// The weapon does it properly - the renderer composes a full rotation
		// matrix - so the gun and the aim agreed at the anchor and diverged
		// everywhere else. That is precisely the reported behaviour: accurate
		// at the spot it was zeroed, worse the further the controller moves
		// from it, and correct again on returning. Turning with the stick was
		// unaffected because that only moves the body heading, which both
		// sides share.
		//
		// Instead: express the controller's direction relative to its anchor
		// as a direction vector, then rotate that by the body's orientation.
		// At the anchor the relative direction is +Z and the result is exactly
		// the body direction, as before - but it stays correct at any offset.
		float ctrlYaw = 0.0f, ctrlPitch = 0.0f;
		if (!GetControllerAim(&ctrlYaw, &ctrlPitch))
			REDIRECT_BAIL("controller aim unavailable at composition");

		// The body contributes YAW ONLY. Elevation comes from the hand.
		//
		// Composing through the body's pitch as well tilted the frame the
		// controller's offset was expressed in, and a tilted frame mixes the
		// axes. Measured with body pitch at -18 degrees: pitch came through at
		// 0.99 of the input while yaw over-responded by 66% (+6.6 in, +11.0
		// out). Pitch survived because it is the axis the tilt is about; yaw
		// did not.
		//
		// Yaw-only is also the right model for a motion shooter: where the
		// player is facing sets the heading, and where they point their hand
		// sets everything else. The camera's own pitch is the engine's
		// business - it is driven by our culling injection - and should not
		// steer the shot.
		//
		// Plain angle arithmetic is safe here now. It was not before, when the
		// controller's axis read 84 degrees up and sat near gimbal lock; with
		// the axis corrected the offsets are small and well-conditioned, and
		// this form has no cross-axis coupling at all.
		// ELEVATION IS ABSOLUTE, NOT ANCHOR-RELATIVE.
		//
		// aimPitch used dPitch = ctrlPitch - sAnchorCtrlPitch, and that anchor
		// is captured ONCE at startup from wherever the controller happened to
		// be pointing after a 30-frame settle ("this direction is now straight
		// ahead"). Elevation was therefore measured against an arbitrary
		// snapshot: if the hand was tilted down when it anchored, holding the
		// gun down reads as level and the shot flies horizontally while the
		// barrel points at the floor.
		//
		// Measured live, which is what identified it: ourTgt.Y sat at the
		// camera's own height on EVERY shot - dir[1] ~ 0 regardless of aim -
		// while the engine's own target was down at ground level, i.e. the
		// player was plainly aiming below the horizon. Reported as "bullets go
		// well above where I'm shooting".
		//
		// Yaw genuinely needs the anchor: it is an offset from where the body
		// faces, and the body's heading is the engine's. Pitch has no such
		// reference - there is no body pitch to offset from - so the
		// controller's own pitch IS the elevation, absolutely. Any constant
		// error left in it belongs to the grip-axis tilt in GetControllerAim
		// and is one number to calibrate, not a per-session accident.
		// YAW IS ABSOLUTE TOO, for the same reason pitch is.
		//
		// aimYaw was baseYaw + (ctrlYaw - sAnchorCtrlYaw): the body's heading
		// plus the controller's offset from an anchor captured at whatever
		// moment the session happened to anchor. That anchor is an arbitrary
		// per-session constant, which is why the redirect point moved every
		// time we tested - reported directly, and it also quietly makes any
		// calibration worthless, since a zeroing number measured against one
		// session's accident is wrong on the next launch.
		//
		// It is also redundant. ctrlYaw is already expressed in the shared
		// tracking reference, and baseYaw is the body heading in game space
		// with our own culling injection removed - the same reference the
		// head delta is measured against. So baseYaw + ctrlYaw is already
		// "where the gun points, in the world", and the anchor was an extra
		// offset layered on a relationship that was correct without it.
		// Physically: turning the body with the stick moves baseYaw, turning
		// the head moves neither term, and moving the hand moves ctrlYaw -
		// exactly the behaviour wanted.
		//
		// Scoped to the SHOT only, as the absolute-pitch change was. The
		// camera-steering loop uses sAnchorCtrlYaw for a different job - a
		// clamped follow offset - and it is heavily tuned, so it is left
		// alone rather than disturbed for a fix aimed at where rounds land.
		static const bool kAbsoluteAimPitch = true;
		static const bool kAbsoluteAimYaw = true;
		// sShotAimBias is the LIVE zeroing nudge, applied in the same place as
		// the baked constants so the two are the same quantity - dial it in,
		// read it out, add it to the constant above. No transplant error.
		// No bias terms here any more. The zeroing moved into GetControllerAim,
		// where it rotates the barrel axis in the controller's own frame -
		// see kBarrelPitchBias/kBarrelYawBias. Applying it in angle space as
		// well would double it, and applying it here at all is what made the
		// correction elevation-dependent in the first place.
		const float aimYaw = baseYaw + (kAbsoluteAimYaw ? ctrlYaw : dYaw) + kAimBiasYaw;
		float aimPitch = (kAbsoluteAimPitch ? ctrlPitch : dPitch) + kAimBiasPitch;
		if (aimPitch > 1.4f) aimPitch = 1.4f;
		if (aimPitch < -1.4f) aimPitch = -1.4f;

		const float acp = cosf(aimPitch);
		float dir[3] = {
			sinf(aimYaw) * acp,
			sinf(aimPitch),
			cosf(aimYaw) * acp,
		};

		// AIM DOWN THE GUN'S OWN BARREL.
		//
		// The Euler form above is derived from the controller through a
		// hard-coded 64.6 degree grip-tilt assumption, plus bias constants to
		// patch up the difference. That could never be stable: measurement
		// showed the discrepancy between this direction and the DRAWN gun's
		// barrel is a fixed rotation of ~15.7 degrees (14.7-16.3 across a wide
		// spread of elevations and wrist angles), but its yaw/pitch
		// DECOMPOSITION swung from dYaw -16.4/dPitch -0.7 to dYaw +0.9/dPitch
		// -15.2 depending on orientation. Two Euler constants cannot represent
		// one fixed rotation across orientations, which is exactly why three
		// calibration passes oscillated instead of converging.
		//
		// The weapon transform already knows the answer. The viewmodel lives
		// in view space with its rest pose down view +Z, so the barrel after
		// the mod's rotation D is D's third column; the view matrix rows are
		// the camera basis in world, so that converts straight to world space.
		// Aiming along it makes the sights and the shot agree BY CONSTRUCTION,
		// with no constant to tune and nothing to re-measure when grip posture
		// changes.
		//
		// Falls through to the Euler direction if the weapon transform is not
		// available yet (before the viewmodel is first seen), so a shot in
		// that window behaves as it did rather than going somewhere random.
		static const bool kAimFromWeaponModel = true;
		if (kAimFromWeaponModel && G->vrCachedViewValid) {
			float bv[3] = { 0.0f, 0.0f, 1.0f };
			if (GetCanonicalBarrelViewDirection(bv)) {
				// The rest-pose axis, corrected - and corrected in MODEL space,
				// before D, which is where the error actually lives.
				//
				// Taking D's third column assumes the viewmodel points exactly
				// down view +Z at rest. Live result: both weapons shot slightly
				// HIGH by the same amount, which is that assumption being a
				// little off rather than anything per-weapon.
				//
				// Applying the correction here rather than to the finished
				// world direction is the whole point. The offset is fixed
				// relative to the MODEL, so rotating the model-space axis keeps
				// it fixed no matter how the gun is held. Correcting afterwards
				// would reintroduce exactly the posture dependence that made
				// three controller-space calibrations oscillate.
				//
				// sShotAimBias is added live so the zeroing mode still tunes
				// the value that now matters - it drives this, not the retired
				// Euler path.
				// Measured 2026-08-11, and the MAGNITUDE is the point: the old
				// controller-space constants needed ~26 degrees, this needs
				// ~3.4. A trim that small is a genuine model-space offset -
				// the viewmodel's rest axis being slightly off view +Z - not a
				// constant cancelling a structural error. One calibration
				// covered BOTH weapons, which is the other thing a shared
				// model-space cause predicts and a per-weapon one does not.
				float currentView[9];
				float bw[3] = {};
				if (GetCurrentRenderedViewBasis(currentView)) {
					// Compact 3x3: right/up/forward occupy rows 0/1/2.
					bw[0] = bv[0]*currentView[0] + bv[1]*currentView[3] + bv[2]*currentView[6];
					bw[1] = bv[0]*currentView[1] + bv[1]*currentView[4] + bv[2]*currentView[7];
					bw[2] = bv[0]*currentView[2] + bv[1]*currentView[5] + bv[2]*currentView[8];
				} else {
					// Cached 3x4: the same rows have a four-float stride.
					const float *v = G->vrCachedView3x4;
					bw[0] = bv[0]*v[0] + bv[1]*v[4] + bv[2]*v[8];
					bw[1] = bv[0]*v[1] + bv[1]*v[5] + bv[2]*v[9];
					bw[2] = bv[0]*v[2] + bv[1]*v[6] + bv[2]*v[10];
				}
				const float bl = sqrtf(bw[0]*bw[0] + bw[1]*bw[1] + bw[2]*bw[2]);
				if (bl > 1e-4f && isfinite(bl)) {
					dir[0] = bw[0] / bl;
					dir[1] = bw[1] / bl;
					dir[2] = bw[2] / bl;
				}
			}
		}
		const float barrelDirBeforeSpread[3] = { dir[0], dir[1], dir[2] };
		float nativeSpreadYawApplied = 0.0f;
		float nativeSpreadPitchRaw = 0.0f;

		// Preserve the engine-generated angular deviation.  The old version
		// used +0x0F0 as the native direction, but the live captures proved
		// that field is not in the same frame as camFwd: its apparent yaw was
		// clamped to +/-2 degrees on most shots and its apparent pitch often
		// differed by 20-50 degrees.  Applying that difference injected a
		// false sideways error into otherwise good VR shots.
		//
		// +0x150 is the engine's completed hit point, so its direction from
		// camPos is the native shot direction after Metro has applied its own
		// spread/recoil.  Use that direction instead.  The large-deviation
		// guard excludes the duplicate/invalid fire records already visible in
		// the PAIR diagnostics; normal native spread/recoil is far smaller.
		if (kPreserveNativeShotDeviation && nativeTargetRange > 0.5f) {
			float nativeAimDir[3] = {
				nativeTarget[0] - camPos[0],
				nativeTarget[1] - camPos[1],
				nativeTarget[2] - camPos[2]
			};
			const float nativeAimLen = sqrtf(nativeAimDir[0] * nativeAimDir[0]
				+ nativeAimDir[1] * nativeAimDir[1]
				+ nativeAimDir[2] * nativeAimDir[2]);
			if (nativeAimLen > 0.5f && isfinite(nativeAimLen)) {
				nativeAimDir[0] /= nativeAimLen;
				nativeAimDir[1] /= nativeAimLen;
				nativeAimDir[2] /= nativeAimLen;
				const float nativeYaw = atan2f(nativeAimDir[0], nativeAimDir[2]);
				const float nativePitch = asinf(nativeAimDir[1] < -1.0f ? -1.0f
					: (nativeAimDir[1] > 1.0f ? 1.0f : nativeAimDir[1]));
			const float cameraYaw = atan2f(camFwd[0], camFwd[2]);
			const float cameraPitch = asinf(camFwd[1] < -1.0f ? -1.0f
				: (camFwd[1] > 1.0f ? 1.0f : camFwd[1]));
			const float rawSpreadYaw = WrapAngle(nativeYaw - cameraYaw);
			const float rawSpreadPitch = nativePitch - cameraPitch;
			nativeSpreadPitchRaw = rawSpreadPitch;
			const float nativeDelta = sqrtf(rawSpreadYaw * rawSpreadYaw
				+ rawSpreadPitch * rawSpreadPitch);
			if (isfinite(nativeDelta) && nativeDelta < 0.2617994f) { // < 15 deg
				nativeSpreadYawApplied = rawSpreadYaw;
				const float preservedPitch = asinf(dir[1] < -1.0f ? -1.0f
					: (dir[1] > 1.0f ? 1.0f : dir[1])) + rawSpreadPitch;
				const float preservedYaw = atan2f(dir[0], dir[2]) + rawSpreadYaw;
				const float pc = cosf(preservedPitch);
				dir[0] = sinf(preservedYaw) * pc;
				dir[1] = sinf(preservedPitch);
				dir[2] = cosf(preservedYaw) * pc;
			}
			}
		}

		// Trace from the GUN, not from the eye.
		//
		// The engine's shot starts at the camera, which sits ~20cm above and
		// slightly to the side of where we draw the weapon. A ray from the
		// eye along the gun's direction therefore passes above the barrel
		// line, and rounds land high - exactly the reported "bullets shoot
		// above the gun". No angular bias fixes that honestly, because the
		// error depends on range: it is parallax, not misalignment.
		//
		// We know where the gun is in view space, and the camera object
		// gives us the basis to put it in world space. Aiming from there
		// makes the shot follow the barrel at every distance.
		// Aim from the CAMERA, because that is where the engine traces from.
		//
		// The previous attempt put the target on a line starting at the gun,
		// reasoning that a shot should leave the barrel. But we can only
		// move the target point - the engine still starts its ray at the
		// camera - so the effective direction became
		// (gunOrigin + dir*dist) - camPos, which depends on dist. And dist
		// is the engine's own per-shot value: measured live at 9.6, 8.9,
		// 8.4, 9.6 on consecutive shots. So the aim wandered from shot to
		// shot, which is what "inconsistent, and different at the range
		// versus a nearby wall" was.
		//
		// Building the target from camPos makes the camera-to-target
		// direction exactly the gun's direction, whatever dist happens to
		// be. The cost is that rounds leave the eye rather than the muzzle -
		// a fixed ~20cm offset, and the honest trade until the engine's own
		// ray origin can be moved.
		// ZERO THE SIGHTS: put the target on the BARREL line, at a fixed range.
		//
		// The engine always starts its ray at the camera; we can only choose
		// where it ends. Ending it at camPos + dir*d makes the shot leave the
		// EYE along the gun's direction - parallel to the barrel but offset by
		// however far the gun is from the head. Looking down the sights hides
		// that, because the eye is on the sight line; holding the gun anywhere
		// else does not, and the error grows with the offset. That is the
		// "less accurate as I move it around".
		//
		// Ending it on the barrel line instead makes the camera-to-target ray
		// cross that line at the chosen range - exactly how a real rifle is
		// zeroed: correct at the zero distance, slightly off nearer and
		// further, and independent of where the gun is held.
		//
		// USE THE ENGINE'S OWN DISTANCE, not a fixed zero range.
		//
		// A fixed 15 m was tried and broke the impact effect: the engine
		// evidently spawns the hit decal at the target point we write, so
		// placing that point 15 m out put the decal behind a wall three metres
		// away - invisible - while damage still landed through the engine's
		// own trace. Reported exactly that way: things break, no impact mark.
		//
		// The +0x178 field is almost certainly the engine's own hit distance.
		// Using it is not a compromise, it is strictly better: the target then
		// sits where the barrel line ACTUALLY meets geometry, so the shot is
		// correct at every range instead of only at one zeroed distance.
		//
		// This was tried once before and reverted for making the aim wander,
		// because the direction from the camera to a barrel-line point does
		// depend on the distance. But that attempt predates two real bugs
		// found since: the controller's aim axis reading 84 degrees off (grip
		// axis rather than barrel), and the body's pitch being composed into
		// the offset frame. Either alone produced far more wander than this
		// term can, and both are fixed - so the earlier verdict was measuring
		// something else.
		// Prefer the distance to Metro's already-computed native hit point.
		// +0x178 is a preparation/trace-distance field and is frequently
		// exactly 20.0 in live captures; using it as our replacement endpoint
		// imposed an artificial ~20 m invisible wall.  +0x150 is read before
		// we overwrite it above, so its distance is the engine's actual hit
		// distance and can be retained while changing only the shot direction.
		const float shotRange = nativeTargetRange > 0.5f
			? nativeTargetRange : dist;

		// The gun's world position: its offset from the head, rotated by the
		// body's heading. Pitch is deliberately excluded, for the same reason
		// it is excluded from the aim - the camera's pitch is the engine's
		// business and should not move the shot.
		float gunOrigin[3] = { camPos[0], camPos[1], camPos[2] };
		float shotHeadDeltaY = 0.0f;
		{
			// Match the shot origin to the rendered 6DOF eye position. Metro's
			// camera remains at the body position, so without this translation
			// lean/step shots originate behind the view.
			float headDelta[3] = { 0, 0, 0 };
			if (GetHeadTranslationDelta(headDelta)) {
				shotHeadDeltaY = headDelta[1];
				const float headYaw = atan2f(camFwd[0], camFwd[2]);
				const float hc = cosf(headYaw), hs = sinf(headYaw);
				gunOrigin[0] += hc * headDelta[0] + hs * headDelta[2];
				gunOrigin[1] += headDelta[1];
				gunOrigin[2] += -hs * headDelta[0] + hc * headDelta[2];
			}

			float rel[3];
			if (GetControllerRelativeToHead(rel)) {
				// Convert through the same rendered view basis used for the
				// canonical barrel direction.  Rotating this position by Metro's
				// engine-camera yaw leaves the written origin in a different frame
				// after a physical body turn, even though the rendered gun and shot
				// direction have already followed the VR view.
				//
				// rel is the controller measured in the HEAD's frame, so the
				// rows of the rendered view convert it into world space.
				//
				if (G->vrCachedViewValid) {
					const float *v = G->vrCachedView3x4;
					gunOrigin[0] += rel[0]*v[0] + rel[1]*v[4] + rel[2]*v[8];
					gunOrigin[1] += rel[0]*v[1] + rel[1]*v[5] + rel[2]*v[9];
					gunOrigin[2] += rel[0]*v[2] + rel[1]*v[6] + rel[2]*v[10];
				} else {
					const float camYaw = atan2f(camFwd[0], camFwd[2]);
					const float c = cosf(camYaw), s = sinf(camYaw);
					gunOrigin[0] += c * rel[0] + s * rel[2];
					gunOrigin[1] += rel[1];
					gunOrigin[2] += -s * rel[0] + c * rel[2];
				}
			}
		}

		// BACK TO THE CAMERA, not the gun's barrel line.
		//
		// The "zero the sights at shotRange" model (building the target
		// from gunOrigin) was chosen to fix the impact-effect-vanishing
		// bug, and does make the camera-to-target ray cross the barrel
		// line at the chosen range - but the engine ALWAYS traces from
		// camPos, never from gunOrigin, so any range other than the exact
		// zero distance leaves a real angular gap between the intended
		// aim direction and the direction the engine's ray actually
		// travels. Measured directly (aimVsDamage in the fire log):
		// consistently 1.5-3.2 degrees across a live session, growing
		// specifically at SHORTER range - about 20-30cm of miss at the
		// 5-6m distances tested. Systematic, not noise: it tracks
		// gunOffset/shotRange exactly as the geometry predicts.
		//
		// Building the target from camPos instead makes target - camPos
		// exactly dir * shotRange - mathematically identical to the aim
		// direction, zero angular error at any range. The cost is the
		// shot conceptually leaves the eye rather than the barrel - a
		// small, CONSTANT offset (not one that grows at close range like
		// the bug just fixed), and one aiming down the sights already
		// mostly hides, since the eye sits on the sight line anyway.
		float target[3] = {
			camPos[0] + dir[0] * shotRange,
			camPos[1] + shotHeadDeltaY + dir[1] * shotRange,
			camPos[2] + dir[2] * shotRange,
		};

		// PUT THE TARGET ON THE BARREL LINE, AT THE ENGINE'S OWN HIT DEPTH.
		//
		// Building it from camPos makes the traced ray exactly parallel to
		// the barrel but offset by however far the gun is from the eye -
		// measured live at 0.42..0.50 m. That offset is CONSTANT in world
		// space, so as an angle it is ~8 deg at 3 m and under 1 deg at 30 m,
		// and no fixed correction can cancel it at more than one range. That
		// is exactly why zeroing produced 10.7, -1.2 and 25.6 degrees on
		// three attempts at three distances: it was chasing parallax with a
		// constant.
		//
		// The engine only lets us choose where the ray ENDS (the origin field
		// it actually uses is not reachable - proven today), so choose that
		// endpoint on the BARREL line. The camera-to-target ray then crosses
		// the barrel line exactly at the target, and the round lands where
		// the barrel points, at any range, with no constant to tune.
		//
		// The depth is chosen so the target sits at the same distance ALONG
		// THE VIEW AXIS as the engine's own hit - not simply gunOrigin +
		// dir*shotRange. That earlier form put the point on the barrel line
		// but at the wrong depth, so it could land short of or beyond the
		// surface - and since the impact effect spawns at the target, that
		// is what produced hits on an "invisible wall" in mid-air. Matching
		// the depth keeps the point on the surface while still being on the
		// barrel line.
		// RANGE-matched, not depth-matched.
		//
		// Matching the engine's hit DEPTH along the view axis assumed the
		// surface was roughly perpendicular to the view. Floors and ceilings
		// are grazing, and the divisor (dir . camFwd) shrinks as the aim
		// tilts, inflating the distance and putting the point past the real
		// surface - more so the steeper the shot, and symmetrically either
		// side of level. Live symptom: shooting DOWN landed above the sights
		// and shooting UP landed below, i.e. elevation compressed. That was
		// introduced by the depth term, not present before it.
		//
		// Matching the engine's hit RANGE instead keeps the target the right
		// DISTANCE from the gun, which holds whatever angle the surface sits
		// at. A sphere is a better approximation of "where the wall is" than
		// a plane perpendicular to the view, precisely because it makes no
		// assumption about the surface's orientation.
		// OFF - and the measurement says it cannot work.
		//
		// Barrel-line targeting needs the distance to the surface ALONG THE
		// BARREL. The only range available is shotRange, which the engine
		// measured along its OWN camera ray - and that ray is nowhere near
		// the barrel. Measured live: the camera sits 23-32 degrees off the
		// hand, and saturates around +20.7 degrees looking up while the hand
		// reaches +47.6. Placing the target that foreign distance along the
		// barrel overshoots or undershoots depending on which way the two
		// diverge, oppositely for up versus down - exactly the reported
		// asymmetry, and it appeared when this was switched on.
		//
		// So keep the target on the CAMERA ray: target = camPos + dir*range.
		// The traced direction is then exactly dir - our aim, which the same
		// measurement confirms is correct to the hundredth of a degree
		// (ctrlPitch == aimPitch == dirPitch on every shot) - and it is
		// independent of where the camera happens to point, so elevation
		// cannot skew it. The residual is the parallax of firing from the eye
		// rather than the muzzle: a CONSTANT world offset, small while the
		// gun is near the sight line, and the thing a single zeroing constant
		// can legitimately absorb.
		static const bool kBarrelLineTarget = false;
		if (kBarrelLineTarget) {
			const float endpointRange = nativeTargetRange > 0.0f
				? nativeTargetRange : shotRange;
			target[0] = camPos[0] + dir[0] * endpointRange;
			target[1] = camPos[1] + dir[1] * endpointRange;
			target[2] = camPos[2] + dir[2] * endpointRange;
		}

		// What the ENGINE intended, before we touch it. Read here so the log
		// can show the original target beside ours - if the engine's own
		// point is where the rounds actually land, our write is not reaching
		// the path that matters, and that is a different problem from the
		// write being wrong.
		float engineTarget[3] = { 0, 0, 0 };
		{
			SIZE_T got = 0;
			ReadProcessMemory(GetCurrentProcess(), (void *)(objPtr + 0x150),
				engineTarget, sizeof(engineTarget), &got);
		}

		// ELEVATION DIAGNOSTIC.
		//
		// Three derived fixes have failed to remove the up/down asymmetry, so
		// measure the chain instead of reasoning about it a fourth time.
		// Everything that could compress or flip elevation, in one line:
		//
		//   ctrlPitch : where the hand actually points (absolute)
		//   aimPitch  : what we decided to aim
		//   dirPitch  : the direction about to be written
		//   camPitch  : where the GAME's camera points - the ray the engine
		//               measured shotRange along. The culling follow CLAMPS
		//               how far that camera pitches, so on a steep shot it
		//               points far less steeply than the gun. Then shotRange
		//               belongs to a different ray than the barrel, and
		//               placing the target that far along the barrel
		//               overshoots - oppositely for up versus down, which is
		//               precisely the reported symptom.
		{
			const float kDeg = 57.29578f;
			const float dp = asinf(dir[1] < -1.0f ? -1.0f : (dir[1] > 1.0f ? 1.0f : dir[1]));
			const float cpv = camFwd[1] < -1.0f ? -1.0f : (camFwd[1] > 1.0f ? 1.0f : camFwd[1]);
			const float cp2 = asinf(cpv);
			static int sElevLogged = 0;
			if (sElevLogged < 80) {
				sElevLogged++;
				LogInfo("VRPose fire:   ELEV ctrlPitch=%+7.2f aimPitch=%+7.2f dirPitch=%+7.2f "
					"camPitch=%+7.2f range=%6.2f\n",
					ctrlPitch * kDeg, aimPitch * kDeg, dp * kDeg, cp2 * kDeg, shotRange);
			}
		}

		SIZE_T wrote = 0;

		// WRITE THE ACTUAL RAY, not just its endpoint.
		//
		// The sweep found the pair the whole hunt was for:
		//   +0x0E0 = position, w=1, tracking the camera to within 6 cm
		//   +0x0F0 = unit vector, w=0
		// an origin and a direction, sitting in the object one instruction
		// before the engine consumes them - so both are writable from here.
		//
		// This explains BOTH live symptoms at once, which is what makes it
		// convincing rather than merely plausible. Damage traced along
		// +0x0F0 - the camera's forward - so rounds landed where the flat
		// game's reticle sits, while the decal spawned at the +0x150 target
		// we rewrite, which is why impacts appeared on an invisible wall
		// whenever that point sat in mid-air. Damage and visuals were
		// following two different rays.
		//
		// Writing origin AND direction makes the shot leave the muzzle along
		// the barrel: correct at every range, independent of how the gun is
		// held, with no zeroing constant and no eye-parallax. That retires
		// the "rounds leave the eye" trade the target-only path could never
		// escape - see the comment above, kept as the record of why the
		// endpoint alone was not enough.
		// RESULT: no visible change. Bullet holes stayed exactly where they
		// were, at the flat game's reticle, across 424 redirected shots with
		// both fields written every time. So this pair is NOT what the engine
		// traces with at this point in the chain - either it is consumed
		// before the wrapper runs, or it feeds a different subsystem.
		//
		// Turned off rather than deleted: the sweep that found it is solid
		// (a w=1 position tracking the camera to 6 cm, beside a w=0 unit
		// vector, consistently over 40 shots), so this is worth retrying if
		// the chain is ever picked up further downstream. Left ON it writes
		// two fields the engine actively uses for no benefit, which is a
		// risk with no upside.
		// RETRY, and the earlier negative result does not stand.
		//
		// This was tested when the aim was still metres off - before absolute
		// pitch and yaw - and moving the ray origin only shifts the shot by
		// the gun-to-eye distance, ~0.42 m. Against an aim that wrong, that
		// shift was invisible. The test could not have shown a result either
		// way, so "no visible change" said nothing.
		//
		// The wide sweep since then makes the pair much stronger evidence:
		// across the WHOLE object, +0x0E0 is the only position tracking the
		// camera and +0x0F0 is the only unit vector agreeing with the camera
		// FORWARD. That is an origin and a direction, and there is no other
		// candidate.
		//
		// Writing both also removes the dependence on shotRange entirely.
		// With origin AND target both on the barrel line, the ray direction
		// is exactly dir; the range only slides the endpoint along that line
		// and cannot tilt it. That is what makes this different from every
		// barrel-line attempt so far - those moved one end while the engine
		// held the other at the eye.
		static const bool kWriteRayOriginAndDir = true;
		if (kWriteRayOriginAndDir) {
			WriteProcessMemory(GetCurrentProcess(), (void *)(objPtr + 0x0E0),
				gunOrigin, sizeof(float) * 3, &wrote);
			WriteProcessMemory(GetCurrentProcess(), (void *)(objPtr + 0x0F0),
				dir, sizeof(float) * 3, &wrote);

			// Published so the inline hook can re-read these AFTER the
			// engine's own function has run. If they still hold what we put
			// there, the engine consumed our ray; if they have reverted to
			// camera values, it recomputed them and the write was pointless.
			// That distinction cannot be seen from here - only after the
			// call - and it is the whole question.
			memcpy(sLastWrittenOrigin, gunOrigin, sizeof(sLastWrittenOrigin));
			memcpy(sLastWrittenDir, dir, sizeof(sLastWrittenDir));
			memcpy(sLastCamPosAtShot, camPos, sizeof(sLastCamPosAtShot));
			InterlockedExchange(&sRayWritePending, 1);
		}

		// Keep the endpoint consistent with the ray we just wrote, so the
		// decal lands on the same line the damage travels rather than on the
		// old camera-based one.
		if (kWriteRayOriginAndDir) {
			// FAR ENDPOINT, deliberately past all geometry.
			//
			// The ray's DIRECTION is right; only its length is wrong, because
			// shotRange is measured along the camera ray and the camera is
			// nowhere near the hand. Measured: on floor shots the camera sat
			// at -58.5 deg against a hand at -26.9 (range too SHORT for the
			// barrel), on ceiling shots +20.7 against +43.7 (too LONG) -
			// opposite errors, matching "above when shooting at the floor,
			// below when shooting at the ceiling" exactly.
			//
			// If the engine TRACES along our ray and stops at geometry, then
			// putting the endpoint beyond everything makes the range
			// irrelevant and the shot lands where the barrel line genuinely
			// meets the world, at any elevation. If instead it just uses the
			// endpoint as the hit point, impacts will land 60 m away or
			// vanish - which is equally informative, and says we need a real
			// distance along the barrel rather than a borrowed one.
			//
			// One test, two clearly distinguishable outcomes.
			// RESULT: the engine does NOT trace along our ray. With the
			// endpoint at 60 m the impact went to 60 m - no mark on the wall
			// two metres away. So +0x150 is not a target to trace toward, it
			// is the already-computed HIT POINT: the engine traced along its
			// camera ray before this hook ever runs, and everything we do
			// here only relocates the result. It is also why every
			// "invisible wall" impact happened - moving the hit point off
			// the surface is exactly what we were doing.
			//
			// Back to the engine's own range, which at least keeps the hit
			// point ON a surface. The elevation asymmetry comes with it,
			// because that range belongs to the camera's ray rather than the
			// barrel's - a limit of relocating a finished result, not
			// something another endpoint formula can solve.
			const float endpointRange = nativeTargetRange > 0.0f
				? nativeTargetRange : shotRange;
			target[0] = camPos[0] + dir[0] * endpointRange;
			target[1] = camPos[1] + dir[1] * endpointRange;
			target[2] = camPos[2] + dir[2] * endpointRange;
		}
		if (kRewriteNativeHitPoint) {
			WriteProcessMemory(GetCurrentProcess(), (void *)(objPtr + 0x150),
				target, sizeof(target), &wrote);
		}
		memcpy(sReticleDirection, dir, sizeof(sReticleDirection));
		InterlockedExchange(&sReticleDirectionValid, 1);
		PulseFireHaptics();

		// DID THE WRITE STICK, AND IS THIS OBJECT THE ONE THAT MATTERS?
		//
		// Fire calls arrive in PAIRS a frame or less apart (shots #419/#421,
		// #423/#424 in the last run). If only one of each pair is the
		// authoritative shot, we may be steering the wrong one every time -
		// which would explain holes staying put no matter what we write, and
		// it is a far simpler explanation than anything further down the
		// call chain.
		//
		// objPtr distinguishes the pair members; the read-back proves the
		// write landed rather than assuming WriteProcessMemory's return; and
		// the engine's own target shows where the shot was headed before we
		// intervened. Comparing that against the camera forward says whether
		// the engine aims down its own camera - i.e. whether the rounds
		// landing "at the reticle" are simply the engine's untouched intent.
		{
			float readBack[3] = { 0, 0, 0 };
			SIZE_T got = 0;
			ReadProcessMemory(GetCurrentProcess(), (void *)(objPtr + 0x150),
				readBack, sizeof(readBack), &got);
			const float stuck = fabsf(readBack[0] - target[0])
				+ fabsf(readBack[1] - target[1]) + fabsf(readBack[2] - target[2]);

			// Where the engine's own target sat relative to its camera, as a
			// direction, so it can be compared with camFwd directly.
			float eDir[3] = { engineTarget[0] - camPos[0],
				engineTarget[1] - camPos[1], engineTarget[2] - camPos[2] };
			const float eLen = sqrtf(eDir[0]*eDir[0] + eDir[1]*eDir[1] + eDir[2]*eDir[2]);
			if (eLen > 1e-4f) { eDir[0] /= eLen; eDir[1] /= eLen; eDir[2] /= eLen; }
			const float dotCam = eDir[0]*camFwd[0] + eDir[1]*camFwd[1] + eDir[2]*camFwd[2];
			const float clamped = dotCam > 1.0f ? 1.0f : (dotCam < -1.0f ? -1.0f : dotCam);

			static int sPairLogged = 0;
			if (sPairLogged < 120) {
				sPairLogged++;
				LogInfo("VRPose fire:   PAIR obj=%016llX stuck=%.4f engineTgt=(%.2f %.2f %.2f) "
					"engineVsCamFwd=%.2f deg ourTgt=(%.2f %.2f %.2f)\n",
					(unsigned long long)objPtr, stuck,
					engineTarget[0], engineTarget[1], engineTarget[2],
					acosf(clamped) * 57.29578f,
					target[0], target[1], target[2]);
			}
		}

		// UNCAPPED, one line per redirected shot: is the impact effect vanishing
		// because the decal pool is simply being spent twice as fast as the
		// player intends?
		//
		// This session's earlier logs show near-identical fire entries in
		// PAIRS throughout - two redirects a frame or two apart for what reads
		// as one trigger pull. If that is the breakpoint firing twice for a
		// single logical shot (both hits land in the same frame, or a couple
		// of milliseconds apart) rather than the weapon's own rate of fire,
		// each pull is spending two rounds' worth of decal budget instead of
		// one, and a small pool would visibly run out "after a few pulls" -
		// matching exactly what was reported, and consistent with "still
		// breaking things" since damage is a separate system from the decal.
		// A genuine full-auto burst has gaps close to the weapon's real
		// cyclic rate, not to a single frame.
		//
		// This settles which it is from data instead of guessing again.
		{
			static unsigned sShotsRedirected = 0;
			static unsigned sLastShotFrame = 0xFFFFFFFF;
			static ULONGLONG sLastShotTick = 0;
			sShotsRedirected++;
			const unsigned nowFrame = G->frame_no;
			const ULONGLONG nowTick = GetTickCount64();
			const unsigned frameGap = (sLastShotFrame == 0xFFFFFFFF) ? 0 : (nowFrame - sLastShotFrame);
			const ULONGLONG msGap = (sLastShotTick == 0) ? 0 : (nowTick - sLastShotTick);
			sLastShotFrame = nowFrame;
			sLastShotTick = nowTick;
			sGlobalLastShotFrame = nowFrame;
			// dist included: if it is the engine's hit distance it should track
			// how far away the thing being shot is, and a stable value while
			// shooting one wall is the confirmation. If instead it is noise,
			// the target depth is wrong and the decal will misplace again.
			//
			// aimVsDamage: the engine only ever receives a TARGET POINT
			// (WriteProcessMemory above), never a direction - it traces
			// from camPos to that point itself. This computes what
			// direction that trace ACTUALLY travels (target - camPos,
			// normalised) and compares it, in degrees, against the aim
			// direction (dir) the target point was built from. Those are
			// only guaranteed equal when gunOrigin coincides with camPos;
			// since the gun sits offset from the head, they generally do
			// not, and the size of the gap depends on both that offset
			// and shotRange - exactly the "matches down the sights,
			// doesn't downrange, inconsistent" symptom reported. If this
			// number is small and stable, the redirect itself is sound
			// and the mismatch is happening somewhere else (the engine's
			// own consumption of the target point); if it is large or
			// grows at short range, the redirect's own geometry is the
			// cause.
			float toTarget[3] = {
				target[0] - camPos[0], target[1] - camPos[1], target[2] - camPos[2]
			};
			const float toTargetLen = sqrtf(toTarget[0] * toTarget[0]
				+ toTarget[1] * toTarget[1] + toTarget[2] * toTarget[2]);
			float aimVsDamageDeg = 0.0f;
			if (toTargetLen > 0.0001f) {
				const float invLen = 1.0f / toTargetLen;
				const float nx = toTarget[0] * invLen, ny = toTarget[1] * invLen, nz = toTarget[2] * invLen;
				// Angle between the two unit directions via the dot product -
				// robust at both small and large angles, unlike differencing
				// yaw/pitch separately (which mishandles pitch near the poles).
				const float dot = nx * dir[0] + ny * dir[1] + nz * dir[2];
				const float clampedDot = dot > 1.0f ? 1.0f : (dot < -1.0f ? -1.0f : dot);
				aimVsDamageDeg = acosf(clampedDot) * 57.29578f;
			}
			const float gunOffsetDist = sqrtf(
				(gunOrigin[0] - camPos[0]) * (gunOrigin[0] - camPos[0])
				+ (gunOrigin[1] - camPos[1]) * (gunOrigin[1] - camPos[1])
				+ (gunOrigin[2] - camPos[2]) * (gunOrigin[2] - camPos[2]));
			const float spreadDot = barrelDirBeforeSpread[0] * dir[0]
				+ barrelDirBeforeSpread[1] * dir[1]
				+ barrelDirBeforeSpread[2] * dir[2];
			const float spreadClamped = spreadDot > 1.0f ? 1.0f
				: (spreadDot < -1.0f ? -1.0f : spreadDot);
			const float finalDeviationDeg = acosf(spreadClamped) * 57.29578f;
			LogInfo("VRPose fire: shot #%u frame=%u (+%u frames, +%llu ms) dist=%.2f "
				"aimVsDamage=%.3f deg gunOffset=%.3f m ADS=%d "
				"nativeYaw=%+.3f nativePitchRaw=%+.3f finalDeviation=%.3f deg\n",
				sShotsRedirected, nowFrame, frameGap, (unsigned long long)msGap, dist,
				aimVsDamageDeg, gunOffsetDist, IsAiming() ? 1 : 0,
				nativeSpreadYawApplied * 57.29578f,
				nativeSpreadPitchRaw * 57.29578f, finalDeviationDeg);
		}

		static int logged = 0;
		if (logged < 40) {
			logged++;
			// Full decomposition, so a wrong shot can be traced to which term
			// is wrong rather than guessed at from where the round landed.
			// Everything needed to work out the RESPONSE, in degrees.
			//
			// Shots going diagonal when the controller moves along one axis is
			// axis coupling: the aim is not responding on the axis the
			// controller moved. Reasoning about rotation order has not settled
			// it, so print the input and the output in the same units and let
			// the numbers show the coupling directly.
			//
			// ctrlOff is how far the controller has turned from its anchor.
			// aimOff is how far the resulting aim has turned from the body.
			// For a correct composition those two should match, axis for axis.
			const float kDeg = 57.29578f;
			const float aimYawOut = atan2f(dir[0], dir[2]);
			const float aimClampY = dir[1] < -1.0f ? -1.0f : (dir[1] > 1.0f ? 1.0f : dir[1]);
			const float aimPitchOut = asinf(aimClampY);
			LogInfo("VRPose fire: ctrlOff=(yaw %+7.2f, pitch %+7.2f) "
				"aimOff=(yaw %+7.2f, pitch %+7.2f) deg\n",
				WrapAngle(ctrlYaw - sAnchorCtrlYaw) * kDeg,
				(ctrlPitch - sAnchorCtrlPitch) * kDeg,
				WrapAngle(aimYawOut - baseYaw) * kDeg,
				aimPitchOut * kDeg);   // absolute: pitch no longer comes from the body
			LogInfo("VRPose fire:   body=(yaw %+7.2f, pitch %+7.2f) cullInj=%+.2f "
				"ctrl=(%+7.2f,%+7.2f) anchor=(%+7.2f,%+7.2f) deg\n",
				baseYaw * kDeg, basePitch * kDeg, GetCullingCancellation() * kDeg,
				ctrlYaw * kDeg, ctrlPitch * kDeg,
				sAnchorCtrlYaw * kDeg, sAnchorCtrlPitch * kDeg);
			LogInfo("VRPose fire:   camFwd=(%.3f %.3f %.3f) dir=(%.3f %.3f %.3f) dist=%.1f\n",
				camFwd[0], camFwd[1], camFwd[2], dir[0], dir[1], dir[2], dist);

			// THE GUN'S OWN BARREL DIRECTION, IN WORLD SPACE.
			//
			// Three calibration passes have oscillated (pitch +11.3/-13.2/-3.9,
			// yaw -29.1/-17.8/-26.5) rather than converging, because a constant
			// cannot absorb an error that depends on wrist posture. The way out
			// is to stop guessing the barrel axis and read it off the transform
			// that actually DRAWS the weapon.
			//
			// The viewmodel lives in view space and its authored rest pose points
			// down view +Z, so after the mod's rotation D the barrel is D's third
			// column. The view matrix's rows are the camera basis in world, so
			// summing that view-space direction against the rows converts it to
			// world - the same space `dir` is in, which makes the comparison
			// meaningful (an earlier attempt compared a view-space DELTA against
			// a world direction and measured nothing).
			//
			// If this angle is roughly CONSTANT across shots and postures, the
			// assumption holds and the aim can simply BE this vector - no bias
			// constants, nothing to re-measure. If it wanders with how the gun is
			// held, the rest-pose assumption is wrong and this is the wrong tree.
			{
				float D[9] = { 1,0,0, 0,1,0, 0,0,1 };
				GetWeaponRotationDelta(D);
				const float bv[3] = { D[2], D[5], D[8] };   // barrel in view space

				if (G->vrCachedViewValid) {
					const float *v = G->vrCachedView3x4;
					float bw[3] = {
						bv[0]*v[0] + bv[1]*v[4] + bv[2]*v[8],
						bv[0]*v[1] + bv[1]*v[5] + bv[2]*v[9],
						bv[0]*v[2] + bv[1]*v[6] + bv[2]*v[10],
					};
					const float bl = sqrtf(bw[0]*bw[0] + bw[1]*bw[1] + bw[2]*bw[2]);
					if (bl > 1e-4f) {
						bw[0] /= bl; bw[1] /= bl; bw[2] /= bl;
						const float dot = bw[0]*dir[0] + bw[1]*dir[1] + bw[2]*dir[2];
						const float c = dot > 1.0f ? 1.0f : (dot < -1.0f ? -1.0f : dot);
						const float kDeg = 57.29578f;
						float dy = atan2f(bw[0], bw[2]) - atan2f(dir[0], dir[2]);
						while (dy > 3.14159265f) dy -= 6.28318531f;
						while (dy < -3.14159265f) dy += 6.28318531f;
						const float dp = asinf(bw[1] < -1.f ? -1.f : (bw[1] > 1.f ? 1.f : bw[1]))
							- asinf(dir[1] < -1.f ? -1.f : (dir[1] > 1.f ? 1.f : dir[1]));
						LogInfo("VRPose fire:   MODELAIM barrel=(%.3f %.3f %.3f) "
							"angle=%.2f deg dYaw=%.2f dPitch=%.2f\n",
							bw[0], bw[1], bw[2], acosf(c) * kDeg, dy * kDeg, dp * kDeg);
					}
				}
			}

			// THE ROOT CAUSE MEASUREMENT.
			//
			// The drawn gun and the shot derive their direction from two
			// different places: the model from GetWeaponRotationDelta, the
			// shot from GetControllerAim's hard-coded 64.6 degree grip tilt.
			// Two independent approximations of one physical axis agree only
			// by coincidence, and the leftover angle IS the sights-vs-impact
			// error - which is what sWeaponRotAdjust has been manually
			// cancelling, and why it needs a large value.
			//
			// Log the model's own rotation next to the aim it is supposed to
			// match, so the fixed offset between them can be read off real
			// numbers instead of assumed. Once known, the aim can be built
			// from the model's axis and the fudge constant retires.
			{
				const float kDeg = 57.29578f;

				// BOTH DIRECTIONS IN ONE FRAME.
				//
				// The previous version of this compared a view-space DELTA
				// against a world-space direction and so measured nothing
				// usable. Put them in the same frame instead - the game's own
				// view frame - and the angle between them is the actual
				// sights-vs-impact error.
				//
				// The weapon's authored rest pose points down the view's +Z
				// (how viewmodels are built), so after the mod's rotation D
				// the barrel is D's third column. If that assumption is wrong
				// the offset below will not be constant across shots, which
				// is exactly the tell to watch for.
				float D[9] = { 1,0,0, 0,1,0, 0,0,1 };
				GetWeaponRotationDelta(D);
				const float bv[3] = { D[2], D[5], D[8] };

				// View basis from the game's own forward, assuming no roll.
				float f[3] = { camFwd[0], camFwd[1], camFwd[2] };
				float fl = sqrtf(f[0]*f[0] + f[1]*f[1] + f[2]*f[2]);
				if (fl > 1e-6f) { f[0] /= fl; f[1] /= fl; f[2] /= fl; }
				float r[3] = { f[2], 0.0f, -f[0] };            // worldUp x fwd
				float rl = sqrtf(r[0]*r[0] + r[2]*r[2]);
				if (rl > 1e-6f) { r[0] /= rl; r[2] /= rl; }
				const float u[3] = {                            // fwd x right
					f[1]*r[2] - f[2]*r[1],
					f[2]*r[0] - f[0]*r[2],
					f[0]*r[1] - f[1]*r[0],
				};

				const float av[3] = {
					dir[0]*r[0] + dir[1]*r[1] + dir[2]*r[2],
					dir[0]*u[0] + dir[1]*u[1] + dir[2]*u[2],
					dir[0]*f[0] + dir[1]*f[1] + dir[2]*f[2],
				};

				const float bYaw = atan2f(bv[0], bv[2]);
				const float bPitch = asinf(bv[1] < -1.0f ? -1.0f : (bv[1] > 1.0f ? 1.0f : bv[1]));
				const float aYaw = atan2f(av[0], av[2]);
				const float aPitch = asinf(av[1] < -1.0f ? -1.0f : (av[1] > 1.0f ? 1.0f : av[1]));

				LogInfo("VRPose fire:   OFFSET yaw=%+7.2f pitch=%+7.2f deg "
					"[barrel=(%+7.2f,%+7.2f) aim=(%+7.2f,%+7.2f) rotAdj=(%+.2f,%+.2f)]\n",
					WrapAngle(bYaw - aYaw) * kDeg, (bPitch - aPitch) * kDeg,
					bYaw * kDeg, bPitch * kDeg, aYaw * kDeg, aPitch * kDeg,
					sWeaponRotAdjust[0] * kDeg, sWeaponRotAdjust[1] * kDeg);
			}
			LogInfo("VRPose fire:   origin=(%.2f %.2f %.2f) target=(%.2f %.2f %.2f)\n",
				gunOrigin[0], gunOrigin[1], gunOrigin[2], target[0], target[1], target[2]);
		}
	}

	// ---------------------------------------------------------------
	// INLINE HOOK ON THE FIRE CALL
	//
	// Replaces the hardware breakpoint as the way shots are caught.
	//
	// The breakpoint had to be armed PER THREAD, from a snapshot of the
	// threads existing at the time, with a background sweep every five
	// seconds to catch new ones. Any shot serviced by a thread created
	// between sweeps went through unredirected - which is the live report
	// that it "takes more than 10 shots for the redirect to start", plus the
	// occasional stray low shot once it is working. Tuning the sweep would
	// only shorten that window, never close it.
	//
	// An inline hook has no per-thread state at all: the function itself is
	// patched, so every call is caught on every thread from the moment it is
	// installed - no warm-up, no misses, and the exception handler, thread
	// walking and debug registers all leave the hot path.
	//
	// Signature: the wrapper reads its object out of RDX and passes RCX
	// through untouched, so under the x64 convention it is (rcx, rdx). The
	// return value is preserved rather than swallowed, since the wrapper's
	// own caller may test it.
	typedef __int64 (*tFireWrapper)(void *rcx, void *obj);
	static tFireWrapper trampoline_FireWrapper = NULL;
	typedef __int64 (*tPreFireTrace)(void *rcx, void *rdx, void *r8, void *r9);
	static tPreFireTrace trampoline_PreFireTrace = NULL;
	typedef __int64 (*tPreFirePrepare)(void *rcx, void *rdx, void *r8, void *r9);
	static tPreFirePrepare trampoline_PreFirePrepare = NULL;
	typedef __int64 (*tPreFireVectorCopy)(void *rcx, void *rdx, void *r8, void *r9);
	static tPreFireVectorCopy trampoline_PreFireVectorCopy = NULL;
	typedef void (*tPrepareShot)(void *shot, const float *origin, void *weaponData,
		unsigned int weaponIndex, float distance, void *outA, void *outB, void *outC);
	static tPrepareShot trampoline_PrepareShot = NULL;
	typedef __int64 (*tAuthoritativeShot)(void *owner, void *shot, void *context);
	static tAuthoritativeShot trampoline_AuthoritativeShot = NULL;
	typedef void (*tShotProducer)(void *owner, unsigned int kind, unsigned char active,
		void *shot, void *weaponData, unsigned short weaponIndex, float distance,
		void *context);
	static tShotProducer trampoline_ShotProducer = NULL;
	typedef __int64 (*tBallisticCollision)(void *owner, void *shot, void *outHits,
		void *distanceState, void *outState, void *outFlag, void *context);
	static tBallisticCollision trampoline_BallisticCollision = NULL;
	typedef __int64 (*tProjectileCreate)(void *owner, const float *origin,
		const float *direction, void *arg4, unsigned __int64 arg5,
		unsigned __int64 arg6, unsigned __int64 arg7, unsigned __int64 arg8,
		unsigned __int64 arg9, unsigned __int64 arg10);
	static tProjectileCreate trampoline_ProjectileCreate = NULL;
	typedef void *(*tHelsingArrowFactory)(void *factoryContext);
	static tHelsingArrowFactory trampoline_HelsingArrowFactory = NULL;
	static bool BuildVRWorldProjectileOrigin(const float *nativeOrigin, float out[4]);
	static bool BuildVRWorldBarrelDirection(float out[3]);
	typedef void *(*tHelsingArrowTransformCopy)(void *destination, void *source);
	static tHelsingArrowTransformCopy trampoline_HelsingArrowTransformCopy = NULL;
	typedef __int64 (*tHelsingArrowPhysicsLaunch)(void *arrow,
		unsigned __int64 arg2, unsigned __int64 arg3, unsigned __int64 arg4,
		unsigned __int64 arg5, unsigned __int64 arg6, unsigned __int64 arg7,
		unsigned __int64 arg8, unsigned __int64 arg9, unsigned __int64 arg10,
		unsigned __int64 arg11, unsigned __int64 arg12, unsigned __int64 arg13,
		unsigned __int64 arg14, unsigned __int64 arg15, unsigned __int64 arg16,
		unsigned int arg17, float arg18);
	static tHelsingArrowPhysicsLaunch trampoline_HelsingArrowPhysicsLaunch = NULL;
	typedef void (*tHelsingArrowRelease)(void *arrow);
	static tHelsingArrowRelease trampoline_HelsingArrowRelease = NULL;
	typedef __int64 (*tNativeRecoilApply)(void *rcx, void *rdx, void *r8, void *r9);
	static tNativeRecoilApply trampoline_NativeRecoilApply = NULL;

	// Passive, type-specific trace for Helsing's physical arrow object. Unlike
	// the shared hitscan/projectile hooks, this factory is registered only for
	// HELSING_ARROW. The trace never writes to the object, so NPC arrows remain
	// untouched too. Full snapshots let the launch transform be identified
	// offline without relying on an overlay that is not visible in the HMD.
	static const DWORD kHelsingArrowTraceMagic = 0x48534C47; // "HSLG"
	static const SIZE_T kHelsingArrowObjectSize = 0x610;
	static volatile LONG sHelsingArrowFactoryCalls = 0;
	static volatile LONG sHelsingArrowTraceFileErrorLogged = 0;
	static void *sLocalHelsingArrows[64] = {};
	static volatile LONG sLocalHelsingArrowWriteIndex = 0;

	static bool IsHelsingArrowAtLocalPlayerRoot(void *arrow)
	{
		if (!arrow || InterlockedCompareExchange(&sHelsingActive, 0, 0) == 0)
			return false;
		BYTE *base = (BYTE *)GetModuleHandleA(NULL);
		unsigned __int64 vtable = 0;
		float position[4] = {};
		float camera[3] = {};
		SIZE_T got = 0;
		if (!base || !ReadProcessMemory(GetCurrentProcess(), arrow, &vtable,
			sizeof(vtable), &got) || got != sizeof(vtable)
			|| vtable != (unsigned __int64)(base + 0xA0EB60)
			|| !ReadProcessMemory(GetCurrentProcess(), (BYTE *)arrow + 0xF0,
				position, sizeof(position), &got) || got != sizeof(position)
			|| !ReadCameraVec(0x180, camera))
			return false;
		const float dx = position[0] - camera[0];
		const float dy = position[1] - camera[1];
		const float dz = position[2] - camera[2];
		const float horizontalDistance = sqrtf(dx * dx + dz * dz);
		// Metro uses about -1.80 m while standing and -1.26 m in its lower
		// stance. The X/Z coincidence is the strong ownership signal; accept the
		// full plausible local-player height interval instead of keying ownership
		// to one stance. An NPC arrow remains excluded unless its creation point
		// is virtually on the player's exact body axis.
		return isfinite(horizontalDistance) && horizontalDistance < 0.35f
			&& isfinite(dy) && dy < -0.75f && dy > -2.25f;
	}

	// One-shot writer trace for the incoming camera state that is copied through
	// +0xD23270 to the submitted matrix at +0xD22F20. Static scans find the
	// downstream readers but not this writer because Metro reaches the
	// destination through a register. Arm after gameplay has settled, capture
	// one write, and permanently disarm.
	static void ArmMainCameraSourceWriterProbeOnce()
	{
		static unsigned settledFrames = 0;
		if (++settledFrames < 120 ||
			InterlockedCompareExchange(&sMainCameraSourceWatchState, 1, 0) != 0)
			return;

		HMODULE base = GetModuleHandleA(NULL);
		if (!base) {
			InterlockedExchange(&sMainCameraSourceWatchState, 3);
			return;
		}
		if (!sProbeVehHandle)
			sProbeVehHandle = AddVectoredExceptionHandler(1, CameraWriteProbeVeh);
		if (!sProbeVehHandle) {
			LogInfo("VRPose upstream camera watch: exception handler install failed\n");
			InterlockedExchange(&sMainCameraSourceWatchState, 3);
			return;
		}

		sProbeWatchAddress = (BYTE *)base + kMainCameraSourceMatrixRva;
		sProbeDr7 = kDr7Write4;
		InterlockedExchange(&sProbeReported, 0);
		InterlockedExchange(&sStage2Armed, 0);
		InterlockedExchange(&sStage3Armed, 0);
		const int others = ArmWatchpointOnAllThreads(sProbeWatchAddress);
		RaiseException(kArmExceptionCode, 0, 0, NULL);
		LogInfo("VRPose upstream camera watch: armed one-shot write watch at "
			"metro.exe+0x%llX across this and %d other threads\n",
			(unsigned long long)kMainCameraSourceMatrixRva, others);
	}

	static void TrackLocalHelsingArrow(void *arrow)
	{
		if (!IsHelsingArrowAtLocalPlayerRoot(arrow))
			return;
		for (SIZE_T i = 0; i < ARRAYSIZE(sLocalHelsingArrows); ++i) {
			if (InterlockedCompareExchangePointer(&sLocalHelsingArrows[i], arrow,
				arrow) == arrow)
				return;
		}
		const LONG index = InterlockedIncrement(&sLocalHelsingArrowWriteIndex) - 1;
		InterlockedExchangePointer(
			&sLocalHelsingArrows[(unsigned int)index % ARRAYSIZE(sLocalHelsingArrows)],
			arrow);
	}

	static bool ConsumeLocalHelsingArrow(void *arrow)
	{
		if (!arrow)
			return false;
		for (SIZE_T i = 0; i < ARRAYSIZE(sLocalHelsingArrows); ++i) {
			if (InterlockedCompareExchangePointer(&sLocalHelsingArrows[i], NULL,
				arrow) == arrow)
				return true;
		}
		return false;
	}

	static bool RewriteHelsingArrowAtSynchronousRelease(void *arrow)
	{
		if (!arrow || !IsFiring()
			|| InterlockedCompareExchange(&sHelsingActive, 0, 0) == 0)
			return false;

		BYTE *base = (BYTE *)GetModuleHandleA(NULL);
		unsigned __int64 vtable = 0;
		SIZE_T got = 0;
		if (!base || !ReadProcessMemory(GetCurrentProcess(), arrow, &vtable,
			sizeof(vtable), &got) || got != sizeof(vtable)
			|| vtable != (unsigned __int64)(base + 0xA0EB60))
			return false;

		float nativeOrigin[4] = {};
		if (!ReadProcessMemory(GetCurrentProcess(), (BYTE *)arrow + 0xF0,
			nativeOrigin, sizeof(nativeOrigin), &got) || got != sizeof(nativeOrigin))
			return false;
		float camera[3] = {};
		if (!ReadCameraVec(0x180, camera))
			return false;
		const float dx = nativeOrigin[0] - camera[0];
		const float dy = nativeOrigin[1] - camera[1];
		const float dz = nativeOrigin[2] - camera[2];
		const float cameraDistance = sqrtf(dx * dx + dy * dy + dz * dz);
		const float horizontalDistance = sqrtf(dx * dx + dz * dz);
		// Before Helsing enters its physics-launch routine, its arrow transform is
		// parked at the local player's body root: same world X/Z as the camera and
		// exactly eye height (about 1.8 m) below it. After launch it moves to the
		// eye-space origin handled by the worker below. Accept both representations
		// while still rejecting an NPC arrow merely because an NPC is nearby.
		const bool localEyeOrigin = cameraDistance < 1.0f;
		const bool localBodyRoot = horizontalDistance < 0.35f
			&& fabsf(dy + 1.8f) < 0.25f;
		if (!isfinite(cameraDistance) || (!localEyeOrigin && !localBodyRoot))
			return false;

		float muzzle[4] = {};
		float direction[3] = {};
		if (!BuildVRWorldProjectileOrigin(nativeOrigin, muzzle)
			|| !BuildVRWorldBarrelDirection(direction))
			return false;
		float right[3] = { direction[2], 0.0f, -direction[0] };
		const float rightLength = sqrtf(right[0] * right[0] + right[2] * right[2]);
		if (rightLength < 1e-4f)
			return false;
		right[0] /= rightLength;
		right[2] /= rightLength;
		const float up[3] = {
			direction[1] * right[2] - direction[2] * right[1],
			direction[2] * right[0] - direction[0] * right[2],
			direction[0] * right[1] - direction[1] * right[0]
		};
		const float transform[16] = {
			right[0], right[1], right[2], 0.0f,
			up[0], up[1], up[2], 0.0f,
			direction[0], direction[1], direction[2], 0.0f,
			muzzle[0], muzzle[1], muzzle[2], 1.0f
		};
		__try {
			memcpy((BYTE *)arrow + 0xC0, transform, sizeof(transform));
			memcpy((BYTE *)arrow + 0x500, muzzle, sizeof(float) * 3);
			memcpy((BYTE *)arrow + 0x510, direction, sizeof(direction));
			memcpy((BYTE *)arrow + 0x520, right, sizeof(right));
			memcpy((BYTE *)arrow + 0x530, muzzle, sizeof(float) * 3);
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
		static volatile LONG sRewriteCount = 0;
		const LONG count = InterlockedIncrement(&sRewriteCount);
		if (count <= 20)
			LogInfo("VRPose Helsing-arrow: SYNCHRONOUS RELEASE rewrite=%ld "
				"cameraDistance=%.3f muzzle=(%.3f %.3f %.3f) "
				"direction=(%.4f %.4f %.4f)\n", count, cameraDistance,
				muzzle[0], muzzle[1], muzzle[2],
				direction[0], direction[1], direction[2]);
		return true;
	}

	static void *Hooked_HelsingArrowTransformCopy(void *destination, void *source)
	{
		void *result = trampoline_HelsingArrowTransformCopy(destination, source);
		static volatile LONG sCopyLogCount = 0;
		const LONG copyLogCount = InterlockedIncrement(&sCopyLogCount);
		if (copyLogCount <= 40 || sHelsingActive) {
			void *vtable = NULL;
			__try { vtable = *(void **)destination; }
			__except (EXCEPTION_EXECUTE_HANDLER) { vtable = NULL; }
			const unsigned char *moduleBase = (const unsigned char *)GetModuleHandleA(NULL);
			const unsigned char *returnAddress = (const unsigned char *)_ReturnAddress();
			const SIZE_T returnRva = (moduleBase && returnAddress >= moduleBase) ?
				(SIZE_T)(returnAddress - moduleBase) : 0;
			LogInfo("VRPose Helsing-arrow: SYNC COPY call=%ld dst=%p src=%p "
				"vtable=%p active=%ld firing=%d returnRva=+0x%zx\n", copyLogCount,
				destination, source, vtable, sHelsingActive, IsFiring() ? 1 : 0,
				returnRva);
		}
		RewriteHelsingArrowAtSynchronousRelease(destination);
		return result;
	}

	static void InstallHelsingArrowTransformCopyHook(const unsigned char *base)
	{
		static bool sTried = false;
		if (sTried || !base)
			return;
		sTried = true;
		void *fn = (void *)(base + 0x80930);
		static const unsigned char expected[] = {
			0x0F, 0x28, 0x02, 0x0F, 0x29, 0x01, 0x0F, 0x28
		};
		if (memcmp(fn, expected, sizeof(expected)) != 0) {
			const unsigned char *actual = (const unsigned char *)fn;
			LogInfo("VRPose Helsing-arrow: synchronous transform signature mismatch "
				"at +0x80930; bytes=%02x %02x %02x %02x %02x %02x %02x %02x "
				"hook not installed\n", actual[0], actual[1], actual[2], actual[3],
				actual[4], actual[5], actual[6], actual[7]);
			return;
		}
		SIZE_T hookId = 0;
		const DWORD err = cHookMgr.Hook(&hookId,
			(LPVOID *)&trampoline_HelsingArrowTransformCopy, fn,
			Hooked_HelsingArrowTransformCopy);
		if (err || !trampoline_HelsingArrowTransformCopy)
			LogInfo("VRPose Helsing-arrow: synchronous transform hook FAILED "
				"at +0x80930 (err=0x%x)\n", err);
		else
			LogInfo("VRPose Helsing-arrow: synchronous transform hook installed "
				"at +0x80930 (player-gated)\n");
	}

	static __int64 Hooked_HelsingArrowPhysicsLaunch(void *arrow,
		unsigned __int64 arg2, unsigned __int64 arg3, unsigned __int64 arg4,
		unsigned __int64 arg5, unsigned __int64 arg6, unsigned __int64 arg7,
		unsigned __int64 arg8, unsigned __int64 arg9, unsigned __int64 arg10,
		unsigned __int64 arg11, unsigned __int64 arg12, unsigned __int64 arg13,
		unsigned __int64 arg14, unsigned __int64 arg15, unsigned __int64 arg16,
		unsigned int arg17, float arg18)
	{
		// The arrow remains parked at the local player's body root throughout
		// Helsing's delayed firing animation. Persist that strong ownership result
		// so release does not depend on the transient trigger bit hundreds of
		// milliseconds later.
		TrackLocalHelsingArrow(arrow);
		return trampoline_HelsingArrowPhysicsLaunch(arrow,
			arg2, arg3, arg4, arg5, arg6, arg7, arg8, arg9, arg10,
			arg11, arg12, arg13, arg14, arg15, arg16, arg17, arg18);
	}

	static void InstallHelsingArrowPhysicsLaunchHook(const unsigned char *base)
	{
		static bool sTried = false;
		if (sTried || !base)
			return;
		sTried = true;
		void *fn = (void *)(base + 0x37D5B0);
		static const unsigned char expected[] = {
			0x40, 0x53, 0x48, 0x83, 0xEC, 0x30, 0x44, 0x8B,
			0x8C, 0x24, 0x88, 0x00, 0x00, 0x00
		};
		if (memcmp(fn, expected, sizeof(expected)) != 0) {
			LogInfo("VRPose Helsing-arrow: physics-launch signature mismatch "
				"at +0x37D5B0; hook not installed\n");
			return;
		}
		SIZE_T hookId = 0;
		const DWORD err = cHookMgr.Hook(&hookId,
			(LPVOID *)&trampoline_HelsingArrowPhysicsLaunch, fn,
			Hooked_HelsingArrowPhysicsLaunch);
		if (err || !trampoline_HelsingArrowPhysicsLaunch)
			LogInfo("VRPose Helsing-arrow: physics-launch hook FAILED "
				"at +0x37D5B0 (err=0x%x)\n", err);
		else
			LogInfo("VRPose Helsing-arrow: physics-launch hook installed "
				"at +0x37D5B0 (local-root ownership)\n");
	}

	static void Hooked_HelsingArrowRelease(void *arrow)
	{
		// This is the arrow-specific release operation. Metro creates the live
		// physics body and applies its native (head-facing) velocity inside it.
		trampoline_HelsingArrowRelease(arrow);

		if (!arrow || InterlockedCompareExchange(&sHelsingActive, 0, 0) == 0
			|| !ConsumeLocalHelsingArrow(arrow))
			return;
		BYTE *base = (BYTE *)GetModuleHandleA(NULL);
		__try {
			if (!base || *(unsigned __int64 *)arrow
				!= (unsigned __int64)(base + 0xA0EB60))
				return;

			float muzzle[4] = {};
			float direction[3] = {};
			if (!BuildVRWorldProjectileOrigin((float *)((BYTE *)arrow + 0xF0), muzzle)
				|| !BuildVRWorldBarrelDirection(direction))
				return;

			// component+0x110 is the projectile settings object. The native release
			// uses its +0x1c scalar for launch speed when Helsing passes -1.0f.
			BYTE *component = (BYTE *)arrow + 0x4F0;
			BYTE *settings = *(BYTE **)(component + 0x110);
			float speed = settings ? *(float *)(settings + 0x1C) : 0.0f;
			if (!isfinite(speed) || speed <= 0.0f || speed > 10000.0f)
				speed = 128.0f;
			const float velocity[4] = {
				direction[0] * speed, direction[1] * speed,
				direction[2] * speed, 0.0f };

			void *physics = *(void **)((BYTE *)arrow + 0x318);
			void **physicsVtable = physics ? *(void ***)physics : NULL;
			if (!physicsVtable || !physicsVtable[8] || !physicsVtable[11])
				return;
			typedef void (*tPhysicsSetVector)(void *, const float *);
			// Slots +0x58 and +0x40 are the same position and velocity setters
			// called by Metro's native release path immediately above.
			((tPhysicsSetVector)physicsVtable[11])(physics, muzzle);
			((tPhysicsSetVector)physicsVtable[8])(physics, velocity);

			memcpy((BYTE *)arrow + 0xF0, muzzle, sizeof(float) * 4);
			memcpy((BYTE *)arrow + 0x500, muzzle, sizeof(float) * 3);
			memcpy((BYTE *)arrow + 0x510, direction, sizeof(direction));
			memcpy((BYTE *)arrow + 0x530, muzzle, sizeof(float) * 3);

		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			static volatile LONG sReleaseFaultLogged = 0;
			if (InterlockedCompareExchange(&sReleaseFaultLogged, 1, 0) == 0)
				LogInfo("VRPose Helsing-arrow: physics release rewrite faulted\n");
		}
	}

	static void InstallHelsingArrowReleaseHook(const unsigned char *base)
	{
		static bool sTried = false;
		if (sTried || !base)
			return;
		sTried = true;
		void *fn = (void *)(base + 0x37D730);
		static const unsigned char expected[] = {
			0x40, 0x53, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x8B,
			0xD9, 0xE8
		};
		if (memcmp(fn, expected, sizeof(expected)) != 0) {
			LogInfo("VRPose Helsing-arrow: release signature mismatch "
				"at +0x37D730; hook not installed\n");
			return;
		}
		SIZE_T hookId = 0;
		const DWORD err = cHookMgr.Hook(&hookId,
			(LPVOID *)&trampoline_HelsingArrowRelease, fn,
			Hooked_HelsingArrowRelease);
		if (err || !trampoline_HelsingArrowRelease)
			LogInfo("VRPose Helsing-arrow: release hook FAILED "
				"at +0x37D730 (err=0x%x)\n", err);
		else
			LogInfo("VRPose Helsing-arrow: release hook installed "
				"at +0x37D730 (tracked local arrows only)\n");
	}

	struct HelsingArrowTraceRecord
	{
		DWORD magic;
		DWORD recordSize;
		DWORD sequence;
		DWORD delayMs;
		DWORD firing;
		DWORD helsingActive;
		DWORD bytesRead;
		DWORD reserved;
		unsigned __int64 arrow;
		unsigned __int64 caller;
		BYTE objectBytes[kHelsingArrowObjectSize];
	};

	struct HelsingArrowTraceWork
	{
		void *arrow;
		unsigned __int64 caller;
		LONG sequence;
		bool activeAtCreation;
	};

	static void CaptureHelsingArrowSnapshot(const HelsingArrowTraceWork &work,
		DWORD delayMs, bool logSummary)
	{
		HelsingArrowTraceRecord record = {};
		record.magic = kHelsingArrowTraceMagic;
		record.recordSize = sizeof(record);
		record.sequence = (DWORD)work.sequence;
		record.delayMs = delayMs;
		record.firing = IsFiring() ? 1 : 0;
		record.helsingActive =
			InterlockedCompareExchange(&sHelsingActive, 0, 0) != 0 ? 1 : 0;
		record.arrow = (unsigned __int64)work.arrow;
		record.caller = work.caller;

		SIZE_T bytesRead = 0;
		if (work.arrow)
			ReadProcessMemory(GetCurrentProcess(), work.arrow, record.objectBytes,
				kHelsingArrowObjectSize, &bytesRead);
		record.bytesRead = (DWORD)bytesRead;

		const DWORD disposition = work.sequence == 1 && delayMs == 0
			? CREATE_ALWAYS : OPEN_ALWAYS;
		HANDLE file = CreateFileW(L"helsing_arrow_trace.bin", FILE_APPEND_DATA,
			FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, disposition,
			FILE_ATTRIBUTE_NORMAL, NULL);
		if (file != INVALID_HANDLE_VALUE) {
			DWORD written = 0;
			WriteFile(file, &record, sizeof(record), &written, NULL);
			CloseHandle(file);
		} else if (InterlockedCompareExchange(
			&sHelsingArrowTraceFileErrorLogged, 1, 0) == 0) {
			LogInfo("VRPose Helsing-arrow: could not open helsing_arrow_trace.bin "
				"(error=%lu)\n", GetLastError());
		}

		float position[4] = {};
		float vector100[4] = {};
		if (bytesRead >= 0x110) {
			memcpy(position, record.objectBytes + 0xF0, sizeof(position));
			memcpy(vector100, record.objectBytes + 0x100, sizeof(vector100));
		}
		if (logSummary)
			LogInfo("VRPose Helsing-arrow: seq=%ld delay=%lums arrow=%p bytes=0x%X "
				"firing=%u active=%u posF0=(%+.5f,%+.5f,%+.5f,%+.5f) "
				"v100=(%+.5f,%+.5f,%+.5f,%+.5f)\n",
				work.sequence, delayMs, work.arrow, record.bytesRead,
				record.firing, record.helsingActive,
				position[0], position[1], position[2], position[3],
				vector100[0], vector100[1], vector100[2], vector100[3]);
	}

	static DWORD WINAPI HelsingArrowTraceWorker(LPVOID parameter)
	{
		HelsingArrowTraceWork *work = (HelsingArrowTraceWork *)parameter;
		if (!work)
			return 0;
		static const DWORD delays[] = { 1, 5, 20, 75 };
		DWORD elapsed = 0;
		for (size_t i = 0; i < _countof(delays); ++i) {
			Sleep(delays[i] - elapsed);
			elapsed = delays[i];
			CaptureHelsingArrowSnapshot(*work, elapsed, true);
		}

		// Helsing constructs the loaded bolts during its mandatory equip/reload
		// animation, then releases those same objects later. Follow only bolts
		// created while the local Helsing viewmodel is active across that gap.
		// Sampling remains read-only; the vtable check stops if the object is
		// destroyed and its allocation reused for another class.
		if (work->activeAtCreation) {
			unsigned __int64 expectedVtable = 0;
			SIZE_T got = 0;
			ReadProcessMemory(GetCurrentProcess(), work->arrow,
				&expectedVtable, sizeof(expectedVtable), &got);
			BYTE previousTransform[0x40] = {};
			ReadProcessMemory(GetCurrentProcess(),
				(BYTE *)work->arrow + 0xC0, previousTransform,
				sizeof(previousTransform), &got);
			float loadedPosition[3] = {};
			memcpy(loadedPosition, previousTransform + 0x30,
				sizeof(loadedPosition));
			bool redirected = false;
			DWORD redirectElapsed = 0;
			float lockedTransform[16] = {};
			float lockedMuzzle[4] = {};
			float lockedDirection[3] = {};
			float lockedRight[3] = {};
			for (unsigned sample = 0; sample < 30000; ++sample) { // about 30 sec
				Sleep(1);
				elapsed += 1;
				unsigned __int64 currentVtable = 0;
				if (!ReadProcessMemory(GetCurrentProcess(), work->arrow,
					&currentVtable, sizeof(currentVtable), &got)
					|| got != sizeof(currentVtable)
					|| currentVtable != expectedVtable)
					break;

				BYTE currentTransform[0x40] = {};
				const bool transformReadable = ReadProcessMemory(GetCurrentProcess(),
					(BYTE *)work->arrow + 0xC0, currentTransform,
					sizeof(currentTransform), &got) && got == sizeof(currentTransform);
				const bool transformChanged = transformReadable
					&& memcmp(previousTransform, currentTransform,
						sizeof(currentTransform)) != 0;

				// A loaded Helsing bolt follows only head yaw and remains at its
				// parked position. Release is unambiguous: Metro moves +0xF0 by
				// roughly eye height and simultaneously seeds the physics origin
				// and direction at +0x500/+0x510. Require that release point to be
				// within one metre of the local camera before changing anything;
				// NPC arrows originate at their NPC and fail this ownership gate.
				if (!redirected && transformReadable) {
					float currentPosition[3] = {};
					memcpy(currentPosition, currentTransform + 0x30,
						sizeof(currentPosition));
					const float releaseDx = currentPosition[0] - loadedPosition[0];
					const float releaseDy = currentPosition[1] - loadedPosition[1];
					const float releaseDz = currentPosition[2] - loadedPosition[2];
					const float releaseMove = sqrtf(releaseDx * releaseDx
						+ releaseDy * releaseDy + releaseDz * releaseDz);
					float camera[3] = {};
					const bool haveCamera = ReadCameraVec(0x180, camera);
					const float cameraDx = currentPosition[0] - camera[0];
					const float cameraDy = currentPosition[1] - camera[1];
					const float cameraDz = currentPosition[2] - camera[2];
					const float cameraDistance = sqrtf(cameraDx * cameraDx
						+ cameraDy * cameraDy + cameraDz * cameraDz);
					const bool localRelease = releaseMove > 0.5f && haveCamera
						&& isfinite(cameraDistance) && cameraDistance < 1.0f
						&& InterlockedCompareExchange(&sHelsingActive, 0, 0) != 0;
					if (localRelease) {
						float muzzle[4] = {};
						float direction[3] = {};
						if (BuildVRWorldProjectileOrigin(currentPosition, muzzle)
							&& BuildVRWorldBarrelDirection(direction)) {
							float right[3] = { direction[2], 0.0f, -direction[0] };
							float rightLength = sqrtf(right[0] * right[0]
								+ right[2] * right[2]);
							if (rightLength < 1e-4f) {
								right[0] = 1.0f; right[1] = 0.0f; right[2] = 0.0f;
							} else {
								right[0] /= rightLength;
								right[2] /= rightLength;
							}
							const float up[3] = {
								direction[1] * right[2] - direction[2] * right[1],
								direction[2] * right[0] - direction[0] * right[2],
								direction[0] * right[1] - direction[1] * right[0]
							};
							float replacementTransform[16] = {
								right[0], right[1], right[2], 0.0f,
								up[0], up[1], up[2], 0.0f,
								direction[0], direction[1], direction[2], 0.0f,
								muzzle[0], muzzle[1], muzzle[2], 1.0f
							};
							__try {
								memcpy((BYTE *)work->arrow + 0xC0,
									replacementTransform, sizeof(replacementTransform));
								memcpy((BYTE *)work->arrow + 0x500, muzzle,
									sizeof(float) * 3);
								memcpy((BYTE *)work->arrow + 0x510, direction,
									sizeof(direction));
								memcpy((BYTE *)work->arrow + 0x520, right,
									sizeof(right));
								memcpy((BYTE *)work->arrow + 0x530, muzzle,
									sizeof(float) * 3);
								memcpy(lockedTransform, replacementTransform,
									sizeof(lockedTransform));
								memcpy(lockedMuzzle, muzzle, sizeof(lockedMuzzle));
								memcpy(lockedDirection, direction,
									sizeof(lockedDirection));
								memcpy(lockedRight, right, sizeof(lockedRight));
								redirectElapsed = elapsed;
								redirected = true;
							}
							__except (EXCEPTION_EXECUTE_HANDLER) {
								redirected = false;
							}
							LogInfo("VRPose Helsing-arrow: PLAYER RELEASE redirected=%d "
								"cameraDistance=%.3f nativeOrigin=(%.3f %.3f %.3f) "
								"muzzle=(%.3f %.3f %.3f) direction=(%.4f %.4f %.4f)\n",
								redirected ? 1 : 0, cameraDistance,
								currentPosition[0], currentPosition[1], currentPosition[2],
								muzzle[0], muzzle[1], muzzle[2],
								direction[0], direction[1], direction[2]);
						}
					}
				}

				// Metro performs one final native transform copy after the release
				// callback. The physics vectors at +0x500 survive our first write,
				// but +0xC0..+0xF0 is restored to the head ray about 8 ms later.
				// Hold the corrected orientation across that handoff. Keep the
				// position pinned only while the bolt is still at the launch point;
				// once it has moved, preserve its live integrated position.
				if (redirected && elapsed - redirectElapsed <= 64
					&& transformReadable) {
					float livePosition[3] = {};
					memcpy(livePosition, currentTransform + 0x30,
						sizeof(livePosition));
					const float mx = livePosition[0] - lockedMuzzle[0];
					const float my = livePosition[1] - lockedMuzzle[1];
					const float mz = livePosition[2] - lockedMuzzle[2];
					const float fromMuzzle = sqrtf(mx * mx + my * my + mz * mz);
					__try {
						memcpy((BYTE *)work->arrow + 0xC0, lockedTransform,
							sizeof(float) * 12);
						if (isfinite(fromMuzzle) && fromMuzzle < 0.5f)
							memcpy((BYTE *)work->arrow + 0xF0, lockedMuzzle,
								sizeof(lockedMuzzle));
						memcpy((BYTE *)work->arrow + 0x500, lockedMuzzle,
							sizeof(float) * 3);
						memcpy((BYTE *)work->arrow + 0x510, lockedDirection,
							sizeof(lockedDirection));
						memcpy((BYTE *)work->arrow + 0x520, lockedRight,
							sizeof(lockedRight));
					}
					__except (EXCEPTION_EXECUTE_HANDLER) {}
				}

				if (transformChanged && (redirected || (sample % 16) == 0))
					CaptureHelsingArrowSnapshot(*work, elapsed, true);
				if (transformChanged)
					memcpy(previousTransform, currentTransform,
						sizeof(previousTransform));
			}
		}
		HeapFree(GetProcessHeap(), 0, work);
		return 0;
	}

	static void *Hooked_HelsingArrowFactory(void *factoryContext)
	{
		void *arrow = trampoline_HelsingArrowFactory(factoryContext);
		const LONG sequence = InterlockedIncrement(&sHelsingArrowFactoryCalls);
		if (!arrow || sequence > 256)
			return arrow;

		HelsingArrowTraceWork immediate = {};
		immediate.arrow = arrow;
		immediate.caller = (unsigned __int64)_ReturnAddress();
		immediate.sequence = sequence;
		immediate.activeAtCreation =
			InterlockedCompareExchange(&sHelsingActive, 0, 0) != 0;
		CaptureHelsingArrowSnapshot(immediate, 0, true);

		HelsingArrowTraceWork *work = (HelsingArrowTraceWork *)HeapAlloc(
			GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(HelsingArrowTraceWork));
		if (work) {
			*work = immediate;
			HANDLE thread = CreateThread(NULL, 0, HelsingArrowTraceWorker, work, 0, NULL);
			if (thread)
				CloseHandle(thread);
			else
				HeapFree(GetProcessHeap(), 0, work);
		}
		return arrow;
	}

	static void InstallHelsingArrowFactoryDiagnosticHook(const unsigned char *base)
	{
		static bool sTried = false;
		if (sTried || !base)
			return;
		sTried = true;

		void *factoryFn = (void *)(base + 0x1D48F8);
		static const unsigned char expected[] = {
			0x40, 0x53, 0x48, 0x83, 0xEC, 0x30
		};
		bool matches = false;
		__try {
			matches = memcmp(factoryFn, expected, sizeof(expected)) == 0;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			matches = false;
		}
		if (!matches) {
			LogInfo("VRPose Helsing-arrow: factory signature mismatch at +0x1D48F8; "
				"passive trace not installed\n");
			return;
		}

		SIZE_T hookId = 0;
		const DWORD err = cHookMgr.Hook(&hookId,
			(LPVOID *)&trampoline_HelsingArrowFactory, factoryFn,
			Hooked_HelsingArrowFactory);
		if (err || !trampoline_HelsingArrowFactory)
			LogInfo("VRPose Helsing-arrow: factory hook FAILED at +0x1D48F8 "
				"(err=0x%x)\n", err);
		else
			LogInfo("VRPose Helsing-arrow: passive factory trace installed at "
				"+0x1D48F8 (read-only, type-specific)\n");
	}

	static __int64 Hooked_NativeRecoilApply(void *rcx, void *rdx, void *r8, void *r9)
	{
		static volatile LONG sCalls = 0;
		const LONG callNo = InterlockedIncrement(&sCalls);
		DWORD64 configRef = 0, config = 0;
		float recoilPower = 0.0f;
		bool valid = false;
		__try {
			if (rcx) {
				configRef = *(const DWORD64 *)((const BYTE *)rcx + 0x1B8);
				if (configRef >= 0xA8) {
					config = configRef - 0xA8;
					recoilPower = *(const float *)(config + 0x73C);
					valid = _finite(recoilPower) != 0 && fabsf(recoilPower) < 10000.0f;
				}
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			valid = false;
		}

		if (callNo <= 80 || IsAiming()) {
			char callerDesc[160] = {};
			DescribeCodeAddress((DWORD64)_ReturnAddress(), callerDesc, sizeof(callerDesc));
			LogInfo("VRPose recoil-native: call=%ld gesture=%d rcx=%p rdx=%p "
				"configRef=%016llX config=%016llX recoilPower=%+.6f valid=%d caller=%s\n",
				callNo, IsAiming() ? 1 : 0, rcx, rdx,
				(unsigned long long)configRef, (unsigned long long)config,
				recoilPower, valid ? 1 : 0, callerDesc);
		}

		return trampoline_NativeRecoilApply(rcx, rdx, r8, r9);
	}

	static void InstallNativeRecoilDiagnosticHook()
	{
		static bool sTried = false;
		if (sTried)
			return;
		sTried = true;

		const unsigned char *base = (const unsigned char *)GetModuleHandleA(NULL);
		if (!base)
			return;
		static const unsigned char expected[] = {
			0x48, 0x8B, 0xC4, 0x48, 0x89, 0x48, 0x08, 0x55, 0x56, 0x41, 0x54, 0x41, 0x56
		};
		void *fn = (void *)(base + 0x366850);
		if (memcmp(fn, expected, sizeof(expected)) != 0) {
			LogInfo("VRPose recoil-native: diagnostic hook refused - unexpected bytes at +0x366850\n");
			return;
		}
		SIZE_T hookId = 0;
		const DWORD err = cHookMgr.Hook(&hookId, (LPVOID *)&trampoline_NativeRecoilApply,
			fn, Hooked_NativeRecoilApply);
		if (err || !trampoline_NativeRecoilApply)
			LogInfo("VRPose recoil-native: diagnostic hook FAILED at +0x366850 (err=0x%x)\n", err);
		else
			LogInfo("VRPose recoil-native: diagnostic hook installed at +0x366850\n");
	}

	// Common weapon action handler. Its state machine contains the explicit
	// aim-in/aim-out path (including the weapon virtual at vtable+0x1110).
	// Capture the live object and arguments from one ordinary native ADS press
	// so the VR gesture can later invoke the same gameplay transition directly.
	typedef void (*tWeaponActionHandler)(void *object, int action, int state);
	static tWeaponActionHandler trampoline_WeaponActionHandler = NULL;
	static void *sLastWeaponActionObject = NULL;

	static void Hooked_WeaponActionHandler(void *object, int action, int state)
	{
		unsigned char weaponState = 0, wasAiming = 0, aimingState = 0;
		if (object) {
			__try {
				weaponState = *((unsigned char *)object + 0x70B);
				wasAiming = *((unsigned char *)object + 0x724);
				aimingState = *((unsigned char *)object + 0x727);
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {}
			sLastWeaponActionObject = object;
		}
		static volatile LONG count = 0;
		const LONG n = InterlockedIncrement(&count);
		if (n <= 80) {
			char caller[160] = {};
			DescribeCodeAddress((DWORD64)_ReturnAddress(), caller, sizeof(caller));
			LogInfo("VRPose ADS action: call=%ld object=%p action=%d state=%d "
				"weaponState=0x%02X wasAiming=%u aimingState=%u caller=%s\n",
				n, object, action, state, weaponState, wasAiming, aimingState, caller);
		}
		trampoline_WeaponActionHandler(object, action, state);
		if (n <= 80 && object) {
			__try {
				LogInfo("VRPose ADS action: after call=%ld state70B=0x%02X "
					"wasAiming=%u aimingState=%u\n", n,
					*((unsigned char *)object + 0x70B),
					*((unsigned char *)object + 0x724),
					*((unsigned char *)object + 0x727));
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {}
		}
	}

	static void InstallWeaponActionDiagnosticHook()
	{
		static bool tried = false;
		if (tried)
			return;
		tried = true;
		const unsigned char *base = (const unsigned char *)GetModuleHandleA(NULL);
		if (!base)
			return;
		static const unsigned char expected[] = {
			0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10,
			0x57, 0x48, 0x83, 0xEC, 0x20
		};
		void *fn = (void *)(base + 0x2A5A60);
		if (memcmp(fn, expected, sizeof(expected)) != 0) {
			LogInfo("VRPose ADS action: hook refused - unexpected bytes at +0x2A5A60\n");
			return;
		}
		SIZE_T hookId = 0;
		const DWORD err = cHookMgr.Hook(&hookId,
			(LPVOID *)&trampoline_WeaponActionHandler, fn, Hooked_WeaponActionHandler);
		if (err || !trampoline_WeaponActionHandler)
			LogInfo("VRPose ADS action: hook FAILED at +0x2A5A60 (err=0x%x)\n", err);
		else
			LogInfo("VRPose ADS action: diagnostic hook installed at +0x2A5A60\n");
	}

	// Read-only trace to locate Metro's authoritative wrist-display controller.
	// The normal clock path imports MSVCR110!_localtime64; its immediate caller
	// is the game routine that constructs/selects the displayed wrist value.
	typedef struct tm *(__cdecl *tMetroLocaltime64)(const __time64_t *value);
	static tMetroLocaltime64 trampoline_MetroLocaltime64 = NULL;
	static struct tm *__cdecl Hooked_MetroLocaltime64(const __time64_t *value)
	{
		struct tm *result = trampoline_MetroLocaltime64(value);
		static volatile LONG calls = 0;
		const LONG n = InterlockedIncrement(&calls);
		if (n <= 20 || (n % 300) == 0) {
			char caller[160] = {};
			DescribeCodeAddress((DWORD64)_ReturnAddress(), caller, sizeof(caller));
			LogInfo("VRPose watch-controller: localtime call=%ld input=%lld "
				"result=%02d:%02d:%02d caller=%s\n", n,
				value ? (long long)*value : -1LL,
				result ? result->tm_hour : -1, result ? result->tm_min : -1,
				result ? result->tm_sec : -1, caller);
		}
		return result;
	}

	static void InstallWatchControllerTrace()
	{
		static bool tried = false;
		if (tried)
			return;
		tried = true;
		HINSTANCE crt = GetModuleHandleA("MSVCR110.dll");
		const int fail = InstallHookLate(crt, "_localtime64",
			(void **)&trampoline_MetroLocaltime64, Hooked_MetroLocaltime64);
		if (fail || !trampoline_MetroLocaltime64)
			LogInfo("VRPose watch-controller: localtime hook FAILED\n");
		else
			LogInfo("VRPose watch-controller: localtime hook installed\n");
	}

	// GAMEPLAY-ONLY NATIVE ADS
	//
	// The active weapon instance used by the live fire route stores its native
	// ADS gameplay state at +0x726. A controlled native-ADS transition proved
	// this byte changes 0->1 on aim-in and 1->0 on aim-out. Drive only that
	// byte, leaving the surrounding animation state untouched.
	typedef float (*tComputeWeaponDispersion)(void *weapon, float scale);
	static tComputeWeaponDispersion trampoline_ComputeWeaponDispersion = NULL;
	static volatile PVOID sGameplayADSWeapon = NULL;

	static void WriteGameplayADSFlag(void *weapon, bool aiming)
	{
		if (!weapon)
			return;
		__try {
			*(volatile unsigned char *)((BYTE *)weapon + 0x726) = aiming ? 1 : 0;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			InterlockedCompareExchangePointer(&sGameplayADSWeapon, NULL, weapon);
		}
	}

	static void UpdateGameplayADSFlag(bool aiming)
	{
		void *weapon = InterlockedCompareExchangePointer(&sADSProbeWeapon, NULL, NULL);
		WriteGameplayADSFlag(weapon, aiming);
	}

	// Run Metro's complete native aim-in state machine only across the actual
	// fire call, then immediately run its matching aim-out state machine.  A
	// render frame cannot occur inside this synchronous window, so gameplay
	// code sees genuine ADS while the viewmodel remains controller-pinned.
	static bool PulseNativeADS(void *weapon, bool enter)
	{
		if (!weapon)
			return false;
		__try {
			void **vtable = *(void ***)weapon;
			typedef void (*tNativeADS)(void *);
			const size_t slot = enter ? 0x1100 : 0x1110;
			tNativeADS fn = (tNativeADS)*(void **)((BYTE *)vtable + slot);
			if (!fn)
				return false;
			fn(weapon);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			LogInfo("VRPose ADS pulse: %s fault at weapon=%p\n",
				enter ? "enter" : "exit", weapon);
			return false;
		}
	}

	static float Hooked_ComputeWeaponDispersion(void *weapon, float scale)
	{
		const bool aiming = IsAiming();
		if (weapon) {
			WriteGameplayADSFlag(weapon, aiming);
			InterlockedExchangePointer(&sGameplayADSWeapon, weapon);
		}

		const float result = trampoline_ComputeWeaponDispersion(weapon, scale);
		static volatile LONG sLogged = 0;
		const LONG n = InterlockedIncrement(&sLogged);
		if (n <= 20 && weapon) {
			float hip = 0.0f, aimed = 0.0f;
			__try {
				hip = *(const float *)((const BYTE *)weapon + 0x718);
				aimed = *(const float *)((const BYTE *)weapon + 0x71C);
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {}
			LogInfo("VRPose ADS: dispersion call weapon=%p gesture=%d hip=%.6f "
				"aimed=%.6f scale=%.6f result=%.6f\n",
				weapon, aiming ? 1 : 0, hip, aimed, scale, result);
		}
		return result;
	}

	static void InstallGameplayADSHook()
	{
		static bool sTried = false;
		if (sTried)
			return;
		sTried = true;

		const unsigned char *base = (const unsigned char *)GetModuleHandleA(NULL);
		if (!base)
			return;

		// Fresh decrypted Metro 2033 Redux build: function prologue at RVA
		// 0x376FF0. Refuse to patch a different executable revision.
		static const unsigned char expected[] = {
			0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x30
		};
		void *fn = (void *)(base + 0x376FF0);
		if (memcmp(fn, expected, sizeof(expected)) != 0) {
			LogInfo("VRPose ADS: hook refused - unexpected bytes at +0x376FF0\n");
			return;
		}

		SIZE_T hookId = 0;
		const DWORD err = cHookMgr.Hook(&hookId,
			(LPVOID *)&trampoline_ComputeWeaponDispersion, fn,
			Hooked_ComputeWeaponDispersion);
		if (err || !trampoline_ComputeWeaponDispersion) {
			LogInfo("VRPose ADS: gameplay hook FAILED at +0x376FF0 (err=0x%x)\n", err);
			return;
		}
		LogInfo("VRPose ADS: gameplay-only native dispersion hook installed at +0x376FF0\n");
	}

	static __int64 Hooked_PreFireVectorCopy(void *rcx, void *rdx, void *r8, void *r9)
	{
		static volatile LONG sVectorCopyLogged = 0;
		const LONG n = InterlockedIncrement(&sVectorCopyLogged);
		float v[4] = {};
		if (r8)
			memcpy(v, r8, sizeof(v));
		if (n <= 200) {
			LogInfo("VRPose fire: native vector-copy rcx=%p rdx=%p r8=%p r9=%p "
				"r8Vec=(%.5f %.5f %.5f %.5f)\n",
				rcx, rdx, r8, r9, v[0], v[1], v[2], v[3]);
		}

		return trampoline_PreFireVectorCopy(rcx, rdx, r8, r9);
	}

	static __int64 Hooked_PreFirePrepare(void *rcx, void *rdx, void *r8, void *r9)
	{
		static volatile LONG sPrepareLogged = 0;
		if (InterlockedIncrement(&sPrepareLogged) <= 200) {
			float v[4] = {};
			if (r8)
				memcpy(v, r8, sizeof(v));
			LogInfo("VRPose fire: pre-fire prepare rcx=%p rdx=%p r8=%p r9=%p "
				"r8Vec=(%.5f %.5f %.5f %.5f)\n",
				rcx, rdx, r8, r9, v[0], v[1], v[2], v[3]);
		}
		return trampoline_PreFirePrepare(rcx, rdx, r8, r9);
	}

	// Metro creates the projectile from its body-camera origin. The rendered VR
	// camera, however, also includes the headset's tracked 6DOF displacement.
	// Under a physical turn the headset moves several centimetres around the
	// original standing point, so leaving the projectile at Metro's unshifted
	// origin produces a signed left/right parallax error on nearby surfaces even
	// when the redirected direction itself is exact. Apply the same translation
	// mapping used by PatchMappedVRCameraData while preserving Metro's native
	// origin, including any weapon-specific offset already present there.
	static bool BuildVRWorldProjectileOrigin(const float *nativeOrigin, float out[4])
	{
		if (!nativeOrigin || !out)
			return false;

		__try {
			memcpy(out, nativeOrigin, sizeof(float) * 4);
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
		if (!isfinite(out[0]) || !isfinite(out[1]) || !isfinite(out[2]))
			return false;

		float headDelta[3] = {};
		if (!GetHeadTranslationDelta(headDelta))
			return false;

		// GetHeadTranslationDelta is expressed in the levelled tracking/reference
		// frame. Rotate it into Metro world space by the tracked body heading,
		// exactly as the rendered-camera translation path does. Do not use Metro's
		// transient +0x170 camera vector here: projectile creation can observe a
		// synthetic zero vector while the rendered camera still has a valid heading.
		float bodyYaw = 0.0f;
		if (!GetRenderedBodyHeading(&bodyYaw))
			return false;
		const float c = cosf(bodyYaw);
		const float s = sinf(bodyYaw);
		out[0] += c * headDelta[0] + s * headDelta[2];
		out[1] += headDelta[1];
		out[2] += -s * headDelta[0] + c * headDelta[2];

		// Ordinary firearms already provide a muzzle-like native origin and keep
		// the headset-validated translation above. Helsing instead supplies a
		// camera-relative origin. Rebuild only that weapon's origin from the
		// tracked controller position using the same rendered-view conversion as
		// the validated normal-firearm gunOrigin path. sWeaponViewPos is not a
		// world/view muzzle coordinate here; at Helsing release it sits only a few
		// centimetres from the HMD, which made arrows visibly originate at the
		// player's head. NPCs never reach this helper because its callers require
		// a positively identified local arrow.
		if (InterlockedCompareExchange(&sHelsingActive, 0, 0) != 0) {
			float camera[3] = {};
			float controllerRelativeToHead[3] = {};
			float viewBasis[9] = {};
			if (ReadCameraVec(0x180, camera)
				&& GetControllerRelativeToHead(controllerRelativeToHead)
				&& GetCurrentRenderedViewBasis(viewBasis)) {
				out[0] = camera[0] + c * headDelta[0] + s * headDelta[2]
					+ controllerRelativeToHead[0] * viewBasis[0]
					+ controllerRelativeToHead[1] * viewBasis[3]
					+ controllerRelativeToHead[2] * viewBasis[6];
				out[1] = camera[1] + headDelta[1]
					+ controllerRelativeToHead[0] * viewBasis[1]
					+ controllerRelativeToHead[1] * viewBasis[4]
					+ controllerRelativeToHead[2] * viewBasis[7];
				out[2] = camera[2] - s * headDelta[0] + c * headDelta[2]
					+ controllerRelativeToHead[0] * viewBasis[2]
					+ controllerRelativeToHead[1] * viewBasis[5]
					+ controllerRelativeToHead[2] * viewBasis[8];
			}
		}
		return isfinite(out[0]) && isfinite(out[1]) && isfinite(out[2]);
	}

	static bool IsPlayerProjectileOwner(void *owner)
	{
		if (!owner)
			return false;
		void *player = NULL;
		if (!ResolveMetroPlayer(&player) || !player)
			return false;
		return owner == player;
	}

	static bool BuildVRWorldBarrelDirection(float out[3])
	{
		// Use the exact same canonical barrel axis used by the rendered weapon,
		// reticle, and shot path.
		float barrelView[3] = {};
		if (!GetCanonicalBarrelViewDirection(barrelView))
			return false;

		float currentView[9] = {};
		if (GetCurrentRenderedViewBasis(currentView)) {
			out[0] = barrelView[0]*currentView[0] + barrelView[1]*currentView[3] + barrelView[2]*currentView[6];
			out[1] = barrelView[0]*currentView[1] + barrelView[1]*currentView[4] + barrelView[2]*currentView[7];
			out[2] = barrelView[0]*currentView[2] + barrelView[1]*currentView[5] + barrelView[2]*currentView[8];
		} else {
			if (!G->vrCachedViewValid)
				return false;
			const float *view = G->vrCachedView3x4;
			out[0] = barrelView[0]*view[0] + barrelView[1]*view[4] + barrelView[2]*view[8];
			out[1] = barrelView[0]*view[1] + barrelView[1]*view[5] + barrelView[2]*view[9];
			out[2] = barrelView[0]*view[2] + barrelView[1]*view[6] + barrelView[2]*view[10];
		}
		const float len = sqrtf(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]);
		if (!isfinite(len) || len < 1e-4f)
			return false;
		out[0] /= len; out[1] /= len; out[2] /= len;
		return isfinite(out[0]) && isfinite(out[1]) && isfinite(out[2]);
	}

	static bool BuildUpstreamVRTraceEndpoint(float out[4])
	{
		if (!IsFiring())
			return false;
		float camPos[3] = {}, dir[3] = {};
		if (!ReadCameraVec(0x180, camPos) || !BuildVRWorldBarrelDirection(dir))
			return false;

		// The pre-fire input is an endpoint. Put it well beyond normal level
		// geometry so Metro's own trace, not our old hit-point relocation, picks
		// the first collision and retains native range/collision behavior.
		const float kTraceDistance = 1000.0f;
		out[0] = camPos[0] + dir[0] * kTraceDistance;
		out[1] = camPos[1] + dir[1] * kTraceDistance;
		out[2] = camPos[2] + dir[2] * kTraceDistance;
		out[3] = 1.0f;
		return true;
	}

	static __int64 Hooked_ProjectileCreate(void *owner, const float *origin,
		const float *direction, void *arg4, unsigned __int64 arg5,
		unsigned __int64 arg6, unsigned __int64 arg7, unsigned __int64 arg8,
		unsigned __int64 arg9, unsigned __int64 arg10)
	{
		float originalOrigin[4] = {};
		float replacementOrigin[4] = {};
		float original[4] = {};
		float replacement[4] = {};
		float nativeDeviationDeg = 0.0f;
		float dispersionScale = 1.0f;
		bool redirected = false;
		bool originRedirected = false;
		// The projectile constructor is shared by the player and NPC weapons.
		// BuildVRWorldBarrelDirection() is always available while a VR weapon is
		// rendered, so using it as the only eligibility test redirects NPC rounds
		// toward the player's current aim even when the player is not firing.
		// Restrict the rewrite to the player's active trigger window; NPC rounds
		// must keep Metro's native origin and direction.
		// Helsing can create its projectile after the physical trigger state has
		// already fallen back to idle.  Keep the trigger gate for normal shots,
		// but also accept Metro's live player object as a positive ownership test.
		// NPC projectiles have neither the player owner nor the player trigger
		// window, so they retain their native origin and direction.
		const bool playerOwner = IsPlayerProjectileOwner(owner);
		const bool helsingActive =
			InterlockedCompareExchange(&sHelsingActive, 0, 0) != 0;
		// Helsing does not use this constructor for its authoritative bolt.
		// Suppress the shared redirect while it is equipped so unrelated NPC
		// projectile traffic cannot be captured merely because the trigger is held.
		const bool playerFireWindow = !helsingActive
			&& (IsFiring() || playerOwner);
		const bool aiming = IsAiming();
		static const bool kProjectileBasisDiagnostic = false;
		if (kProjectileBasisDiagnostic) {
			float camFwd[3] = {};
			float rendered[9] = {};
			const bool camValid = ReadCameraVec(0x170, camFwd);
			const bool renderedValid = GetCurrentRenderedViewBasis(rendered);
			static unsigned sProjectileBasisLogged = 0;
			if (sProjectileBasisLogged < 160) {
				sProjectileBasisLogged++;
				const float camYaw = camValid ? atan2f(camFwd[0], camFwd[2]) : 0.0f;
				const float renderYaw = renderedValid ? atan2f(rendered[6], rendered[8]) : 0.0f;
				LogInfo("VRPose fire: PROJECTILE BASIS camYaw=%+.2f renderedYaw=%+.2f valid=%d\n",
					camYaw * 57.29578f, renderYaw * 57.29578f,
					renderedValid ? 1 : 0);
			}
		}

		if (kProjectileCreationRedirect && playerFireWindow && direction
			&& BuildVRWorldBarrelDirection(replacement)) {
			__try {
				memcpy(original, direction, sizeof(original));
				replacement[3] = original[3];
				const float ol = sqrtf(original[0]*original[0]
					+ original[1]*original[1] + original[2]*original[2]);
				if (isfinite(ol) && ol > 0.5f && ol < 1.5f) {
					original[0] /= ol;
					original[1] /= ol;
					original[2] /= ol;

					// The incoming creation direction already contains Metro's native
					// dispersion. Carry its angular departure from the native camera ray
					// onto the VR barrel. While braced, scale that departure by Metro's
					// own aimed/hip dispersion ratio from the live weapon.
					if (aiming) {
						dispersionScale = 0.0f;
						void *weapon = InterlockedCompareExchangePointer(
							&sADSProbeWeapon, NULL, NULL);
						if (weapon) {
							float hip = 0.0f, aimed = 0.0f;
							__try {
								hip = *(const float *)((const BYTE *)weapon + 0x718);
								aimed = *(const float *)((const BYTE *)weapon + 0x71C);
							}
							__except (EXCEPTION_EXECUTE_HANDLER) {}
							if (isfinite(hip) && isfinite(aimed) && hip > 1e-6f
								&& aimed >= 0.0f) {
								dispersionScale = aimed / hip;
								if (dispersionScale < 0.0f) dispersionScale = 0.0f;
								if (dispersionScale > 1.0f) dispersionScale = 1.0f;
							}
						}
					}

					float camFwd[3] = {};
					if (ReadCameraVec(0x170, camFwd)) {
						const float clampedOriginalY = original[1] < -1.0f ? -1.0f
							: (original[1] > 1.0f ? 1.0f : original[1]);
						const float clampedCameraY = camFwd[1] < -1.0f ? -1.0f
							: (camFwd[1] > 1.0f ? 1.0f : camFwd[1]);
						float dy = WrapAngle(atan2f(original[0], original[2])
							- atan2f(camFwd[0], camFwd[2]));
						float dp = asinf(clampedOriginalY) - asinf(clampedCameraY);
						const float deviation = sqrtf(dy*dy + dp*dp);
						nativeDeviationDeg = deviation * 57.29578f;
						if (isfinite(deviation) && deviation < 0.2617994f) {
							dy *= dispersionScale;
							dp *= dispersionScale;
							const float yaw = atan2f(replacement[0], replacement[2]) + dy;
							const float baseY = replacement[1] < -1.0f ? -1.0f
								: (replacement[1] > 1.0f ? 1.0f : replacement[1]);
							const float pitch = asinf(baseY) + dp;
							const float cp = cosf(pitch);
							replacement[0] = sinf(yaw) * cp;
							replacement[1] = sinf(pitch);
							replacement[2] = cosf(yaw) * cp;
						}
					}
					redirected = true;
				}
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {}
		}
		// Only move the origin for a projectile whose direction was positively
		// identified and redirected by the existing player-shot path.
		if (redirected && origin) {
			__try {
				memcpy(originalOrigin, origin, sizeof(originalOrigin));
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {}
			originRedirected = BuildVRWorldProjectileOrigin(origin,
				replacementOrigin);
		}

		static volatile LONG sLogged = 0;
		// Keep the bounded native diagnostic, but never let NPC traffic consume
		// the records needed to verify a player/Helsing shot.
		if (playerFireWindow || InterlockedIncrement(&sLogged) <= 40) {
			const float originShift = originRedirected ? sqrtf(
				(replacementOrigin[0] - originalOrigin[0]) * (replacementOrigin[0] - originalOrigin[0])
				+ (replacementOrigin[1] - originalOrigin[1]) * (replacementOrigin[1] - originalOrigin[1])
				+ (replacementOrigin[2] - originalOrigin[2]) * (replacementOrigin[2] - originalOrigin[2])) : 0.0f;
			LogInfo("VRPose fire: PROJECTILE CREATE redirected=%d originRedirected=%d originShift=%.4f aiming=%d "
				"firing=%d playerOwner=%d helsing=%d owner=%p "
				"nativeDeviation=%.3f dispersionScale=%.3f "
				"origin=(%.4f %.4f %.4f) newOrigin=(%.4f %.4f %.4f) "
				"original=(%.4f %.4f %.4f) replacement=(%.4f %.4f %.4f)\n",
				redirected ? 1 : 0, originRedirected ? 1 : 0, originShift,
				 aiming ? 1 : 0,
				 IsFiring() ? 1 : 0, playerOwner ? 1 : 0,
				 InterlockedCompareExchange(&sHelsingActive, 0, 0) != 0 ? 1 : 0,
				 owner,
				nativeDeviationDeg, dispersionScale,
				originalOrigin[0], originalOrigin[1], originalOrigin[2],
				replacementOrigin[0], replacementOrigin[1], replacementOrigin[2],
				original[0], original[1], original[2],
				replacement[0], replacement[1], replacement[2]);
		}

		return trampoline_ProjectileCreate(owner,
			originRedirected ? replacementOrigin : origin,
			redirected ? replacement : direction, arg4, arg5, arg6, arg7,
			arg8, arg9, arg10);
	}

	static __int64 Hooked_BallisticCollision(void *owner, void *shot, void *outHits,
		void *distanceState, void *outState, void *outFlag, void *context)
	{
		float original[3] = {}, replacement[3] = {};
		float nativeDeviationDeg = 0.0f;
		bool redirected = false;
		bool propagatedRedirect = false;
		static float sLastRedirectedDirection[3] = {};
		static unsigned sLastRedirectFrame = ~0u;

		// +0x3D9A20 is the first confirmed consumer of the direction used by
		// Metro's collision loop. Replace the centre ray here, after Metro has
		// constructed the shot but before it tests any world geometry.
		if (kBallisticCollisionRedirect && shot
			&& BuildVRWorldBarrelDirection(replacement)) {
			__try {
				memcpy(original, (BYTE *)shot + 0xF0, sizeof(original));
				const float ol = sqrtf(original[0]*original[0]
					+ original[1]*original[1] + original[2]*original[2]);
				if (isfinite(ol) && ol > 0.5f && ol < 1.5f) {
					original[0] /= ol;
					original[1] /= ol;
					original[2] /= ol;

					// Metro immediately runs a second collision pass using a copy of
					// the descriptor modified by the first pass. Do not interpret that
					// already-VR direction as a new native spread delta and rotate it a
					// second time. The paired calls occur in the same render frame and
					// the copied vector matches our preceding output.
					const unsigned frame = G ? G->frame_no : 0;
					const float propagatedError =
						fabsf(original[0] - sLastRedirectedDirection[0])
						+ fabsf(original[1] - sLastRedirectedDirection[1])
						+ fabsf(original[2] - sLastRedirectedDirection[2]);
					const unsigned frameDelta = frame - sLastRedirectFrame;
					propagatedRedirect = frameDelta <= 1
						&& propagatedError < 0.0003f;
					if (propagatedRedirect)
						memcpy(replacement, original, sizeof(replacement));

					// Preserve Metro's already-computed recoil/spread as a small angular
					// departure from its native camera ray, then apply that departure to
					// the VR barrel. A large departure means the vectors are not in the
					// same convention, so use the barrel direction without that delta.
					float camFwd[3] = {};
					if (!propagatedRedirect && ReadCameraVec(0x170, camFwd)) {
						const float clampedOriginalY = original[1] < -1.0f ? -1.0f
							: (original[1] > 1.0f ? 1.0f : original[1]);
						const float clampedCameraY = camFwd[1] < -1.0f ? -1.0f
							: (camFwd[1] > 1.0f ? 1.0f : camFwd[1]);
						const float dy = WrapAngle(atan2f(original[0], original[2])
							- atan2f(camFwd[0], camFwd[2]));
						const float dp = asinf(clampedOriginalY) - asinf(clampedCameraY);
						const float deviation = sqrtf(dy*dy + dp*dp);
						nativeDeviationDeg = deviation * 57.29578f;
						if (isfinite(deviation) && deviation < 0.2617994f) {
							const float yaw = atan2f(replacement[0], replacement[2]) + dy;
							const float baseY = replacement[1] < -1.0f ? -1.0f
								: (replacement[1] > 1.0f ? 1.0f : replacement[1]);
							const float pitch = asinf(baseY) + dp;
							const float cp = cosf(pitch);
							replacement[0] = sinf(yaw) * cp;
							replacement[1] = sinf(pitch);
							replacement[2] = cosf(yaw) * cp;
						}
					}

					memcpy((BYTE *)shot + 0xF0, replacement, sizeof(replacement));
					if (!propagatedRedirect) {
						memcpy(sLastRedirectedDirection, replacement,
							sizeof(sLastRedirectedDirection));
						sLastRedirectFrame = frame;
					}
					redirected = true;
				}
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {}
		}

		static volatile LONG sLogged = 0;
		if (InterlockedIncrement(&sLogged) <= 30) {
			LogInfo("VRPose fire: BALLISTIC COLLISION redirected=%d propagated=%d frame=%u "
				"nativeDeviation=%.3f original=(%.4f %.4f %.4f) "
				"replacement=(%.4f %.4f %.4f) shot=%p\n",
				redirected ? 1 : 0, propagatedRedirect ? 1 : 0,
				G ? G->frame_no : 0, nativeDeviationDeg,
				original[0], original[1], original[2],
				replacement[0], replacement[1], replacement[2], shot);
		}

		return trampoline_BallisticCollision(owner, shot, outHits, distanceState,
			outState, outFlag, context);
	}

	static void Hooked_ShotProducer(void *owner, unsigned int kind,
		unsigned char active, void *shot, void *weaponData,
		unsigned short weaponIndex, float distance, void *context)
	{
		float original[3] = {}, replacement[3] = {};
		bool redirected = false;
		if (kShotProducerRedirect && shot
			&& BuildVRWorldBarrelDirection(replacement)) {
			__try {
				memcpy(original, (BYTE *)shot + 0xF0, sizeof(original));
				memcpy((BYTE *)shot + 0xF0, replacement, sizeof(replacement));
				redirected = true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {}
		}
		static volatile LONG sLogged = 0;
		if (InterlockedIncrement(&sLogged) <= 30)
			LogInfo("VRPose fire: SHARED PRODUCER redirected=%d "
				"original=(%.4f %.4f %.4f) replacement=(%.4f %.4f %.4f) "
				"shot=%p kind=%u active=%u\n",
				redirected ? 1 : 0,
				original[0], original[1], original[2],
				replacement[0], replacement[1], replacement[2],
				shot, kind, (unsigned)active);

		trampoline_ShotProducer(owner, kind, active, shot, weaponData,
			weaponIndex, distance, context);
	}

	static __int64 Hooked_AuthoritativeShot(void *owner, void *shot, void *context)
	{
		float original[3] = {}, replacement[3] = {};
		bool redirected = false;
		float nativeDeviationDeg = 0.0f;
		if (kAuthoritativeShotRedirect && shot
			&& BuildVRWorldBarrelDirection(replacement)) {
			__try {
				memcpy(original, (BYTE *)shot + 0xF0, sizeof(original));
				const float ol = sqrtf(original[0]*original[0]
					+ original[1]*original[1] + original[2]*original[2]);
				if (isfinite(ol) && ol > 0.5f && ol < 1.5f) {
					original[0] /= ol; original[1] /= ol; original[2] /= ol;

					// +0xF0 has already received Metro's native dispersion by this
					// stage. Carry only its small angular departure from the native
					// camera ray onto the VR barrel centre; large departures indicate
					// a different coordinate convention and are safely ignored.
					float camFwd[3] = {};
					if (ReadCameraVec(0x170, camFwd)) {
						const float clampedOriginalY = original[1] < -1.0f ? -1.0f
							: (original[1] > 1.0f ? 1.0f : original[1]);
						const float clampedCameraY = camFwd[1] < -1.0f ? -1.0f
							: (camFwd[1] > 1.0f ? 1.0f : camFwd[1]);
						const float dy = WrapAngle(atan2f(original[0], original[2])
							- atan2f(camFwd[0], camFwd[2]));
						const float dp = asinf(clampedOriginalY) - asinf(clampedCameraY);
						const float deviation = sqrtf(dy*dy + dp*dp);
						nativeDeviationDeg = deviation * 57.29578f;
						if (isfinite(deviation) && deviation < 0.2617994f) {
							const float yaw = atan2f(replacement[0], replacement[2]) + dy;
							const float baseY = replacement[1] < -1.0f ? -1.0f
								: (replacement[1] > 1.0f ? 1.0f : replacement[1]);
							const float pitch = asinf(baseY) + dp;
							const float cp = cosf(pitch);
							replacement[0] = sinf(yaw) * cp;
							replacement[1] = sinf(pitch);
							replacement[2] = cosf(yaw) * cp;
						}
					}
					memcpy((BYTE *)shot + 0xF0, replacement, sizeof(replacement));
					redirected = true;
				}
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {}
		}

		static volatile LONG sLogged = 0;
		const LONG logIndex = InterlockedIncrement(&sLogged);
		if (logIndex <= 30)
			LogInfo("VRPose fire: AUTHORITATIVE pre-collision redirected=%d "
				"nativeDeviation=%.3f original=(%.4f %.4f %.4f) "
				"replacement=(%.4f %.4f %.4f) shot=%p\n",
				redirected ? 1 : 0, nativeDeviationDeg,
				original[0], original[1], original[2],
				replacement[0], replacement[1], replacement[2], shot);

		return trampoline_AuthoritativeShot(owner, shot, context);
	}

	static void Hooked_PrepareShot(void *shot, const float *origin, void *weaponData,
		unsigned int weaponIndex, float distance, void *outA, void *outB, void *outC)
	{
		if (kPreSpreadShotRedirect && shot) {
			float direction[3] = {};
			if (BuildVRWorldBarrelDirection(direction)) {
				float original[3] = {};
				__try {
					memcpy(original, (BYTE *)shot + 0xF0, sizeof(original));
					memcpy((BYTE *)shot + 0xF0, direction, sizeof(direction));
				}
				__except (EXCEPTION_EXECUTE_HANDLER) {}
				static volatile LONG sLogged = 0;
				if (InterlockedIncrement(&sLogged) <= 20)
					LogInfo("VRPose fire: PRE-SPREAD direction original=(%.4f %.4f %.4f) "
						"replacement=(%.4f %.4f %.4f) shot=%p\n",
						original[0], original[1], original[2],
						direction[0], direction[1], direction[2], shot);
			}
		}
		trampoline_PrepareShot(shot, origin, weaponData, weaponIndex,
			distance, outA, outB, outC);
	}

	static __int64 Hooked_PreFireTrace(void *rcx, void *rdx, void *r8, void *r9)
	{
		static volatile LONG sTraceLogged = 0;
		if (kUpstreamPreFireRedirect && rdx) {
			float replacement[4] = {};
			if (BuildUpstreamVRTraceEndpoint(replacement)) {
				float original[4] = {};
				memcpy(original, rdx, sizeof(original));
				memcpy(rdx, replacement, sizeof(replacement));
				static volatile LONG sRedirectLogged = 0;
				if (InterlockedIncrement(&sRedirectLogged) <= 20)
					LogInfo("VRPose fire: PRETRACE VR endpoint original=(%.3f %.3f %.3f %.3f) "
						"replacement=(%.3f %.3f %.3f %.3f)\n",
						original[0], original[1], original[2], original[3],
						replacement[0], replacement[1], replacement[2], replacement[3]);
			}
		}
		if (InterlockedIncrement(&sTraceLogged) <= 200) {
			float v[4] = {};
			if (rdx)
				memcpy(v, rdx, sizeof(v));
			LogInfo("VRPose fire: pre-fire trace rcx=%p rdx=%p r8=%p r9=%p "
				"rdxVec=(%.5f %.5f %.5f %.5f)\n",
				rcx, rdx, r8, r9, v[0], v[1], v[2], v[3]);
		}
		return trampoline_PreFireTrace(rcx, rdx, r8, r9);
	}

	static __int64 Hooked_FireWrapper(void *rcx, void *obj)
	{
		// Retired shot-construction diagnostics. These performed several
		// ReadProcessMemory calls around each sampled shot and captured/resolved
		// native stacks. Keep all functional fire-wrapper work below unchanged.
		static const bool kFireWrapperDiagnostics = false;

		// Do not collapse same-frame records. Metro deliberately produces more
		// than one consumer record for some rounds even though ammo drops once.
		// The shared producer now gives all of them the same VR direction;
		// discarding one here can unpredictably discard the damage consumer.

		// Read-only split diagnostic.  Capture the native shot fields before
		// RedirectShot, immediately after our rewrite, and after Metro's fire
		// routine returns.  This distinguishes a native collision result from a
		// relocated visual hit point without changing any fields itself.
		float diagBeforeOrigin[3] = {}, diagBeforeDir[3] = {}, diagBeforeTarget[3] = {};
		bool diagCaptured = false;
		if (kFireWrapperDiagnostics && obj) {
			static volatile LONG sDiagCalls = 0;
			if (InterlockedIncrement(&sDiagCalls) <= 16) {
				SIZE_T got = 0;
				diagCaptured = ReadProcessMemory(GetCurrentProcess(),
					(void *)((DWORD64)obj + 0x0E0), diagBeforeOrigin, sizeof(diagBeforeOrigin), &got)
					&& got == sizeof(diagBeforeOrigin);
				if (diagCaptured)
					diagCaptured = ReadProcessMemory(GetCurrentProcess(),
					(void *)((DWORD64)obj + 0x0F0), diagBeforeDir, sizeof(diagBeforeDir), &got)
					&& got == sizeof(diagBeforeDir);
				if (diagCaptured)
					diagCaptured = ReadProcessMemory(GetCurrentProcess(),
					(void *)((DWORD64)obj + 0x150), diagBeforeTarget, sizeof(diagBeforeTarget), &got)
					&& got == sizeof(diagBeforeTarget);
				if (diagCaptured)
					LogInfo("VRPose shot-split: PRE frame=%u obj=%016llX origin=(%.3f %.3f %.3f) "
						"dir=(%.3f %.3f %.3f) target=(%.3f %.3f %.3f)\n",
						G ? G->frame_no : 0, (unsigned long long)obj,
						diagBeforeOrigin[0], diagBeforeOrigin[1], diagBeforeOrigin[2],
						diagBeforeDir[0], diagBeforeDir[1], diagBeforeDir[2],
						diagBeforeTarget[0], diagBeforeTarget[1], diagBeforeTarget[2]);
			}
		}

		void *liveWeapon = NULL;
		if (rcx)
			InterlockedExchangePointer(&sADSProbeFireOwner, rcx);
		if (obj) {
			void *service = NULL;
			void *weapon = NULL;
			__try {
				service = *(void **)((BYTE *)obj + 0x108);
				if (service)
					weapon = *(void **)service;
				if (weapon)
					weapon = (BYTE *)weapon - 0xA0;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {}
			if (service)
				InterlockedExchangePointer(&sADSProbeService, service);
			if (weapon) {
				liveWeapon = weapon;
				InterlockedExchangePointer(&sADSProbeWeapon, weapon);
				InterlockedExchangePointer(&sGameplayADSWeapon, weapon);
				// Refresh immediately before the native fire wrapper consumes
				// weapon state; this removes any frame-order ambiguity.
				WriteGameplayADSFlag(weapon, IsAiming());
			}
		}
		if (kFireWrapperDiagnostics && obj) {
			static volatile LONG sCallerLogged = 0;
			if (InterlockedIncrement(&sCallerLogged) <= 3) {
				char desc[160] = { 0 };
				const DWORD64 caller = (DWORD64)_ReturnAddress();
				DescribeCodeAddress(caller, desc, sizeof(desc));
				LogInfo("VRPose fire: wrapper caller %s\n", desc);

				void *frames[8] = {};
				const USHORT count = CaptureStackBackTrace(1, ARRAYSIZE(frames), frames, NULL);
				for (USHORT i = 0; i < count; ++i) {
					char frameDesc[160] = { 0 };
					DescribeCodeAddress((DWORD64)frames[i], frameDesc, sizeof(frameDesc));
					LogInfo("VRPose fire:   stack[%u] %s\n", (unsigned)i, frameDesc);
				}

				// The caller at metro.exe+0x3D85A4 first invokes a virtual
				// function with &obj+0x140 and &obj+0x150, then calls the fire
				// wrapper at +0x4F3E60. Resolve that vtable slot while the same
				// object is live; this is the likely native shot-construction
				// routine where recoil/spread is applied.
				DWORD64 service = 0, candidate = 0, adjusted = 0, vtable = 0, target = 0;
				SIZE_T got = 0;
				ReadProcessMemory(GetCurrentProcess(), (const void *)((DWORD64)obj + 0x108),
					&service, sizeof(service), &got);
				if (service)
					ReadProcessMemory(GetCurrentProcess(), (const void *)service,
						&candidate, sizeof(candidate), &got);
				// The call site adjusts the candidate by -0xA0 before loading
				// its vtable, matching the engine's embedded-subobject layout.
				if (candidate >= 0xA0)
					adjusted = candidate - 0xA0;
				if (adjusted)
					ReadProcessMemory(GetCurrentProcess(), (const void *)adjusted,
						&vtable, sizeof(vtable), &got);
				if (vtable)
					ReadProcessMemory(GetCurrentProcess(), (const void *)(vtable + 0x200),
						&target, sizeof(target), &got);
				char targetDesc[160] = { 0 };
				if (target)
					DescribeCodeAddress(target, targetDesc, sizeof(targetDesc));
				LogInfo("VRPose fire: shot-prep virtual service=%016llX candidate=%016llX "
					"adjusted=%016llX vtable=%016llX slot+0x200=%016llX (%s)\n",
					(unsigned long long)service, (unsigned long long)candidate,
					(unsigned long long)adjusted, (unsigned long long)vtable,
					(unsigned long long)target, target ? targetDesc : "unresolved");
			}
		}

		if (liveWeapon
			&& InterlockedCompareExchange(&sHelsingActive, 0, 0) != 0) {
			static volatile LONG sHelsingWeaponLogged = 0;
			if (InterlockedIncrement(&sHelsingWeaponLogged) <= 12) {
				void **vtable = NULL;
				BYTE *base = (BYTE *)GetModuleHandleA(NULL);
				unsigned long long vtableRva = 0;
				unsigned long long slots[6] = {};
				__try {
					vtable = *(void ***)liveWeapon;
					if (vtable && base) {
						vtableRva = (BYTE *)vtable - base;
						for (unsigned i = 0; i < ARRAYSIZE(slots); ++i)
							slots[i] = (BYTE *)vtable[0x27 + i] - base;
					}
				}
				__except (EXCEPTION_EXECUTE_HANDLER) {
					vtable = NULL;
					vtableRva = 0;
					memset(slots, 0, sizeof(slots));
				}
				LogInfo("VRPose Helsing: wrapper rcx=%p shot=%p weapon=%p vtable=%p "
					"vtableRva=+0x%llX slots27-2c=(+%llX +%llX +%llX +%llX +%llX +%llX)\n",
					rcx, obj, liveWeapon, vtable,
					vtableRva, slots[0], slots[1], slots[2], slots[3],
					slots[4], slots[5]);
			}
			static volatile LONG sHelsingRegistryLogged = 0;
			if (InterlockedIncrement(&sHelsingRegistryLogged) <= 4) {
				BYTE *base = (BYTE *)GetModuleHandleA(NULL);
				const LONG cursor = InterlockedCompareExchange(
					&sPneumaticWeaponRegistryCursor, 0, 0);
				LogInfo("VRPose Helsing: pneumatic registry cursor=%ld live=%p\n",
					cursor, InterlockedCompareExchangePointer(
						&sLivePneumaticWeapon, NULL, NULL));
				for (unsigned age = 0; age < kPneumaticWeaponRegistrySize
					&& age < (unsigned)cursor; ++age) {
					const unsigned slot = (unsigned)(cursor - 1 - (LONG)age)
						% kPneumaticWeaponRegistrySize;
					void *candidate = InterlockedCompareExchangePointer(
						&sPneumaticWeaponRegistry[slot], NULL, NULL);
					void *candidateVtable = NULL;
					__try {
						if (candidate)
							candidateVtable = *(void **)candidate;
					}
					__except (EXCEPTION_EXECUTE_HANDLER) {
						candidateVtable = NULL;
					}
					LogInfo("VRPose Helsing: pneumatic registry age=%u weapon=%p "
						"vtable=%p rva=+0x%llX\n", age, candidate,
						candidateVtable,
						(unsigned long long)(candidateVtable && base
							? (BYTE *)candidateVtable - base : 0));
				}
			}
		}

		// Helsing bypasses this shared shot object and launches its concrete
		// arrow through the type-specific path above. Do not redirect every
		// shared wrapper merely because the local player is holding Helsing:
		// NPC fire reaches this wrapper too.
		if (obj && InterlockedCompareExchange(&sHelsingActive, 0, 0) == 0
			&& !kUpstreamPreFireRedirect && !kPreSpreadShotRedirect
			&& !kAuthoritativeShotRedirect && !kShotProducerRedirect
			&& !kBallisticCollisionRedirect && !kProjectileCreationRedirect)
			RedirectShot((DWORD64)obj);

		if (diagCaptured) {
			float diagAfterOrigin[3] = {}, diagAfterDir[3] = {}, diagAfterTarget[3] = {};
			SIZE_T got = 0;
			if (ReadProcessMemory(GetCurrentProcess(), (void *)((DWORD64)obj + 0x0E0),
				diagAfterOrigin, sizeof(diagAfterOrigin), &got)
				&& got == sizeof(diagAfterOrigin)
				&& ReadProcessMemory(GetCurrentProcess(), (void *)((DWORD64)obj + 0x0F0),
					diagAfterDir, sizeof(diagAfterDir), &got)
				&& got == sizeof(diagAfterDir)
				&& ReadProcessMemory(GetCurrentProcess(), (void *)((DWORD64)obj + 0x150),
					diagAfterTarget, sizeof(diagAfterTarget), &got)
				&& got == sizeof(diagAfterTarget)) {
				LogInfo("VRPose shot-split: REDIRECT frame=%u obj=%016llX origin=(%.3f %.3f %.3f) "
					"dir=(%.3f %.3f %.3f) target=(%.3f %.3f %.3f)\n",
					G ? G->frame_no : 0, (unsigned long long)obj,
					diagAfterOrigin[0], diagAfterOrigin[1], diagAfterOrigin[2],
					diagAfterDir[0], diagAfterDir[1], diagAfterDir[2],
					diagAfterTarget[0], diagAfterTarget[1], diagAfterTarget[2]);
			}
		}

		// Native-shot diagnostics: snapshot the object immediately before the
		// real wrapper/callee runs.  Comparing it afterward tells us which
		// fields Metro actually mutates during a shot, without changing any
		// inputs or requiring a data breakpoint on a newly allocated object.
		unsigned char nativeBefore[0x300] = {};
		const bool captureNativeDiff = kNativeShotDiagnostics && obj
			&& InterlockedCompareExchange(&sNativeShotDiagnosticLogged, 0, 0) != 0;
		if (captureNativeDiff)
			ReadProcessMemory(GetCurrentProcess(), obj, nativeBefore,
				sizeof(nativeBefore), NULL);

		// Arm this immediately before the real fire call.  The engine's recoil
		// can move its camera on the same frame or on one of the next few frames;
		// protecting the classifier, rather than cancelling the camera motion,
		// preserves the native recoil path while keeping the VR view head-stable.
		if (G)
			InterlockedExchange(&sRecoilPitchIgnoreUntilFrame,
				(LONG)G->frame_no + kRecoilPitchIgnoreFrames);

		const bool pulseADS = IsAiming() && PulseNativeADS(liveWeapon, true);
		if (pulseADS) {
			static volatile LONG sPulseLogged = 0;
			if (InterlockedIncrement(&sPulseLogged) <= 20)
				LogInfo("VRPose ADS pulse: native aim-in around fire call weapon=%p\n",
					liveWeapon);
		}
		const __int64 result = trampoline_FireWrapper(rcx, obj);
		if (pulseADS)
			PulseNativeADS(liveWeapon, false);

		if (diagCaptured) {
			float diagPostOrigin[3] = {}, diagPostDir[3] = {}, diagPostTarget[3] = {};
			SIZE_T got = 0;
			if (ReadProcessMemory(GetCurrentProcess(), (void *)((DWORD64)obj + 0x0E0),
				diagPostOrigin, sizeof(diagPostOrigin), &got)
				&& got == sizeof(diagPostOrigin)
				&& ReadProcessMemory(GetCurrentProcess(), (void *)((DWORD64)obj + 0x0F0),
					diagPostDir, sizeof(diagPostDir), &got)
				&& got == sizeof(diagPostDir)
				&& ReadProcessMemory(GetCurrentProcess(), (void *)((DWORD64)obj + 0x150),
					diagPostTarget, sizeof(diagPostTarget), &got)
				&& got == sizeof(diagPostTarget)) {
				LogInfo("VRPose shot-split: POST frame=%u obj=%016llX origin=(%.3f %.3f %.3f) "
					"dir=(%.3f %.3f %.3f) target=(%.3f %.3f %.3f) result=%lld\n",
					G ? G->frame_no : 0, (unsigned long long)obj,
					diagPostOrigin[0], diagPostOrigin[1], diagPostOrigin[2],
					diagPostDir[0], diagPostDir[1], diagPostDir[2],
					diagPostTarget[0], diagPostTarget[1], diagPostTarget[2], result);
			}
		}

		if (captureNativeDiff) {
			static volatile LONG sNativeDiffShots = 0;
			const LONG shotNo = InterlockedIncrement(&sNativeDiffShots);
			if (shotNo <= 3) {
				unsigned char nativeAfter[0x300] = {};
				SIZE_T got = 0;
				if (ReadProcessMemory(GetCurrentProcess(), obj, nativeAfter,
						sizeof(nativeAfter), &got) && got == sizeof(nativeAfter)) {
					LogInfo("VRPose fire: native object diff shot %ld (16-byte blocks changed by callee):\n",
						shotNo);
					for (DWORD off = 0; off < sizeof(nativeBefore); off += 0x10) {
						if (memcmp(nativeBefore + off, nativeAfter + off, 0x10) == 0)
							continue;
						const unsigned int *before = (const unsigned int *)(nativeBefore + off);
						const unsigned int *after = (const unsigned int *)(nativeAfter + off);
						LogInfo("VRPose fire:   +0x%03X %08X %08X %08X %08X -> "
							"%08X %08X %08X %08X\n", off,
							before[0], before[1], before[2], before[3],
							after[0], after[1], after[2], after[3]);
					}
				}
			}
		}

		if (kNativeShotDiagnostics && obj)
			ArmNextShotWriteWatch((DWORD64)obj);

		// DID THE ENGINE KEEP OUR RAY?
		//
		// Read the same fields back now the engine's own function has run.
		// Three outcomes, and they call for different work:
		//
		//   KEPT      - the fields still hold the muzzle values we wrote, so
		//               the engine consumed our ray. Any remaining error is
		//               in the aim, not the origin.
		//   OVERWRITTEN - they have gone back to camera values, so the engine
		//               recomputes the origin itself and writing it here can
		//               never work. The hunt moves downstream of this call.
		//
		// dOurs/dCam say which of the two the value now matches, in metres,
		// so it is not a judgement call.
		if (obj && InterlockedExchange(&sRayWritePending, 0) == 1) {
			float o[3] = { 0, 0, 0 }, d[3] = { 0, 0, 0 };
			SIZE_T got = 0;
			ReadProcessMemory(GetCurrentProcess(), (void *)((DWORD64)obj + 0x0E0),
				o, sizeof(o), &got);
			ReadProcessMemory(GetCurrentProcess(), (void *)((DWORD64)obj + 0x0F0),
				d, sizeof(d), &got);

			const float dOurs = fabsf(o[0] - sLastWrittenOrigin[0])
				+ fabsf(o[1] - sLastWrittenOrigin[1])
				+ fabsf(o[2] - sLastWrittenOrigin[2]);
			const float dCam = fabsf(o[0] - sLastCamPosAtShot[0])
				+ fabsf(o[1] - sLastCamPosAtShot[1])
				+ fabsf(o[2] - sLastCamPosAtShot[2]);
			const float dDir = fabsf(d[0] - sLastWrittenDir[0])
				+ fabsf(d[1] - sLastWrittenDir[1])
				+ fabsf(d[2] - sLastWrittenDir[2]);

			static int sPostLogged = 0;
			if (sPostLogged < 40) {
				sPostLogged++;
				LogInfo("VRPose fire:   POSTCALL origin=(%.3f %.3f %.3f) dOurs=%.3f dCam=%.3f "
					"dirDelta=%.4f -> %s\n",
					o[0], o[1], o[2], dOurs, dCam, dDir,
					(dOurs < 0.01f) ? "KEPT (engine used our ray)"
					                : "OVERWRITTEN (engine recomputed)");
			}
		}

		return result;
	}

	static void ArmNextShotWriteWatch(DWORD64 objPtr)
	{
		(void)objPtr;
		if (InterlockedCompareExchange(&sShotWriteWatchCount, 0, 0) >= 8)
			return;
		if (InterlockedCompareExchange(&sShotPrepWatchPending, 1, 0) != 0
			|| InterlockedCompareExchange(&sShotPrepWatchMode, 0, 0) != 0)
			return;

		HMODULE base = GetModuleHandleA(NULL);
		if (!base) {
			InterlockedExchange(&sShotPrepWatchPending, 0);
			return;
		}
		sProbeWatchAddress = (void *)((BYTE *)base + kShotPrepRva);
		InterlockedIncrement(&sShotWriteWatchCount);
		const int others = ArmShotPrepExecOnAllThreads(sProbeWatchAddress);
		LogInfo("VRPose fire: armed DR3 shot-prep execution watch at RVA 0x%llX "
			"on %d other threads\n", (unsigned long long)kShotPrepRva, others);
		RaiseException(kArmExceptionCode, 0, 0, NULL);
	}

	static void InstallFireInlineHook()
	{
		InstallGameplayADSHook();
		static const bool kInstallRetiredFireDiagnostics = false;
		if (kInstallRetiredFireDiagnostics) {
			InstallNativeRecoilDiagnosticHook();
			InstallWeaponActionDiagnosticHook();
		}
		static bool sTried = false;
		if (sTried)
			return;
		sTried = true;

		const unsigned char *base = (const unsigned char *)GetModuleHandleA(NULL);
		if (!base) {
			LogInfo("VRPose fire: inline hook - no module base\n");
			return;
		}
		InstallHelsingArrowPhysicsLaunchHook(base);
		InstallHelsingArrowReleaseHook(base);

		SIZE_T hookId = 0;
		void *fn = (void *)(base + 0x4F3E60);
		const DWORD err = cHookMgr.Hook(&hookId, (LPVOID *)&trampoline_FireWrapper,
			fn, Hooked_FireWrapper);
		if (err || !trampoline_FireWrapper) {
			LogInfo("VRPose fire: INLINE hook FAILED at +0x4F3E60 (err=0x%x) - "
				"falling back to the debug-register probe\n", err);
			return;
		}

		InterlockedExchange(&sInlineFireHookActive, 1);
		LogInfo("VRPose fire: INLINE hook installed at +0x4F3E60 - every call caught "
			"on every thread, no per-thread arming, no warm-up\n");
		LogInfo("VRPose fire: CONFIG nativeDiagnostics=%d rewriteHitPoint=%d writeRay=%d preserveNativeDeviation=%d\n",
			kNativeShotDiagnostics ? 1 : 0, kRewriteNativeHitPoint ? 1 : 0,
			1, kPreserveNativeShotDeviation ? 1 : 0);

		if (kPreSpreadShotRedirect) {
			SIZE_T prepareShotHookId = 0;
			void *prepareShotFn = (void *)(base + 0x4F2770);
			const DWORD prepareShotErr = cHookMgr.Hook(&prepareShotHookId,
				(LPVOID *)&trampoline_PrepareShot, prepareShotFn, Hooked_PrepareShot);
			if (prepareShotErr || !trampoline_PrepareShot)
				LogInfo("VRPose fire: PRE-SPREAD shot hook FAILED at +0x4F2770 (err=0x%x)\n",
					prepareShotErr);
			else
				LogInfo("VRPose fire: PRE-SPREAD shot hook installed at +0x4F2770\n");
		}
		if (kAuthoritativeShotRedirect) {
			SIZE_T authoritativeHookId = 0;
			void *authoritativeFn = (void *)(base + 0x4F2E40);
			const DWORD authoritativeErr = cHookMgr.Hook(&authoritativeHookId,
				(LPVOID *)&trampoline_AuthoritativeShot, authoritativeFn,
				Hooked_AuthoritativeShot);
			if (authoritativeErr || !trampoline_AuthoritativeShot)
				LogInfo("VRPose fire: AUTHORITATIVE shot hook FAILED at +0x4F2E40 (err=0x%x)\n",
					authoritativeErr);
			else
				LogInfo("VRPose fire: AUTHORITATIVE shot hook installed at +0x4F2E40\n");
		}
		if (kShotProducerRedirect) {
			SIZE_T producerHookId = 0;
			void *producerFn = (void *)(base + 0x3D9360);
			const DWORD producerErr = cHookMgr.Hook(&producerHookId,
				(LPVOID *)&trampoline_ShotProducer, producerFn, Hooked_ShotProducer);
			if (producerErr || !trampoline_ShotProducer)
				LogInfo("VRPose fire: SHARED PRODUCER hook FAILED at +0x3D9360 (err=0x%x)\n",
					producerErr);
			else
				LogInfo("VRPose fire: SHARED PRODUCER hook installed at +0x3D9360\n");
		}
		if (kBallisticCollisionRedirect) {
			SIZE_T ballisticHookId = 0;
			void *ballisticFn = (void *)(base + 0x3D9A20);
			const DWORD ballisticErr = cHookMgr.Hook(&ballisticHookId,
				(LPVOID *)&trampoline_BallisticCollision, ballisticFn,
				Hooked_BallisticCollision);
			if (ballisticErr || !trampoline_BallisticCollision)
				LogInfo("VRPose fire: BALLISTIC COLLISION hook FAILED at +0x3D9A20 (err=0x%x)\n",
					ballisticErr);
			else
				LogInfo("VRPose fire: BALLISTIC COLLISION hook installed at +0x3D9A20\n");
		}
		if (kProjectileCreationRedirect) {
			SIZE_T projectileHookId = 0;
			void *projectileFn = (void *)(base + 0x3D7FF0);
			const DWORD projectileErr = cHookMgr.Hook(&projectileHookId,
				(LPVOID *)&trampoline_ProjectileCreate, projectileFn,
				Hooked_ProjectileCreate);
			if (projectileErr || !trampoline_ProjectileCreate)
				LogInfo("VRPose fire: PROJECTILE CREATE hook FAILED at +0x3D7FF0 (err=0x%x)\n",
					projectileErr);
			else
				LogInfo("VRPose fire: PROJECTILE CREATE hook installed at +0x3D7FF0\n");
		}

		// Retired diagnostic hooks. +0x4F48F0 was proven to consume a
		// downstream query/output copy: its endpoint changed with the controller
		// while Metro's damaging impact continued along the native camera ray.
		if (false) {
			SIZE_T traceHookId = 0;
			void *traceFn = (void *)(base + 0x4F48F0);
			const DWORD traceErr = cHookMgr.Hook(&traceHookId,
				(LPVOID *)&trampoline_PreFireTrace, traceFn, Hooked_PreFireTrace);
			if (traceErr || !trampoline_PreFireTrace)
				LogInfo("VRPose fire: pre-fire trace hook FAILED at +0x4F48F0 (err=0x%x)\n", traceErr);
			else
				LogInfo("VRPose fire: pre-fire trace hook installed at +0x4F48F0\n");

			SIZE_T prepareHookId = 0;
			void *prepareFn = (void *)(base + 0x6A4120);
			const DWORD prepareErr = cHookMgr.Hook(&prepareHookId,
				(LPVOID *)&trampoline_PreFirePrepare, prepareFn, Hooked_PreFirePrepare);
			if (prepareErr || !trampoline_PreFirePrepare)
				LogInfo("VRPose fire: pre-fire prepare hook FAILED at +0x6A4120 (err=0x%x)\n", prepareErr);
			else
				LogInfo("VRPose fire: pre-fire prepare hook installed at +0x6A4120\n");

			SIZE_T vectorCopyHookId = 0;
			void *vectorCopyFn = (void *)(base + 0x6A4160);
			const DWORD vectorCopyErr = cHookMgr.Hook(&vectorCopyHookId,
				(LPVOID *)&trampoline_PreFireVectorCopy, vectorCopyFn, Hooked_PreFireVectorCopy);
			if (vectorCopyErr || !trampoline_PreFireVectorCopy)
				LogInfo("VRPose fire: native vector-copy hook FAILED at +0x6A4160 (err=0x%x)\n", vectorCopyErr);
			else
				LogInfo("VRPose fire: native vector-copy hook installed at +0x6A4160\n");
		}
	}

	void ReportFireCall(DWORD64 objPtr)
	{
		float cam[3] = { 0, 0, 0 }, camFwd[3] = { 0, 0, 0 };
		ReadCameraVec(0x180, cam);
		ReadCameraVec(0x170, camFwd);

		LogInfo("========================================================\n");
		LogInfo("VRPose fire: FIRE CALL CAUGHT - object %016llX\n", objPtr);
		LogInfo("VRPose fire: camera position (%.4f %.4f %.4f)\n", cam[0], cam[1], cam[2]);
		LogInfo("VRPose fire: camera forward  (%.4f %.4f %.4f)\n", camFwd[0], camFwd[1], camFwd[2]);
		LogInfo("VRPose fire: object fields - looking for those two vectors:\n");
		for (DWORD_PTR off = 0x100; off < 0x1A0; off += 0x10) {
			float v[4];
			SIZE_T got = 0;
			if (!ReadProcessMemory(GetCurrentProcess(), (void *)(objPtr + off), v, sizeof(v), &got)
			    || got != sizeof(v))
				continue;
			// Flag anything that looks like the camera's own position or a
			// unit-length direction, so the match is obvious in the log
			// rather than something to eyeball across sixteen rows.
			const char *note = "";
			float dp = fabsf(v[0] - cam[0]) + fabsf(v[1] - cam[1]) + fabsf(v[2] - cam[2]);
			float df = fabsf(v[0] - camFwd[0]) + fabsf(v[1] - camFwd[1]) + fabsf(v[2] - camFwd[2]);
			float len = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
			if (dp < 1.5f)            note = "   <== MATCHES CAMERA POSITION (shot origin)";
			else if (df < 0.05f)      note = "   <== MATCHES CAMERA FORWARD (shot direction)";
			else if (fabsf(len - 1.0f) < 0.02f) note = "   <== unit vector (a direction)";
			LogInfo("VRPose fire:   +0x%03llX = %10.4f %10.4f %10.4f %10.4f%s\n",
				(unsigned long long)off, v[0], v[1], v[2], v[3], note);
		}
		LogInfo("========================================================\n");
	}

	static int ArmADSWriterWatchOnAllThreads()
	{
		const DWORD pid = GetCurrentProcessId();
		const DWORD tid = GetCurrentThreadId();
		HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
		if (snap == INVALID_HANDLE_VALUE)
			return 0;

		int armed = 0;
		THREADENTRY32 te = {};
		te.dwSize = sizeof(te);
		if (Thread32First(snap, &te)) {
			do {
				if (te.th32OwnerProcessID != pid || te.th32ThreadID == tid)
					continue;
				HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
					THREAD_SET_CONTEXT, FALSE, te.th32ThreadID);
				if (!th)
					continue;
				if (SuspendThread(th) != (DWORD)-1) {
					CONTEXT ctx = {};
					ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
					if (GetThreadContext(th, &ctx)) {
						ctx.Dr0 = sADSWriterAddresses[0];
						ctx.Dr1 = sADSWriterAddresses[1];
						ctx.Dr2 = sADSWriterAddresses[2];
						ctx.Dr3 = sADSWriterAddresses[3];
						ctx.Dr7 = kADSWriterDr7;
						ctx.Dr6 = 0;
						ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
						if (SetThreadContext(th, &ctx))
							armed++;
					}
					ResumeThread(th);
				}
				CloseHandle(th);
			} while (Thread32Next(snap, &te));
		}
		CloseHandle(snap);
		return armed;
	}

	static void ProcessADSWriterDiagnostic()
	{
		LONG reportMask = InterlockedExchange(&sADSWriterReportMask, 0);
		for (unsigned slot = 0; slot < ARRAYSIZE(sADSWriterAddresses); ++slot) {
			if (!(reportMask & (1 << slot)))
				continue;
			const CONTEXT c = sADSWriterContexts[slot];
			char where[MAX_PATH + 64];
			DescribeCodeAddress(c.Rip, where, sizeof(where));
			LogInfo("========================================================\n");
			LogInfo("VRPose ADS writer: candidate %u address=%016llX written at %s\n",
				slot, sADSWriterAddresses[slot], where);
			LogInfo("VRPose ADS writer: RAX=%016llX RBX=%016llX RCX=%016llX RDX=%016llX\n",
				c.Rax, c.Rbx, c.Rcx, c.Rdx);
			LogInfo("VRPose ADS writer: RSI=%016llX RDI=%016llX RBP=%016llX RSP=%016llX\n",
				c.Rsi, c.Rdi, c.Rbp, c.Rsp);
			for (unsigned i = 0, shown = 0;
			     i < ARRAYSIZE(sADSWriterStacks[slot]) && shown < 12; ++i) {
				DWORD64 value = sADSWriterStacks[slot][i];
				HMODULE mod = NULL;
				if (value > 0x10000 && GetModuleHandleExA(
					GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
					GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
					(LPCSTR)value, &mod) && mod) {
					char desc[MAX_PATH + 64];
					DescribeCodeAddress(value, desc, sizeof(desc));
					LogInfo("VRPose ADS writer:   [rsp+%03X] %s\n", i * 8, desc);
					shown++;
				}
			}
			LogInfo("========================================================\n");
		}

		if (InterlockedCompareExchange(&sADSWriterState, 0, 0) != 0)
			return;
		static unsigned pollFrames = 0;
		if (++pollFrames % 120 != 0)
			return;

		FILE *file = NULL;
		if (fopen_s(&file, "vr_ads_watch_candidates.txt", "rt") != 0 || !file)
			return;
		unsigned long long parsed[4] = {};
		unsigned count = 0;
		while (count < ARRAYSIZE(parsed) && fscanf_s(file, "%llx", &parsed[count]) == 1)
			count++;
		fclose(file);
		if (count != ARRAYSIZE(parsed)) {
			LogInfo("VRPose ADS writer: candidate file needs four addresses (found %u)\n", count);
			return;
		}
		for (unsigned i = 0; i < ARRAYSIZE(parsed); ++i)
			sADSWriterAddresses[i] = parsed[i];

		if (!sProbeVehHandle)
			sProbeVehHandle = AddVectoredExceptionHandler(1, CameraWriteProbeVeh);
		if (!sProbeVehHandle) {
			LogInfo("VRPose ADS writer: failed to install exception handler\n");
			return;
		}
		InterlockedExchange(&sADSWriterState, 1);
		const int others = ArmADSWriterWatchOnAllThreads();
		RaiseException(kADSWriterArmExceptionCode, 0, 0, NULL);
		LogInfo("VRPose ADS writer: armed four candidates across this and %d other threads\n",
			others);
	}

	void ArmFireProbe()
	{
		if (InterlockedCompareExchange(&sOwnershipProbeArmed, 0, 0) != 0)
			return;
		// Read-only capture of concrete pneumatic weapon objects. The retired
		// pump input/action path remains disabled; this registry is needed only
		// to identify Helsing's class-specific firing implementation.
		InstallNativePneumaticConstructorHook();
		if (kADSWriterDiagnostic) {
			// The writer trace owns DR0-DR3, but the inline fire hook does not
			// use debug registers. Keep it installed so a diagnostic shot still
			// resolves the live weapon/service virtual target.
			InstallFireInlineHook();
			ProcessADSWriterDiagnostic();
			return;
		}
		static const bool kFireProbeEnabled = true;
		if (!kFireProbeEnabled || !sVRSystem)
			return;
		if (InterlockedCompareExchange(&sFireProbeState, 0, 0) == 3)
			return;

		// Only in-world.
		float live[3];
		if (!ReadCameraVec(0x180, live))
			return;
		if (fabsf(live[0]) + fabsf(live[1]) + fabsf(live[2]) < 1.0f)
			return;

		// The inline hook is the proven all-thread path. Install it before the
		// debug-register fallback so a successful hook never walks every process
		// thread or starts the periodic re-arm worker.
		if (kFireRedirectTest) {
			InstallFireInlineHook();
			if (InterlockedCompareExchange(&sInlineFireHookActive, 0, 0) != 0) {
				InterlockedExchange(&sFireProbeState, 3);
				return;
			}
		}
		if (!sProbeVehHandle) {
			sProbeVehHandle = AddVectoredExceptionHandler(1, CameraWriteProbeVeh);
			if (!sProbeVehHandle) {
				LogInfo("VRPose fire: fallback exception handler installation failed\n");
				InterlockedExchange(&sFireProbeState, 3);
				return;
			}
		}

		if (InterlockedCompareExchange(&sFireProbeState, 1, 0) == 0) {
			HMODULE base = GetModuleHandleA(NULL);
			if (!base) {
				InterlockedExchange(&sFireProbeState, 3);
				return;
			}
			void *addr = (BYTE *)base + kFireFnRva;
			sProbeDr7 = kDr7Exec;
			sProbeWatchAddress = addr;
			InterlockedExchange(&sFireProbeArmed, 1);
			InterlockedExchange(&sProbeReported, 0);
			int others = ArmWatchpointOnAllThreads(addr);
			RaiseException(kArmExceptionCode, 0, 0, NULL);
			LogInfo("VRPose fire: armed on the fire call at RVA 0x%llX across %d threads; "
				"firing to trigger it\n", (unsigned long long)kFireFnRva, others);
			return;
		}

		// RE-BROADCAST PERIODICALLY. The first arm is a ONE-TIME snapshot of
		// whatever threads exist at that instant (ArmWatchpointOnAllThreads
		// uses Thread32First/Next, a snapshot, not a subscription). Any thread
		// the engine creates afterwards never gets the debug register set.
		//
		// Confirmed from a real session's log, not inferred: exactly one
		// "armed on the fire call" line, near the start; shots redirected
		// correctly for the next ~10,000 log lines; then FIRE CALL CAUGHT and
		// every fire-probe line simply stop - no bail message either, meaning
		// the breakpoint itself was never hit again - while ordinary per-frame
		// logging continued for thousands more lines. Total, permanent
		// silence for the rest of the session is the signature of the
		// breakpoint no longer existing on the thread doing the work, not of
		// anything about the player's aim.
		//
		// The engine almost certainly runs fire logic on a job-system worker
		// thread, and worker pools recycle threads over a session's lifetime.
		// Once the specific thread(s) that served the fire job early on are
		// replaced, the new ones never had Dr0 written and the probe goes
		// permanently dark - explaining "worked for the first several shots,
		// then never again" exactly, including that it does not come back.
		//
		// Re-broadcasting every few seconds catches any new thread almost
		// immediately, at the cost of one thread-snapshot walk that often -
		// cheap next to a frame budget measured in milliseconds.
		// Re-broadcast from a BACKGROUND THREAD, never from here.
		//
		// This function runs from Present, i.e. on the render thread. Doing
		// the re-broadcast inline cost a visible, perfectly periodic frame
		// dip: CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD) enumerates every
		// thread on the WHOLE SYSTEM, not just this process, which is
		// milliseconds of work regardless of how few threads actually need
		// arming. Skipping the suspends (see ArmWatchpointOnAllThreads) removed
		// most of the cost but not the snapshot, which is the expensive part.
		//
		// Nothing about this work needs the render thread. Arming ANOTHER
		// thread goes through Suspend/SetThreadContext/Resume, which any
		// thread may do - only arming the CALLING thread has to be
		// synchronous, and that is the one-time RaiseException path above.
		if (kFireRedirectTest) {
			// Preferred path. If it takes, the breakpoint machinery below
			// stays armed but stops redirecting (see sInlineFireHookActive),
			// so it costs nothing and remains as a fallback.
			InstallFireInlineHook();

			static volatile LONG sRearmStarted = 0;
			if (InterlockedExchange(&sRearmStarted, 1) == 0) {
				CreateThread(NULL, 0, [](LPVOID) -> DWORD {
					// Below normal: this is housekeeping and must never
					// compete with the game's own threads for a core.
					SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
					for (;;) {
						Sleep(5000);
						if (InterlockedCompareExchange(&sOwnershipProbeArmed, 1, 1) != 0)
							continue;
						if (InterlockedCompareExchange(&sFireProbeArmed, 1, 1) != 1)
							continue;
						if (!sProbeWatchAddress)
							continue;
						const int n = ArmWatchpointOnAllThreads(sProbeWatchAddress);
						static unsigned passes = 0;
						if ((passes++ % 12) == 0)
							LogInfo("VRPose fire: re-armed across %d threads "
								"(background, catches threads created since "
								"the last pass)\n", n);
					}
				}, NULL, 0, NULL);
				LogInfo("VRPose fire: started background re-arm thread\n");

				// The code dump moved to HackerContext (DumpFireCodeOnce), so
				// it runs without a headset - the bytes are decrypted the same
				// either way, and gating pure static analysis behind VR meant
				// putting the headset on just to read memory.
			}
			return;
		}
		static unsigned f = 0;
		if ((f % 45) == 0)
			SendMouseButton(false, true);
		else if ((f % 45) == 15)
			SendMouseButton(false, false);
		f++;
	}

	// -----------------------------------------------------------------
	// Culling follow.
	//
	// The engine culls to ITS camera, which knows nothing about the player's
	// head. Turn your head far enough and you are looking at geometry the
	// engine already discarded - props and characters vanish, and bullet
	// impacts disappear with them because the surfaces they landed on are not
	// being drawn.
	//
	// This was unfixable while the camera WAS the aim: moving it to follow
	// the head would have moved where shots go. It is not anymore. Shot
	// direction comes from the weapon object's +0x150 (Notes/14), completely
	// independent of the camera, so the camera is now free to chase the head.
	//
	// Injected slowly and cancelled out of the rendered view, so:
	//   - the VIEW is exact by construction - view = camera - injected + head,
	//     and camera = body + injected, so view = body + head regardless of
	//     how far the injection has actually got;
	//   - injection error only affects CULLING, where a frame or two of lag
	//     at the edges is invisible.
	//
	// That is the crucial difference from the earlier injection work, where
	// the cancellation had to be perfect because aim depended on it and every
	// imprecision showed up as the world sliding around.
	static float sCullInjectedYaw = 0.0f;

	bool GetCameraWorldPos(float out[3])
	{
		return ReadCameraVec(0x180, out);
	}

	bool GetCameraForward(float out[3])
	{
		return ReadCameraVec(0x170, out);
	}

	// Recent history of the injected total, so the cancellation can use what
	// the engine has ALREADY applied rather than what we have just asked for.
	//
	// Injection was being counted the instant it was sent, but the engine
	// takes about two frames to act on it (measured during the aim work). For
	// those frames the view subtracted rotation that had not happened yet, so
	// it lagged the head and the world appeared to drag along at the START of
	// a turn, settling once the camera caught up. Small, but exactly the
	// "moves with me briefly then stops" that gets noticed.
	static const int kCullHistory = 8;
	static const int kCullLagFrames = 2;
	static float sCullInjectedHistory[kCullHistory] = { 0 };
	static float sCullInjectedPitchHistory[kCullHistory] = { 0 };
	static int sCullHistoryPos = 0;
	static float sCullInjectedPitch = 0.0f;

	// The engine camera's ACTUAL pitch, read from the game rather than booked.
	// See the follow loop for why the booked value cannot be trusted.
	static float sCullMeasuredYaw = 0.0f;
	static bool sCullMeasuredYawValid = false;
	static float sCullMeasuredPitch = 0.0f;
	static bool sCullMeasuredPitchValid = false;

	static float NativeFollowYawError(float headYaw)
	{
		return NativeFollowWrap(headYaw - sCullInjectedYaw);
	}

	static void CreditNativeFollowYaw(float applied)
	{
		sCullInjectedYaw = NativeFollowWrap(sCullInjectedYaw + applied);
		sCullInjectedHistory[sCullHistoryPos] = sCullInjectedYaw;
	}

	static void CreditNativeFollowPitch(float nativePitch)
	{
		sCullInjectedPitch = -nativePitch;
		sCullMeasuredPitchValid = false; // refreshed from published camera downstream
		sCullInjectedPitchHistory[sCullHistoryPos] = sCullInjectedPitch;
	}

	static void SeedNativeFollowHistory()
	{
		for (int i = 0; i < kCullHistory; ++i) {
			sCullInjectedHistory[i] = sCullInjectedYaw;
			sCullInjectedPitchHistory[i] = sCullInjectedPitch;
		}
		// Unlike the basis experiment, native state really changed. Restoring a
		// frozen entry credit on disable would misclassify our applied yaw as body turn.
	}

	static void PublishUpstreamDirectCancellation(float yaw, float pitch)
	{
		sCullInjectedYaw = yaw;
		sCullInjectedPitch = pitch;
		sCullInjectedHistory[sCullHistoryPos] = yaw;
		sCullInjectedPitchHistory[sCullHistoryPos] = pitch;
	}

	static void CaptureUpstreamLegacyCancellation()
	{
		sUpstreamEntryInjectedYaw = sCullInjectedYaw;
		sUpstreamEntryInjectedPitch = sCullInjectedPitch;
	}

	static void RestoreUpstreamLegacyCancellation()
	{
		sCullInjectedYaw = sUpstreamEntryInjectedYaw;
		sCullInjectedPitch = sUpstreamEntryInjectedPitch;
		for (int i = 0; i < kCullHistory; ++i) {
			sCullInjectedHistory[i] = sUpstreamEntryInjectedYaw;
			sCullInjectedPitchHistory[i] = sUpstreamEntryInjectedPitch;
		}
		sCullMeasuredPitchValid = false;
	}

	static void ApplyUpstreamDirectLocomotion(float *x, float *y)
	{
		if (!x || !y || !G || !G->vrHeadRotationValid ||
			InterlockedCompareExchange(&sUpstreamCameraBasisProbeActive, 0, 0) == 0 ||
			InterlockedCompareExchange(&sUpstreamCameraYieldingToScript, 0, 0) != 0)
			return;

		const float *head = G->vrHeadRotation3x3;
		const float currentYaw = atan2f(head[2], head[8]);
		const float referenceYaw = atan2f(
			sUpstreamHeadReferenceYawPitch[2], sUpstreamHeadReferenceYawPitch[8]);
		float delta = currentYaw - referenceYaw;
		while (delta > 3.14159265f) delta -= 6.28318531f;
		while (delta < -3.14159265f) delta += 6.28318531f;
		const float c = cosf(delta), s = sinf(delta);
		const float oldX = *x, oldY = *y;
		*x = c * oldX + s * oldY;
		*y = -s * oldX + c * oldY;
	}

	// SCRIPTED CAMERA HANDOVER.
	//
	// The closed loop drives Metro's camera to the head. When a scripted event
	// wants to point the camera somewhere, the two pull against each other and
	// the cutscene does not frame the way it was authored.
	//
	// So detect when something OTHER than us is moving the camera, stand down
	// while it is, show the player what the script is pointing at, and
	// re-converge once it lets go.
	//
	// Detection needs no reverse engineering because we already know both
	// halves: what we commanded, and what the camera actually did. The give-
	// away is camera movement while our own command is quiet. Deliberately
	// gated on a quiet command rather than on the difference from a predicted
	// one - kPitchResponse is only approximate, so a large command carries a
	// large prediction error and would false-trigger. When a script starts the
	// loop is normally converged and commanding nothing, which is exactly when
	// the measurement is trustworthy. Once latched we stop injecting, so the
	// command stays quiet and the reading stays clean for as long as it runs.
	static bool sScriptedCamActive = false;
	static int sScriptedCamOnset = 0;
	static int sScriptedCamQuiet = 0;
	static float sScriptedPitchOffset = 0.0f;   // pitch the SCRIPT contributed
	static float sScriptedBlend = 0.0f;         // 1 while scripted, ramps to 0 after
	// Preserve the final authored angle only for a short, fixed handback ease.
	// It is never re-measured after native ownership clears.
	static const unsigned kScriptedReleaseEaseFrames = 12;
	static bool sScriptedReleaseEaseActive = false;
	static unsigned sScriptedReleaseStartFrame = 0;
	static float sScriptedReleasePitchOffset = 0.0f;

	// SUSTAINED-DIVERGENCE GATE on the pass-through.
	//
	// The onset detector latches on anything that momentarily looks scripted,
	// and since the pass-through is its only consumer, every such latch is felt
	// as the world moving vertically with the player's head. The old logs show
	// why no single-frame test separates them: real cutscenes latched at +14 to
	// +72 degrees, but so did events at +4.1, +5.6 and +6.3 - and those overlap
	// ordinary play.
	//
	// DURATION is what separates them. Recoil is a ~250 ms kick with its own
	// ignore window; follow-loop lag closes within a few frames now that the
	// loop measures the engine rather than its own bookkeeping. Neither can hold
	// a LARGE divergence for a full second. A scripted camera does exactly that
	// and nothing else - it takes the camera and keeps it.
	//
	// Still a threshold, but the margin is measured in seconds rather than
	// degrees per frame, which is the axis every earlier attempt failed on.
	static float sPrevHeadPitchGate = 0.0f;
	static bool sPrevHeadPitchGateValid = false;
	static int sScriptedSustain = 0;
	static bool sScriptedSustained = false;
	static float sPrevEnginePitch = 0.0f;
	static bool sPrevEnginePitchValid = false;
	static float sLastCommandedPitch = 0.0f;    // predicted effect of our last dy
	// Frame-indexed pitch-command history for compensating input still in flight.
	// Slot zero is the command issued during the current rendered frame; older
	// slots are addressed relative to sPitchCommandHistoryPos.
	static const int kPitchCommandHistory = 8;
	static float sPitchCommandHistory[kPitchCommandHistory] = { 0 };
	static int sPitchCommandHistoryPos = 0;

	// Anti-windup state - see the note at the pitch injection. sPitchLastMoved
	// is how far the engine's camera actually moved last frame, which is what
	// tells us whether our synthetic mouse input is landing at all.
	// Yield state - see the note at the pitch injection. Separate from the
	// anti-windup: that one covers a DEAD camera (menus), this one covers a
	// camera that is very much alive but being driven by somebody else.
	static bool sInjectionYield = false;
	static float sPrevHeadPitchForYield = 0.0f;
	static bool sPrevHeadPitchValid = false;
	static int sFightFrames = 0;
	static float sFightRefErr = 0.0f;
	static int sYieldProbeWait = 0;
	static bool sYieldProbing = false;

	static float sPitchLastMoved = 0.0f;
	static int sPitchDeadFrames = 0;
	static bool sPitchSuppressLogged = false;

	// YAW RESPONSE MEASUREMENT.
	//
	// The yaw booking assumes the engine applies EXACTLY dx * kRadPerMouseUnit.
	// Pitch made the same assumption and was wrong by a factor of 0.54, which
	// is what let its residue build to +16 deg. Nothing has ever measured the
	// horizontal factor, so any error there accumulates into the body heading
	// we derive as engineYaw - bookedYaw - and that heading feeds the rendered
	// view, the weapon's body frame AND the shot's baseYaw, which is why a
	// single wrong value shifts view-locked UI and gun-locked reticle and
	// bullets together and in the same direction.
	//
	// Measured non-circularly: divide the camera's ACTUAL yaw change by what we
	// commanded. No knowledge of the body's true heading is needed, which is
	// the thing that made the pitch trick unavailable here.
	// A STALL DETECTOR WAS TRIED HERE AND REMOVED. Do not re-add it as-is.
	//
	// The idea was sound in isolation: the motion detector cannot see a script
	// that parks the camera and holds it, so watch the loop's own progress
	// instead and stand down when the error stops shrinking. It did detect the
	// held-camera scenes - and it broke them. Standing down on a camera the
	// game is actively holding left the view pointing somewhere the scene was
	// never composed for, with the character's hands in shot.
	//
	// The lesson is about what "the engine is refusing us" actually means. It
	// does not distinguish a script that wants the camera somewhere specific
	// from one that merely will not let us move it, and those need opposite
	// responses. Telling them apart needs the game's own state, not an
	// inference from how the camera responds to us.
	//
	// Cost of the version that shipped instead: slight hitching in held-camera
	// scenes, which the player explicitly preferred to broken framing.

	// ---------------------------------------------------------------
	// SCRIPTED-STATE FLAG HUNT
	//
	// Find the variable Metro itself sets while it owns the player, so the
	// scripted state can be READ instead of inferred. Two heuristics have now
	// failed at this, both because they could not represent the distinction
	// that matters; the game plainly can, because it behaves differently.
	//
	// The stall test above is a poor CONTROLLER but it was a good DETECTOR -
	// it correctly identified the held-camera scenes, which is exactly why
	// standing down during them was so visible. So it is kept here purely as a
	// trigger, driving nothing but this scan. No injection is gated on it and
	// no view is changed by it: this code is inert with respect to gameplay.
	//
	// Method is a differential scan, the same idea as a memory scanner's
	// successive filtering, except we run inside the process and already know
	// when to sample. Snapshot the module's writable data during ordinary play
	// and again while a scene holds the camera; the flag must differ between
	// them. Every later sample filters the survivors: a candidate has to hold
	// its play value on every play sample and its scripted value on every
	// scripted one. Timers, positions and counters fail that within a couple of
	// scenes; a genuine state flag does not.
	// Round 1 narrowed 10340 candidates to 4 and then lost them all. Both
	// halves of that taught something, and this is round 2.
	//
	// WHAT SURVIVED was never a flag: metro.exe+0xD128BC and friends held
	// 0x00007FF7 during play and 0x000001AD while scripted - the HIGH HALVES of
	// 64-bit pointers, one into the exe image and one into the heap, on a
	// 16-byte stride. And +0xD13DBC was null during play and a heap object
	// during a script, which is the shape of "the scripted sequence currently
	// in charge".
	//
	// WHY THEY DIED was the filter, not the data. Exact equality is the wrong
	// test for a pointer: a script allocates a fresh object each time, so the
	// value changes between scenes while its MEANING does not. So compare what
	// a pointer points AT - null, inside the module, or heap - and a slot that
	// is empty during play and occupied during a script survives no matter
	// which object got allocated.
	//
	// Second change: score instead of eliminate. Hard filtering means one
	// mislabelled sample destroys a true candidate permanently, and the labels
	// come from a heuristic known to be imperfect - the player reports that
	// hold-E door interactions register as scripted, and they may well be
	// input-locked moments genuinely. Counting agreements and disagreements
	// lets the right answer win on weight of evidence and survive a few bad
	// samples.
	enum HuntClass { HC_NULL = 0, HC_MODULE = 1, HC_HEAP = 2, HC_OTHER = 3 };

	struct ByteCand { DWORD off; BYTE pval, sval; unsigned hit, miss; };
	struct PtrCand { DWORD off; BYTE pcls, scls; unsigned hit, miss; };
	static std::vector<ByteCand> sHuntBytes;
	static std::vector<PtrCand> sHuntPtrs;
	static std::vector<BYTE> sHuntNormal, sHuntScripted, sHuntCur;
	static size_t sHuntSize = 0;
	static ULONGLONG sHuntModLo = 0, sHuntModHi = 0;
	static bool sHuntHaveNormal = false, sHuntHaveScripted = false, sHuntSeeded = false;

	static BYTE HuntClassify(ULONGLONG v)
	{
		if (v == 0)
			return HC_NULL;
		if (v >= sHuntModLo && v < sHuntModHi)
			return HC_MODULE;
		// User-mode addresses live below the canonical split; anything in that
		// band and past the null page is plausibly a live allocation. Coarse on
		// purpose - VirtualQuery per value would cost more than the whole scan.
		if (v > 0x10000ull && v < 0x00007FFFFFFF0000ull)
			return HC_HEAP;
		return HC_OTHER;
	}
	static bool sHuntStalled = false;
	static int sHuntStallFrames = 0;
	static float sHuntStallRefErr = 0.0f;
	static int sHuntSettle = 0;
	static int sHuntSamples = 0;

	// The span covered by the module's WRITABLE sections. Code cannot hold the
	// flag, and skipping .text keeps the scan to a size worth diffing.
	static bool HuntFindRegion()
	{
		HMODULE mod = GetModuleHandleA(NULL);
		if (!mod)
			return false;
		BYTE *b = (BYTE *)mod;
		IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)b;
		if (dos->e_magic != IMAGE_DOS_SIGNATURE)
			return false;
		IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(b + dos->e_lfanew);
		if (nt->Signature != IMAGE_NT_SIGNATURE)
			return false;
		sHuntModLo = (ULONGLONG)b;
		sHuntModHi = (ULONGLONG)b + nt->OptionalHeader.SizeOfImage;
		IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
		DWORD lo = 0, hi = 0;
		for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
			if (!(sec[i].Characteristics & IMAGE_SCN_MEM_WRITE))
				continue;
			const DWORD start = sec[i].VirtualAddress;
			const DWORD end = start + sec[i].Misc.VirtualSize;
			if (lo == 0 || start < lo) lo = start;
			if (end > hi) hi = end;
		}
		if (hi <= lo)
			return false;
		size_t span = hi - lo;
		const size_t kCap = 32u * 1024u * 1024u;
		if (span > kCap) span = kCap;
		sHuntBase = b + lo;
		sHuntSize = span & ~(size_t)3;
		LogInfo("VRPose hunt: scanning %zu KB of writable data at RVA 0x%X\n",
			sHuntSize / 1024, lo);
		return true;
	}

	// Page-at-a-time so an unmapped or guarded page cannot take the process
	// down; anything unreadable reads as zero and simply never survives a
	// filter.
	static void HuntSnapshot(std::vector<BYTE> &out)
	{
		out.assign(sHuntSize, 0);
		for (size_t off = 0; off < sHuntSize; off += 4096) {
			SIZE_T want = sHuntSize - off;
			if (want > 4096) want = 4096;
			SIZE_T got = 0;
			ReadProcessMemory(GetCurrentProcess(), sHuntBase + off,
				&out[off], want, &got);
		}
	}

	// INPUT-DEVICE HUNT - find what Metro flips when it decides "mouse" vs
	// "controller", so the button prompts can be pinned to controller.
	//
	// Our synthetic look input tells Metro the player switched to mouse, so the
	// prompts flip whenever the injection runs and flip back when the player
	// touches the pad. Blocking the injection is not an option (the follow is
	// required), so the fix is to find the flag and hold it.
	//
	// This search is well posed in the way the scripted-state hunt never was:
	// BOTH LABELS ARE GENERATED BY US. We know when mouse input is being sent
	// because we are sending it. No heuristic, no inference.
	//
	//   phase 0 - suppress our mouse entirely for ~1 s. The player's own pad
	//             input is then the most recent device. Snapshot = CONTROLLER.
	//   phase 1 - force a small mouse pulse every frame for ~1 s (alternating
	//             sign so the camera does not drift). Snapshot = MOUSE.
	//
	// Alternate forever, scoring byte candidates that hold the right value in
	// each phase. A device flag flips both ways every cycle; almost nothing
	// else does.
	static bool sInputHuntSuppressMouse = false;
	static bool sInputHuntForceMouse = false;

	static void InputDeviceHuntTick()
	{
		// OFF. Ran 96 cycles with self-generated labels on both sides and the
		// best candidates scored 102 hits / 89 misses out of ~192 - 53%, i.e.
		// chance. The active-input-device flag is not a plain byte in the
		// module's writable statics (444 KB at RVA 0xCD3000), the same region
		// and the same negative as the scripted-state hunt in Notes/24 section 5.
		// Do not re-run this variant; a heap search or the code that ARBITRATES
		// the device would be the next honest step, not more scoring.
		//
		// It must stay off in normal play regardless: it forces mouse pulses
		// every other second, which is precisely the input it was hunting.
		static const bool kInputDeviceHunt = false;
		if (!kInputDeviceHunt)
			return;
		if (!sHuntBase && !HuntFindRegion())
			return;

		static std::vector<BYTE> snapCtrl, snapMouse, cur;
		static bool haveCtrl = false, haveMouse = false, seeded = false;
		static std::vector<ByteCand> cands;
		static int phase = 0, frames = 0, cycles = 0;

		frames++;
		const bool controllerPhase = (phase == 0);
		sInputHuntSuppressMouse = controllerPhase;
		sInputHuntForceMouse = !controllerPhase;

		if (frames == 55) {
			HuntSnapshot(cur);
			if (controllerPhase) { snapCtrl = cur; haveCtrl = true; }
			else { snapMouse = cur; haveMouse = true; }

			if (!seeded) {
				if (haveCtrl && haveMouse) {
					const BYTE *c = snapCtrl.data(), *m = snapMouse.data();
					cands.clear();
					for (size_t i = 0; i < sHuntSize; ++i)
						if (c[i] != m[i]) {
							ByteCand bc = { (DWORD)i, c[i], m[i], 0, 0 };
							cands.push_back(bc);
						}
					seeded = true;
					LogInfo("VRPose devhunt: seeded with %zu byte candidates\n",
						cands.size());
				}
			} else {
				const BYTE *v = cur.data();
				for (size_t i = 0; i < cands.size(); ++i) {
					const BYTE want = controllerPhase ? cands[i].pval : cands[i].sval;
					if (v[cands[i].off] == want) cands[i].hit++; else cands[i].miss++;
				}
				LogInfo("VRPose devhunt: cycle %d %s, %zu tracked\n", cycles,
					controllerPhase ? "CONTROLLER" : "mouse", cands.size());
				if ((cycles % 3) == 0 && !cands.empty()) {
					std::vector<ByteCand> top = cands;
					std::sort(top.begin(), top.end(),
						[](const ByteCand &a, const ByteCand &b) {
							return (int)(a.hit - a.miss) > (int)(b.hit - b.miss);
						});
					HMODULE mod = GetModuleHandleA(NULL);
					const size_t show = top.size() < 10 ? top.size() : 10;
					for (size_t i = 0; i < show; ++i) {
						const DWORD rva = (DWORD)((sHuntBase + top[i].off) - (BYTE *)mod);
						LogInfo("VRPose devhunt:   metro.exe+0x%X controller=0x%02X "
							"mouse=0x%02X hit=%u miss=%u\n", rva,
							(unsigned)top[i].pval, (unsigned)top[i].sval,
							top[i].hit, top[i].miss);
					}
				}
			}
		}
		if (frames >= 65) {
			frames = 0;
			if (!controllerPhase) cycles++;
			phase ^= 1;
		}
	}

	static void HuntReport()
	{
		HMODULE mod = GetModuleHandleA(NULL);
		static const char *kClassName[4] = { "null", "module", "heap", "other" };

		// Rank by weight of evidence, best first, and show the score so a
		// candidate that is merely winning can be told from one that has never
		// once disagreed.
		std::vector<PtrCand> ptrs = sHuntPtrs;
		std::sort(ptrs.begin(), ptrs.end(), [](const PtrCand &a, const PtrCand &b) {
			return (int)(a.hit - a.miss) > (int)(b.hit - b.miss);
		});
		LogInfo("VRPose hunt: --- top pointer slots (%zu tracked) ---\n", ptrs.size());
		for (size_t i = 0; i < ptrs.size() && i < 12; ++i) {
			const DWORD rva = (DWORD)((sHuntBase + ptrs[i].off) - (BYTE *)mod);
			LogInfo("VRPose hunt:   metro.exe+0x%X  play=%s scripted=%s  hit=%u miss=%u\n",
				rva, kClassName[ptrs[i].pcls], kClassName[ptrs[i].scls],
				ptrs[i].hit, ptrs[i].miss);
		}

		std::vector<ByteCand> bytes = sHuntBytes;
		std::sort(bytes.begin(), bytes.end(), [](const ByteCand &a, const ByteCand &b) {
			return (int)(a.hit - a.miss) > (int)(b.hit - b.miss);
		});
		LogInfo("VRPose hunt: --- top byte flags (%zu tracked) ---\n", bytes.size());
		for (size_t i = 0; i < bytes.size() && i < 12; ++i) {
			const DWORD rva = (DWORD)((sHuntBase + bytes[i].off) - (BYTE *)mod);
			LogInfo("VRPose hunt:   metro.exe+0x%X  play=0x%02X scripted=0x%02X  hit=%u miss=%u\n",
				rva, bytes[i].pval, bytes[i].sval, bytes[i].hit, bytes[i].miss);
		}
	}

	static void HuntSample(bool scriptedNow)
	{
		if (!sHuntBase && !HuntFindRegion())
			return;

		HuntSnapshot(sHuntCur);
		sHuntSamples++;

		if (!sHuntSeeded) {
			if (scriptedNow) {
				sHuntScripted = sHuntCur;
				sHuntHaveScripted = true;
			} else {
				sHuntNormal = sHuntCur;
				sHuntHaveNormal = true;
			}
			if (!sHuntHaveNormal || !sHuntHaveScripted)
				return;

			const BYTE *n = sHuntNormal.data();
			const BYTE *s = sHuntScripted.data();
			sHuntBytes.clear();
			for (size_t i = 0; i < sHuntSize; ++i) {
				if (n[i] != s[i]) {
					ByteCand c = { (DWORD)i, n[i], s[i], 0, 0 };
					sHuntBytes.push_back(c);
				}
			}
			const ULONGLONG *nq = (const ULONGLONG *)n;
			const ULONGLONG *sq = (const ULONGLONG *)s;
			sHuntPtrs.clear();
			for (size_t i = 0; i < sHuntSize / 8; ++i) {
				const BYTE pc = HuntClassify(nq[i]), sc = HuntClassify(sq[i]);
				if (pc != sc) {
					PtrCand c = { (DWORD)(i * 8), pc, sc, 0, 0 };
					sHuntPtrs.push_back(c);
				}
			}
			sHuntSeeded = true;
			LogInfo("VRPose hunt: seeded with %zu byte flags, %zu pointer slots\n",
				sHuntBytes.size(), sHuntPtrs.size());
			// Persist the differential immediately. The process may be closed
			// after the second marker, so keeping this only in RAM loses the
			// entire result before it can be inspected.
			FILE *report = NULL;
			if (fopen_s(&report, "vr_control_state_candidates.txt", "w") == 0 && report) {
				fprintf(report, "scripted_vs_player_candidates\n");
				fprintf(report, "byte_candidates=%zu pointer_candidates=%zu\n",
					sHuntBytes.size(), sHuntPtrs.size());
				for (const ByteCand &c : sHuntBytes)
					fprintf(report, "byte metro.exe+0x%X player=0x%02X scripted=0x%02X\n",
						(DWORD)((sHuntBase + c.off) - (BYTE *)GetModuleHandleA(NULL)),
						(unsigned)c.pval, (unsigned)c.sval);
				for (const PtrCand &c : sHuntPtrs)
					fprintf(report, "pointer metro.exe+0x%X playerClass=%u scriptedClass=%u\n",
						(DWORD)((sHuntBase + c.off) - (BYTE *)GetModuleHandleA(NULL)),
						(unsigned)c.pcls, (unsigned)c.scls);
				fclose(report);
				LogInfo("VRPose hunt: persisted differential to vr_control_state_candidates.txt\n");
			}
			return;
		}

		// Score, do not eliminate.
		const BYTE *cur = sHuntCur.data();
		const ULONGLONG *curq = (const ULONGLONG *)cur;
		for (size_t i = 0; i < sHuntBytes.size(); ++i) {
			const BYTE v = cur[sHuntBytes[i].off];
			const bool ok = scriptedNow ? (v == sHuntBytes[i].sval)
				: (v == sHuntBytes[i].pval);
			if (ok) sHuntBytes[i].hit++; else sHuntBytes[i].miss++;
		}
		for (size_t i = 0; i < sHuntPtrs.size(); ++i) {
			const BYTE c = HuntClassify(curq[sHuntPtrs[i].off / 8]);
			const bool ok = scriptedNow ? (c == sHuntPtrs[i].scls)
				: (c == sHuntPtrs[i].pcls);
			if (ok) sHuntPtrs[i].hit++; else sHuntPtrs[i].miss++;
		}

		LogInfo("VRPose hunt: frame=%u sample %d (%s)\n",
			G->frame_no, sHuntSamples, scriptedNow ? "SCRIPTED" : "play");

		// Every few samples is often enough to watch it converge without
		// burying the rest of the log.
		if ((sHuntSamples % 5) == 0)
			HuntReport();
	}

	// Passive. Watches the follow loop's progress to decide when to sample, and
	// changes nothing about how the loop behaves.
	// WATCH THE SHORTLIST DIRECTLY.
	//
	// Round 2 scored its best candidates at roughly 70 hits to 16 misses. That
	// is far above chance but nowhere near proof, and a third correlation pass
	// would inherit the same weakness: the labels. "SCRIPTED" comes from the
	// stall test, which also trips when the player looks past Metro's pitch
	// clamp, so some samples are labelled wrong and no amount of scoring
	// against bad labels turns into certainty.
	//
	// The shortlist is short enough to check directly instead. These are the
	// contiguous pointer slots that went null during scripted state - two
	// clusters of consecutive 8-byte fields cleared together, which is what
	// detaching an object looks like. Watching them every frame and logging
	// only TRANSITIONS answers the question outright: does this slot change
	// exactly when a scene starts and ends, or merely often enough to score
	// well? Correlation cannot distinguish those; a timeline can.
	static const DWORD kWatchRvas[] = {
		0xD235E0, 0xD235E8, 0xD235F0, 0xD235F8, 0xD23600, 0xD23608, 0xD23630,
		0xD28A88, 0xD28A90, 0xD28A98,
		0xD3E640, 0xD3E658,
	};
	static const int kWatchCount = (int)(sizeof(kWatchRvas) / sizeof(kWatchRvas[0]));
	static ULONGLONG sWatchPrev[kWatchCount] = { 0 };
	static bool sWatchPrimed = false;

	// RESULT: ALL TWELVE CLEARED. The flag is not here.
	//
	// Watched every frame across a full run with several scripted scenes:
	//
	//   0xD28A88/90/98, 0xD3E640/658 - changed ONCE each, null to heap during
	//       the first scripted scene, and never returned. One-time allocations.
	//   0xD235E0/F0/600/630 - float PAIRS, e.g. 0xC134DD9F 43577849 =
	//       (-11.3, 215.5). Coordinates, not pointers; classified as "other"
	//       only because they are not valid addresses. Every transition
	//       occurred outside scripted state.
	//   0xD23608 - a counter: 1, 2, 3, 4, 5.
	//
	// Five transitions total occurred during scripted state and not one of them
	// reversed when the scene ended. A state flag must toggle both ways on the
	// scene boundary; none of these does.
	//
	// Left disabled rather than deleted. Anyone resuming this should NOT rerun
	// the correlation scan as it stands - its weakness was never the scoring,
	// it was that the SCRIPTED label comes from the stall heuristic, which also
	// fires at Metro's pitch clamp. Ground-truth labels (a manual marker the
	// player presses during a scene) would be worth more than any further
	// refinement of the scan itself. The alternative route, untried: breakpoint
	// the camera-pitch write and read the branch that skips it while a script
	// holds the input - that finds the condition directly instead of hunting
	// for something correlated with it.
	static void WatchScriptCandidates(float curErr, bool stalledNow)
	{
		static const bool kWatchEnabled = false;
		if (!kWatchEnabled)
			return;
		HMODULE mod = GetModuleHandleA(NULL);
		if (!mod)
			return;
		static const char *kClassName[4] = { "null", "module", "heap", "other" };

		for (int i = 0; i < kWatchCount; ++i) {
			ULONGLONG v = 0;
			SIZE_T got = 0;
			if (!ReadProcessMemory(GetCurrentProcess(),
				(BYTE *)mod + kWatchRvas[i], &v, sizeof(v), &got)
			    || got != sizeof(v))
				continue;
			if (sWatchPrimed && v == sWatchPrev[i])
				continue;
			if (sWatchPrimed) {
				LogInfo("VRPose watch: frame=%u metro.exe+0x%X  %s -> %s  "
					"(0x%llX) stall=%d err=%+.2f\n",
					G->frame_no, kWatchRvas[i],
					kClassName[HuntClassify(sWatchPrev[i])],
					kClassName[HuntClassify(v)],
					v, stalledNow ? 1 : 0, curErr * 57.29578f);
			}
			sWatchPrev[i] = v;
		}
		sWatchPrimed = true;
	}

	static void ScriptFlagHuntTick(float curErr)
	{
		// Ground-truth mode for the scripted-state hunt. The old automatic labels
		// came from the same pitch-response heuristic we are trying to replace,
		// so they cannot prove a candidate state. F7 marks ordinary gameplay and
		// F8 marks a moment where Metro is definitely driving a scripted camera.
		// This is diagnostics only: it never changes the camera or view.
		static const bool kHuntEnabled = kControlStateSamplerEnabled;
		static bool sManualHuntMode = false;
		if (InterlockedCompareExchange(&sControlStateSampleRequest, 0, 0) != 0)
			sManualHuntMode = true;
		if (kHuntEnabled && sManualHuntMode) {
			const LONG request = InterlockedExchange(&sControlStateSampleRequest, 0);
			if (request != 0) {
				HuntSample(request == 2);
				LogInfo("VRPose hunt: ground-truth sample recorded as %s\n",
					request == 2 ? "SCRIPTED" : "ordinary play");
			}
			return;
		}

		const float kTryingErr = 0.05f;     // ~3 deg - worth correcting
		const float kSettledErr = 0.035f;   // ~2 deg - close enough
		const int kStallWindow = 45;        // ~0.6 s of failing to close the gap

		const bool wasStalled = sHuntStalled;
		if (!sHuntStalled) {
			if (curErr > kTryingErr) {
				if (sHuntStallFrames == 0)
					sHuntStallRefErr = curErr;
				if (++sHuntStallFrames >= kStallWindow) {
					if (curErr > sHuntStallRefErr * 0.6f)
						sHuntStalled = true;
					sHuntStallFrames = 0;
					sHuntStallRefErr = curErr;
				}
			} else {
				sHuntStallFrames = 0;
			}
		} else if (curErr < kSettledErr) {
			sHuntStalled = false;
		}

		// The stall state above is only an annotation on the watch log now -
		// something to line the transitions up against, not a trigger.
		WatchScriptCandidates(curErr, sHuntStalled);

		// Correlation scanning is done; the shortlist is being verified
		// directly. Leave the machinery in place - if the watch clears these
		// candidates, re-seeding is one flag away.
		if (!kHuntEnabled)
			return;

		if (sHuntStalled != wasStalled) {
			sHuntSettle = 0;
			LogInfo("VRPose hunt: frame=%u %s held-camera state (err %+.2f deg)\n",
				G->frame_no, sHuntStalled ? "ENTERED" : "left",
				curErr * 57.29578f);
			return;
		}

		// Let the state settle before sampling, so a transition frame does not
		// contaminate either side of the comparison.
		sHuntSettle++;
		if (sHuntStalled) {
			if (sHuntSettle == 20)
				HuntSample(true);
		} else if (sHuntSettle == 240) {
			// Sample ordinary play rarely - every few seconds is plenty, and it
			// keeps the filtering honest across different parts of a level.
			sHuntSettle = 0;
			HuntSample(false);
		}
	}

	// How much pitch the scripted camera is contributing to the rendered view.
	// Zero during ordinary play, so the view is head-pitch only as before.
	// SCRIPTED PITCH PASS-THROUGH: OFF.
	//
	// This is the only consumer of the scripted-camera detection, and every
	// false positive it produces is felt as the world moving vertically with
	// the player's head during ordinary gameplay - the reported symptom. The
	// detector cannot be made reliable by inference: recoil, follow-loop lag
	// and a genuine cutscene all present as "engine pitch diverges from HMD
	// pitch", and the recoil-window fix narrowed that without closing it.
	//
	// Turning the pass-through off removes the failure mode outright rather
	// than tuning it. The cost is that a scripted camera no longer pitches the
	// view - during the ladder climb the player looks up themselves instead of
	// being looked up for them. That is the normal behaviour for VR mods, where
	// forced camera pitch is usually stripped deliberately because it is
	// nauseating and fights the neck.
	//
	// Everything else stays: the closed pitch loop still drives Metro's camera
	// to the head, so culling, the interaction ray and the weapon transform all
	// keep following, and scripted YAW is untouched.
	//
	// Set true to restore cinematic pitch framing, and accept the false
	// positives that come with it. The detector is left intact for that reason.
	static const bool kScriptedPitchPassthrough = true;

	float GetScriptedPitchOffset()
	{
		if (!kScriptedPitchPassthrough)
			return 0.0f;
		// Only positively identified native camera/player-performance ownership
		// can move the rendered view. The old motion classifier remains available
		// for logging but is no longer authoritative.
		if (sScriptedCamActive)
			return sScriptedPitchOffset * sScriptedBlend;
		if (!sScriptedReleaseEaseActive || !G)
			return 0.0f;
		const unsigned age = G->frame_no - sScriptedReleaseStartFrame;
		if (age >= kScriptedReleaseEaseFrames)
			return 0.0f;
		const float t = (float)age / (float)kScriptedReleaseEaseFrames;
		const float weight = 1.0f - t * t * (3.0f - 2.0f * t);
		return sScriptedReleasePitchOffset * weight;
	}

	bool ScriptedCameraActive()
	{
		return sScriptedCamActive;
	}

	float GetCullingCancellation()
	{
		if (InterlockedCompareExchange(&sUpstreamCameraBasisProbeActive, 0, 0) != 0 &&
			InterlockedCompareExchange(&sUpstreamCameraYieldingToScript, 0, 0) == 0)
			return sCullInjectedYaw;
		const int idx = ((sCullHistoryPos - kCullLagFrames) % kCullHistory + kCullHistory) % kCullHistory;
		return sCullInjectedHistory[idx];
	}

	float GetCullingCancellationPitch()
	{
		if (!kNativeStateFollowEnabled &&
			InterlockedCompareExchange(&sUpstreamCameraBasisProbeActive, 0, 0) != 0 &&
			InterlockedCompareExchange(&sUpstreamCameraYieldingToScript, 0, 0) == 0)
			return sCullInjectedPitch;
		// NO LAG COMPENSATION ON THE MEASURED PATH.
		//
		// The history delay exists because the booked value is a PREDICTION -
		// we send mouse input and the engine acts on it about two frames later,
		// so the cancellation had to look back to stay in step. A measurement
		// of the camera's current pitch has nothing to predict: it is already
		// the state the engine is in. Delaying it would reintroduce exactly the
		// lag the delay was invented to remove.
		if (sCullMeasuredPitchValid)
			return sCullMeasuredPitch;
		const int idx = ((sCullHistoryPos - kCullLagFrames) % kCullHistory + kCullHistory) % kCullHistory;
		return sCullInjectedPitchHistory[idx];
	}

	// ---------------------------------------------------------------
	// PORTAL/SECTOR VISIBILITY OVERRIDE
	//
	// Metro's CPU portal traversal lives at metro.exe+0x802330 in the Steam
	// build.  It clears a 384-bit sector mask at context+0x2C0, recursively
	// clips portal polygons through +0x80E400, and returns that mask to the
	// render-recording path.  This is early enough to explain whole NPCs,
	// props and their deferred lights disappearing together; changing GPU
	// matrices or occlusion-query results happens after this decision.
	//
	// Crucially, this hook changes only the completed visibility mask.  It does
	// not rotate the engine camera or touch input, menu, weapon, projectile or
	// render matrices.  Filling the mask is deliberately conservative for the
	// first diagnostic: if the directional pop-in disappears, we have proved
	// the exact culling stage before replacing this with a tighter widened
	// portal frustum.
	typedef void (*tBuildPortalVisibility)(void *context);
	static tBuildPortalVisibility trampoline_BuildPortalVisibility = NULL;
	static volatile LONG sPortalVisibilityCalls = 0;
	static volatile LONG sPortalVisibilityHookActive = 0;

	static void Hooked_BuildPortalVisibility(void *context)
	{
		trampoline_BuildPortalVisibility(context);
		if (context) {
			// The original function itself clears exactly these six qwords at
			// +0x2C0..+0x2E8 before traversing, establishing the mask size.
			memset((BYTE *)context + 0x2C0, 0xFF, 0x30);
		}
		InterlockedIncrement(&sPortalVisibilityCalls);
	}

	static void InstallPortalVisibilityHook()
	{
		static bool sTried = false;
		if (sTried)
			return;
		sTried = true;

		const BYTE *base = (const BYTE *)GetModuleHandleA(NULL);
		if (!base) {
			LogInfo("VRPose portal visibility: no module base; hook disabled\n");
			return;
		}

		// Refuse to hook a different executable build.  These are the first
		// bytes disassembled from the decrypted Steam image at +0x802330.
		static const BYTE expected[] = {
			0x40, 0x53, 0x56, 0x57,
			0x48, 0x81, 0xEC, 0x50, 0x01, 0x00, 0x00,
			0x33, 0xC0, 0xF6, 0x81, 0x08
		};
		BYTE actual[sizeof(expected)] = {};
		SIZE_T got = 0;
		void *fn = (void *)(base + 0x802330);
		if (!ReadProcessMemory(GetCurrentProcess(), fn, actual, sizeof(actual), &got)
			|| got != sizeof(actual) || memcmp(actual, expected, sizeof(expected)) != 0) {
			LogInfo("VRPose portal visibility: signature mismatch at +0x802330; hook disabled\n");
			return;
		}

		SIZE_T hookId = 0;
		const DWORD err = cHookMgr.Hook(&hookId,
			(LPVOID *)&trampoline_BuildPortalVisibility, fn,
			Hooked_BuildPortalVisibility);
		if (err || !trampoline_BuildPortalVisibility) {
			LogInfo("VRPose portal visibility: hook FAILED at +0x802330 (err=0x%x)\n", err);
			return;
		}

		InterlockedExchange(&sPortalVisibilityHookActive, 1);
		LogInfo("VRPose portal visibility: culling-only hook installed at +0x802330; "
			"final 384-sector mask forced visible\n");
	}

	// Metro performs a later, independent frustum test for individual scene
	// objects at +0x8031C7.  If its predicate returns false, +0x8031CF branches
	// directly to the rejection counter and returns without recording the
	// object.  Portal sectors can therefore all be visible while their contents
	// still pop according to camera angle and distance.
	//
	// There are two bound representations here. The first uses the frustum's
	// sphere predicate at +0x8031C7; the second calls the AABB helper at
	// +0x8031F5. Both rejection branches must be bypassed or a scene containing
	// instanced/AABB props can remain unchanged even though the sphere path was
	// successfully patched. The predicates still run, preserving bookkeeping.
	static volatile LONG sObjectFrustumBypassActive = 0;
	static void InstallObjectFrustumBypass()
	{
		static bool sTried = false;
		if (sTried)
			return;
		sTried = true;

		BYTE *base = (BYTE *)GetModuleHandleA(NULL);
		if (!base) {
			LogInfo("VRPose object visibility: no module base; bypass disabled\n");
			return;
		}

		static const BYTE expected[] = {
			0xFF, 0x90, 0x90, 0x01, 0x00, 0x00, // call [rax+0x190]
			0x84, 0xC0,                         // test al,al
			0x75, 0x38,                         // jne accepted
			0xFF, 0x47, 0x68                    // inc rejection counter
		};
		BYTE actual[sizeof(expected)] = {};
		SIZE_T got = 0;
		BYTE *site = base + 0x8031C7;
		if (!ReadProcessMemory(GetCurrentProcess(), site, actual, sizeof(actual), &got)
			|| got != sizeof(actual) || memcmp(actual, expected, sizeof(expected)) != 0) {
			LogInfo("VRPose object visibility: signature mismatch at +0x8031C7; "
				"bypass disabled\n");
			return;
		}
		static const BYTE expectedAabb[] = {
			0xE8, 0xB6, 0x5D, 0x0E, 0x00, // call AABB/frustum helper
			0x85, 0xC0,                   // test eax,eax
			0x75, 0x0B,                   // jne accepted
			0xFF, 0x47, 0x68              // inc rejection counter
		};
		BYTE actualAabb[sizeof(expectedAabb)] = {};
		SIZE_T gotAabb = 0;
		BYTE *siteAabb = base + 0x8031F5;
		if (!ReadProcessMemory(GetCurrentProcess(), siteAabb, actualAabb,
				sizeof(actualAabb), &gotAabb)
			|| gotAabb != sizeof(actualAabb)
			|| memcmp(actualAabb, expectedAabb, sizeof(expectedAabb)) != 0) {
			LogInfo("VRPose object visibility: AABB signature mismatch at +0x8031F5; "
				"bypass disabled\n");
			return;
		}

		BYTE *branch = base + 0x8031CF;
		DWORD oldProtect = 0;
		if (!VirtualProtect(branch, 1, PAGE_EXECUTE_READWRITE, &oldProtect)) {
			LogInfo("VRPose object visibility: VirtualProtect failed at +0x8031CF\n");
			return;
		}
		const BYTE unconditionalJump = 0xEB;
		SIZE_T wrote = 0;
		const BOOL writeOk = WriteProcessMemory(GetCurrentProcess(), branch,
			&unconditionalJump, 1, &wrote);
		FlushInstructionCache(GetCurrentProcess(), branch, 1);
		DWORD ignoredProtect = 0;
		VirtualProtect(branch, 1, oldProtect, &ignoredProtect);
		if (!writeOk || wrote != 1 || *branch != unconditionalJump) {
			LogInfo("VRPose object visibility: branch patch failed at +0x8031CF\n");
			return;
		}

		BYTE *branchAabb = base + 0x8031FC;
		DWORD oldProtectAabb = 0;
		if (!VirtualProtect(branchAabb, 1, PAGE_EXECUTE_READWRITE, &oldProtectAabb)) {
			LogInfo("VRPose object visibility: VirtualProtect failed at +0x8031FC\n");
			return;
		}
		SIZE_T wroteAabb = 0;
		const BOOL writeAabbOk = WriteProcessMemory(GetCurrentProcess(), branchAabb,
			&unconditionalJump, 1, &wroteAabb);
		FlushInstructionCache(GetCurrentProcess(), branchAabb, 1);
		VirtualProtect(branchAabb, 1, oldProtectAabb, &ignoredProtect);
		if (!writeAabbOk || wroteAabb != 1 || *branchAabb != unconditionalJump) {
			LogInfo("VRPose object visibility: AABB branch patch failed at +0x8031FC\n");
			return;
		}

		InterlockedExchange(&sObjectFrustumBypassActive, 1);
		LogInfo("VRPose object visibility: sphere and AABB rejection bypassed "
			"at +0x8031CF/+0x8031FC (culling only)\n");
	}

	static volatile LONG sClusterFrustumBypassActive = 0;
	static void InstallClusterFrustumBypass()
	{
		static bool sTried = false;
		if (sTried)
			return;
		sTried = true;

		BYTE *base = (BYTE *)GetModuleHandleA(NULL);
		if (!base)
			return;

		// The spatial-list walker at +0x802C30 tests a cluster against one
		// of its active frusta, then skips the cluster at +0x802D1F when the
		// predicate returns false.  A skipped cluster never reaches the later
		// per-object diagnostic above.
		static const BYTE expected[] = {
			0x41, 0xFF, 0x90, 0x90, 0x01, 0x00, 0x00, // call [r8+0x190]
			0x84, 0xC0,                               // test al,al
			0x74, 0x16,                               // je skip cluster
			0x8B, 0x5F, 0x68
		};
		BYTE actual[sizeof(expected)] = {};
		SIZE_T got = 0;
		BYTE *site = base + 0x802D16;
		if (!ReadProcessMemory(GetCurrentProcess(), site, actual, sizeof(actual), &got)
			|| got != sizeof(actual) || memcmp(actual, expected, sizeof(expected)) != 0) {
			LogInfo("VRPose cluster visibility: signature mismatch at +0x802D16; "
				"bypass disabled\n");
			return;
		}

		BYTE *branch = base + 0x802D1F;
		DWORD oldProtect = 0;
		if (!VirtualProtect(branch, 2, PAGE_EXECUTE_READWRITE, &oldProtect)) {
			LogInfo("VRPose cluster visibility: VirtualProtect failed at +0x802D1F\n");
			return;
		}
		const BYTE nops[2] = { 0x90, 0x90 };
		SIZE_T wrote = 0;
		const BOOL writeOk = WriteProcessMemory(GetCurrentProcess(), branch,
			nops, sizeof(nops), &wrote);
		FlushInstructionCache(GetCurrentProcess(), branch, sizeof(nops));
		DWORD ignoredProtect = 0;
		VirtualProtect(branch, 2, oldProtect, &ignoredProtect);
		if (!writeOk || wrote != sizeof(nops)
			|| branch[0] != 0x90 || branch[1] != 0x90) {
			LogInfo("VRPose cluster visibility: branch patch failed at +0x802D1F\n");
			return;
		}

		InterlockedExchange(&sClusterFrustumBypassActive, 1);
		LogInfo("VRPose cluster visibility: early spatial-cluster frustum rejection "
			"bypassed at +0x802D1F (culling only)\n");
	}

	// Trace the CPU-side recorder for a draw proven by paired frame analyses to
	// exist only at the head angle where the missing room contents appear.
	// +0x838240 converts a mesh descriptor into opcode 0x7000000E, the 28-byte
	// indexed-instanced command replayed at +0x7EE977. Its descriptor fields map
	// directly to the eventual D3D draw:
	//   +0x18 = BaseVertexLocation
	//   +0x20 = StartIndexLocation (ordinary path)
	//   +0x24 low 24 bits = IndexCountPerInstance / 3
	// The batch's +0x800 word is packed into the command's instance count.
	// Capturing the recorder's caller stack gets us upstream of D3D replay and
	// into the render-list selection path without changing any game state.
	typedef void (*tRecordIndexedBatch)(void *context, void *batch);
	static tRecordIndexedBatch trampoline_RecordIndexedBatch = NULL;
	static volatile LONG sRenderListRecorderTraceCaptured = 0;

	static void Hooked_RecordIndexedBatch(void *context, void *batch)
	{
		bool matched = false;
		BYTE *capturedFirstEntry = NULL;
		BYTE *capturedMesh = NULL;
		BYTE *capturedDescriptor = NULL;
		INT baseVertex = 0;
		UINT startIndex = 0;
		UINT primitiveCount = 0;
		UINT instanceCount = 0;
		__try {
			BYTE *firstEntry = *(BYTE **)batch;
			BYTE *mesh = firstEntry ? *(BYTE **)firstEntry : NULL;
			BYTE *descriptor = mesh ? *(BYTE **)(mesh + 0x78) : NULL;
			capturedFirstEntry = firstEntry;
			capturedMesh = mesh;
			capturedDescriptor = descriptor;
			if (descriptor) {
				baseVertex = *(INT *)(descriptor + 0x18);
				startIndex = *(UINT *)(descriptor + 0x20);
				primitiveCount = *(UINT *)(descriptor + 0x24) & 0x00FFFFFFu;
				instanceCount = *(USHORT *)((BYTE *)batch + 0x800);
				struct Target {
					UINT primitiveCount;
					UINT startIndex;
					INT baseVertex;
				};
				static const Target targets[] = {
					{ 64,   3072216, 870537 },
					{ 28,   3434067, 959533 },
					{ 434,  3434151, 959581 },
					{ 1210, 2968917, 839236 },
					{ 1840,  340497,  59036 },
					{ 5308, 1569900, 357067 },
				};
				for (const auto &target : targets) {
					// Base vertex and primitive count are sufficient to identify
					// the mesh even if the recorder takes its alternate subrange
					// path and derives StartIndexLocation.
					if (primitiveCount == target.primitiveCount
						&& baseVertex == target.baseVertex) {
						matched = true;
						break;
					}
				}
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			matched = false;
		}

		if (matched && capturedFirstEntry
		    && InterlockedCompareExchangePointer(
				(PVOID volatile *)&sQueueEntryWatchAddress,
				capturedFirstEntry, NULL) == NULL) {
			InterlockedExchange(&sQueueEntryWatchState, 1);
			LogInfo("VRPose queue watch: target slot %p captured; arming next Present\n",
				capturedFirstEntry);
		}

		if (matched
			&& InterlockedCompareExchange(&sRenderListRecorderTraceCaptured, 1, 0) == 0) {
			void *frames[40] = {};
			const USHORT count = CaptureStackBackTrace(0, ARRAYSIZE(frames), frames, NULL);
			BYTE *moduleBase = (BYTE *)GetModuleHandleA(NULL);
			SIZE_T imageSize = 0;
			if (moduleBase) {
				const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)moduleBase;
				if (dos->e_magic == IMAGE_DOS_SIGNATURE) {
					const IMAGE_NT_HEADERS *nt =
						(const IMAGE_NT_HEADERS *)(moduleBase + dos->e_lfanew);
					if (nt->Signature == IMAGE_NT_SIGNATURE)
						imageSize = nt->OptionalHeader.SizeOfImage;
				}
			}
			LogInfo("VRPose render-list recorder trace: matched base=%d start=%u "
				"primitives=%u instances=%u; stackFrames=%u\n",
				baseVertex, startIndex, primitiveCount, instanceCount,
				(unsigned)count);
			LogInfo("VRPose render-list object: context=%p batch=%p first=%p mesh=%p "
				"descriptor=%p frame=%u\n", context, batch, capturedFirstEntry,
				capturedMesh, capturedDescriptor, G->frame_no);
			// A read-only shallow pointer/field dump gives us stable identities to
			// look for in the upstream scene-list functions.  Do this once only;
			// the recorder is a render worker and continuous logging would perturb
			// timing enough to muddy the visibility test.
			struct DumpRegion { const char *name; BYTE *address; size_t bytes; };
			const DumpRegion regions[] = {
				{ "batch", (BYTE *)batch, 0x100 },
				{ "first", capturedFirstEntry, 0x100 },
				{ "mesh", capturedMesh, 0x100 },
				{ "descriptor", capturedDescriptor, 0x80 },
			};
			for (const auto &region : regions) {
				if (!region.address)
					continue;
				__try {
					for (size_t offset = 0; offset < region.bytes; offset += 0x20) {
						const unsigned long long *q =
							(const unsigned long long *)(region.address + offset);
						LogInfo("VRPose render-list object: %s+%03llX "
							"%016llX %016llX %016llX %016llX\n",
							region.name, (unsigned long long)offset,
							q[0], q[1], q[2], q[3]);
					}
				}
				__except (EXCEPTION_EXECUTE_HANDLER) {
					LogInfo("VRPose render-list object: %s unreadable at %p\n",
						region.name, region.address);
				}
			}
			unsigned metroFrame = 0;
			for (USHORT i = 0; i < count; ++i) {
				BYTE *address = (BYTE *)frames[i];
				if (moduleBase && imageSize && address >= moduleBase
					&& address < moduleBase + imageSize) {
					LogInfo("VRPose render-list recorder trace: metro[%u] +0x%llX\n",
						metroFrame++, (unsigned long long)(address - moduleBase));
				}
			}
		}

		trampoline_RecordIndexedBatch(context, batch);
	}

	static void InstallRenderListRecorderTrace()
	{
		static bool sTried = false;
		if (sTried)
			return;
		sTried = true;
		BYTE *base = (BYTE *)GetModuleHandleA(NULL);
		if (!base)
			return;
		static const BYTE expected[] = {
			0x48, 0x89, 0x4C, 0x24, 0x08, // mov [rsp+8],rcx
			0x53, 0x55, 0x56, 0x57,       // push rbx/rbp/rsi/rdi
			0x41, 0x55, 0x41, 0x56, 0x41, 0x57
		};
		BYTE *fn = base + 0x838240;
		if (memcmp(fn, expected, sizeof(expected)) != 0) {
			LogInfo("VRPose render-list recorder trace: signature mismatch at +0x838240\n");
			return;
		}
		SIZE_T hookId = 0;
		const DWORD err = cHookMgr.Hook(&hookId,
			(LPVOID *)&trampoline_RecordIndexedBatch, fn, Hooked_RecordIndexedBatch);
		if (err || !trampoline_RecordIndexedBatch) {
			LogInfo("VRPose render-list recorder trace: hook FAILED (err=0x%x)\n", err);
			return;
		}
		LogInfo("VRPose render-list recorder trace: observer installed at +0x838240\n");
	}

	// The disappearing mesh's recorder stack led back to +0x802990, whose
	// worker callback +0x801240 performs an earlier root spatial-bound frustum
	// test before any of the cluster/object gates tested previously. A false
	// result at +0x8013B3 skips directly to the end of list construction at
	// +0x801696, omitting every child object together. Bypass only that reject;
	// all inner distance, portal, cluster, and object tests remain intact.
	static volatile LONG sPrimarySpatialRootBypassActive = 0;
	static void InstallPrimarySpatialRootFrustumBypass()
	{
		static bool sTried = false;
		if (sTried)
			return;
		sTried = true;
		BYTE *base = (BYTE *)GetModuleHandleA(NULL);
		if (!base)
			return;
		static const BYTE expected[] = {
			0xFF, 0x90, 0xF0, 0x01, 0x00, 0x00, // call [rax+0x1F0]
			0x84, 0xC0,                         // test al,al
			0x0F, 0x84, 0xDD, 0x02, 0x00, 0x00  // je +0x801696
		};
		BYTE *site = base + 0x8013AB;
		if (memcmp(site, expected, sizeof(expected)) != 0) {
			LogInfo("VRPose primary spatial root: signature mismatch at +0x8013AB\n");
			return;
		}
		BYTE *branch = base + 0x8013B3;
		DWORD oldProtect = 0;
		if (!VirtualProtect(branch, 6, PAGE_EXECUTE_READWRITE, &oldProtect)) {
			LogInfo("VRPose primary spatial root: VirtualProtect failed at +0x8013B3\n");
			return;
		}
		const BYTE nops[6] = { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };
		SIZE_T wrote = 0;
		const BOOL ok = WriteProcessMemory(GetCurrentProcess(), branch,
			nops, sizeof(nops), &wrote);
		FlushInstructionCache(GetCurrentProcess(), branch, sizeof(nops));
		DWORD ignoredProtect = 0;
		VirtualProtect(branch, 6, oldProtect, &ignoredProtect);
		if (!ok || wrote != sizeof(nops) || memcmp(branch, nops, sizeof(nops)) != 0) {
			LogInfo("VRPose primary spatial root: branch patch failed at +0x8013B3\n");
			return;
		}
		InterlockedExchange(&sPrimarySpatialRootBypassActive, 1);
		LogInfo("VRPose primary spatial root: root-frustum rejection bypassed "
			"at +0x8013B3 (culling only)\n");
	}

	// After the root bounds pass, +0x80157D tests the primary scene object's
	// sphere against the active visibility frustum. A miss jumps to a separate
	// cluster fallback at +0x80163D. The previous cluster experiment only
	// loosened rejection *inside* that fallback; it never forced objects down
	// the direct submission path at +0x80158B. Test this upstream decision in
	// isolation from the ineffective root-bound bypass.
	static volatile LONG sPrimaryObjectSphereBypassActive = 0;
	static void InstallPrimaryObjectSphereFrustumBypass()
	{
		static bool sTried = false;
		if (sTried)
			return;
		sTried = true;
		BYTE *base = (BYTE *)GetModuleHandleA(NULL);
		if (!base)
			return;
		static const BYTE expected[] = {
			0xFF, 0x90, 0x90, 0x01, 0x00, 0x00, // call [rax+0x190]
			0x84, 0xC0,                         // test al,al
			0x0F, 0x84, 0xB2, 0x00, 0x00, 0x00  // je +0x80163D
		};
		BYTE *site = base + 0x80157D;
		if (memcmp(site, expected, sizeof(expected)) != 0) {
			LogInfo("VRPose primary object sphere: signature mismatch at +0x80157D\n");
			return;
		}
		BYTE *branch = base + 0x801585;
		DWORD oldProtect = 0;
		if (!VirtualProtect(branch, 6, PAGE_EXECUTE_READWRITE, &oldProtect)) {
			LogInfo("VRPose primary object sphere: VirtualProtect failed at +0x801585\n");
			return;
		}
		const BYTE nops[6] = { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };
		SIZE_T wrote = 0;
		const BOOL ok = WriteProcessMemory(GetCurrentProcess(), branch,
			nops, sizeof(nops), &wrote);
		FlushInstructionCache(GetCurrentProcess(), branch, sizeof(nops));
		DWORD ignoredProtect = 0;
		VirtualProtect(branch, 6, oldProtect, &ignoredProtect);
		if (!ok || wrote != sizeof(nops) || memcmp(branch, nops, sizeof(nops)) != 0) {
			LogInfo("VRPose primary object sphere: branch patch failed at +0x801585\n");
			return;
		}
		InterlockedExchange(&sPrimaryObjectSphereBypassActive, 1);
		LogInfo("VRPose primary object sphere: frustum fallback bypassed "
			"at +0x801585 (culling only)\n");
	}

	// In the same proven builder, objects outside a distance/radius threshold
	// branch at +0x80155E directly to the narrow cluster fallback, never
	// reaching the primary sphere decision above. This matches the measured
	// behaviour: objects settle when approached, while the head angle needed
	// to reveal them grows with distance. Keep distant objects on the same
	// direct submission path used for nearby geometry.
	static volatile LONG sDistanceFallbackBypassActive = 0;
	static void InstallDistanceFallbackBypass()
	{
		static bool sTried = false;
		if (sTried)
			return;
		sTried = true;
		BYTE *base = (BYTE *)GetModuleHandleA(NULL);
		if (!base)
			return;
		static const BYTE expected[] = {
			0x0F, 0x2F, 0xD3,                   // comiss xmm2,xmm3
			0x0F, 0x29, 0x55, 0x10,             // movaps [rbp+0x10],xmm2
			0x0F, 0x83, 0xE0, 0x00, 0x00, 0x00  // jae +0x801644
		};
		BYTE *site = base + 0x801557;
		if (memcmp(site, expected, sizeof(expected)) != 0) {
			LogInfo("VRPose distance fallback: signature mismatch at +0x801557\n");
			return;
		}
		BYTE *branch = base + 0x80155E;
		DWORD oldProtect = 0;
		if (!VirtualProtect(branch, 6, PAGE_EXECUTE_READWRITE, &oldProtect)) {
			LogInfo("VRPose distance fallback: VirtualProtect failed at +0x80155E\n");
			return;
		}
		const BYTE nops[6] = { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };
		SIZE_T wrote = 0;
		const BOOL ok = WriteProcessMemory(GetCurrentProcess(), branch,
			nops, sizeof(nops), &wrote);
		FlushInstructionCache(GetCurrentProcess(), branch, sizeof(nops));
		DWORD ignoredProtect = 0;
		VirtualProtect(branch, 6, oldProtect, &ignoredProtect);
		if (!ok || wrote != sizeof(nops) || memcmp(branch, nops, sizeof(nops)) != 0) {
			LogInfo("VRPose distance fallback: branch patch failed at +0x80155E\n");
			return;
		}
		InterlockedExchange(&sDistanceFallbackBypassActive, 1);
		LogInfo("VRPose distance fallback: distant objects kept on direct path "
			"at +0x80155E (culling only)\n");
	}

	// Before the primary object/cluster routing, the proven builder computes a
	// screen-space-area proxy (bound radius squared divided by distance
	// squared) and rejects the whole entry when it falls below a threshold.
	// This naturally makes small props require roughly 5-6 feet while larger
	// bounds remain visible farther away. Test this sole early SSA discard in
	// isolation from the ineffective route/frustum bypasses.
	static volatile LONG sSsaDiscardBypassActive = 0;
	static void InstallSsaDiscardBypass()
	{
		static bool sTried = false;
		if (sTried)
			return;
		sTried = true;
		BYTE *base = (BYTE *)GetModuleHandleA(NULL);
		if (!base)
			return;
		static const BYTE expected[] = {
			0xF3, 0x0F, 0x5E, 0xC2,             // divss xmm0,xmm2
			0x0F, 0x2F, 0x85, 0x58, 0x01, 0x00, 0x00, // comiss xmm0,[rbp+158]
			0x0F, 0x82, 0x80, 0x02, 0x00, 0x00  // jb +0x801696
		};
		BYTE *site = base + 0x801405;
		if (memcmp(site, expected, sizeof(expected)) != 0) {
			LogInfo("VRPose SSA discard: signature mismatch at +0x801405\n");
			return;
		}
		BYTE *branch = base + 0x801410;
		DWORD oldProtect = 0;
		if (!VirtualProtect(branch, 6, PAGE_EXECUTE_READWRITE, &oldProtect)) {
			LogInfo("VRPose SSA discard: VirtualProtect failed at +0x801410\n");
			return;
		}
		const BYTE nops[6] = { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };
		SIZE_T wrote = 0;
		const BOOL ok = WriteProcessMemory(GetCurrentProcess(), branch,
			nops, sizeof(nops), &wrote);
		FlushInstructionCache(GetCurrentProcess(), branch, sizeof(nops));
		DWORD ignoredProtect = 0;
		VirtualProtect(branch, 6, oldProtect, &ignoredProtect);
		if (!ok || wrote != sizeof(nops) || memcmp(branch, nops, sizeof(nops)) != 0) {
			LogInfo("VRPose SSA discard: branch patch failed at +0x801410\n");
			return;
		}
		InterlockedExchange(&sSsaDiscardBypassActive, 1);
		LogInfo("VRPose SSA discard: early radius/distance rejection bypassed "
			"at +0x801410 (culling only)\n");
	}

	// The exact disappearing draw's recorder stack reaches the world-list
	// construction call at +0x812D7B.  That call initializes a private
	// visibility context through +0x802580.  Crucially, +0x802580 copies an
	// already-built frustum from its fourth argument into context+0x60; this is
	// a separate snapshot from both the public camera object and the projection
	// constants patched for VR.  The snapshot contains up to twelve 32-byte
	// clipping planes and a count at +0x180.
	//
	// Earlier experiments bypassed individual predicates reached from this
	// context.  They could not cover every list/portal/detail path consuming the
	// same snapshot.  For this isolated diagnostic, give only the proven main
	// world-list call a copy with zero active planes.  All gameplay camera state,
	// input, rendering matrices, shadows, weapons and menus remain untouched.
	typedef void *(*tInitVisibilityContext)(void *context, int mode, void *work,
		void *frustum, void *matrix, void *cameraPos, void *sceneRoot,
		int passId, int flags, void *output);
	static tInitVisibilityContext trampoline_InitVisibilityContext = NULL;
	static volatile LONG sVisibilityContextBypassCalls = 0;

	static void *Hooked_InitVisibilityContext(void *context, int mode, void *work,
		void *frustum, void *matrix, void *cameraPos, void *sceneRoot,
		int passId, int flags, void *output)
	{
		BYTE *base = (BYTE *)GetModuleHandleA(NULL);
		BYTE *returnAddress = (BYTE *)_ReturnAddress();
		const bool mainWorldList = base && returnAddress == base + 0x812D80;
		alignas(16) BYTE openFrustum[0x1A0] = {};
		void *selectedFrustum = frustum;

		if (mainWorldList && frustum) {
			bool copied = false;
			__try {
				memcpy(openFrustum, frustum, sizeof(openFrustum));
				// No clipping planes: every bound is inside this diagnostic
				// visibility volume. Preserve the function pointer at +0x190.
				*(USHORT *)(openFrustum + 0x180) = 0;
				selectedFrustum = openFrustum;
				copied = true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				selectedFrustum = frustum;
			}
			if (copied) {
				const LONG calls = InterlockedIncrement(&sVisibilityContextBypassCalls);
				if (calls == 1)
					LogInfo("VRPose world visibility context: zero-plane frustum supplied "
						"at +0x812D7B (culling only)\n");
			}
		}

		return trampoline_InitVisibilityContext(context, mode, work,
			selectedFrustum, matrix, cameraPos, sceneRoot, passId, flags, output);
	}

	static void InstallVisibilityContextFrustumBypass()
	{
		static bool sTried = false;
		if (sTried)
			return;
		sTried = true;
		BYTE *base = (BYTE *)GetModuleHandleA(NULL);
		if (!base)
			return;
		static const BYTE expected[] = {
			0x48, 0x89, 0x5C, 0x24, 0x08,
			0x48, 0x89, 0x6C, 0x24, 0x10,
			0x48, 0x89, 0x74, 0x24, 0x18,
			0x57, 0x48, 0x83, 0xEC, 0x30
		};
		BYTE *fn = base + 0x802580;
		if (memcmp(fn, expected, sizeof(expected)) != 0) {
			LogInfo("VRPose world visibility context: signature mismatch at +0x802580\n");
			return;
		}
		SIZE_T hookId = 0;
		const DWORD err = cHookMgr.Hook(&hookId,
			(LPVOID *)&trampoline_InitVisibilityContext, fn,
			Hooked_InitVisibilityContext);
		if (err || !trampoline_InitVisibilityContext) {
			LogInfo("VRPose world visibility context: hook FAILED (err=0x%x)\n", err);
			return;
		}
		LogInfo("VRPose world visibility context: observer/bypass installed at +0x802580\n");
	}

	static void LogWorldVisibilityOwnerOnce()
	{
		static bool logged = false;
		if (logged)
			return;
		BYTE *base = (BYTE *)GetModuleHandleA(NULL);
		if (!base)
			return;
		// +0x647ED6 loads this global renderer pointer before dispatching the
		// frame method whose stack reaches +0x812820.  Reading the owner here is
		// safer than intercepting the protected visibility-context initializer.
		static const DWORD_PTR kRendererOwnerPtrRva = 0xD01E10;
		__try {
			BYTE *owner = *(BYTE **)(base + kRendererOwnerPtrRva);
			if (!owner)
				return;
			BYTE *vtable = *(BYTE **)owner;
			BYTE *prepareFn = vtable ? *(BYTE **)(vtable + 0x190) : NULL;
			BYTE *frustum = *(BYTE **)(owner + 0xA70);
			if (!vtable || !prepareFn || !frustum)
				return;
			const USHORT planeCount = *(USHORT *)(frustum + 0x180);
			BYTE *predicate = *(BYTE **)(frustum + 0x190);
			float *matrix = (float *)(owner + 0x870);
			float *camera = (float *)(owner + 0x470);
			LogInfo("VRPose world visibility owner: owner=%p vtable=%p prepare=%p "
				"(+0x%llX) frustum=%p planes=%u predicate=%p (+0x%llX)\n",
				owner, vtable, prepareFn,
				(unsigned long long)(prepareFn - base), frustum,
				(unsigned)planeCount, predicate,
				(unsigned long long)(predicate - base));
			LogInfo("VRPose world visibility owner: camera470=(%.5f %.5f %.5f %.5f)\n",
				camera[0], camera[1], camera[2], camera[3]);
			for (unsigned row = 0; row < 4; ++row) {
				const float *m = matrix + row * 4;
				LogInfo("VRPose world visibility owner: matrix870[%u]=(%.6f %.6f %.6f %.6f)\n",
					row, m[0], m[1], m[2], m[3]);
			}
			const unsigned dumpPlanes = planeCount < 12 ? planeCount : 12;
			for (unsigned i = 0; i < dumpPlanes; ++i) {
				const float *p = (const float *)(frustum + i * 0x20);
				LogInfo("VRPose world visibility owner: plane[%u]=(%.6f %.6f %.6f %.6f) "
					"aux=(%.6f %.6f %.6f %.6f)\n", i,
					p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
			}
			logged = true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			// Renderer owner is not fully initialized during the intro. Retry on
			// a later Present instead of touching any of its state.
		}
	}

	static void InstallPrimaryVisibilityPlaneMaskBypass()
	{
		static bool tried = false;
		if (tried)
			return;
		tried = true;
		BYTE *base = (BYTE *)GetModuleHandleA(NULL);
		if (!base)
			return;
		// +0x812AFD supplies the global world render matrix to Metro's native
		// frustum constructor. The following LEA loads the five-plane selection
		// mask (0x1F). A zero mask is a supported constructor case: +0x8E92A0
		// has an explicit count-zero dispatch entry and installs the matching
		// predicate. This tests the primary visibility volume without fabricating
		// or mutating a live frustum object.
		static const BYTE expected[] = {
			0x48, 0x8D, 0x15, 0x9C, 0x04, 0x51, 0x00, // lea rdx,[+0xD22FA0]
			0x44, 0x8D, 0x46, 0x1F,                   // lea r8d,[rsi+0x1F]
			0x49, 0x8B, 0xCD                          // mov rcx,r13
		};
		BYTE *site = base + 0x812AFD;
		if (memcmp(site, expected, sizeof(expected)) != 0) {
			LogInfo("VRPose primary visibility mask: signature mismatch at +0x812AFD\n");
			return;
		}
		BYTE *maskImmediate = base + 0x812B07;
		DWORD oldProtect = 0;
		if (!VirtualProtect(maskImmediate, 1, PAGE_EXECUTE_READWRITE, &oldProtect)) {
			LogInfo("VRPose primary visibility mask: VirtualProtect failed\n");
			return;
		}
		const BYTE zero = 0;
		SIZE_T wrote = 0;
		const BOOL ok = WriteProcessMemory(GetCurrentProcess(), maskImmediate,
			&zero, sizeof(zero), &wrote);
		FlushInstructionCache(GetCurrentProcess(), maskImmediate, 1);
		DWORD ignoredProtect = 0;
		VirtualProtect(maskImmediate, 1, oldProtect, &ignoredProtect);
		if (!ok || wrote != 1 || *maskImmediate != 0) {
			LogInfo("VRPose primary visibility mask: patch failed at +0x812B07\n");
			return;
		}
		LogInfo("VRPose primary visibility mask: native frustum constructor mask "
			"changed 0x1F -> 0 at +0x812B07 (culling only)\n");
	}

	// The hardware watch on a render-queue slot belonging to a proven missing
	// mesh led directly to this gate. +0x826B50 projects all eight corners of
	// the object's bounds, scans a CPU-side screen/depth buffer and returns zero
	// when every covered sample is considered occluded. That is a completely
	// separate visibility path from D3D11_QUERY_OCCLUSION, explaining why
	// forcing all GPU query results visible had no effect. It also runs before
	// the renderer records the draw, matching the paired frame analyses.
	//
	// All six direct callers treat a nonzero return as visible. Return visible at
	// the common routine itself so every render-list category uses the same VR-
	// safe result. This also removes the CPU scan cost and touches neither the
	// engine camera nor controls, weapon transforms, menus, ballistics or shadows.
	static void InstallCpuScreenOcclusionBypass()
	{
		static bool tried = false;
		if (tried)
			return;
		tried = true;
		BYTE *base = (BYTE *)GetModuleHandleA(NULL);
		if (!base)
			return;
		static const BYTE expected[] = {
			0x4C, 0x8B, 0xDC,                   // mov r11,rsp
			0x49, 0x89, 0x5B, 0x20,             // mov [r11+20h],rbx
			0x41, 0x56,                         // push r14
			0x48, 0x81, 0xEC, 0xA0, 0x00, 0x00, 0x00
		};
		BYTE *site = base + 0x826B50;
		if (memcmp(site, expected, sizeof(expected)) != 0) {
			LogInfo("VRPose CPU screen occlusion: signature mismatch at +0x826B50\n");
			return;
		}
		BYTE *branch = site;
		static const BYTE forceVisible[] = {
			0xB8, 0x01, 0x00, 0x00, 0x00, // mov eax,1
			0xC3                          // ret
		};
		DWORD oldProtect = 0;
		if (!VirtualProtect(branch, sizeof(forceVisible), PAGE_EXECUTE_READWRITE,
			&oldProtect)) {
			LogInfo("VRPose CPU screen occlusion: VirtualProtect failed\n");
			return;
		}
		SIZE_T wrote = 0;
		const BOOL ok = WriteProcessMemory(GetCurrentProcess(), branch,
			forceVisible, sizeof(forceVisible), &wrote);
		FlushInstructionCache(GetCurrentProcess(), branch, sizeof(forceVisible));
		DWORD ignoredProtect = 0;
		VirtualProtect(branch, sizeof(forceVisible), oldProtect, &ignoredProtect);
		if (!ok || wrote != sizeof(forceVisible)
		    || memcmp(branch, forceVisible, sizeof(forceVisible)) != 0) {
			LogInfo("VRPose CPU screen occlusion: function patch failed at +0x826B50\n");
			return;
		}
		LogInfo("VRPose CPU screen occlusion: common six-caller routine forced visible "
			"at +0x826B50 (culling only)\n");
	}

	// A committed page within rel32 reach of the executable (for redirected
	// RIP-relative operands), searched downward from the image base first.
	static BYTE *AllocNearModule(DWORD64 base, DWORD64 end)
	{
		const DWORD64 kStep = 0x100000;
		for (DWORD64 d = kStep; d < 0x40000000ull; d += kStep) {
			if (base > d + 0x10000) {
				void *p = VirtualAlloc((LPVOID)((base - d) & ~0xFFFFull), 0x1000,
					MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
				if (p)
					return (BYTE *)p;
			}
			void *p = VirtualAlloc((LPVOID)((end + d) & ~0xFFFFull), 0x1000,
				MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
			if (p)
				return (BYTE *)p;
		}
		return NULL;
	}

	// Rewrites a live disp32 with a single 4-byte store.
	static void WriteDisp32(BYTE *at, LONG value)
	{
		DWORD old = 0;
		if (!VirtualProtect(at, sizeof(LONG), PAGE_EXECUTE_READWRITE, &old))
			return;
		InterlockedExchange((LONG *)at, value);
		DWORD ignored = 0;
		VirtualProtect(at, sizeof(LONG), old, &ignored);
		FlushInstructionCache(GetCurrentProcess(), at, sizeof(LONG));
	}

	// Deferred light culling FOV.
	//
	// Metro culls deferred lights with a camera whose vertical FOV is the world
	// FOV (RVA 0xCE710C) read at +0x69CC72 (`mulss xmm3, [fov]`). That camera is
	// much narrower than the headset, so lights above the view were dropped when
	// looking up and nearby characters lost their lighting. Only this operand is
	// pointed at a private 110-degree value; the world FOV itself is untouched.
	// Known side effect: faint dark shapes can appear at the very bottom of the
	// view near some lights. vr_vis_light_fov_off.txt leaves the code unpatched.
	static void ApplyLightCullingFov()
	{
		static int state = -1;   // -1 not tried, 0 not applied, 1 applied
		static BYTE *disp = NULL;
		static LONG redirected = 0;
		static float *value = NULL;
		if (state < 0) {
			state = 0;
			if (StereoSinglePass::FlagFilePresent(L"vr_vis_light_fov_off.txt"))
				return;
			static const BYTE kSignature[8] = { 0xF3, 0x0F, 0x59, 0x1D, 0x92, 0xA4, 0x64, 0x00 };
			const HMODULE exe = GetModuleHandleA(NULL);
			BYTE *at = exe ? (BYTE *)exe + 0x69CC72 : NULL;
			BYTE bytes[8] = {};
			SIZE_T got = 0;
			if (at && ReadProcessMemory(GetCurrentProcess(), at, bytes, sizeof(bytes), &got)
				&& got == sizeof(bytes) && memcmp(bytes, kSignature, sizeof(bytes)) == 0) {
				const DWORD64 base = (DWORD64)exe;
				const IMAGE_NT_HEADERS64 *nt = (const IMAGE_NT_HEADERS64 *)((const BYTE *)exe
					+ ((const IMAGE_DOS_HEADER *)exe)->e_lfanew);
				if (BYTE *page = AllocNearModule(base, base + nt->OptionalHeader.SizeOfImage)) {
					value = (float *)(page + 0x800);
					*value = 110.0f;
					const LONG64 rel = (LONG64)(DWORD64)value - (LONG64)(DWORD64)(at + 8);
					if (rel == (LONG64)(LONG)rel) {
						disp = at + 4;
						redirected = (LONG)rel;
						state = 1;
					}
				}
			}
			LogInfo("VRPose light culling FOV: %s\n", state == 1
				? "applied at +0x69CC72" : "signature or allocation mismatch, not applied");
		}
		if (state == 1 && *(LONG *)disp != redirected)
			WriteDisp32(disp, redirected);
	}

	// A controlled same-save A/B found that the remapped CPU screen-occlusion
	// path can still reject visible world objects. Keep only that proven bypass
	// on by default; the object and cluster frustum bypasses remain opt-in so the
	// PR's performance recovery is preserved. An off marker exists solely for a
	// bounded diagnostic rollback. All marker files are read once at startup.
	enum VisibilityBypass { kVisOcclusion, kVisObject, kVisCluster };
	static bool VisibilityBypassWanted(VisibilityBypass which)
	{
		static int wanted[3] = { -1, -1, -1 };
		if (wanted[0] < 0) {
			wanted[kVisOcclusion] = StereoSinglePass::FlagFilePresent(L"vr_vis_occlusion_bypass_off.txt") ? 0 : 1;
			wanted[kVisObject] = StereoSinglePass::FlagFilePresent(L"vr_vis_object_bypass_on.txt") ? 1 : 0;
			wanted[kVisCluster] = StereoSinglePass::FlagFilePresent(L"vr_vis_cluster_bypass_on.txt") ? 1 : 0;
		}
		return wanted[which] != 0;
	}

	void ForceWideCullingFov()
	{
		ApplyLightCullingFov();
		// These patches are the broad expanded-visibility mode. They modify live
		// executable code, so VRMenu captures the saved choice at process startup;
		// never install or restore them in response to a mid-frame menu change.
		// This keeps the CPU screen-occlusion, object-frustum, cluster-frustum and
		// native world-list coverage paths in one coherent launch-time state.
		if (!VRMenu::ExpandedVisibilityActive())
			return;
		if (VisibilityBypassWanted(kVisOcclusion))
			InstallCpuScreenOcclusionBypass();
		if (VisibilityBypassWanted(kVisObject))
			InstallObjectFrustumBypass();
		if (VisibilityBypassWanted(kVisCluster))
			InstallClusterFrustumBypass();
		return;
		// Metro 2033 Redux Steam keeps the world camera's vertical FOV here.
		// The user.cfg cvar is clamped to 90 degrees: even with
		// `r_base_fov 220`, the live value and the engine projection remain
		// exactly 90 (P11=1).  That projection is built before 3Dmigoto sees
		// any D3D constant buffer and is also what CPU frustum/portal traversal
		// uses to decide which sectors and objects to submit.  Replacing m_P at
		// Map() time is therefore too late to recover rejected draws.
		//
		// Write a deliberately wider visibility FOV before Metro records the
		// next frame. HackerContext still replaces
		// the resulting GPU projection with the headset's real per-eye frustum,
		// so this changes visibility only, not scale or headset optics. Combined
		// with UpdateCullingFollow it also leaves enough overlap if portal
		// traversal follows a body-facing camera instead of the published render
		// camera -- the left-edge-only symptom seen in the station room.
		static const DWORD_PTR kWorldFovRva = 0xCE710C;
		// Live value storage embedded in the r_lod_discard console-variable
		// record. The record starts at RVA 0xCF47D8 and its value pointer resolves
		// to RVA 0xCF47F8 (the same embedded float). Its registered range is
		// 0..16 and the stock value is 4.
		static const DWORD_PTR kLodDiscardValueRva = 0xCF47F8;
		static const float kLodDiscardOverride = 0.0f;
		// 110 vertical is about 137 horizontal at Metro's 16:9 aspect. That
		// safely contains the ~109 degree headset union plus camera-follow
		// residual, without approaching the near-singular 170 degree projection
		// that made portal clipping and centre detail less stable in testing.
		static const float kCullingVerticalFovDeg = 110.0f;
		static bool addressValidated = false;
		static bool rejected = false;
		static bool lodDiscardValidated = false;
		static bool lodDiscardRejected = false;
		static unsigned lastLoggedFrame = 0;

		if (rejected)
			return;
		HMODULE base = GetModuleHandleA(NULL);
		if (!base)
			return;

		float *worldFov = (float *)((BYTE *)base + kWorldFovRva);
		float current = 0.0f;
		SIZE_T got = 0;
		if (!ReadProcessMemory(GetCurrentProcess(), worldFov, &current,
				sizeof(current), &got) || got != sizeof(current) || !isfinite(current)) {
			rejected = true;
			LogInfo("VRPose visibility FOV: Steam world-FOV RVA is unreadable; patch disabled\n");
			return;
		}

		// Validate the executable before the first write.  A normal camera or
		// zoom value is in this range; random data at this RVA in another build
		// almost certainly is not. Once our own forced value is present it remains
		// valid on subsequent frames.
		if (!addressValidated) {
			if (current < 10.0f || current > 120.0f) {
				rejected = true;
				LogInfo("VRPose visibility FOV: unexpected value %.3f at RVA 0x%llX; patch disabled\n",
					current, (unsigned long long)kWorldFovRva);
				return;
			}
			addressValidated = true;
			LogInfo("VRPose visibility FOV: validated Steam world FOV %.3f at RVA 0x%llX\n",
				current, (unsigned long long)kWorldFovRva);
		}

		if (fabsf(current - kCullingVerticalFovDeg) > 0.001f) {
			SIZE_T wrote = 0;
			if (!WriteProcessMemory(GetCurrentProcess(), worldFov,
					&kCullingVerticalFovDeg, sizeof(kCullingVerticalFovDeg), &wrote)
					|| wrote != sizeof(kCullingVerticalFovDeg)) {
				rejected = true;
				LogInfo("VRPose visibility FOV: write failed; patch disabled\n");
				return;
			}
		}

		// Disable the final whole-model screen-area discard threshold. This exact
		// 110-degree/zero combination produced the smallest reproducible
		// one-sided visibility boundary in the station-room tests.
		if (!lodDiscardRejected) {
			float *lodDiscard = (float *)((BYTE *)base + kLodDiscardValueRva);
			float discardValue = 0.0f;
			SIZE_T discardGot = 0;
			if (!ReadProcessMemory(GetCurrentProcess(), lodDiscard, &discardValue,
					sizeof(discardValue), &discardGot) || discardGot != sizeof(discardValue)
					|| !isfinite(discardValue)) {
				lodDiscardRejected = true;
				LogInfo("VRPose visibility LOD: r_lod_discard storage unreadable; compensation disabled\n");
			} else {
				if (!lodDiscardValidated) {
					if (discardValue < 0.0f || discardValue > 16.0f) {
						lodDiscardRejected = true;
						LogInfo("VRPose visibility LOD: unexpected r_lod_discard %.3f at RVA 0x%llX; "
							"compensation disabled\n", discardValue,
							(unsigned long long)kLodDiscardValueRva);
					} else {
						lodDiscardValidated = true;
						LogInfo("VRPose visibility LOD: validated r_lod_discard %.3f at RVA 0x%llX\n",
							discardValue, (unsigned long long)kLodDiscardValueRva);
					}
				}
				if (lodDiscardValidated && discardValue != kLodDiscardOverride) {
					SIZE_T discardWrote = 0;
					if (!WriteProcessMemory(GetCurrentProcess(), lodDiscard, &kLodDiscardOverride,
							sizeof(kLodDiscardOverride), &discardWrote)
							|| discardWrote != sizeof(kLodDiscardOverride)) {
						lodDiscardRejected = true;
						LogInfo("VRPose visibility LOD: r_lod_discard write failed\n");
					}
				}
			}
		}

		if (lastLoggedFrame == 0 || G->frame_no - lastLoggedFrame >= 600) {
			lastLoggedFrame = G->frame_no;
			LogInfo("VRPose visibility FOV: forcing %.1f deg vertical for CPU/portal culling; "
				"r_lod_discard=%s\n", kCullingVerticalFovDeg,
				lodDiscardValidated && !lodDiscardRejected ? "0" : "unchanged");
			LogInfo("VRPose object visibility: frustum_bypass=%s\n",
				InterlockedCompareExchange(&sObjectFrustumBypassActive, 0, 0)
					? "active" : "inactive");
			LogInfo("VRPose cluster visibility: frustum_bypass=%s\n",
				InterlockedCompareExchange(&sClusterFrustumBypassActive, 0, 0)
					? "active" : "inactive");
		}
	}

	void UpdateCullingFollow()
	{
		static const bool kCullFollowEnabled = true;
		if (!kCullFollowEnabled || !sVRSystem || !G->vrHeadRotationValid)
			return;

		// Advance the history once per frame, whether or not anything is
		// injected this frame - the delay has to be in frames, not in
		// injections, or a pause mid-turn would skew it.
		static unsigned lastFrame = 0xFFFFFFFF;
		if (lastFrame != G->frame_no) {
			lastFrame = G->frame_no;
			sCullHistoryPos = (sCullHistoryPos + 1) % kCullHistory;
			sCullInjectedHistory[sCullHistoryPos] = sCullInjectedYaw;
			sCullInjectedPitchHistory[sCullHistoryPos] = sCullInjectedPitch;
			sPitchCommandHistoryPos =
				(sPitchCommandHistoryPos + 1) % kPitchCommandHistory;
			sPitchCommandHistory[sPitchCommandHistoryPos] = 0.0f;
		}

		// Yaw of the head delta. It is a camera-to-world style rotation, so
		// its forward is column 2.
		const float *d = G->vrHeadRotation3x3;
		// The station-room visibility sector becomes complete only after the
		// player turns right. Aim Metro's engine/culling camera slightly right
		// of the headset to move that hidden acceptance band toward the centre.
		// The injected amount is still booked in sCullInjectedYaw and canceled
		// from rendering, so this must not rotate the visible headset camera.
		// A generic world-UI draw is ambiguous by itself, but the startup menu
		// disappearing immediately after the player confirms Continue/New Game
		// identifies the loading transition. Require several consecutive frames
		// without it, then latch gameplay permanently. The unique ammo marker is
		// retained as a fallback for unusual menu/control paths.
		static unsigned noWorldUIFrames = 0;
		static bool startupHeadReferenceReset = false;
		if (!InterlockedCompareExchange(&sGameplayModeActive, 0, 0)
			&& InterlockedCompareExchange(&sStartupExitArmed, 0, 0)) {
			const LONG lastWorldUI =
				InterlockedCompareExchange(&sLastWorldPlacedUIFrame, 0, 0);
			const unsigned age = lastWorldUI >= 0
				? G->frame_no - (unsigned)lastWorldUI : 0xFFFFFFFFu;
			if (age > 1) {
				// OpenVR initializes before the player has finished menus/loading
				// and often before the headset is resting naturally on their head.
				// A reference captured there can therefore make the first visible
				// gameplay frame tilted. Re-capture ONCE during the startup exit,
				// while the UI has disappeared and before gameplay is latched. This
				// is a fixed session baseline, not a live horizon correction.
				if (!startupHeadReferenceReset) {
					startupHeadReferenceReset = true;
					G->vrReferenceRotationCaptured = false;
					sReferenceTiltInverseValid = false;
					sHeadReferencePosValid = false;
					LogInfo("VRPose: startup UI exited - recapturing level HMD reference before gameplay\n");
				}
				if (++noWorldUIFrames >= 30) {
					InterlockedExchange(&sGameplayModeActive, 1);
					LogInfo("VRPose cull: gameplay latched after startup-menu transition\n");
				}
			} else {
				noWorldUIFrames = 0;
			}
		}
		const bool gameplayStarted =
			InterlockedCompareExchange(&sGameplayModeActive, 0, 0) != 0;
		// A reload selected from Metro's pause menu dismisses the menu without
		// another Start/B edge, leaving sPauseMenuExpected stuck true.  The ammo
		// HUD is optional and did not appear in the failing checkpoint, so it
		// cannot be the only recovery signal.  The dedicated loading-panel shader
		// is emitted during both checkpoint and save loads.  Arm while that panel
		// is present, keep input suppressed throughout the load, then clear the
		// stale pause latch once the panel has been absent for a few frames.
		static bool pauseReloadArmed = false;
		const LONG loadingFrame =
			InterlockedCompareExchange(&sLoadingScreenFrame, -1000, -1000);
		const unsigned loadingAge = loadingFrame >= 0
			? G->frame_no - (unsigned)loadingFrame : 0xFFFFFFFFu;
		// A chapter/save load can reuse this Metro process. Reset the local
		// scripted-camera classifier once per loading transition so the previous
		// scene's pitch history or input-yield state cannot leak into the new
		// scene. This only clears our bookkeeping; it does not alter the engine
		// camera, culling, or viewmodel routing.
		static bool scriptedLoadResetArmed = false;
		if (loadingAge <= 1 && !scriptedLoadResetArmed) {
			scriptedLoadResetArmed = true;
			sScriptedCamActive = false;
			sScriptedCamOnset = 0;
			sScriptedCamQuiet = 0;
			sScriptedPitchOffset = 0.0f;
			sScriptedBlend = 0.0f;
			sScriptedReleaseEaseActive = false;
			sScriptedReleaseStartFrame = 0;
			sScriptedReleasePitchOffset = 0.0f;
			sScriptedSustain = 0;
			sScriptedSustained = false;
			sPrevHeadPitchGateValid = false;
			sPrevEnginePitchValid = false;
			sPrevHeadPitchValid = false;
			sInjectionYield = false;
			sFightFrames = 0;
			sYieldProbeWait = 0;
			sYieldProbing = false;
			sPitchDeadFrames = 0;
			sPitchSuppressLogged = false;
			sLastCommandedPitch = 0.0f;
			for (int i = 0; i < kPitchCommandHistory; ++i)
				sPitchCommandHistory[i] = 0.0f;
			LogInfo("VRPose script: reset classifier at chapter/loading transition\n");
		} else if (scriptedLoadResetArmed && loadingAge > 4) {
			scriptedLoadResetArmed = false;
		}
		if (InterlockedCompareExchange(&sPauseMenuExpected, 0, 0)) {
			if (loadingAge <= 1 && !pauseReloadArmed) {
				pauseReloadArmed = true;
				LogInfo("VRPose input: loading screen seen with pause latch - recovery armed\n");
			}
			else if (pauseReloadArmed && loadingAge > 4) {
				pauseReloadArmed = false;
				InterlockedExchange(&sPauseMenuExpected, 0);
				LogInfo("VRPose input: loading screen exited - cleared stale pause-menu state\n");
			}
		} else {
			pauseReloadArmed = false;
		}
		// Never synthesize mouse movement while Metro is showing the startup
		// or pause/menu UI. Head pose should not move the menu cursor; menu
		// navigation is handled by the controller bindings instead.
		const bool pauseMenuExpected =
			InterlockedCompareExchange(&sPauseMenuExpected, 0, 0) != 0;
		if (!gameplayStarted || pauseMenuExpected)
			return;
		// Self-driven input-device search; also sets the phase flags the two
		// injection sites below honour.
		InputDeviceHuntTick();

		const float activeCullingBias = gameplayStarted
			? kCullingYawBiasRadians : 0.0f;
		float headYaw = atan2f(d[2], d[8]) + activeCullingBias;
		while (headYaw > 3.14159265f) headYaw -= 6.28318531f;
		while (headYaw < -3.14159265f) headYaw += 6.28318531f;
		if (!isfinite(headYaw))
			return;

		// Pitch too, not just yaw.
		//
		// This was yaw-only on the assumption that neck range and a forgiving
		// vertical FOV would hide the divergence. They do not: looking up or
		// down leaves the engine's camera where it was, so geometry above and
		// below gets culled and props vanish - very noticeable, while left and
		// right stayed clean precisely because the camera followed there.
		//
		// Held short of the engine's own pitch limit on purpose. The
		// cancellation books what we believe the engine applied; if the engine
		// refuses the last few degrees at its clamp, that belief goes wrong and
		// the error is permanent - a view offset that never recovers. Staying
		// inside the clamp means it never has to be discovered where it is.
		const float clampedFwdY = d[5] < -1.0f ? -1.0f : (d[5] > 1.0f ? 1.0f : d[5]);
		const float kMaxFollowPitch = 1.15f;   // ~66 degrees
		float headPitch = asinf(clampedFwdY);
		if (!isfinite(headPitch))
			return;
		if (headPitch > kMaxFollowPitch) headPitch = kMaxFollowPitch;
		if (headPitch < -kMaxFollowPitch) headPitch = -kMaxFollowPitch;

		// The retired basis mode has already put this tracked yaw/pitch into Metro's upstream
		// camera basis. Publish exact, non-delayed cancellation for the rendered
		// path and leave the mouse/stick channels completely quiet.
		if (!kNativeStateFollowEnabled &&
			InterlockedCompareExchange(&sUpstreamCameraBasisProbeActive, 0, 0) != 0 &&
			InterlockedCompareExchange(&sUpstreamCameraYieldingToScript, 0, 0) == 0) {
			PublishUpstreamDirectCancellation(headYaw, headPitch);
			sLastCommandedPitch = 0.0f;
			sPitchCommandHistory[sPitchCommandHistoryPos] = 0.0f;
			InterlockedExchange(&sPadFollowRX, 0);
			InterlockedExchange(&sPadFollowRY, 0);
			return;
		}

		float err = headYaw - sCullInjectedYaw;
		while (err > 3.14159265f) err -= 6.28318531f;
		while (err < -3.14159265f) err += 6.28318531f;

		// CLOSE THE PITCH LOOP ON THE ENGINE, NOT ON OUR OWN BELIEF.
		//
		// errPitch was headPitch - sCullInjectedPitch: the error between the
		// head and an accumulator WE increment by what we assume the engine
		// applied. The loop therefore converges on its own bookkeeping and
		// reports success while the engine's camera sits somewhere else. Any
		// time the engine does not move as commanded - its pitch clamp, a
		// scripted camera, a sensitivity that is not exactly kPitchResponse -
		// the belief and the camera part company permanently, because nothing
		// in the loop ever compares them.
		//
		// The cost was not theoretical. The engine's camera ended up ~16 deg
		// above the player's line of sight, which put the culling frustum, the
		// interaction ray and the weapon transform all up there with it:
		// geometry popping in along the bottom of the view, having to crouch to
		// use things, and a gun that sits too high.
		//
		// ReadCameraVec gives us the camera's real forward vector every frame,
		// so measure it. Two consequences, both wanted:
		//
		//   - the error becomes a TRUE error, so the loop drives the engine's
		//     camera to the head and self-corrects after anything moves it,
		//     including a scripted event.
		//   - the cancellation becomes exact. The body cannot pitch (stick look
		//     is disabled), so by definition ALL of the engine camera's pitch is
		//     ours to remove - there is no booking error left to accumulate.
		float engineCamFwd[3];
		if (ReadCameraVec(0x170, engineCamFwd)) {
			const float ec = engineCamFwd[1] < -1.0f ? -1.0f
				: (engineCamFwd[1] > 1.0f ? 1.0f : engineCamFwd[1]);
			const float enginePitch = asinf(ec);
			if (isfinite(enginePitch)) {
				sCullMeasuredYaw = atan2f(engineCamFwd[0], engineCamFwd[2]);
				sCullMeasuredYawValid = true;
				sCullMeasuredPitch = enginePitch;
				sCullMeasuredPitchValid = true;
				sCullInjectedPitch = enginePitch;
				sCullInjectedPitchHistory[sCullHistoryPos] = enginePitch;

				// Who moved the camera since last frame - us, or something else?
				//
				// The first version required our own command to be QUIET before
				// it would trust the reading. That could never fire: a script
				// pulls the camera away from the head, the loop sees a large
				// error and starts commanding on the very next frame, so the
				// command was never quiet for the three frames the latch wanted.
				// The ladder climb at the start of the game went undetected for
				// exactly that reason.
				//
				// Compare against the PREDICTED movement instead, with a
				// tolerance proportional to the size of the command, because
				// kPitchResponse is only approximate and a big command carries a
				// correspondingly big prediction error. One test then covers
				// both signatures of external control, and needs no quiet
				// window: the camera moving when we asked for nothing, AND the
				// camera ignoring what we asked - which is what a cutscene
				// holding the input actually looks like.
				const float moved = sPrevEnginePitchValid
					? (enginePitch - sPrevEnginePitch) : 0.0f;
				sPitchLastMoved = moved;   // anti-windup: did our input land?
				const float external = moved - sLastCommandedPitch;
				const float kExternalRate = 0.0087f;   // ~0.5 deg/frame
				const float kResponseTolerance = 0.35f;
				const float allowedSlip = kExternalRate
					+ kResponseTolerance * fabsf(sLastCommandedPitch);
				const int kOnsetFrames = 2;
				// Back to 20. This was raised to 50 on the theory that pauses
				// inside a script were causing release/re-latch flapping; it did
				// not help the hitching and it visibly delayed re-levelling
				// afterwards. The stall detector covers the held-camera case
				// properly, so this window can stay short and the view can snap
				// back to level promptly when a script really does end.
				const int kReleaseFrames = 20;

				// Exact camera/vendor owners remain authoritative. Player performance
				// joins them only when Metro's own native look gate is closed; `(1,1)`
				// alone persists during ordinary control in some towns.
				const bool nativeScriptedCamera = NativeAuthoredCameraOwnerActive()
					|| ScriptedPlayerCameraOwnerActive();
				if (nativeScriptedCamera) {
					sScriptedCamOnset = 0;
					sScriptedCamQuiet = 0;
					if (!sScriptedCamActive) {
						sScriptedCamActive = true;
						sScriptedBlend = 1.0f;
						sScriptedPitchOffset = 0.0f;
						sScriptedReleaseEaseActive = false;
						sPitchDeadFrames = 0;
						sPitchSuppressLogged = false;
						LogInfo("VRPose script: native camera/player ownership acquired "
							"engine=%+.2f head=%+.2f deg\n",
							enginePitch * 57.29578f, headPitch * 57.29578f);
					}
				} else {
					sScriptedCamOnset = 0;
					if (sScriptedCamActive && ++sScriptedCamQuiet >= 3) {
						sScriptedReleasePitchOffset = enginePitch - headPitch;
						sScriptedReleaseStartFrame = G->frame_no;
						sScriptedReleaseEaseActive =
							fabsf(sScriptedReleasePitchOffset) >= 0.017f;
						sScriptedCamActive = false;
						sScriptedSustained = false;
						sScriptedSustain = 0;
						sPitchDeadFrames = 0;
						sPitchSuppressLogged = false;
						LogInfo("VRPose script: native camera/player ownership released "
							"after %+.2f deg - easing and re-converging\n",
							sScriptedReleasePitchOffset * 57.29578f);
					}
				}

				// THE OFFSET IS MEASURED, NEVER ACCUMULATED.
				//
				// This used to sum the script's contribution frame by frame and
				// then fade it on a fixed timer. That gave the view and the
				// camera two independent timelines: the loop hauled the camera
				// back in a handful of frames while the view faded over
				// twenty-five, and the gap between them is what made the gun
				// bounce into place afterwards.
				//
				// Taking the offset as the LIVE difference between the engine's
				// camera and the head deletes the second timeline. It is exactly
				// the script's contribution for as long as the script holds the
				// camera, and it falls to zero on its own as the loop
				// re-converges - so the view arrives with the camera by
				// construction, instead of by two rates happening to agree.
				// Passive - drives the flag hunt only, changes no behaviour.
				ScriptFlagHuntTick(fabsf(headPitch - enginePitch));

				sScriptedPitchOffset = enginePitch - headPitch;

					// Big AND sustained - see the gate's note at its declaration.
					{
						// HOW LONG TO WAIT SCALES WITH HOW VIOLENT IT IS.
						//
						// A flat one-second wait is right for a marginal
						// divergence and far too slow for a dramatic one. When a
						// creature pins the player, the camera has to snap to it
						// immediately or the player cannot fight it off - half a
						// second late is already too late.
						//
						// The two cases are separable by magnitude, which is the
						// one thing the old logs did show cleanly: false positives
						// during ordinary play sat at +4 to +6 degrees, while real
						// scripted cameras reached +14 to +72. A swing past 30
						// degrees is nothing recoil or follow lag produces, so it
						// does not need the long guard - only the marginal band
						// does.
						const float kScriptPitchMin = 0.21f;    // ~12 degrees
						const float kScriptPitchFast = 0.52f;   // ~30 degrees
						const float kScriptPitchDrop = 0.07f;   // ~4 degrees
						const int kScriptSustainFrames = 72;    // ~1 s at 72 Hz
						const int kScriptSustainFast = 6;       // ~85 ms
						const LONG recoilUntil = InterlockedCompareExchange(
							&sRecoilPitchIgnoreUntilFrame, 0, 0);
						const bool recoilWindow = recoilUntil >= 0
							&& (LONG)G->frame_no <= recoilUntil;
						const float mag = fabsf(sScriptedPitchOffset);
						// THE HEAD MUST BE STILL FOR DIVERGENCE TO MEAN ANYTHING.
						//
						// This gate triggered on |enginePitch - headPitch| alone, but that
						// quantity IS the follow error. Measured over one ordinary session:
						// mean gap 21 deg, max 108. So the 30-deg fast tier - added so a
						// creature grab snaps the camera immediately - cleared on any
						// moderate head movement. The view snapped onto the lagging engine
						// camera and the world dragged with the head until the loop caught
						// up and it released. Measured effect on the rendered view:
						// gamePitch averaging 15 deg during ordinary play, peaking at 107.
						//
						// The follow converges within a frame or two once the head stops, so
						// a large divergence WITH A STILL HEAD is what actually implies
						// somebody else holds the camera. A creature grab still qualifies:
						// the camera is yanked while the head is not the thing moving.
						const float headMovedGate = sPrevHeadPitchGateValid
							? fabsf(headPitch - sPrevHeadPitchGate) : 0.0f;
						sPrevHeadPitchGate = headPitch;
						sPrevHeadPitchGateValid = true;
						const bool gateHeadStill = headMovedGate < 0.004f;   // ~0.23 deg/frame
						if (!recoilWindow && gateHeadStill && mag > kScriptPitchMin) {
							if (sScriptedSustain < kScriptSustainFrames)
								sScriptedSustain++;
						} else if (mag < kScriptPitchDrop) {
							sScriptedSustain = 0;
						}
						const int required = (mag > kScriptPitchFast)
							? kScriptSustainFast : kScriptSustainFrames;
						const bool nowSustained = sScriptedSustain >= required;
						if (nowSustained != sScriptedSustained) {
							sScriptedSustained = nowSustained;
							LogInfo("VRPose script: pass-through %s "
								"(offset %+.2f deg, frame %u)\n",
								nowSustained ? "ENGAGED" : "released",
								sScriptedPitchOffset * 57.29578f, G->frame_no);
						}
					}
				if (sScriptedCamActive) {
					sScriptedBlend = 1.0f;
				} else if (sScriptedBlend > 0.0f) {
					// Backstop only. If the loop cannot converge - the engine at
					// its pitch clamp, or something still holding the camera -
					// the view must not stay pitched indefinitely.
					if (fabsf(sScriptedPitchOffset) < 0.017f)   // ~1 degree
						sScriptedBlend = 0.0f;
					else
						sScriptedBlend -= 0.006f;   // ~2.5 s backstop
					if (sScriptedBlend < 0.0f)
						sScriptedBlend = 0.0f;
				}

				sPrevEnginePitch = enginePitch;
				sPrevEnginePitchValid = true;

			}
		}
		// Preserve measurement, credits and authored-camera handoff above.
		// Native angular updates are the only HMD follow delivery, even if
		// installation fails: no synthetic mouse or virtual-stick fallback.
		sLastCommandedPitch = 0.0f;
		sPitchCommandHistory[sPitchCommandHistoryPos] = 0.0f;
		InterlockedExchange(&sPadFollowRX, 0);
		InterlockedExchange(&sPadFollowRY, 0);
	}

	// -----------------------------------------------------------------
	// Weapon pose from the controller.
	//
	// The viewmodel's instance data is a 3x4 affine LOCAL-TO-VIEW transform,
	// which is a lucky fit: "relative to the eye" is exactly the space the
	// controller's pose relative to the headset lives in, so this is close to
	// a direct mapping rather than a derivation.
	//
	// Returns the controller's rotation relative to the HMD, in game view
	// space, measured from an anchor - so whatever pose the controller was
	// held in when tracking settled counts as the weapon's default, and no
	// assumption is needed about how the player holds it.
	static float sWeaponAnchorRot[9] = { 1,0,0, 0,1,0, 0,0,1 };
	static bool sTwoHandActive = false;
	static float sTwoHandBaseDirLocal[3] = { 0,0,1 };

	// How far the controller has moved from its rest position, in game view
	// space - the other half of 6DOF.
	//
	// Same idea as the rotation: the viewmodel transform is local-to-VIEW, so
	// "the controller's offset from the headset" maps almost directly onto
	// it. Taken relative to an anchor so the rest position is wherever the
	// controller happened to be when tracking settled.
	//
	// OpenVR is metres and the game's units are close enough to metres that
	// the world scale already looked right in the headset, so the only
	// conversion needed is the handedness flip on Z.
	static float sWeaponAnchorPos[3] = { 0, 0, 0 };
	static bool sLeftHandAnchored = false;
	static bool sLeftHandPositionAnchored = false;
	static int sLeftHandSettle = 0;
	static float sLeftHandAnchorRot[9] = { 1,0,0, 0,1,0, 0,0,1 };
	static float sLeftHandAnchorPos[3] = { 0, 0, 0 };
	static float sRightHandAnchorRot[9] = { 1,0,0, 0,1,0, 0,0,1 };
	static bool sRightHandAnchored = false;
	static int sRightHandSettle = 0;
	static volatile LONG sControllerRecenterGeneration = 0;

	unsigned GetControllerRecenterGeneration()
	{
		return static_cast<unsigned>(InterlockedCompareExchange(
			&sControllerRecenterGeneration, 0, 0));
	}

	// The head position the weapon is measured against - see
	// ComputeWeaponTranslationDelta. Cleared by RecenterAiming so recentring
	// re-captures it wherever the player is standing.
	static float sHeadRefPos[3] = { 0, 0, 0 };
	static bool sHeadRefValid = false;

	// The controller's position relative to the head with NO anchor removed -
	// game view axes, game units. Refreshed once per frame alongside the deltas.
	// Declared here rather than beside its accessors because it is written from
	// ComputeWeaponTranslationDelta below, and a static declared after its first
	// use does not compile.
	static float sCtrlRelHead[3] = { 0, 0, 0 };
	static bool sCtrlRelHeadValid = false;

	// Both weapon deltas are computed ONCE per frame and cached.
	//
	// They were being recomputed per weapon draw, and each one calls
	// GetDeviceToAbsoluteTrackingPose - which fills poses for all 64 tracked
	// devices. At up to 120 weapon draws a frame that is ~240 full pose
	// queries per frame, which is both expensive and erratic: the cost scales
	// with how many weapon draws a scene happens to have, which is exactly
	// the "framerate jumping between the mid 40s and high 60s for no obvious
	// reason" that showed up in the underground town.
	//
	// Hammering the API mid-frame is also a plausible source of the rotation
	// silently coming back as identity while translation kept working.
	static unsigned sWeaponDeltaFrame = 0xFFFFFFFF;
	static float sCachedRot[9] = { 1,0,0, 0,1,0, 0,0,1 };
	static float sCachedTrans[3] = { 0, 0, 0 };
	static bool sCachedRotValid = false;
	static bool sCachedTransValid = false;

	bool ComputeWeaponRotationDelta(float out[9]);
	bool ComputeWeaponTranslationDelta(float out[3]);

	static void EnsureWeaponDeltasForFrame()
	{
		if (sWeaponDeltaFrame == G->frame_no)
			return;
		sWeaponDeltaFrame = G->frame_no;
		sCachedRotValid = ComputeWeaponRotationDelta(sCachedRot);
		sCachedTransValid = ComputeWeaponTranslationDelta(sCachedTrans);

		// Translation works and rotation does not, from the same pose data
		// through the same shader - so exactly one side is at fault, and
		// guessing which has already cost several runs. This says outright
		// whether the CPU is producing a rotation at all.
		static unsigned n = 0;
		if (false && (n++ % 120) == 0) {   // answered: rotation was always live
			const float offDiag = fabsf(sCachedRot[1]) + fabsf(sCachedRot[2])
				+ fabsf(sCachedRot[3]) + fabsf(sCachedRot[5])
				+ fabsf(sCachedRot[6]) + fabsf(sCachedRot[7]);
			LogInfo("VRPose weapon: rot valid=%d offDiag=%.4f [%.3f %.3f %.3f / %.3f %.3f %.3f / %.3f %.3f %.3f]\n",
				sCachedRotValid ? 1 : 0, offDiag,
				sCachedRot[0], sCachedRot[1], sCachedRot[2],
				sCachedRot[3], sCachedRot[4], sCachedRot[5],
				sCachedRot[6], sCachedRot[7], sCachedRot[8]);
			LogInfo("VRPose weapon: trans valid=%d (%.3f %.3f %.3f)\n",
				sCachedTransValid ? 1 : 0,
				sCachedTrans[0], sCachedTrans[1], sCachedTrans[2]);
		}
	}

	bool GetWeaponRotationDelta(float out[9])
	{
		EnsureWeaponDeltasForFrame();
		if (!sCachedRotValid)
			return false;
		memcpy(out, sCachedRot, sizeof(float) * 9);
		// The current weapon's persistent pitch/yaw/roll calibration. The same
		// adjusted D feeds viewmodel placement, reticle direction,
		// and redirected shots, so rotating the model cannot break iron-sight aim.
		const float pitch = sWeaponRotAdjust[0];
		const float yaw = sWeaponRotAdjust[1];
		const float roll = sWeaponRollAdjust;
		if (pitch != 0.0f || yaw != 0.0f || roll != 0.0f) {
			const float cp = cosf(pitch);
			const float sp = sinf(pitch);
			const float cy = cosf(yaw);
			const float sy = sinf(yaw);
			const float yawPitch[9] = {
				cy, sy * sp, sy * cp,
				0.0f, cp, -sp,
				-sy, cy * sp, cy * cp,
			};
			const float cr = cosf(roll);
			const float sr = sinf(roll);
			const float rollMatrix[9] = {
				cr, -sr, 0.0f,
				sr, cr, 0.0f,
				0.0f, 0.0f, 1.0f,
			};
			float trim[9];
			Multiply3x3(rollMatrix, yawPitch, trim);
			float adjusted[9];
			Multiply3x3(trim, out, adjusted);
			memcpy(out, adjusted, sizeof(adjusted));
		}
		return true;
	}

	bool GetWeaponTranslationDelta(float out[3])
	{
		EnsureWeaponDeltasForFrame();
		if (!sCachedTransValid)
			return false;
		out[0] = sCachedTrans[0];
		out[1] = sCachedTrans[1];
		out[2] = sCachedTrans[2];
		return true;
	}

	// Where the controller IS, not how far it has moved - see sCtrlRelHead.
	// Only meaningful for the calibration measurement; the weapon transform
	// itself deliberately works in deltas.
	bool GetControllerRelativeToHead(float out[3])
	{
		EnsureWeaponDeltasForFrame();
		if (!sCtrlRelHeadValid)
			return false;
		out[0] = sCtrlRelHead[0];
		out[1] = sCtrlRelHead[1];
		out[2] = sCtrlRelHead[2];
		return true;
	}

	bool GetControllerRotationRelativeToHead(float out[9])
	{
		if (!out || !sVRSystem || !sLastPolledPoseValid)
			return false;

		const vr::TrackedDeviceIndex_t idx =
			sVRSystem->GetTrackedDeviceIndexForControllerRole(WeaponControllerRole());
		if (idx == vr::k_unTrackedDeviceIndexInvalid)
			return false;

		vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount];
		GetTrackedPosesForFrame(poses);
		if (!poses[idx].bDeviceIsConnected || !poses[idx].bPoseIsValid)
			return false;

		float ctrlR[9], hmdR[9], hmdT[9], rel[9];
		ExtractRotation3x3(poses[idx].mDeviceToAbsoluteTracking, ctrlR);
		ExtractRotation3x3(sLastPolledPose, hmdR);
		Transpose3x3(hmdR, hmdT);
		Multiply3x3(hmdT, ctrlR, rel);
		ConvertOpenVRRotationToGameSpace(rel, out);
		return true;
	}

	bool GetWeaponRestAnchor(float out[3])
	{
		if (!sWeaponPosAnchored)
			return false;
		out[0] = sWeaponAnchorPos[0];
		out[1] = sWeaponAnchorPos[1];
		out[2] = sWeaponAnchorPos[2];
		return true;
	}

	bool ComputeWeaponTranslationDelta(float out[3])
	{
		static const bool kWeaponTranslationDiagnosticsEnabled = false;
		if (VRMenu::GetSettings().gamepadMode)
			return false;
		// Invalidate first: every early return below means we have no fresh
		// controller position this frame, and a stale one would read as a
		// perfectly good measurement.
		sCtrlRelHeadValid = false;

		if (!sVRSystem || !sLastPolledPoseValid)
			return false;

		vr::TrackedDeviceIndex_t idx =
			sVRSystem->GetTrackedDeviceIndexForControllerRole(WeaponControllerRole());
		if (idx == vr::k_unTrackedDeviceIndexInvalid)
			return false;

		vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount];
		GetTrackedPosesForFrame(poses);
		// WHY DOES THE WEAPON STOP?
		//
		// Nothing in this path clamps anything - it is a straight pass-through
		// of the runtime's pose - yet the weapon stops following the hand a
		// short way in. That leaves the pose itself: either it goes invalid, or
		// it stays "valid" while the tracker has lost sight of the controller
		// and is holding or extrapolating a stale position.
		//
		// Inside-out tracking is the obvious suspect: bringing a controller in
		// toward the face puts it in the headset cameras' blind spot, and the
		// distance at which that happens is short, which matches the symptom.
		// eTrackingResult distinguishes them - Running_OK (200) means tracked,
		// Running_OutOfRange (202) means the runtime knows it has lost it.
		{
			static int lastState = -99;
			const int state = poses[idx].bPoseIsValid
				? (int)poses[idx].eTrackingResult : -1;
			if (state != lastState) {
				lastState = state;
				LogInfo("VRPose track: right controller changed - valid=%d connected=%d "
					"trackingResult=%d (200=OK, 202=OutOfRange)\n",
					poses[idx].bPoseIsValid ? 1 : 0,
					poses[idx].bDeviceIsConnected ? 1 : 0,
					(int)poses[idx].eTrackingResult);
			}
		}

		// The RAW pose, before anything of ours touches it. If this stops
		// changing while the hand keeps moving, the fault is upstream of this
		// entire project and no code change here can fix it.
		if (kWeaponTranslationDiagnosticsEnabled) {
			// Raw and derived on ONE line, at a rate fast enough to
			// differentiate. What matters is the RATIO: how many centimetres
			// reach the weapon per centimetre the hand actually moves. Logging
			// them separately, at different rates, made that impossible to
			// compute - which is why three attempts at this have been guesses.
			static unsigned n = 0;
			if ((n++ % 6) == 0)
				LogInfo("VRPose ratio: rawC=(%.4f %.4f %.4f) rawH=(%.4f %.4f %.4f) "
					"href=(%.4f %.4f %.4f)\n",
					poses[idx].mDeviceToAbsoluteTracking.m[0][3],
					poses[idx].mDeviceToAbsoluteTracking.m[1][3],
					poses[idx].mDeviceToAbsoluteTracking.m[2][3],
					sLastPolledPose.m[0][3], sLastPolledPose.m[1][3],
					sLastPolledPose.m[2][3],
					sHeadRefPos[0], sHeadRefPos[1], sHeadRefPos[2]);
		}

		if (!poses[idx].bDeviceIsConnected || !poses[idx].bPoseIsValid)
			return false;

		const vr::HmdMatrix34_t &c = poses[idx].mDeviceToAbsoluteTracking;
		const vr::HmdMatrix34_t &h = sLastPolledPose;

		// Measure against a FIXED head position, not the live one.
		//
		// The rendered view does not respond to head TRANSLATION at all - the
		// camera comes from the engine and we only rotate it - so the world
		// stays put in view space when the player leans. Measuring the
		// controller against the live head made the weapon the one thing that
		// did respond, and it responded backwards: leaning back while pulling
		// the hand in subtracted the head's motion from the hand's.
		//
		// Measured: 61.5 cm of hand travel, 12 cm of head travel, 49 cm reached
		// the weapon. The gun fell 20% short of the hand, only on the axis
		// where the head moves - which is exactly why pulling back felt like it
		// was dragging while left/right and up/down felt correct.
		//
		// Rotation still comes from the LIVE head, because the view does rotate
		// with it: the weapon has to stay put in the world as the player looks
		// around. Only translation is referenced, which is the only part the
		// view ignores.
		// REVERTED to the live head position.
		//
		// A fixed reference did not help, and it is the wrong model: what
		// should place the weapon is where the hand is relative to the EYE,
		// and the eye is the view origin. That is C - H_live. A stale
		// reference also drifts as soon as the player moves at all.
		//
		// The head-motion arithmetic that motivated it was real but not the
		// cause: the player perceives their hand relative to their own eye
		// too, so both sides of the comparison shift together.
		const float d[3] = { c.m[0][3] - h.m[0][3], c.m[1][3] - h.m[1][3],
			c.m[2][3] - h.m[2][3] };
		float rel[3];
		for (int r = 0; r < 3; r++)
			rel[r] = h.m[0][r] * d[0] + h.m[1][r] * d[1] + h.m[2][r] * d[2];

		// OpenVR (+X right, +Y up, -Z forward) -> game view space (+Z forward).
		const float g[3] = { rel[0], rel[1], -rel[2] };

		// Keep the UNANCHORED position too.
		//
		// The delta returned below is measured from wherever the controller
		// happened to be when tracking settled, which is fine for "move the gun
		// as the hand moves" but says nothing about where the hand actually IS.
		// Task 27 is precisely a question about absolute registration - the gun
		// sits above the controller - and that cannot be answered from a delta.
		// This is the same numbers with no anchor subtracted: the controller's
		// position relative to the head, in game view axes and game units.
		const float worldScale = TrackingWorldScale();
		sCtrlRelHead[0] = g[0] * kMetresToGameUnits * worldScale;
		sCtrlRelHead[1] = g[1] * kMetresToGameUnits * worldScale;
		sCtrlRelHead[2] = g[2] * kMetresToGameUnits * worldScale;

		// The derived value, on the same line cadence as the raw one above, so
		// the two can be differenced sample for sample.
		if (kWeaponTranslationDiagnosticsEnabled) {
			static unsigned n = 0;
			if ((n++ % 6) == 0)
				LogInfo("VRPose ratio: derived rel=(%.4f %.4f %.4f)\n", g[0], g[1], g[2]);
		}
		sCtrlRelHeadValid = true;

		if (!sWeaponPosAnchored) {
			if (!sWeaponAnchored)
				return false;   // share the rotation's settle period
			memcpy(sWeaponAnchorPos, g, sizeof(sWeaponAnchorPos));
			sWeaponPosAnchored = true;
			LogInfo("VRPose weapon: rest position anchored at (%.3f %.3f %.3f)\n", g[0], g[1], g[2]);
			return false;
		}

		const float weaponWorldScale = TrackingWorldScale();
		out[0] = (g[0] - sWeaponAnchorPos[0]) * weaponWorldScale;
		out[1] = (g[1] - sWeaponAnchorPos[1]) * weaponWorldScale;
		out[2] = (g[2] - sWeaponAnchorPos[2]) * weaponWorldScale;
		return true;
	}

	bool ComputeWeaponRotationDelta(float out[9])
	{
		if (VRMenu::GetSettings().gamepadMode)
			return false;
		if (!sVRSystem || !sLastPolledPoseValid)
			return false;

		vr::TrackedDeviceIndex_t idx =
			sVRSystem->GetTrackedDeviceIndexForControllerRole(WeaponControllerRole());
		if (idx == vr::k_unTrackedDeviceIndexInvalid)
			return false;

		vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount];
		GetTrackedPosesForFrame(poses);
		if (!poses[idx].bDeviceIsConnected || !poses[idx].bPoseIsValid
			|| poses[idx].eTrackingResult != vr::TrackingResult_Running_OK) {
			if (!sWeaponAnchored)
				sWeaponAnchorSettle = 0;
			return false;
		}

		float ctrlR[9], hmdR[9], hmdT[9], rel[9], relGame[9];
		ExtractRotation3x3(poses[idx].mDeviceToAbsoluteTracking, ctrlR);
		ExtractRotation3x3(sLastPolledPose, hmdR);

		// Controller relative to the head, which is what "relative to the
		// eye" needs.
		Transpose3x3(hmdR, hmdT);
		Multiply3x3(hmdT, ctrlR, rel);

		// Same conjugation the head pose uses. Note diag(-1,-1,1) and
		// diag(1,1,-1) give identical conjugations - the overall sign
		// cancels because S appears twice - so this is the right one despite
		// the direction mapping needing the other form.
		ConvertOpenVRRotationToGameSpace(rel, relGame);

		if (!sWeaponAnchored) {
			if (++sWeaponAnchorSettle < 30)
				return false;

			// Use the same calibrated controller-local barrel axis as
			// GetControllerAim. S=diag(1,1,-1) converts a bare OpenVR
			// direction into the game convention, and relGame is S*rel*S,
			// so relGame*(S*v) gives the barrel direction in view space.
			static const float kControllerAimTilt = 64.6f * 0.0174532925f;
			static const float kBarrelPitchBias = -0.06720f;
			static const float kBarrelYawBias = -0.46304f;
			// Live shot-zeroing bias is deliberately excluded here: it is already
			// applied to the canonical model-space barrel axis downstream. Including
			// it in both places would double the adjustment after a recenter.
			const float tilt = kControllerAimTilt + kBarrelPitchBias;
			const float yawB = kBarrelYawBias;
			const float tc = cosf(tilt), ts = sinf(tilt);
			const float yc = cosf(yawB), ysn = sinf(yawB);
			const float barrelLocalGame[3] = { -ysn, -ts * yc, tc * yc };
			float barrelView[3] = {
				relGame[0]*barrelLocalGame[0] + relGame[1]*barrelLocalGame[1] + relGame[2]*barrelLocalGame[2],
				relGame[3]*barrelLocalGame[0] + relGame[4]*barrelLocalGame[1] + relGame[5]*barrelLocalGame[2],
				relGame[6]*barrelLocalGame[0] + relGame[7]*barrelLocalGame[1] + relGame[8]*barrelLocalGame[2],
			};
			const float barrelLen = sqrtf(barrelView[0]*barrelView[0]
				+ barrelView[1]*barrelView[1] + barrelView[2]*barrelView[2]);
			if (!isfinite(barrelLen) || barrelLen < 0.001f) {
				sWeaponAnchorSettle = 0;
				return false;
			}
			for (int axis = 0; axis < 3; ++axis)
				barrelView[axis] /= barrelLen;

			// Shortest rotation from the viewmodel's canonical +Z barrel axis
			// to the controller barrel. At the anchor this is intentionally
			// not identity: it makes recenter affect the gun and projectile ray.
			const float dot = max(-1.0f, min(1.0f, barrelView[2]));
			if (dot > -0.999f) {
				const float vx = -barrelView[1];
				const float vy = barrelView[0];
				const float k = 1.0f / (1.0f + dot);
				sWeaponAnchorOutputRot[0] = 1.0f - vy*vy*k;
				sWeaponAnchorOutputRot[1] = vx*vy*k;
				sWeaponAnchorOutputRot[2] = vy;
				sWeaponAnchorOutputRot[3] = vy*vx*k;
				sWeaponAnchorOutputRot[4] = 1.0f - vx*vx*k;
				sWeaponAnchorOutputRot[5] = -vx;
				sWeaponAnchorOutputRot[6] = -vy;
				sWeaponAnchorOutputRot[7] = vx;
				sWeaponAnchorOutputRot[8] = dot;
			} else {
				const float turnAroundY[9] = { -1,0,0, 0,1,0, 0,0,-1 };
				memcpy(sWeaponAnchorOutputRot, turnAroundY, sizeof(turnAroundY));
			}
			memcpy(sWeaponAnchorRot, relGame, sizeof(sWeaponAnchorRot));
			sWeaponAnchored = true;
			sWeaponAnchorSettle = 0;
			LogInfo("VRPose weapon: controller pose anchored; retained barrel direction "
				"(%.3f %.3f %.3f)\n", barrelView[0], barrelView[1], barrelView[2]);
			return false;
		}

		float anchorT[9];
		Transpose3x3(sWeaponAnchorRot, anchorT);
		float poseDelta[9], oneHandDelta[9];
		Multiply3x3(relGame, anchorT, poseDelta);
		Multiply3x3(poseDelta, sWeaponAnchorOutputRot, oneHandDelta);

		// Optional two-handed constraint. The right controller remains the rear
		// pivot and roll source. At grab time, capture the direction from the
		// right controller to the support controller in RIGHT-CONTROLLER LOCAL
		// space. While held, apply the shortest rotation from that baseline to
		// the current local direction. This makes the front follow the support
		// hand without depending on the player's natural grip angle, and moving
		// both hands together leaves the one-handed pose unchanged.
		vr::VRControllerState_t leftState = {};
		const bool leftGripHeld = GetControllerState(
			vr::TrackedControllerRole_LeftHand, &leftState)
			&& (leftState.ulButtonPressed
				& vr::ButtonMaskFromId(vr::k_EButton_Grip));
		// The rectangular weapon volume is an acquisition test only. Once the
		// support grip is active, keep it latched while left grip remains held so
		// natural hand movement cannot fall out of an invisible release box. A
		// grip that began outside the box cannot acquire the weapon by entering it
		// later; the player must release and begin a new press while touching it.
		const bool twoHandHeld = leftGripHeld
			&& (sTwoHandActive
				|| InterlockedCompareExchange(
					&sTwoHandGripAcquireAuthorized, 0, 0) != 0);
		if (twoHandHeld) {
			const vr::TrackedDeviceIndex_t leftIdx =
				sVRSystem->GetTrackedDeviceIndexForControllerRole(
					vr::TrackedControllerRole_LeftHand);
			if (leftIdx != vr::k_unTrackedDeviceIndexInvalid
					&& poses[leftIdx].bDeviceIsConnected && poses[leftIdx].bPoseIsValid) {
				const vr::HmdMatrix34_t &rPose = poses[idx].mDeviceToAbsoluteTracking;
				const vr::HmdMatrix34_t &lPose = poses[leftIdx].mDeviceToAbsoluteTracking;
				const float supportWorld[3] = {
					lPose.m[0][3] - rPose.m[0][3],
					lPose.m[1][3] - rPose.m[1][3],
					lPose.m[2][3] - rPose.m[2][3] };
				// R_right^T transforms the tracking-space vector into the right
				// controller's local axes. Flip local Z to match the game-space
				// conjugation used by relGame.
				float currentDir[3] = {
					rPose.m[0][0] * supportWorld[0] + rPose.m[1][0] * supportWorld[1] + rPose.m[2][0] * supportWorld[2],
					rPose.m[0][1] * supportWorld[0] + rPose.m[1][1] * supportWorld[1] + rPose.m[2][1] * supportWorld[2],
					-(rPose.m[0][2] * supportWorld[0] + rPose.m[1][2] * supportWorld[1] + rPose.m[2][2] * supportWorld[2]) };
				const float lengthSq = currentDir[0] * currentDir[0]
					+ currentDir[1] * currentDir[1] + currentDir[2] * currentDir[2];
				if (lengthSq < 0.0025f) {
					sTwoHandActive = false;
					memcpy(out, oneHandDelta, sizeof(oneHandDelta));
					return true;
				}
				const float invLength = 1.0f / sqrtf(lengthSq);
				for (int axis = 0; axis < 3; ++axis)
					currentDir[axis] *= invLength;

				if (!sTwoHandActive) {
					memcpy(sTwoHandBaseDirLocal, currentDir, sizeof(sTwoHandBaseDirLocal));
					sTwoHandActive = true;
					LogInfo("VRPose weapon: two-hand mode engaged (local support-vector anchor)\n");
				}

				const float *a = sTwoHandBaseDirLocal;
				const float *b = currentDir;
				const float vx = a[1] * b[2] - a[2] * b[1];
				const float vy = a[2] * b[0] - a[0] * b[2];
				const float vz = a[0] * b[1] - a[1] * b[0];
				const float dot = max(-1.0f, min(1.0f,
					a[0] * b[0] + a[1] * b[1] + a[2] * b[2]));
				float supportDelta[9] = { 1,0,0, 0,1,0, 0,0,1 };
				// Rodrigues' shortest-arc form: I + K + K^2/(1+dot).
				// The controllers cannot realistically cross into the exact 180
				// degree singularity; retain identity there instead of exploding.
				if (dot > -0.999f) {
					const float k = 1.0f / (1.0f + dot);
					supportDelta[0] = 1.0f - (vy * vy + vz * vz) * k;
					supportDelta[1] = -vz + vx * vy * k;
					supportDelta[2] = vy + vx * vz * k;
					supportDelta[3] = vz + vy * vx * k;
					supportDelta[4] = 1.0f - (vx * vx + vz * vz) * k;
					supportDelta[5] = -vx + vy * vz * k;
					supportDelta[6] = -vy + vz * vx * k;
					supportDelta[7] = vx + vz * vy * k;
					supportDelta[8] = 1.0f - (vx * vx + vy * vy) * k;
				}
				float desired[9], twoHandDelta[9];
				Multiply3x3(relGame, supportDelta, desired);
				Multiply3x3(desired, anchorT, twoHandDelta);
				Multiply3x3(twoHandDelta, sWeaponAnchorOutputRot, out);
				return true;
			}
		}
		sTwoHandActive = false;
		memcpy(out, oneHandDelta, sizeof(oneHandDelta));
		return true;
	}

	// Threads created after arming have clean debug registers, so re-arm
	// periodically until something trips. Cheap relative to how long the game
	// runs, and it means a late-spawned worker thread cannot quietly hide the
	// answer.
	void ReArmMatrixSourceProbe()
	{
		if (InterlockedCompareExchange(&sOwnershipProbeArmed, 0, 0) != 0)
			return;
		if (InterlockedCompareExchange(&sStage2State, 1, 1) != 1)
			return;   // not armed, or already fired
		static unsigned frames = 0;
		static int attempts = 0;
		if (++frames % 900 != 0 || attempts >= 6)
			return;
		attempts++;
		if (sProbeWatchAddress) {
			int others = ArmWatchpointOnAllThreads(sProbeWatchAddress);
			RaiseException(kArmExceptionCode, 0, 0, NULL);
			LogInfo("VRPose probe2: re-armed (attempt %d) across %d other threads\n",
				attempts, others);
		}
	}

	void ArmCameraWriteProbe(void *mappedData)
	{
		static const bool kProbeEnabled = false; // retired camera-writer breakpoint chain
		if (!kProbeEnabled || !mappedData)
			return;
		if (InterlockedCompareExchange(&sProbeState, 1, 0) != 0)
			return;   // already arming, armed, or finished

		if (!sProbeVehHandle) {
			sProbeVehHandle = AddVectoredExceptionHandler(1, CameraWriteProbeVeh);
			if (!sProbeVehHandle) {
				LogInfo("VRPose probe: AddVectoredExceptionHandler failed\n");
				InterlockedExchange(&sProbeState, 3);
				return;
			}
		}

		// Prove the mechanism first, against a write we make ourselves.
		InterlockedExchange(&sProbeSelfTesting, 1);
		InterlockedExchange(&sProbeSelfTestHit, 0);
		sProbeWatchAddress = (void *)&sProbeSelfTestVar;
		RaiseException(kArmExceptionCode, 0, 0, NULL);
		sProbeSelfTestVar = 0x1234567890ABCDEFull;   // must trap
		InterlockedExchange(&sProbeSelfTesting, 0);

		if (!InterlockedCompareExchange(&sProbeSelfTestHit, 0, 0)) {
			LogInfo("VRPose probe: SELF-TEST FAILED - a hardware breakpoint on our own\n");
			LogInfo("VRPose probe:   variable did not fire, so arming is not taking effect\n");
			LogInfo("VRPose probe:   (debug registers unavailable, or ContextFlags ignored).\n");
			InterlockedExchange(&sProbeState, 3);
			return;
		}
		LogInfo("VRPose probe: self-test PASSED - hardware breakpoints work on this thread\n");

		// Now arm for real.
		sProbeWatchAddress = mappedData;
		LogInfo("VRPose probe: arming on %p (this thread, synchronously)\n", mappedData);
		RaiseException(kArmExceptionCode, 0, 0, NULL);
	}

	static void PollOwnershipProbeHotkeys()
	{
		if (!kReverseEngineeringDiagnosticsEnabled)
			return;
		// F7 labels the next F12 capture ORDINARY; F8 labels it SCRIPTED.
		// F12 watches Metro's actual camera-matrix setter at +0x7E09EC. Labels
		// are recorded in the same session so separate launches are unnecessary.
		static bool sF7Down = false;
		static bool sF8Down = false;
		static bool sF12Down = false;
		static bool sF9Down = false;
		static bool sF6Down = false;
		static bool sF5Down = false;
		const bool f7Down = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
		const bool f8Down = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
		const bool f12Down = (GetAsyncKeyState(VK_F12) & 0x8000) != 0;
		const bool f9Down = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
		const bool f6Down = (GetAsyncKeyState(VK_F6) & 0x8000) != 0;
		const bool f5Down = (GetAsyncKeyState(VK_F5) & 0x8000) != 0;
		if (f7Down && !sF7Down) {
			InterlockedExchange(&sOwnershipProbeLabel, 1);
			InterlockedExchange(&sControlProbeLabel, 1);
			LogInfo("VRPose ownership probe: next F12 labeled ORDINARY\n");
			BYTE a = 0, b = 0;
			PVOID gateA = InterlockedCompareExchangePointer(&sLiveKeyboardGateA, NULL, NULL);
			PVOID gateB = InterlockedCompareExchangePointer(&sLiveKeyboardGateB, NULL, NULL);
			if (gateA) ReadProcessMemory(GetCurrentProcess(), gateA, &a, sizeof(a), NULL);
			if (gateB) ReadProcessMemory(GetCurrentProcess(), gateB, &b, sizeof(b), NULL);
			LogInfo("VRPose ownership probe: keyboard gates ORDINARY A=%u B=%u\n", (unsigned)a, (unsigned)b);
		}
		if (f8Down && !sF8Down) {
			InterlockedExchange(&sOwnershipProbeLabel, 2);
			InterlockedExchange(&sControlProbeLabel, 2);
			LogInfo("VRPose ownership probe: next F12 labeled SCRIPTED\n");
			BYTE a = 0, b = 0;
			PVOID gateA = InterlockedCompareExchangePointer(&sLiveKeyboardGateA, NULL, NULL);
			PVOID gateB = InterlockedCompareExchangePointer(&sLiveKeyboardGateB, NULL, NULL);
			if (gateA) ReadProcessMemory(GetCurrentProcess(), gateA, &a, sizeof(a), NULL);
			if (gateB) ReadProcessMemory(GetCurrentProcess(), gateB, &b, sizeof(b), NULL);
			LogInfo("VRPose ownership probe: keyboard gates SCRIPTED A=%u B=%u\n", (unsigned)a, (unsigned)b);
		}
		if (kRetiredCameraProbesEnabled && f12Down && !sF12Down) {
			const LONG slot = 3;
			if (InterlockedCompareExchange(&sOwnershipProbeArmed, 0, 0) == 0) {
				HMODULE base = GetModuleHandleA(NULL);
				if (base) {
					const DWORD_PTR rva = 0x7E09EC;
					sProbeDr7 = kDr7Exec;
					sProbeWatchAddress = (BYTE *)base + rva;
					InterlockedExchange(&sOwnershipProbeSlot, slot);
					InterlockedExchange(&sOwnershipProbeStage, 1);
					InterlockedExchange(&sOwnershipProbeArmed, 1);
					InterlockedExchange(&sProbeReported, 0);
					if (!sProbeVehHandle)
						sProbeVehHandle = AddVectoredExceptionHandler(1, CameraWriteProbeVeh);
					const int others = ArmWatchpointOnAllThreads(sProbeWatchAddress);
					RaiseException(kArmExceptionCode, 0, 0, NULL);
					LogInfo("VRPose ownership probe: armed camera setter EXEC +0x%llX across %d other threads\n",
						(unsigned long long)rva, others);
				}
			}
		}
		if (kRetiredCameraProbesEnabled && f9Down && !sF9Down) {
			if (InterlockedCompareExchange(&sOwnershipProbeArmed, 0, 0) == 0) {
				HMODULE base = GetModuleHandleA(NULL);
				if (base) {
					sProbeDr7 = kDr7Exec;
					sProbeWatchAddress = InterlockedCompareExchangePointer(
					&sLiveInputDispatcherAddress, NULL, NULL);
					if (!sProbeWatchAddress) {
						LogInfo("VRPose ownership probe: F9 skipped; live input dispatcher address unavailable\n");
						InterlockedExchange(&sOwnershipProbeStage, 0);
						InterlockedExchange(&sOwnershipProbeArmed, 0);
						return;
					}
					InterlockedExchange(&sOwnershipProbeSlot, 4);
					InterlockedExchange(&sOwnershipProbeStage, 4);
					InterlockedExchange(&sOwnershipProbeArmed, 1);
					if (!sProbeVehHandle)
						sProbeVehHandle = AddVectoredExceptionHandler(1, CameraWriteProbeVeh);
					const int others = ArmWatchpointOnAllThreads(sProbeWatchAddress);
					LogInfo("VRPose ownership probe: F9 armed input dispatcher EXEC at %p label=%s\n",
						sProbeWatchAddress,
						InterlockedCompareExchange(&sOwnershipProbeLabel, 0, 0) == 2 ? "SCRIPTED" : "ORDINARY");
					LogInfo("VRPose ownership probe: input dispatcher armed across %d other threads\n", others);
					RaiseException(kArmExceptionCode, 0, 0, NULL);
				}
			}
		}
		if (kRetiredCameraProbesEnabled && f6Down && !sF6Down) {
			if (InterlockedCompareExchange(&sOwnershipProbeArmed, 0, 0) == 0) {
				HMODULE base = GetModuleHandleA(NULL);
				if (base) {
					sProbeDr7 = kDr7Exec;
					sProbeWatchAddress = (BYTE *)base + 0x7EEAC6;
					InterlockedExchange(&sOwnershipProbeSlot, 5);
					InterlockedExchange(&sOwnershipProbeStage, 2);
					InterlockedExchange(&sOwnershipProbeArmed, 1);
					if (!sProbeVehHandle)
						sProbeVehHandle = AddVectoredExceptionHandler(1, CameraWriteProbeVeh);
					const int others = ArmWatchpointOnAllThreads(sProbeWatchAddress);
					LogInfo("VRPose ownership probe: F6 armed source writer EXEC at %p label=%s "
						"across %d other threads\n", sProbeWatchAddress,
						InterlockedCompareExchange(&sOwnershipProbeLabel, 0, 0) == 2 ? "SCRIPTED" : "ORDINARY",
						others);
					RaiseException(kArmExceptionCode, 0, 0, NULL);
				}
			}
		}
		if (kRetiredCameraProbesEnabled && f5Down && !sF5Down) {
			if (InterlockedCompareExchange(&sOwnershipProbeArmed, 0, 0) == 0) {
				HMODULE base = GetModuleHandleA(NULL);
				if (base) {
					sProbeDr7 = kDr7Exec;
					sProbeWatchAddress = (BYTE *)base + kCameraCommandRecorderRva;
					InterlockedExchange(&sOwnershipProbeSlot, 5);
					InterlockedExchange(&sOwnershipProbeStage, 5);
					InterlockedExchange(&sOwnershipProbeArmed, 1);
					if (!sProbeVehHandle)
						sProbeVehHandle = AddVectoredExceptionHandler(1, CameraWriteProbeVeh);
					const int others = ArmWatchpointOnAllThreads(sProbeWatchAddress);
					LogInfo("VRPose control-state probe: F5 armed camera command "
						"recorder EXEC +0x%llX at %p label=%s across %d other threads\n",
						(unsigned long long)kCameraCommandRecorderRva,
						sProbeWatchAddress,
						InterlockedCompareExchange(&sOwnershipProbeLabel, 0, 0) == 2
							? "SCRIPTED" : "ORDINARY", others);
					RaiseException(kArmExceptionCode, 0, 0, NULL);
				}
			}
		}
		sF7Down = f7Down;
		sF8Down = f8Down;
		sF12Down = f12Down;
		sF9Down = f9Down;
		sF6Down = f6Down;
		sF5Down = f5Down;
	}

	void RecenterAiming();
	bool GetControllerAim(float *outYaw, float *outPitch);

	static float sSmoothWantYaw = 0.0f, sSmoothWantPitch = 0.0f;

	static float sLastScaleYaw = 1.0f;
	static float sLastScalePitch = 1.0f;

	// The heading the player's body is facing - the direction the rendered
	// view is anchored to. Moves only when the player deliberately turns.
	static bool sBodyAnchored = false;
	static float sBodyYaw = 0.0f, sBodyPitch = 0.0f;
	static float sRenderedBodyYaw = 0.0f;
	static volatile LONG sRenderedBodyYawValid = 0;

	void GetBodyHeading(float *outYaw, float *outPitch, bool *valid)
	{
		*outYaw = sBodyYaw;
		*outPitch = sBodyPitch;
		*valid = sBodyAnchored;
	}

	void SetRenderedBodyHeading(float yaw)
	{
		if (!isfinite(yaw))
			return;
		sRenderedBodyYaw = yaw;
		InterlockedExchange(&sRenderedBodyYawValid, 1);
	}

	bool GetRenderedBodyHeading(float *outYaw)
	{
		if (!outYaw || InterlockedCompareExchange(&sRenderedBodyYawValid, 0, 0) == 0)
			return false;
		*outYaw = sRenderedBodyYaw;
		return isfinite(*outYaw);
	}

	bool GetHeadTranslationDelta(float out[3])
	{
		static unsigned s6DofLogCounter = 0;
		// Camera translation is independent of the weapon/body aiming anchor.
		// The body anchor can be false during normal gameplay while HMD pose
		// tracking is fully valid; gating on it silently disabled 6DOF.
		const bool valid = out && sLastPolledPoseValid && sHeadReferencePosValid;
		static const bool kHeadTranslationDiagnostic = false;
		if (!valid) {
			if (kHeadTranslationDiagnostic
				&& (s6DofLogCounter++ % 60) == 0)
				LogInfo("VRPose 6DOF: unavailable out=%d pose=%d ref=%d body=%d\n",
					out ? 1 : 0, sLastPolledPoseValid ? 1 : 0,
					sHeadReferencePosValid ? 1 : 0, sBodyAnchored ? 1 : 0);
			return false;
		}
		// Return displacement in the reference/view frame. The camera patch
		// converts this into world space using the game's current body view
		// rotation; applying X/Z directly as world axes causes sideways and
		// forward/back movement to sway when the game heading differs from the
		// tracking frame.
		const float worldScale = TrackingWorldScale();
		out[0] = (sLastPolledPose.m[0][3] - sHeadReferencePos[0]) * kMetresToGameUnits * worldScale;
		out[1] = (sLastPolledPose.m[1][3] - sHeadReferencePos[1]) * kMetresToGameUnits * worldScale;
		out[2] = -(sLastPolledPose.m[2][3] - sHeadReferencePos[2]) * kMetresToGameUnits * worldScale;
		if (kHeadTranslationDiagnostic
			&& (s6DofLogCounter++ % 60) == 0)
			LogInfo("VRPose 6DOF: raw=(%.4f %.4f %.4f) ref=(%.4f %.4f %.4f) delta=(%.4f %.4f %.4f) bodyYaw=%.4f\n",
				sLastPolledPose.m[0][3], sLastPolledPose.m[1][3], sLastPolledPose.m[2][3],
				sHeadReferencePos[0], sHeadReferencePos[1], sHeadReferencePos[2],
				out[0], out[1], out[2], sBodyYaw);
		return true;
	}

	bool GetLeftHandRotationDelta(float out[9])
	{
		if (!out || !sVRSystem || !sLastPolledPoseValid)
			return false;
		vr::TrackedDeviceIndex_t idx = sVRSystem->GetTrackedDeviceIndexForControllerRole(
			vr::TrackedControllerRole_LeftHand);
		if (idx == vr::k_unTrackedDeviceIndexInvalid)
			return false;
		vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount];
		GetTrackedPosesForFrame(poses);
		if (!poses[idx].bDeviceIsConnected || !poses[idx].bPoseIsValid)
			return false;
		float ctrlR[9], hmdR[9], hmdT[9], rel[9], relGame[9];
		ExtractRotation3x3(poses[idx].mDeviceToAbsoluteTracking, ctrlR);
		ExtractRotation3x3(sLastPolledPose, hmdR);
		Transpose3x3(hmdR, hmdT);
		Multiply3x3(hmdT, ctrlR, rel);
		ConvertOpenVRRotationToGameSpace(rel, relGame);
		if (!sLeftHandAnchored) {
			if (++sLeftHandSettle < 30)
				return false;
			memcpy(sLeftHandAnchorRot, relGame, sizeof(sLeftHandAnchorRot));
			const vr::HmdMatrix34_t &c = poses[idx].mDeviceToAbsoluteTracking;
			const vr::HmdMatrix34_t &h = sLastPolledPose;
			const float d[3] = { c.m[0][3] - h.m[0][3],
				c.m[1][3] - h.m[1][3], c.m[2][3] - h.m[2][3] };
			for (int r = 0; r < 3; r++)
				sLeftHandAnchorPos[r] = (h.m[0][r] * d[0]
					+ h.m[1][r] * d[1] + h.m[2][r] * d[2]) * kMetresToGameUnits;
			sLeftHandAnchorPos[2] = -sLeftHandAnchorPos[2];
			sLeftHandPositionAnchored = true;
			sLeftHandAnchored = true;
			LogInfo("VRPose hand: left controller pose anchored\n");
			return false;
		}
		float anchorT[9];
		Transpose3x3(sLeftHandAnchorRot, anchorT);
		Multiply3x3(relGame, anchorT, out);
		return true;
	}

	bool GetLeftHandTranslationDelta(float out[3])
	{
		if (!out || !sVRSystem || !sLastPolledPoseValid)
			return false;
		vr::TrackedDeviceIndex_t idx = sVRSystem->GetTrackedDeviceIndexForControllerRole(
			vr::TrackedControllerRole_LeftHand);
		if (idx == vr::k_unTrackedDeviceIndexInvalid)
			return false;
		vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount];
		GetTrackedPosesForFrame(poses);
		if (!poses[idx].bDeviceIsConnected || !poses[idx].bPoseIsValid)
			return false;
		const vr::HmdMatrix34_t &c = poses[idx].mDeviceToAbsoluteTracking;
		const vr::HmdMatrix34_t &h = sLastPolledPose;
		const float d[3] = { c.m[0][3] - h.m[0][3], c.m[1][3] - h.m[1][3], c.m[2][3] - h.m[2][3] };
		const float rel[3] = {
			h.m[0][0] * d[0] + h.m[1][0] * d[1] + h.m[2][0] * d[2],
			h.m[0][1] * d[0] + h.m[1][1] * d[1] + h.m[2][1] * d[2],
			h.m[0][2] * d[0] + h.m[1][2] * d[1] + h.m[2][2] * d[2] };
		const float g[3] = { rel[0] * kMetresToGameUnits, rel[1] * kMetresToGameUnits,
			-rel[2] * kMetresToGameUnits };
		if (!sLeftHandAnchored)
			return false;
		if (!sLeftHandPositionAnchored) {
			memcpy(sLeftHandAnchorPos, g, sizeof(sLeftHandAnchorPos));
			sLeftHandPositionAnchored = true;
			return false;
		}
		const float worldScale = TrackingWorldScale();
		out[0] = (g[0] - sLeftHandAnchorPos[0]) * worldScale;
		out[1] = (g[1] - sLeftHandAnchorPos[1]) * worldScale;
		out[2] = (g[2] - sLeftHandAnchorPos[2]) * worldScale;
		return true;
	}

	bool GetLeftHandControllerRelativeToHead(float out[3])
	{
		if (!out || !sVRSystem || !sLastPolledPoseValid)
			return false;
		vr::TrackedDeviceIndex_t idx = sVRSystem->GetTrackedDeviceIndexForControllerRole(
			vr::TrackedControllerRole_LeftHand);
		if (idx == vr::k_unTrackedDeviceIndexInvalid)
			return false;
		vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount];
		GetTrackedPosesForFrame(poses);
		if (!poses[idx].bDeviceIsConnected || !poses[idx].bPoseIsValid)
			return false;

		const vr::HmdMatrix34_t &c = poses[idx].mDeviceToAbsoluteTracking;
		const vr::HmdMatrix34_t &h = sLastPolledPose;
		const float d[3] = { c.m[0][3] - h.m[0][3], c.m[1][3] - h.m[1][3],
			c.m[2][3] - h.m[2][3] };
		for (int r = 0; r < 3; r++)
			out[r] = (h.m[0][r] * d[0] + h.m[1][r] * d[1]
			+ h.m[2][r] * d[2]) * kMetresToGameUnits * TrackingWorldScale();
		out[2] = -out[2];
		return true;
	}

	bool GetRightHandRotationDelta(float out[9])
	{
		if (!out || !sVRSystem || !sLastPolledPoseValid)
			return false;
		const vr::TrackedDeviceIndex_t idx =
			sVRSystem->GetTrackedDeviceIndexForControllerRole(WeaponControllerRole());
		if (idx == vr::k_unTrackedDeviceIndexInvalid)
			return false;
		vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount];
		GetTrackedPosesForFrame(poses);
		if (!poses[idx].bDeviceIsConnected || !poses[idx].bPoseIsValid)
			return false;
		float ctrlR[9], hmdR[9], hmdT[9], rel[9], relGame[9];
		ExtractRotation3x3(poses[idx].mDeviceToAbsoluteTracking, ctrlR);
		ExtractRotation3x3(sLastPolledPose, hmdR);
		Transpose3x3(hmdR, hmdT);
		Multiply3x3(hmdT, ctrlR, rel);
		ConvertOpenVRRotationToGameSpace(rel, relGame);
		if (!sRightHandAnchored) {
			if (++sRightHandSettle < 30)
				return false;
			memcpy(sRightHandAnchorRot, relGame, sizeof(sRightHandAnchorRot));
			sRightHandAnchored = true;
			return false;
		}
		float anchorT[9];
		Transpose3x3(sRightHandAnchorRot, anchorT);
		Multiply3x3(relGame, anchorT, out);
		return true;
	}

	bool GetLeftHandControllerRotationRelativeToHead(float out[9])
	{
		if (!out || !sVRSystem || !sLastPolledPoseValid)
			return false;
		const vr::TrackedDeviceIndex_t idx =
			sVRSystem->GetTrackedDeviceIndexForControllerRole(
				vr::TrackedControllerRole_LeftHand);
		if (idx == vr::k_unTrackedDeviceIndexInvalid)
			return false;
		vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount];
		GetTrackedPosesForFrame(poses);
		if (!poses[idx].bDeviceIsConnected || !poses[idx].bPoseIsValid)
			return false;
		float ctrlR[9], hmdR[9], hmdT[9], relative[9];
		ExtractRotation3x3(poses[idx].mDeviceToAbsoluteTracking, ctrlR);
		ExtractRotation3x3(sLastPolledPose, hmdR);
		Transpose3x3(hmdR, hmdT);
		Multiply3x3(hmdT, ctrlR, relative);
		ConvertOpenVRRotationToGameSpace(relative, out);
		return true;
	}

	bool GetHeadTranslationDeltaInHeadFrame(float out[3])
	{
		if (!out || !sLastPolledPoseValid || !sHeadReferencePosValid)
			return false;
		const float dx = sLastPolledPose.m[0][3] - sHeadReferencePos[0];
		const float dy = sLastPolledPose.m[1][3] - sHeadReferencePos[1];
		const float dz = sLastPolledPose.m[2][3] - sHeadReferencePos[2];
		for (int r = 0; r < 3; r++)
			out[r] = (sLastPolledPose.m[0][r] * dx
				+ sLastPolledPose.m[1][r] * dy
				+ sLastPolledPose.m[2][r] * dz) * kMetresToGameUnits * TrackingWorldScale();
		// Match GetControllerRelativeToHead(): game-space forward is -Z.
		out[2] = -out[2];
		return true;
	}

	// Everything the aim system anchors to, reset to "here, now": the
	// controller's current direction becomes centre, the body faces wherever
	// the game's camera currently points, our commanded total is cleared, and
	// the head's reference orientation is recaptured so straight-ahead is
	// wherever the player is looking.
	//
	// Deliberately makes the cancellation zero at the instant it runs, so the
	// view does not jump - it simply becomes correct from here on.
	void RecenterAiming()
	{
		ClearNativeBodyTurnIntent();
		ResetNativeCameraCreditEpoch();
		float gy = 0.0f, gp = 0.0f;
		bool gv = false;
		GetViewAngleSnapshot(&gy, &gp, &gv);
		if (gv) {
			sBodyYaw = gy;
			sBodyPitch = 0.0f;   // level - see the anchor in UpdateMotionAiming
			sBodyAnchored = true;
		}

		float cy = 0.0f, cp = 0.0f;
		if (GetControllerAim(&cy, &cp)) {
			sAnchorCtrlYaw = cy;
			sAnchorCtrlPitch = cp;
			sCtrlAnchored = true;
		}

		sAppliedOffsetYaw = 0.0f;
		sAppliedOffsetPitch = 0.0f;
		sSmoothWantYaw = 0.0f;
		sSmoothWantPitch = 0.0f;
		sPendingTurnYaw = 0.0f;
		G->vrReferenceRotationCaptured = false;

		// Re-anchor the weapon too, so recentring is a genuine CALIBRATION.
		//
		// The dominant aim error is not the gun - it is the angle the
		// controller happened to be held at when the anchor was captured at
		// load time, which varies every session. Clearing these lets the
		// player point deliberately at a target, hold the button, and have
		// the weapon, the aim and the view all align to that pose.
		sWeaponAnchored = false;
		sWeaponPosAnchored = false;
		sWeaponAnchorSettle = 0;
		sTwoHandActive = false;
		sLeftHandAnchored = false;
		sLeftHandPositionAnchored = false;
		sLeftHandSettle = 0;
		InterlockedIncrement(&sControllerRecenterGeneration);
		// Re-capture where the head is, so the weapon is measured from where
		// the player is standing NOW rather than from wherever they were when
		// the game started.
		sHeadRefValid = false;

		LogInfo("VRPose aim: recentred - body heading (%.3f, %.3f) rad, controller centre (%.3f, %.3f)\n",
			sBodyYaw, sBodyPitch, cy, cp);
	}

	// SteamVR has already recentered its own tracking origin when this runs.
	// Refresh only state derived from that tracking coordinate system. Calling
	// RecenterAiming here also overwrote sBodyYaw from a transient game-camera
	// snapshot; the rendered gun stayed correct in view space, but the shot
	// path then transformed its barrel ray through that bogus body heading and
	// could send rounds roughly ninety degrees sideways.
	void RecenterTrackingOrigin()
	{
		G->vrReferenceRotationCaptured = false;
		sWeaponAnchored = false;
		sWeaponPosAnchored = false;
		sWeaponAnchorSettle = 0;
		sTwoHandActive = false;
		sLeftHandAnchored = false;
		sLeftHandPositionAnchored = false;
		sLeftHandSettle = 0;
		InterlockedIncrement(&sControllerRecenterGeneration);
		sHeadRefValid = false;
		sHeadReferencePosValid = false;
		LogInfo("VRPose aim: tracking-origin references cleared; body heading retained "
			"at (%.3f, %.3f) rad\n", sBodyYaw, sBodyPitch);
	}

	// Rolling history of the cumulative commanded totals, so the estimator
	// can look up "what had we commanded as of L frames ago" without having
	// to re-accumulate when its lag estimate changes.
	static const int kAimHistLen = 16;
	static const int kAimMaxLag = 6;
	static float sHistTotalYaw[kAimHistLen] = { 0 };
	static float sHistTotalPitch[kAimHistLen] = { 0 };
	static float sHistCmdYaw[kAimHistLen] = { 0 };   // per-frame commanded delta
	static float sHistCmdPitch[kAimHistLen] = { 0 };
	static int sAimHistPos = 0;

	// Least-squares accumulators, one bucket per candidate lag. Slowly
	// decayed so the fit tracks the game rather than being frozen by the
	// first few seconds.
	static double sSumCmdObsYaw[kAimMaxLag + 1] = { 0 };
	static double sSumCmdSqYaw[kAimMaxLag + 1] = { 0 };
	static double sSumCmdObsPitch[kAimMaxLag + 1] = { 0 };
	static double sSumCmdSqPitch[kAimMaxLag + 1] = { 0 };

	// The same fit restricted to LARGE single-frame commands, which answers
	// whether the fire-swing design is viable.
	//
	// That design snaps the game's camera onto the gun direction for the
	// couple of frames a shot takes, then snaps back - so the camera can
	// otherwise stay with the player's head and culling stops disagreeing
	// with what they see. It only works if the game applies a big one-frame
	// mouse delta in full. If it clamps or smooths large inputs, the same
	// swing needs many small steps instead and takes far too long to hide
	// inside a trigger pull.
	//
	// Comparing the large-command scale against the overall one measures
	// exactly that, from ordinary play - fast hand movements generate the
	// large commands on their own, so this needs no special test.
	static double sSumCmdObsBigYaw[kAimMaxLag + 1] = { 0 };
	static double sSumCmdSqBigYaw[kAimMaxLag + 1] = { 0 };
	static double sSumCmdObsBigPitch[kAimMaxLag + 1] = { 0 };
	static double sSumCmdSqBigPitch[kAimMaxLag + 1] = { 0 };
	static double sBigSampleCount = 0.0;
	static float sLastBigScaleYaw = 0.0f;
	static float sLastBigScalePitch = 0.0f;

	// Regresses the game's observed per-frame rotation onto our commanded
	// input at each candidate lag. The lag whose fit explains the most of
	// the observed motion wins, and its regression slope is the true
	// radians-per-commanded-radian scale.
	//
	// Input the PLAYER produced (stick turning) is uncorrelated with what we
	// command, so it contributes noise to the fit rather than bias, and
	// averages out over the window.
	//
	// Deliberately only ever affects what we RENDER, never what we inject.
	// A bad estimate can therefore make the view wobble, but it cannot feed
	// back into the aim loop and run away.
	void UpdateLagScaleEstimate(float obsYaw, float obsPitch, int *outLag, float *outScaleYaw, float *outScalePitch)
	{
		const double kDecay = 0.999;

		for (int L = 0; L <= kAimMaxLag; L++) {
			int idx = ((sAimHistPos - L) % kAimHistLen + kAimHistLen) % kAimHistLen;
			double cy = sHistCmdYaw[idx];
			double cp = sHistCmdPitch[idx];

			sSumCmdObsYaw[L] = sSumCmdObsYaw[L] * kDecay + cy * obsYaw;
			sSumCmdSqYaw[L] = sSumCmdSqYaw[L] * kDecay + cy * cy;
			sSumCmdObsPitch[L] = sSumCmdObsPitch[L] * kDecay + cp * obsPitch;
			sSumCmdSqPitch[L] = sSumCmdSqPitch[L] * kDecay + cp * cp;

			// ~5 degrees in a single frame - well past anything the aim loop
			// issues while merely tracking a hand, so this bucket only fills
			// on fast movement, which is what a fire-swing looks like.
			const double kBigCmd = 0.09;
			if (fabs(cy) > kBigCmd) {
				sSumCmdObsBigYaw[L] += cy * obsYaw;
				sSumCmdSqBigYaw[L] += cy * cy;
				if (L == 0) sBigSampleCount += 1.0;
			}
			if (fabs(cp) > kBigCmd) {
				sSumCmdObsBigPitch[L] += cp * obsPitch;
				sSumCmdSqBigPitch[L] += cp * cp;
			}
		}

		// Pick the lag by combined explained variance across both axes,
		// since the input pipeline's latency is a single number.
		int bestLag = 1;
		double bestScore = -1.0;
		for (int L = 0; L <= kAimMaxLag; L++) {
			double score = 0.0;
			if (sSumCmdSqYaw[L] > 1e-9)
				score += (sSumCmdObsYaw[L] * sSumCmdObsYaw[L]) / sSumCmdSqYaw[L];
			if (sSumCmdSqPitch[L] > 1e-9)
				score += (sSumCmdObsPitch[L] * sSumCmdObsPitch[L]) / sSumCmdSqPitch[L];
			if (score > bestScore) {
				bestScore = score;
				bestLag = L;
			}
		}

		// Until enough movement has been seen to fit anything, assume the
		// commanded value is the truth with one frame of latency.
		const double kMinEvidence = 1e-4;
		float scaleYaw = 1.0f, scalePitch = 1.0f;
		if (sSumCmdSqYaw[bestLag] > kMinEvidence)
			scaleYaw = (float)(sSumCmdObsYaw[bestLag] / sSumCmdSqYaw[bestLag]);
		if (sSumCmdSqPitch[bestLag] > kMinEvidence)
			scalePitch = (float)(sSumCmdObsPitch[bestLag] / sSumCmdSqPitch[bestLag]);

		// Wide bounds deliberately. The first live fit measured yaw at 0.997
		// but pitch at 0.54 - Metro applies its own vertical sensitivity
		// multiplier, so a "sane" range centred on 1.0 was clamping a real
		// measurement. Only obvious nonsense is rejected now.
		if (!isfinite(scaleYaw) || scaleYaw < 0.15f || scaleYaw > 4.0f) scaleYaw = 1.0f;
		if (!isfinite(scalePitch) || scalePitch < 0.15f || scalePitch > 4.0f) scalePitch = 1.0f;

		// Same regression over large commands only. Reported, not used - a
		// diagnostic must not be able to make the thing it is measuring
		// worse.
		sLastBigScaleYaw = (sSumCmdSqBigYaw[bestLag] > kMinEvidence)
			? (float)(sSumCmdObsBigYaw[bestLag] / sSumCmdSqBigYaw[bestLag]) : 0.0f;
		sLastBigScalePitch = (sSumCmdSqBigPitch[bestLag] > kMinEvidence)
			? (float)(sSumCmdObsBigPitch[bestLag] / sSumCmdSqBigPitch[bestLag]) : 0.0f;

		*outLag = bestLag;
		*outScaleYaw = scaleYaw;
		*outScalePitch = scalePitch;
	}

	void UpdateMotionAiming()
	{
		// Trade/customize UI code may not be decrypted when the early native
		// camera hooks install. This is a read-only signature check until the
		// exact paired controller methods become available, then installs once.
		if (InterlockedCompareExchange(&sNativeVendorCameraOwnerInstalled, 0, 0) == 0)
			InstallNativeVendorCameraOwner();

		// DISABLED - and this is the pop-in fix.
		//
		// Notes/13 "Known limitation: culling" wrote this down long ago: the
		// game culls against ITS camera, which this function aims at the GUN
		// while we render from the head. Props outside that frustum are
		// discarded before we ever see them, bounded but not removed by the
		// ~50 degree offset cap. Live symptom: the scene renders perfectly in
		// a band on the far left - where the gun is pointing - and is empty
		// everywhere else until you get close.
		//
		// It also explains why nothing else touched it. The culling FOLLOW
		// error measured 2-4 degrees because it only accounts for its own
		// injection, not this one; widening the frustum could not help
		// because the frustum was aimed elsewhere; and occlusion queries and
		// LOD were never involved.
		//
		// The same note lays out the fix - camera on the head, gun driven
		// directly, shot swung to the gun only when firing - and lists the
		// blocker as needing the weapon to point at the hand without moving
		// the camera. That blocker is gone: the weapon transform IS driven
		// directly now, and the shot is better than "swung when firing" -
		// its origin and direction are written straight into the engine's
		// fire object (confirmed KEPT on 40 of 40 shots), so aim needs no
		// camera motion at all.
		//
		// So this injection now buys nothing and costs the culling frustum.
		// Turning it off leaves the camera following the head alone, which is
		// what culling needs.
		//
		// Kept behind a flag rather than deleted: if shots ever stop landing
		// where the gun points, this is the first thing to check, because
		// that would mean the fire redirect is not covering some path.
		// REVERTED. The reasoning above was wrong: if the culling frustum
		// followed the gun, swinging the gun with the head still would make
		// geometry appear and disappear, and it demonstrably does not. The
		// note describes a real limitation of injection aiming, but it is not
		// what is causing this pop-in.
		static const bool kMotionAimInjectionEnabled = true;
		if (!kMotionAimInjectionEnabled)
			return;

		// Motion aiming without touching game memory.
		//
		// The engine welds aim to the camera, so steering aim necessarily
		// steers the camera too - which is why an earlier attempt at this
		// felt awful. But we render the view ourselves, and we know exactly
		// how far we pushed the camera, so we can subtract our own push back
		// out of what the headset sees. The game aims where the controller
		// points; the player looks where their head points.
		//
		// The offset is a PURE FUNCTION of the controller's current pose
		// relative to its anchor - not an integral of per-frame deltas.
		// That matters: an integral accumulates calibration error into
		// permanent drift, and a loop that targeted an absolute game yaw
		// would fight the player's own stick input (and could run away if
		// our radians-per-mouse-unit estimate were off). Commanding only
		// CHANGES to the offset leaves ordinary turning completely alone.
		static const bool kMotionAimingEnabled = false;   // parked - see below
		if (!kMotionAimingEnabled || !sVRSystem)
			return;

		// The game's own view angles, captured from its UNMODIFIED view
		// matrix before any of our changes - so this is genuinely how the
		// engine responded to last frame's injected input, which is what
		// the lag/scale fit needs to regress against.
		static float prevGameYaw = 0.0f, prevGamePitch = 0.0f;
		static bool havePrevGame = false;
		float gameYaw = 0.0f, gamePitch = 0.0f;
		bool gameValid = false;
		GetViewAngleSnapshot(&gameYaw, &gamePitch, &gameValid);
		if (!gameValid) {
			havePrevGame = false;
			return;
		}

		float ctrlYaw = 0.0f, ctrlPitch = 0.0f;
		if (!GetControllerAim(&ctrlYaw, &ctrlPitch)) {
			static bool sLoggedNoController = false;
			if (!sLoggedNoController) {
				LogInfo("VRPose aim: no right-hand controller is tracking; motion aiming idle\n");
				sLoggedNoController = true;
			}
			return;
		}

		// Anchor once the controller has been tracking steadily for a moment,
		// not on the very first frame that happens to be valid.
		//
		// Anchoring instantly meant the centre could be captured during a
		// menu or a load, while the game's camera was somewhere unrelated to
		// where the player would end up - which is what "my view is messed up
		// by default even holding the controller straight" looked like. The
		// recentre button below fixes it after the fact; this avoids it.
		static int sSteadyFrames = 0;
		if (!sCtrlAnchored && ++sSteadyFrames < 90)
			return;

		if (!sCtrlAnchored) {
			sAnchorCtrlYaw = ctrlYaw;
			sAnchorCtrlPitch = ctrlPitch;
			sCtrlAnchored = true;
			sBodyYaw = gameYaw;
			// Level, NOT the game's pitch at anchor time. The rendered
			// view is forced to face this heading, so anchoring pitch to
			// whatever the camera happened to be doing - mid-load, or
			// during a scripted look - tilted the horizon permanently.
			// Pitch belongs to the head in VR; the body stays level.
			sBodyPitch = 0.0f;
			sBodyAnchored = true;
			LogInfo("VRPose aim: anchored - controller (%.3f, %.3f) rad is now centre; "
				"motion aiming ACTIVE\n", ctrlYaw, ctrlPitch);
			return;
		}

		// Clamped, because the divergence between where the game's camera
		// points and where we render from is exactly this offset - and the
		// engine culls to its own camera. Keeping it bounded keeps the
		// culling mismatch bounded too.
		const float kMaxOffsetYaw = 0.87f;    // ~50 degrees
		const float kMaxOffsetPitch = 0.70f;  // ~40 degrees
		float wantYaw = WrapAngle(ctrlYaw - sAnchorCtrlYaw);
		float wantPitch = ctrlPitch - sAnchorCtrlPitch;
		if (wantYaw > kMaxOffsetYaw) wantYaw = kMaxOffsetYaw;
		if (wantYaw < -kMaxOffsetYaw) wantYaw = -kMaxOffsetYaw;
		if (wantPitch > kMaxOffsetPitch) wantPitch = kMaxOffsetPitch;
		if (wantPitch < -kMaxOffsetPitch) wantPitch = -kMaxOffsetPitch;

		// Take the tracking jitter out before it becomes gun movement. The
		// pose is re-read every frame and its noise was being commanded
		// straight through, which reads as the gun twitching rather than
		// following the hand. Light enough not to feel laggy.
		const float kWantSmooth = 0.35f;
		sSmoothWantYaw += (wantYaw - sSmoothWantYaw) * kWantSmooth;
		sSmoothWantPitch += (wantPitch - sSmoothWantPitch) * kWantSmooth;
		wantYaw = sSmoothWantYaw;
		wantPitch = sSmoothWantPitch;

		// Compensate for the game's own sensitivity before commanding, so
		// the gun's throw matches the hand's.
		//
		// The first live fit measured yaw at ~1.0 but PITCH AT 0.54 - Metro
		// moves vertically only about half as far as commanded. Targeting
		// the commanded total therefore left the gun pitching half as far as
		// the controller, which looked "correct" because the direction was
		// right. Dividing the target by the measured scale fixes the throw.
		//
		// Feedforward, not feedback: the target is divided by a
		// slowly-varying measurement rather than driven by the lagged
		// estimate of where the camera actually is. Closing the loop on a
		// 2-frame-old estimate would add dead time, and dead time in a
		// proportional loop means overshoot and oscillation - the last thing
		// this needs is the aim hunting back and forth.
		float sYaw = sLastScaleYaw > 0.15f ? sLastScaleYaw : 1.0f;
		float sPitch = sLastScalePitch > 0.15f ? sLastScalePitch : 1.0f;
		float targetCmdYaw = wantYaw / sYaw;
		float targetCmdPitch = wantPitch / sPitch;

		// Bound the compensated target too. `want` is already clamped, but a
		// small scale estimate divides into a large command, and the game's
		// own pitch limit would silently swallow the excess - leaving the
		// commanded total wound far past anything real and taking a long
		// time to unwind when the hand comes back.
		const float kMaxCmd = 2.0f;
		if (targetCmdYaw > kMaxCmd) targetCmdYaw = kMaxCmd;
		if (targetCmdYaw < -kMaxCmd) targetCmdYaw = -kMaxCmd;
		if (targetCmdPitch > kMaxCmd) targetCmdPitch = kMaxCmd;
		if (targetCmdPitch < -kMaxCmd) targetCmdPitch = -kMaxCmd;

		// Only the shortfall is commanded. Whatever integer truncation
		// leaves behind simply shows up in next frame's shortfall, so no
		// residue accumulator is needed.
		float needYaw = targetCmdYaw - sAppliedOffsetYaw;
		float needPitch = targetCmdPitch - sAppliedOffsetPitch;

		// Measured calibration: 180 frames x 6 mouse units produced 1.7188
		// rad, i.e. ~0.00159 rad per unit. Only a starting estimate now -
		// UpdateLagScaleEstimate refines the effective value continuously.
		const float kRadPerMouseUnit = 0.00159f;
		const float kDeadzoneRad = 0.003f;

		// Ease toward the target rather than jumping the whole shortfall.
		// The cancellation is only as accurate as the lag estimate, and the
		// error it leaves is proportional to how much rotation happens per
		// frame - so a high slew rate turns a small timing imprecision into
		// a large visible jolt. Live result: at ~13.7 deg/frame the view
		// shook badly on fast hand movement. A gain below 1 also smooths the
		// start and end of a swing, where the abrupt changes were worst.
		const float kGain = 0.6f;

		int dx = 0, dy = 0;
		if (fabsf(needYaw) > kDeadzoneRad)
			dx = (int)(needYaw * kGain / kRadPerMouseUnit);
		if (fabsf(needPitch) > kDeadzoneRad)
			dy = (int)(-needPitch * kGain / kRadPerMouseUnit);

		// Clamp so a tracking glitch can't fling the view. Also bounds how
		// far the cancellation can be transiently wrong while the estimator
		// settles, which matters more than raw slew rate.
		// ~5.5 deg/frame. Raised to 150 only so the linearity probe would
		// see large commands; it has its answer (LINEAR, ratio 0.990), and
		// that cap made the view shake on fast hand movement.
		const int kMaxStep = 60;
		if (dx > kMaxStep) dx = kMaxStep;
		if (dx < -kMaxStep) dx = -kMaxStep;
		if (dy > kMaxStep) dy = kMaxStep;
		if (dy < -kMaxStep) dy = -kMaxStep;

		float cmdYaw = dx * kRadPerMouseUnit;
		float cmdPitch = -dy * kRadPerMouseUnit;

		if (dx != 0 || dy != 0) {
			INPUT in;
			memset(&in, 0, sizeof(in));
			in.type = INPUT_MOUSE;
			in.mi.dwFlags = MOUSEEVENTF_MOVE;
			in.mi.dx = dx;
			in.mi.dy = dy;
			SendInput(1, &in, sizeof(INPUT));

			sAppliedOffsetYaw += cmdYaw;
			sAppliedOffsetPitch += cmdPitch;
		}

		// Record this frame's command, then fit our history against how the
		// game's camera actually responded.
		sAimHistPos = (sAimHistPos + 1) % kAimHistLen;
		sHistCmdYaw[sAimHistPos] = cmdYaw;
		sHistCmdPitch[sAimHistPos] = cmdPitch;
		sHistTotalYaw[sAimHistPos] = sAppliedOffsetYaw;
		sHistTotalPitch[sAimHistPos] = sAppliedOffsetPitch;

		int lag = 1;
		float scaleYaw = 1.0f, scalePitch = 1.0f;
		if (havePrevGame) {
			float obsYaw = WrapAngle(gameYaw - prevGameYaw);
			float obsPitch = gamePitch - prevGamePitch;
			UpdateLagScaleEstimate(obsYaw, obsPitch, &lag, &scaleYaw, &scalePitch);
			sLastScaleYaw = scaleYaw;
			sLastScalePitch = scalePitch;

			// RECOIL MEASUREMENT.
			//
			// The aim loop is feedforward - it tracks what it COMMANDED and
			// never looks at where the camera actually ended up (deliberately;
			// closing the loop on a lagged estimate would add dead time and
			// make the aim hunt). A consequence nobody had cause to notice
			// until now: rotation the GAME applies - recoil - is invisible to
			// it, so a kick is never corrected and the aim point drifts up
			// away from where the controller is pointing and stays there.
			//
			// The difference between what the camera did and what our own
			// command explains IS that game-applied rotation. Everything
			// needed was already being computed for the scale estimator:
			//
			//   unexplained = observed - (our command, lagged) * scale
			//
			// Measure before cancelling. The counter-impulse has to be sized
			// from the real kick, and "unexplained" also catches estimator
			// error and scripted camera moves, so the shape of this while
			// firing decides whether it can be isolated cleanly at all.
			{
				const int li = (((sAimHistPos - lag) % kAimHistLen) + kAimHistLen) % kAimHistLen;
				const float expectedPitch = sHistCmdPitch[li] * scalePitch;
				const float unexplainedPitch = obsPitch - expectedPitch;
				const bool firing = InterlockedCompareExchange(&sFiringNow, 0, 0) != 0;

				// NOT gated on `firing`: that gate produced zero samples in a
				// whole session even though shots were definitely fired (the
				// fire-call hook caught them), so sFiringNow - the trigger-axis
				// flag - is not a dependable "is shooting" signal. Log
				// regardless and carry the flag in the line instead, so one
				// run shows both the recoil shape AND whether that flag is
				// worth gating anything on.
				static int sRecoilLogged = 0;
				static unsigned sRecoilSkip = 0;
				if (sRecoilLogged < 400 && (sRecoilSkip++ % 15) == 0) {
					sRecoilLogged++;
					LogInfo("VRPose recoil: obsPitch=%+.5f expected=%+.5f unexplained=%+.5f "
						"lag=%d scaleP=%.3f gesture=%d firing=%d\n",
						obsPitch, expectedPitch, unexplainedPitch, lag, scalePitch,
						InterlockedCompareExchange(&sAimingNow, 0, 0) ? 1 : 0,
						firing ? 1 : 0);
				}
			}
		}
		prevGameYaw = gameYaw;
		prevGamePitch = gamePitch;
		havePrevGame = gameValid;

		// ---- Cancellation by measurement, not by prediction ----
		//
		// The previous approach subtracted a MODEL of the camera's response:
		// what we commanded, delayed by the measured lag and scaled by the
		// measured sensitivity. Every imprecision in that model - fractional
		// frame latency, frame timing jitter, any input smoothing the engine
		// applies - leaked through as view movement whenever the hand moved.
		// Tuning shrank it but could never remove it, because the error is in
		// the model rather than in its parameters.
		//
		// So don't model it. We read the game's ACTUAL camera rotation every
		// frame, and now that the player's input comes through us too, we
		// know what part of that rotation is legitimately theirs. Track a
		// body heading we own - moved only by turning - and cancel whatever
		// the difference is between the game's real camera and that heading.
		//
		// The rendered view then equals a value we control directly, so it
		// is stable by construction. There is nothing left to be imprecise
		// about: the aim injection can do whatever it likes and the view
		// cannot follow it.
		// Aim input already sent but not yet fully applied - the game is
		// still rotating from it, so observed motion cannot be attributed to
		// the player during this window.
		bool aimInFlight = false;
		for (int i = 0; i <= lag + 1 && i < kAimHistLen; i++) {
			int idx = ((sAimHistPos - i) % kAimHistLen + kAimHistLen) % kAimHistLen;
			if (fabsf(sHistCmdYaw[idx]) > 1e-4f || fabsf(sHistCmdPitch[idx]) > 1e-4f) {
				aimInFlight = true;
				break;
			}
		}

		// The body heading moves ONLY by rotation we know is deliberate -
		// the thumbstick turn we injected ourselves.
		//
		// An earlier version also credited "unexplained" camera motion to
		// the player whenever no aim command was in flight. That eroded the
		// cancellation badly: every time the hand paused inside the deadzone
		// the aim commands stopped while the camera was still settling from
		// the last one, and that settling was absorbed into the heading. Over
		// many small pauses the heading crept along behind the gun - measured
		// live at want=0.430 rad against a cancellation of only 0.105.
		//
		// Yaw scale measures 1.001, so the turn injection tracks essentially
		// exactly and there is nothing to accumulate. Any error that does
		// build up appears as a slow drift in which way "forward" points,
		// which is invisible and which the recentre button clears outright.
		//
		// PARKED. The injection+cancellation approach has a hard ceiling: the
		// engine culls to its own camera, so aiming that camera at the gun
		// makes props vanish when the hand points off-axis, and overriding the
		// camera globally collides with every other thing the engine uses it
		// for - shadow passes, scripted sequences, recoil, cutscenes. Each fix
		// narrowed which writes to touch and which motion to trust, and each
		// narrowing created a new edge case.
		//
		// Turning it off restores plain head-tracked stereo, which works, and
		// removes a large confound from the reverse-engineering probe runs.
		// VR controller input for movement is unaffected and stays on.
		// Everything here is intact for reference; see Notes/13.
		//
		// Trade-off, stated plainly: rotation from an input we do NOT inject
		// - a gamepad stick - is treated as aim and cancelled, so it will not
		// turn the view. Turning through the VR thumbstick is the supported
		// path now that controller input is handled here.
		sBodyYaw += sPendingTurnYaw;
		sPendingTurnYaw = 0.0f;

		// A level load, death or cutscene can move the game's camera
		// arbitrarily. Without this the whole offset would be cancelled
		// straight into a wildly wrong view, which is what "my view is
		// messed up by default" looked like after a load.
		if (havePrevGame && fabsf(WrapAngle(gameYaw - prevGameYaw)) > 0.5f) {
			LogInfo("VRPose aim: camera jumped %.2f rad in one frame - recentring\n",
				WrapAngle(gameYaw - prevGameYaw));
			RecenterAiming();
		}

		// Note there is no sCancelOffset computed here any more.
		// PatchMappedVRCameraData derives it from the SAME frame's camera
		// rotation that it is about to patch. Computing it here meant using
		// the previous frame's angles and applying the result to the next
		// frame's matrix - one frame of drift, worth up to a full slew step
		// (~5.5 degrees) during a fast swing. That was the residual view
		// movement that survived every round of tuning.

		static unsigned sAimLogCounter = 0;
		if ((sAimLogCounter++ % 300) == 0) {
			LogInfo("VRPose aim: want=(%.3f, %.3f) commanded=(%.3f, %.3f) cancelling=(%.3f, %.3f) rad; "
				"measured lag=%d frames, scale=(%.3f, %.3f)\n",
				wantYaw, wantPitch, sAppliedOffsetYaw, sAppliedOffsetPitch,
				WrapAngle(gameYaw - sBodyYaw), gamePitch - sBodyPitch, lag, scaleYaw, scalePitch);

			// Fire-swing viability. If the large-command scale tracks the
			// overall scale, the game applies big one-frame deltas in full
			// and a shot's camera swing can be hidden in ~2-3 frames. If it
			// is markedly lower, large inputs are being clamped or smoothed
			// and the swing would take far too long to hide.
			if (sBigSampleCount >= 40.0) {
				float ratioYaw = (scaleYaw > 0.01f && sLastBigScaleYaw > 0.0f)
					? sLastBigScaleYaw / scaleYaw : 0.0f;
				LogInfo("VRPose probe: large-command scale=(%.3f, %.3f) vs overall (%.3f, %.3f) "
					"over %.0f samples -> big/small yaw ratio %.3f - %s\n",
					sLastBigScaleYaw, sLastBigScalePitch, scaleYaw, scalePitch,
					sBigSampleCount, ratioYaw,
					(ratioYaw > 0.85f)
						? "LINEAR: a fire-swing can be snapped in 2-3 frames"
						: "NON-LINEAR: large deltas are clamped/smoothed, fire-swing too slow");
			}
		}
	}

	// Self-verifying write test. Rather than writing the controller
	// direction and relying on the player to notice a change - with no
	// working on-screen indicator and no visible log - this writes a
	// deliberately OFFSET direction and then measures whether the game's
	// own view direction, which we already observe every frame, moves
	// toward it. The log then states outright whether the address has any
	// effect, so the run produces an answer no matter what the player does.
	void RunAimWriteTest()
	{
		// Moot now that aiming goes through injection rather than memory
		// writes, and dead anyway with the scanner retired (nothing ever
		// sets sAimOverrideReady).
		static const bool kWriteTestEnabled = false;
		if (!kWriteTestEnabled || !sAimOverrideReady || !sAimVectorBase)
			return;

		enum Phase { Baseline, Writing, Done };
		static Phase phase = Baseline;
		static int frames = 0;
		static float target[3] = { 0, 0, 0 };
		static float baseFwd[3] = { 0, 0, 0 };

		if (phase == Done)
			return;

		float fwd[3];
		bool valid = false;
		GetViewForwardSnapshot(fwd, &valid);
		if (!valid)
			return;

		if (phase == Baseline) {
			memcpy(baseFwd, fwd, sizeof(baseFwd));
			// Aim target: current direction rotated ~40 degrees in yaw.
			// Large enough to be unmistakable, small enough not to look
			// like a glitch if it does take effect.
			const float a = 0.7f;
			target[0] = fwd[0] * cosf(a) + fwd[2] * sinf(a);
			target[1] = fwd[1];
			target[2] = -fwd[0] * sinf(a) + fwd[2] * cosf(a);
			frames = 0;
			phase = Writing;
			LogInfo("VRPose test: writing an offset direction (%.3f %.3f %.3f) over the game's "
				"(%.3f %.3f %.3f) for ~2s to see whether it takes effect.\n",
				target[0], target[1], target[2], fwd[0], fwd[1], fwd[2]);
			return;
		}

		// Writing phase - hold the offset direction there every frame.
		float out[3] = {
			sAimVectorSign * target[0],
			sAimVectorSign * target[1],
			sAimVectorSign * target[2],
		};
		SIZE_T written = 0;
		WriteProcessMemory(GetCurrentProcess(), sAimVectorBase, out, sizeof(out), &written);

		if (++frames > 120) {
			// Did the game's own view direction move toward what we wrote?
			float movedToTarget =
				fabsf(baseFwd[0] - target[0]) - fabsf(fwd[0] - target[0]) +
				fabsf(baseFwd[2] - target[2]) - fabsf(fwd[2] - target[2]);
			float movedAtAll =
				fabsf(fwd[0] - baseFwd[0]) + fabsf(fwd[1] - baseFwd[1]) + fabsf(fwd[2] - baseFwd[2]);

			LogInfo("VRPose test: RESULT - view moved %.4f total, %.4f of it toward the written "
				"direction (now %.3f %.3f %.3f, was %.3f %.3f %.3f)\n",
				movedAtAll, movedToTarget, fwd[0], fwd[1], fwd[2],
				baseFwd[0], baseFwd[1], baseFwd[2]);

			if (movedToTarget > 0.25f) {
				LogInfo("VRPose test: VERDICT = THE GAME READS THIS ADDRESS. Writing it steers the "
					"view/aim, so controller-driven aiming is achievable here.\n");
				LogOverlay(LOG_DIRE, "VR: WRITE WORKS - the game reads this address\n");
			} else {
				LogInfo("VRPose test: VERDICT = NO EFFECT. The address tracks the view but the game "
					"does not read it back - another derived copy. Need the upstream value.\n");
				LogOverlay(LOG_DIRE, "VR: write had no effect - derived copy\n");
			}
			phase = Done;
			sAimOverrideReady = false; // stop writing; leave the game alone
		}
	}

	void ApplyAimOverride()
	{
		// Superseded by RunAimWriteTest while we establish whether writing
		// this address does anything at all. Re-enable once that is known.
		static const bool kControllerOverrideEnabled = false;
		if (!kControllerOverrideEnabled)
			return;

		if (!sAimOverrideReady || !sAimVectorBase || !sVRSystem)
			return;

		// Controller direction, in the same frame the game's forward is
		// expressed in - GetControllerAim already does that conversion, so
		// rebuild a unit vector from the angles it returns.
		float cy = 0.0f, cp = 0.0f;
		if (!GetControllerAim(&cy, &cp))
			return;

		float dir[3];
		dir[0] = sinf(cy) * cosf(cp);
		dir[1] = sinf(cp);
		dir[2] = cosf(cy) * cosf(cp);

		float out[3] = {
			sAimVectorSign * dir[0],
			sAimVectorSign * dir[1],
			sAimVectorSign * dir[2],
		};

		SIZE_T written = 0;
		if (!WriteProcessMemory(GetCurrentProcess(), sAimVectorBase, out, sizeof(out), &written) ||
		    written != sizeof(out)) {
			static bool sLoggedWriteFail = false;
			if (!sLoggedWriteFail) {
				LogInfo("VRPose aim: write to %p failed - the page is probably read-only or has moved\n",
					(void *)sAimVectorBase);
				sLoggedWriteFail = true;
			}
			sAimOverrideReady = false;
			return;
		}

		static unsigned sOverrideLog = 0;
		if ((sOverrideLog++ % 180) == 0) {
			float gameYaw = 0.0f, gamePitch = 0.0f;
			bool v = false;
			GetViewAngleSnapshot(&gameYaw, &gamePitch, &v);
			LogInfo("VRPose aim: wrote controller direction (%.3f %.3f %.3f) to %p; "
				"game yaw now %.3f, controller yaw %.3f\n",
				out[0], out[1], out[2], (void *)sAimVectorBase, gameYaw, cy);
		}
	}

	void EnsureMemoryScannerStarted()
	{
		// Retired. Repeated content and behavioural scans consistently found
		// values that TRACK the camera but are recomputed copies rather than
		// state the engine reads back - the best candidate reached |r|=0.9995
		// and still never confirmed as a contiguous, writable orientation
		// vector. Motion aiming no longer depends on finding one (see
		// UpdateMotionAiming), so this stays off rather than burning CPU and
		// risking the crashes it caused when walking freed regions.
		static const bool kScannerEnabled = false;
		static bool sStarted = false;
		if (!kScannerEnabled || sStarted)
			return;
		sStarted = true;

		EnsureScanLock();

		sScannerThread = CreateThread(NULL, 0, MemoryScannerThread, NULL, 0, NULL);
		if (!sScannerThread)
			LogInfo("VRPose scan: failed to start the memory scanner thread\n");
	}

	void ReleaseCompositorResources()
	{
		HideVideoOverlay();
		ReleaseScaledSubmissionResources();
		ReleaseHighResolutionSceneResources();
		for (int eye = 0; eye < 2; eye++) {
			if (sCompositorTexture[eye]) {
				sCompositorTexture[eye]->Release();
				sCompositorTexture[eye] = nullptr;
			}
			sEyeTextureValid[eye] = false;
		}
		sCompositorTexWidth = 0;
		sCompositorTexHeight = 0;
		sCompositorTexFormat = DXGI_FORMAT_UNKNOWN;
	}

	bool EnsureSubmissionScalePipeline(::ID3D11Device *device)
	{
		if (sScaleVertexShader && sScalePixelShader && sScaleSampler &&
			sScaleParametersCB && sScaleRasterizer && sScaleDepthStencil &&
			sScaleBlendState)
			return true;

		static const char *vsSource =
			"struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };\n"
			"VSOut main(uint id : SV_VertexID) {\n"
			"  float2 p[3] = { float2(-1,-1), float2(-1,3), float2(3,-1) };\n"
			"  float2 t[3] = { float2(0,1), float2(0,-1), float2(2,1) };\n"
			"  VSOut o; o.pos = float4(p[id],0,1); o.uv = t[id]; return o; }\n";
		static const char *psSource =
			"Texture2D sourceImage : register(t0);\n"
			"SamplerState linearSampler : register(s0);\n"
			"cbuffer DisplayParameters : register(b0) { float brightnessGamma; float outputIsLinear; float legacyPresentation; float padding; };\n"
			"float3 LinearToSrgb(float3 colour) {\n"
			"  return lerp(colour * 12.92, 1.055 * pow(max(colour, 0), 1.0 / 2.4) - 0.055, step(0.0031308, colour)); }\n"
			"float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD0) : SV_Target {\n"
			"  float4 colour = sourceImage.Sample(linearSampler, uv);\n"
			"  colour.rgb = pow(saturate(colour.rgb), brightnessGamma);\n"
			"  if (outputIsLinear < 0.5 && legacyPresentation < 0.5)\n"
			"    colour.rgb = LinearToSrgb(saturate(colour.rgb));\n"
			"  return colour; }\n";
		ID3DBlob *vsBlob = nullptr, *psBlob = nullptr, *errors = nullptr;
		HRESULT hr = D3DCompile(vsSource, strlen(vsSource), "MetroVRScaleVS", nullptr,
			nullptr, "main", "vs_5_0", 0, 0, &vsBlob, &errors);
		if (FAILED(hr)) {
			if (errors) errors->Release();
			return false;
		}
		hr = device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
			nullptr, &sScaleVertexShader);
		vsBlob->Release();
		if (FAILED(hr)) {
			if (errors) errors->Release();
			return false;
		}
		hr = D3DCompile(psSource, strlen(psSource), "MetroVRScalePS", nullptr,
			nullptr, "main", "ps_5_0", 0, 0, &psBlob, &errors);
		if (FAILED(hr)) {
			if (errors) errors->Release();
			return false;
		}
		hr = device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(),
			nullptr, &sScalePixelShader);
		psBlob->Release();
		if (FAILED(hr)) {
			if (errors) errors->Release();
			return false;
		}
		D3D11_SAMPLER_DESC sampler = {};
		sampler.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
		sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		sampler.MinLOD = 0;
		sampler.MaxLOD = D3D11_FLOAT32_MAX;
		hr = device->CreateSamplerState(&sampler, &sScaleSampler);
		if (FAILED(hr)) {
			sScaleVertexShader->Release(); sScaleVertexShader = nullptr;
			sScalePixelShader->Release(); sScalePixelShader = nullptr;
			return false;
		}
		D3D11_BUFFER_DESC cbDesc = {};
		cbDesc.ByteWidth = 16;
		cbDesc.Usage = D3D11_USAGE_DEFAULT;
		cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		hr = device->CreateBuffer(&cbDesc, nullptr, &sScaleParametersCB);
		if (FAILED(hr)) {
			sScaleVertexShader->Release(); sScaleVertexShader = nullptr;
			sScalePixelShader->Release(); sScalePixelShader = nullptr;
			sScaleSampler->Release(); sScaleSampler = nullptr;
			return false;
		}
		D3D11_RASTERIZER_DESC rasterDesc = {};
		rasterDesc.FillMode = D3D11_FILL_SOLID;
		rasterDesc.CullMode = D3D11_CULL_NONE;
		rasterDesc.DepthClipEnable = TRUE;
		hr = device->CreateRasterizerState(&rasterDesc, &sScaleRasterizer);
		D3D11_DEPTH_STENCIL_DESC depthDesc = {};
		depthDesc.DepthEnable = FALSE;
		depthDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
		depthDesc.DepthFunc = D3D11_COMPARISON_ALWAYS;
		if (SUCCEEDED(hr))
			hr = device->CreateDepthStencilState(&depthDesc,
				&sScaleDepthStencil);
		D3D11_BLEND_DESC blendDesc = {};
		blendDesc.RenderTarget[0].BlendEnable = FALSE;
		blendDesc.RenderTarget[0].RenderTargetWriteMask =
			D3D11_COLOR_WRITE_ENABLE_ALL;
		if (SUCCEEDED(hr))
			hr = device->CreateBlendState(&blendDesc, &sScaleBlendState);
		if (FAILED(hr)) {
			if (sScaleRasterizer) { sScaleRasterizer->Release(); sScaleRasterizer = nullptr; }
			if (sScaleDepthStencil) { sScaleDepthStencil->Release(); sScaleDepthStencil = nullptr; }
			if (sScaleBlendState) { sScaleBlendState->Release(); sScaleBlendState = nullptr; }
			sScaleVertexShader->Release(); sScaleVertexShader = nullptr;
			sScalePixelShader->Release(); sScalePixelShader = nullptr;
			sScaleSampler->Release(); sScaleSampler = nullptr;
			sScaleParametersCB->Release(); sScaleParametersCB = nullptr;
			return false;
		}
		return true;
	}

	bool CaptureHighResolutionScene(::ID3D11Texture2D *sceneTexture,
		::ID3D11DeviceContext *context, unsigned frame)
	{
		if (!sceneTexture || !context ||
			VRMenu::EffectiveSceneResolutionScale() <= 1.0001f ||
			!StereoTwin::gDoubleDraw)
			return false;

		D3D11_TEXTURE2D_DESC sourceDesc = {};
		sceneTexture->GetDesc(&sourceDesc);
		if (sourceDesc.ArraySize < 2 || sourceDesc.MipLevels < 1 ||
			sourceDesc.SampleDesc.Count != 1 ||
			sourceDesc.Format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
			return false;

		ID3D11Device *device = nullptr;
		sceneTexture->GetDevice(&device);
		if (!device)
			return false;
		if (!EnsureSubmissionScalePipeline(device)) {
			device->Release();
			return false;
		}

		const DXGI_FORMAT desiredOutputFormat = VRMenu::GetSettings().brightnessCorrection
			? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
		const UINT outputWidth = sourceDesc.Width;
		const UINT outputHeight = sourceDesc.Height;
		D3D11_TEXTURE2D_DESC existingOutputDesc = {};
		if (sHighResolutionEyeTexture[0])
			sHighResolutionEyeTexture[0]->GetDesc(&existingOutputDesc);
		const bool recreateOutputs = !sHighResolutionEyeTexture[0] ||
			!sHighResolutionEyeTexture[1] ||
			sHighResolutionWidth != outputWidth ||
			sHighResolutionHeight != outputHeight ||
			existingOutputDesc.Format != desiredOutputFormat;
		if (recreateOutputs) {
			ReleaseHighResolutionSceneResources();
			D3D11_TEXTURE2D_DESC outputDesc = sourceDesc;
			outputDesc.Width = outputWidth;
			outputDesc.Height = outputHeight;
			outputDesc.MipLevels = 1;
			outputDesc.ArraySize = 1;
			// Metro samples the sRGB scene (which decodes it to linear) and writes
			// those values to its linear backbuffer. Keep the isolated eye image
			// linear and high precision through OpenVR submission.
			outputDesc.Format = desiredOutputFormat;
			outputDesc.Usage = D3D11_USAGE_DEFAULT;
			outputDesc.BindFlags = D3D11_BIND_RENDER_TARGET |
				D3D11_BIND_SHADER_RESOURCE;
			outputDesc.CPUAccessFlags = 0;
			outputDesc.MiscFlags = 0;
			// Register the left output with the existing twin system. It creates
			// the matching right resource/view, so every menu draw that Metro's
			// stereo machinery repeats naturally lands in the correct eye.
			if (FAILED(device->CreateTexture2D(&outputDesc, nullptr,
				&sHighResolutionEyeTexture[0]))) {
				ReleaseHighResolutionSceneResources();
				device->Release();
				return false;
			}
			StereoTwin::OnCreateTexture2D(device, &outputDesc,
				sHighResolutionEyeTexture[0], false);
			if (FAILED(device->CreateRenderTargetView(
				sHighResolutionEyeTexture[0], nullptr,
				&sHighResolutionEyeRTV[0]))) {
				ReleaseHighResolutionSceneResources();
				device->Release();
				return false;
			}
			StereoTwin::OnCreateRTV(device, sHighResolutionEyeTexture[0],
				nullptr, sHighResolutionEyeRTV[0]);
			sHighResolutionEyeRTV[1] =
				StereoTwin::TwinRTV(sHighResolutionEyeRTV[0]);
			if (sHighResolutionEyeRTV[1])
				sHighResolutionEyeRTV[1]->AddRef();
			ID3D11Resource *rightResource = nullptr;
			if (sHighResolutionEyeRTV[1])
				sHighResolutionEyeRTV[1]->GetResource(&rightResource);
			if (rightResource) {
				rightResource->QueryInterface(__uuidof(ID3D11Texture2D),
					(void **)&sHighResolutionEyeTexture[1]);
				rightResource->Release();
			}
			if (!sHighResolutionEyeRTV[1] || !sHighResolutionEyeTexture[1]) {
				ReleaseHighResolutionSceneResources();
				device->Release();
				return false;
			}
			sHighResolutionWidth = outputWidth;
			sHighResolutionHeight = outputHeight;
			LogInfo("VR resolution: isolated final-colour eyes created %ux%u fmt=%d\n",
				sHighResolutionWidth, sHighResolutionHeight, outputDesc.Format);
		}

		if (!sHighResolutionScenePS) {
			static const char *psSource =
				"Texture2DArray sourceImage : register(t0);\n"
				"SamplerState linearSampler : register(s0);\n"
				"cbuffer EyeParameters : register(b0) { float eyeSlice; float3 padding; };\n"
				"float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD0) : SV_Target {\n"
				"  return sourceImage.SampleLevel(linearSampler, float3(uv, eyeSlice), 0); }\n";
			ID3DBlob *blob = nullptr, *errors = nullptr;
			HRESULT hr = D3DCompile(psSource, strlen(psSource),
				"MetroVRHighResolutionScenePS", nullptr, nullptr, "main", "ps_5_0",
				0, 0, &blob, &errors);
			if (SUCCEEDED(hr))
				hr = device->CreatePixelShader(blob->GetBufferPointer(),
					blob->GetBufferSize(), nullptr, &sHighResolutionScenePS);
			if (blob) blob->Release();
			if (errors) errors->Release();
			if (FAILED(hr) || !sHighResolutionScenePS) {
				device->Release();
				return false;
			}

			D3D11_BUFFER_DESC cbDesc = {};
			cbDesc.ByteWidth = 16;
			cbDesc.Usage = D3D11_USAGE_DEFAULT;
			cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
			if (FAILED(device->CreateBuffer(&cbDesc, nullptr,
				&sHighResolutionSceneCB))) {
				sHighResolutionScenePS->Release();
				sHighResolutionScenePS = nullptr;
				device->Release();
				return false;
			}
		}

		if (sHighResolutionSceneSource != sceneTexture) {
			if (sHighResolutionSceneSRV) { sHighResolutionSceneSRV->Release(); sHighResolutionSceneSRV = nullptr; }
			if (sHighResolutionSceneSource) { sHighResolutionSceneSource->Release(); sHighResolutionSceneSource = nullptr; }
			D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
			srvDesc.Format = sourceDesc.Format;
			srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
			srvDesc.Texture2DArray.MostDetailedMip = 0;
			srvDesc.Texture2DArray.MipLevels = 1;
			srvDesc.Texture2DArray.FirstArraySlice = 0;
			srvDesc.Texture2DArray.ArraySize = 2;
			if (FAILED(device->CreateShaderResourceView(sceneTexture, &srvDesc,
				&sHighResolutionSceneSRV))) {
				device->Release();
				return false;
			}
			sHighResolutionSceneSource = sceneTexture;
			sHighResolutionSceneSource->AddRef();
		}

		ID3D11RenderTargetView *oldRTV = nullptr;
		ID3D11DepthStencilView *oldDSV = nullptr;
		ID3D11InputLayout *oldLayout = nullptr;
		D3D11_PRIMITIVE_TOPOLOGY oldTopology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
		ID3D11VertexShader *oldVS = nullptr;
		ID3D11PixelShader *oldPS = nullptr;
		ID3D11SamplerState *oldSampler = nullptr;
		ID3D11ShaderResourceView *oldSRV = nullptr;
		ID3D11Buffer *oldCB = nullptr;
		D3D11_VIEWPORT oldViewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
		D3D11_RECT oldScissors[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
		UINT oldViewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
		UINT oldScissorCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
		context->OMGetRenderTargets(1, &oldRTV, &oldDSV);
		context->IAGetInputLayout(&oldLayout);
		context->IAGetPrimitiveTopology(&oldTopology);
		context->VSGetShader(&oldVS, nullptr, nullptr);
		context->PSGetShader(&oldPS, nullptr, nullptr);
		context->PSGetSamplers(0, 1, &oldSampler);
		context->PSGetShaderResources(0, 1, &oldSRV);
		context->PSGetConstantBuffers(0, 1, &oldCB);
		context->RSGetViewports(&oldViewportCount, oldViewports);
		context->RSGetScissorRects(&oldScissorCount, oldScissors);

		D3D11_VIEWPORT viewport = { 0, 0, (float)sHighResolutionWidth,
			(float)sHighResolutionHeight, 0, 1 };
		D3D11_RECT scissor = { 0, 0, (LONG)sHighResolutionWidth,
			(LONG)sHighResolutionHeight };
		context->RSSetViewports(1, &viewport);
		context->RSSetScissorRects(1, &scissor);
		context->IASetInputLayout(nullptr);
		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		context->VSSetShader(sScaleVertexShader, nullptr, 0);
		context->PSSetShader(sHighResolutionScenePS, nullptr, 0);
		context->PSSetSamplers(0, 1, &sScaleSampler);
		context->PSSetShaderResources(0, 1, &sHighResolutionSceneSRV);
		for (int eye = 0; eye < 2; ++eye) {
			const float params[4] = { (float)eye, 0, 0, 0 };
			context->UpdateSubresource(sHighResolutionSceneCB, 0, nullptr,
				params, 0, 0);
			context->PSSetConstantBuffers(0, 1, &sHighResolutionSceneCB);
			context->OMSetRenderTargets(1, &sHighResolutionEyeRTV[eye], nullptr);
			context->Draw(3, 0);
		}

		context->PSSetShaderResources(0, 1, &oldSRV);
		context->PSSetConstantBuffers(0, 1, &oldCB);
		context->PSSetSamplers(0, 1, &oldSampler);
		context->VSSetShader(oldVS, nullptr, 0);
		context->PSSetShader(oldPS, nullptr, 0);
		context->IASetInputLayout(oldLayout);
		context->IASetPrimitiveTopology(oldTopology);
		if (oldViewportCount) context->RSSetViewports(oldViewportCount, oldViewports);
		if (oldScissorCount) context->RSSetScissorRects(oldScissorCount, oldScissors);
		context->OMSetRenderTargets(1, &oldRTV, oldDSV);
		if (oldCB) oldCB->Release();
		if (oldSRV) oldSRV->Release();
		if (oldSampler) oldSampler->Release();
		if (oldVS) oldVS->Release();
		if (oldPS) oldPS->Release();
		if (oldLayout) oldLayout->Release();
		if (oldRTV) oldRTV->Release();
		if (oldDSV) oldDSV->Release();
		sHighResolutionFrame = frame;
		device->Release();
		return true;
	}

	bool GetHighResolutionOverlayTarget(::ID3D11RenderTargetView **leftView,
		unsigned *width, unsigned *height)
	{
		if (!leftView || !width || !height || !sHighResolutionEyeRTV[0])
			return false;
		*leftView = sHighResolutionEyeRTV[0];
		*width = sHighResolutionWidth;
		*height = sHighResolutionHeight;
		return true;
	}

	bool GetRuntimeSubmissionSize(unsigned *width, unsigned *height)
	{
		if (!width || !height || sRecommendedEyeWidth < 640 ||
			sRecommendedEyeHeight < 360 || sRecommendedEyeWidth > 16384 ||
			sRecommendedEyeHeight > 16384)
			return false;
		*width = sRecommendedEyeWidth;
		*height = sRecommendedEyeHeight;
		return true;
	}

	ID3D11Texture2D *PrepareSubmissionTexture(::ID3D11Device *device,
		::ID3D11DeviceContext *context, int eye, ID3D11Texture2D *source,
		bool sourceIsLinear, bool legacyPresentation)
	{
		const float brightness = max(0.0f,
			min(1.0f, VRMenu::GetSettings().brightness));
		const bool brightnessCorrection = VRMenu::GetSettings().brightnessCorrection;
		const bool neutralBrightness = fabsf(brightness - 0.5f) < 0.0001f;
		if (!source)
			return source;
		D3D11_TEXTURE2D_DESC sourceDesc = {};
		source->GetDesc(&sourceDesc);
		const VRCompatibility::SubmissionTargetSize submissionTarget =
			VRCompatibility::SelectSubmissionTargetSize(
				sRecommendedEyeWidth, sRecommendedEyeHeight,
				sourceDesc.Width, sourceDesc.Height);
		const bool targetMatchesSource =
			submissionTarget.width == sourceDesc.Width &&
			submissionTarget.height == sourceDesc.Height;
		if (targetMatchesSource && neutralBrightness && !sourceIsLinear)
			return source;
		// The source is treated as linear for this pass. Encode the result back
		// to display-gamma before submitting because OpenVR Auto treats 8-bit
		// textures as display-native gamma data.
		// Keep the midpoint neutral, with a restrained darkening range. A much
		// higher exponent collapses near-black detail into large blotchy areas.
		// The legacy path retains the original slider mapping. The corrected
		// path is shifted so the appearance previously reached at slider 0.00
		// is reached at the neutral-looking slider position 0.50.
		const float brightnessGamma = brightnessCorrection
			? powf(2.0f, (1.0f - brightness) * 2.0f)
			: powf(2.0f, (0.5f - brightness) * 2.0f);
		const UINT width = submissionTarget.width;
		const UINT height = submissionTarget.height;
		if (!sScaledCompositorTexture[eye] || sScaledCompositorWidth != width ||
			sScaledCompositorHeight != height || sScaledCompositorFormat != sourceDesc.Format) {
			ReleaseScaledSubmissionResources();
			D3D11_TEXTURE2D_DESC desc = sourceDesc;
			desc.Width = width;
			desc.Height = height;
			desc.MipLevels = 1;
			desc.ArraySize = 1;
			desc.SampleDesc.Count = 1;
			desc.SampleDesc.Quality = 0;
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
			desc.CPUAccessFlags = 0;
			desc.MiscFlags = 0;
			for (int i = 0; i < 2; i++) {
				if (FAILED(device->CreateTexture2D(&desc, nullptr, &sScaledCompositorTexture[i])) ||
					FAILED(device->CreateRenderTargetView(sScaledCompositorTexture[i], nullptr,
						&sScaledCompositorRTV[i]))) {
					ReleaseScaledSubmissionResources();
					CompatibilityLog("submission_target=allocation-failed requested=%ux%u "
						"source=%ux%u\n", width, height,
						sourceDesc.Width, sourceDesc.Height);
					return source;
				}
			}
			sScaledCompositorWidth = width;
			sScaledCompositorHeight = height;
			sScaledCompositorFormat = sourceDesc.Format;
			LogInfo("VR resolution: compositor submission %ux%u -> %ux%u%s\n",
				sourceDesc.Width, sourceDesc.Height, width, height,
				submissionTarget.usedRuntimeRecommendation
					? " (OpenVR recommended)" : " (source fallback)");
			CompatibilityLog("submission_target=%ux%u source=%ux%u runtime_shape=%d\n",
				width, height, sourceDesc.Width, sourceDesc.Height,
				submissionTarget.usedRuntimeRecommendation ? 1 : 0);
		}
		if (!EnsureSubmissionScalePipeline(device))
			return source;
		ID3D11ShaderResourceView *sourceView = nullptr;
		if (FAILED(device->CreateShaderResourceView(source, nullptr, &sourceView)) || !sourceView)
			return source;
		D3D11_VIEWPORT viewport = { 0, 0, (float)width, (float)height, 0, 1 };
		ID3D11RenderTargetView *oldRTV = nullptr;
		ID3D11DepthStencilView *oldDSV = nullptr;
		context->OMGetRenderTargets(1, &oldRTV, &oldDSV);
		UINT oldViewportCount =
			D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
		D3D11_VIEWPORT oldViewports[
			D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
		context->RSGetViewports(&oldViewportCount, oldViewports);
		ID3D11InputLayout *oldLayout = nullptr;
		D3D11_PRIMITIVE_TOPOLOGY oldTopology =
			D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
		ID3D11RasterizerState *oldRasterizer = nullptr;
		ID3D11DepthStencilState *oldDepthStencil = nullptr;
		UINT oldStencilRef = 0;
		ID3D11BlendState *oldBlendState = nullptr;
		float oldBlendFactor[4] = {};
		UINT oldSampleMask = 0;
		ID3D11VertexShader *oldVS = nullptr;
		ID3D11PixelShader *oldPS = nullptr;
		ID3D11SamplerState *oldSampler = nullptr;
		ID3D11ShaderResourceView *oldSRV = nullptr;
		ID3D11Buffer *oldCB = nullptr;
		context->IAGetInputLayout(&oldLayout);
		context->IAGetPrimitiveTopology(&oldTopology);
		context->RSGetState(&oldRasterizer);
		context->OMGetDepthStencilState(&oldDepthStencil, &oldStencilRef);
		context->OMGetBlendState(&oldBlendState, oldBlendFactor,
			&oldSampleMask);
		context->VSGetShader(&oldVS, nullptr, nullptr);
		context->PSGetShader(&oldPS, nullptr, nullptr);
		context->PSGetSamplers(0, 1, &oldSampler);
		context->PSGetShaderResources(0, 1, &oldSRV);
		context->PSGetConstantBuffers(0, 1, &oldCB);
		context->OMSetRenderTargets(1, &sScaledCompositorRTV[eye], nullptr);
		context->RSSetViewports(1, &viewport);
		context->RSSetState(sScaleRasterizer);
		context->OMSetDepthStencilState(sScaleDepthStencil, 0);
		const float noBlendFactor[4] = {};
		context->OMSetBlendState(sScaleBlendState, noBlendFactor, 0xffffffff);
		context->IASetInputLayout(nullptr);
		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		context->VSSetShader(sScaleVertexShader, nullptr, 0);
		context->PSSetShader(sScalePixelShader, nullptr, 0);
		context->PSSetSamplers(0, 1, &sScaleSampler);
		context->PSSetShaderResources(0, 1, &sourceView);
		const float displayParameters[4] = { brightnessGamma,
			sourceIsLinear ? 1.0f : 0.0f,
			legacyPresentation ? 1.0f : 0.0f, 0 };
		context->UpdateSubresource(sScaleParametersCB, 0, nullptr,
			displayParameters, 0, 0);
		context->PSSetConstantBuffers(0, 1, &sScaleParametersCB);
		context->Draw(3, 0);
		context->PSSetShaderResources(0, 1, &oldSRV);
		context->PSSetConstantBuffers(0, 1, &oldCB);
		context->PSSetSamplers(0, 1, &oldSampler);
		context->VSSetShader(oldVS, nullptr, 0);
		context->PSSetShader(oldPS, nullptr, 0);
		context->IASetInputLayout(oldLayout);
		context->IASetPrimitiveTopology(oldTopology);
		context->RSSetState(oldRasterizer);
		context->OMSetDepthStencilState(oldDepthStencil, oldStencilRef);
		context->OMSetBlendState(oldBlendState, oldBlendFactor,
			oldSampleMask);
		if (oldViewportCount)
			context->RSSetViewports(oldViewportCount, oldViewports);
		context->OMSetRenderTargets(1, &oldRTV, oldDSV);
		if (oldSRV) oldSRV->Release();
		if (oldCB) oldCB->Release();
		if (oldSampler) oldSampler->Release();
		if (oldVS) oldVS->Release();
		if (oldPS) oldPS->Release();
		if (oldLayout) oldLayout->Release();
		if (oldRasterizer) oldRasterizer->Release();
		if (oldDepthStencil) oldDepthStencil->Release();
		if (oldBlendState) oldBlendState->Release();
		if (oldRTV) oldRTV->Release();
		if (oldDSV) oldDSV->Release();
		sourceView->Release();
		return sScaledCompositorTexture[eye];
	}

	// Set by holding the right controller's menu button. Waiting on a frame
	// count photographed the main menu instead of the game, and the intro
	// video plus menus take far longer than any fixed delay can predict - so
	// the player picks the moment, standing in front of whatever matters.
	static bool sEyeDumpRequested = false;

	void RequestEyeDump()
	{
		sEyeDumpRequested = true;
		LogInfo("VRPose: eye dump requested - capturing both eyes this frame\n");
	}

	// Writes the two eye images to disk, once per session.
	//
	// Every counter says characters receive per-eye matrices and every number
	// says those matrices are correct, yet they show no depth. Rather than
	// ask a person to judge whether something shifted inside a headset - a
	// question I twice described badly - the two submitted images can just be
	// compared pixel by pixel afterwards. Whatever is offset between them is
	// getting stereo; whatever sits at identical coordinates is not.
	static void DumpBothEyesOnce(::ID3D11Device *device, ::ID3D11DeviceContext *context)
	{
		static bool done = false;
		if (done || !sCompositorTexture[0] || !sCompositorTexture[1])
			return;
		if (!sEyeDumpRequested)
			return;
		sEyeDumpRequested = false;

		done = true;

		D3D11_TEXTURE2D_DESC d;
		sCompositorTexture[0]->GetDesc(&d);

		D3D11_TEXTURE2D_DESC sd = d;
		sd.Usage = D3D11_USAGE_STAGING;
		sd.BindFlags = 0;
		sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		sd.MiscFlags = 0;

		for (int eye = 0; eye < 2; eye++) {
			ID3D11Texture2D *stage = NULL;
			if (FAILED(device->CreateTexture2D(&sd, NULL, &stage)) || !stage) {
				LogInfo("VRPose: eye dump - could not create staging texture\n");
				return;
			}
			context->CopyResource(stage, sCompositorTexture[eye]);

			D3D11_MAPPED_SUBRESOURCE m;
			if (FAILED(context->Map(stage, 0, D3D11_MAP_READ, 0, &m))) {
				LogInfo("VRPose: eye dump - Map failed\n");
				stage->Release();
				return;
			}

			char path[256];
			sprintf_s(path, "%s\\eye%d.bmp", "C:\\Projects\\Metro2033ReduxVR\\GameReferences", eye);
			FILE *fp = NULL;
			fopen_s(&fp, path, "wb");
			if (fp) {
				const UINT w = d.Width, h = d.Height;
				const UINT rowBytes = w * 4;
				const UINT imgBytes = rowBytes * h;
				unsigned char fh[14] = { 'B', 'M' };
				const UINT fileSize = 14 + 40 + imgBytes;
				memcpy(fh + 2, &fileSize, 4);
				const UINT off = 54;
				memcpy(fh + 10, &off, 4);
				fwrite(fh, 1, 14, fp);

				unsigned char ih[40] = { 0 };
				const UINT hdrSize = 40;
				memcpy(ih + 0, &hdrSize, 4);
				memcpy(ih + 4, &w, 4);
				memcpy(ih + 8, &h, 4);
				const unsigned short planes = 1, bpp = 32;
				memcpy(ih + 12, &planes, 2);
				memcpy(ih + 14, &bpp, 2);
				memcpy(ih + 20, &imgBytes, 4);
				fwrite(ih, 1, 40, fp);

				// BMP rows run bottom-up.
				for (int y = (int)h - 1; y >= 0; y--)
					fwrite((unsigned char *)m.pData + (size_t)y * m.RowPitch, 1, rowBytes, fp);
				fclose(fp);
				LogInfo("VRPose: wrote %s (%ux%u)\n", path, w, h);
			}

			context->Unmap(stage, 0);
			stage->Release();
		}

		// And the SCENE target with its twin, before any of the final blit.
		//
		// The back buffer pair came back identical even with a quarter-screen
		// shift forced into the right eye, so the second eye's render is not
		// arriving there. This bisects it: if the scene twin HAS the shift,
		// eye two renders correctly and is lost on the way to the back
		// buffer. If it does not, the second eye never rendered differently
		// at all and the fault is upstream of every target.
		{
			StereoTwin::CompareAllTwins(device, context);

			ID3D11Texture2D *o = NULL, *t = NULL;
			if (StereoTwin::GetTwinPair(3620, 2009, 44, 0, &o, &t) && o && t) {
				ID3D11Texture2D *src[2] = { o, t };
				D3D11_TEXTURE2D_DESC od;
				o->GetDesc(&od);
				D3D11_TEXTURE2D_DESC ssd = od;
				ssd.Usage = D3D11_USAGE_STAGING;
				ssd.BindFlags = 0;
				ssd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
				ssd.MiscFlags = 0;
				for (int i = 0; i < 2; i++) {
					ID3D11Texture2D *stage2 = NULL;
					if (FAILED(device->CreateTexture2D(&ssd, NULL, &stage2)) || !stage2)
						break;
					context->CopyResource(stage2, src[i]);
					D3D11_MAPPED_SUBRESOURCE m2;
					if (SUCCEEDED(context->Map(stage2, 0, D3D11_MAP_READ, 0, &m2))) {
						char path2[256];
						sprintf_s(path2, "C:\\Projects\\Metro2033ReduxVR\\GameReferences\\depth%d.raw", i);
						FILE *f2 = NULL;
						fopen_s(&f2, path2, "wb");
						if (f2) {
							unsigned hdr[3] = { od.Width, od.Height, (unsigned)od.Format };
							fwrite(hdr, 4, 3, f2);
							for (UINT y = 0; y < od.Height; y++)
								fwrite((unsigned char *)m2.pData + (size_t)y * m2.RowPitch, 1, od.Width * 4, f2);
							fclose(f2);
							LogInfo("VRPose: wrote %s (%ux%u fmt=%d)\n", path2, od.Width, od.Height, od.Format);
						}
						context->Unmap(stage2, 0);
					}
					stage2->Release();
				}
			} else {
				LogInfo("VRPose: no 3620x2009 twin pair available to photograph\n");
			}
		}
	}

	void SubmitFrameToCompositor(::IDXGISwapChain *swapChain, ::ID3D11Device *device, ::ID3D11DeviceContext *context)
	{
		sCompositorFramePacingEnabled = false;
		if (IsOpenXRSelected()) {
			static bool logged = false;
			if (!swapChain || !device || !context)
				return;
			ID3D11Texture2D *backBuffer = nullptr;
			if (FAILED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&backBuffer)) || !backBuffer)
				return;
			ID3D11Texture2D *rightBackBuffer =
				(ID3D11Texture2D *)StereoTwin::TwinResource(backBuffer);
			if (!rightBackBuffer)
				rightBackBuffer = backBuffer;
			if (sOpenXRRuntime.CreateD3D11Session(device)) {
				const bool submitted = sOpenXRRuntime.SubmitD3D11Frame(
					context, backBuffer, rightBackBuffer);
				if (!logged) {
					LogInfo("VRPose: OpenXR D3D11 session created; first frame submission=%s\n",
						submitted ? "accepted" : "not yet running");
					logged = true;
				}
			} else if (!logged) {
					LogInfo("VRPose: OpenXR D3D11 session creation failed at %s (result=%d)\n",
						sOpenXRRuntime.LastError(), sOpenXRRuntime.LastResult());
				logged = true;
			}
			backBuffer->Release();
			return;
		}
		// Master switch for milestone 2. Everything below is inert
		// while this is false, so a build can be shipped/tested with
		// head tracking only (milestone 1 behaviour) by flipping it.
		static const bool kSubmitToCompositorEnabled = true;
		if (!kSubmitToCompositorEnabled)
			return;

		if (!sVRSystem || !swapChain || !device || !context)
			return;

		// Time since the previous Present reached us. A long gap here
		// means the GAME stopped rendering by itself (level load,
		// video transition) - as opposed to a long WaitGetPoses below,
		// which would mean the compositor stalled us. Between the two
		// measurements the next test can attribute the stall properly
		// instead of us inferring it.
		{
			LARGE_INTEGER freq, now;
			QueryPerformanceFrequency(&freq);
			QueryPerformanceCounter(&now);
			static LONGLONG sPrevPresent = 0;
			if (sPrevPresent != 0) {
				double gapMs = (double)(now.QuadPart - sPrevPresent) * 1000.0 / (double)freq.QuadPart;
				static int sGapLogCount = 0;
				if (gapMs > 500.0 && sGapLogCount < 20) {
					LogInfo("VRPose: %.0f ms since the previous frame reached Present - "
						"the game itself paused rendering (load/transition), not the compositor\n", gapMs);
					sGapLogCount++;
				}
			}
			sPrevPresent = now.QuadPart;
		}

		// Safety cutoff. WaitGetPoses below blocks by design, pacing us
		// to the compositor - which is fine while frames are being
		// accepted, but if submission is broken the game ends up
		// throttled against a compositor that will never show our
		// frames, and it looks hung (observed: game stuck while SteamVR
		// displayed its "waiting for application" room). Rather than
		// leave the user with a frozen game, give up on the compositor
		// path entirely after a run of failures and fall back to
		// milestone-1 behaviour (head tracking, flat output), which is
		// degraded but perfectly playable.
		static int sConsecutiveFailures = 0;
		if (sCompositorGivenUp)
			return;
		if (sConsecutiveFailures > 200) {
			sCompositorGivenUp = true;
			LogInfo("VRPose: giving up on compositor submission after %d consecutive failures - "
				"continuing with head tracking only so the game stays responsive\n", sConsecutiveFailures);
			CompatibilityLog("compositor=give-up consecutive_submission_failures=%d\n",
				sConsecutiveFailures);
			return;
		}

		vr::IVRCompositor *compositor = vr::VRCompositor();
		if (!compositor) {
			static bool sLoggedNoCompositor = false;
			if (!sLoggedNoCompositor) {
				LogInfo("VRPose: no OpenVR compositor available, frame submission disabled\n");
				CompatibilityLog("compositor=unavailable\n");
				sLoggedNoCompositor = true;
			}
			return;
		}
		sCompositorFramePacingEnabled = true;
		// Every pose used to build Metro's camera is in the standing universe.
		// Explicitly make the compositor interpret submitted texture poses in
		// that same space; relying on its default can make pose reprojection use
		// a different origin than the pixels were rendered from.
		compositor->SetTrackingSpace(vr::TrackingUniverseStanding);

		// One-time adapter check. VRCompositorError_SharedTexturesNotSupported
		// (106) persisted regardless of the submission texture's
		// MiscFlags, which points away from texture creation and at a
		// device/adapter-level cause: if the game's D3D device and
		// SteamVR's compositor live on different GPUs, cross-adapter
		// texture sharing genuinely isn't possible and the compositor
		// says exactly this. This machine has both an RTX 5070 Ti and
		// an AMD integrated GPU, so it's a real possibility rather than
		// a theoretical one. GetDXGIOutputInfo reports the adapter
		// index SteamVR expects to be rendered on; compare it against
		// the adapter our device is actually on.
		static bool sLoggedAdapterInfo = false;
		if (!sLoggedAdapterInfo) {
			sLoggedAdapterInfo = true;

			int32_t vrAdapterIndex = -1;
			sVRSystem->GetDXGIOutputInfo(&vrAdapterIndex);

			IDXGIDevice *dxgiDevice = nullptr;
			IDXGIAdapter *gameAdapter = nullptr;
			DXGI_ADAPTER_DESC gameDesc;
			memset(&gameDesc, 0, sizeof(gameDesc));
			bool haveGameAdapter = false;
			if (SUCCEEDED(device->QueryInterface(__uuidof(IDXGIDevice), (void **)&dxgiDevice)) && dxgiDevice) {
				if (SUCCEEDED(dxgiDevice->GetAdapter(&gameAdapter)) && gameAdapter) {
					if (SUCCEEDED(gameAdapter->GetDesc(&gameDesc)))
						haveGameAdapter = true;
				}
				dxgiDevice->Release();
			}

			if (haveGameAdapter) {
				LogInfo("VRPose: game is rendering on adapter \"%S\" (LUID %08x:%08x)\n",
					gameDesc.Description, gameDesc.AdapterLuid.HighPart, gameDesc.AdapterLuid.LowPart);
				CompatibilityLog("game_adapter=\"%S\" luid=%08x:%08x\n",
					gameDesc.Description, gameDesc.AdapterLuid.HighPart,
					gameDesc.AdapterLuid.LowPart);
			} else {
				LogInfo("VRPose: could not determine the game's DXGI adapter\n");
				CompatibilityLog("game_adapter=unknown\n");
			}

			// Resolve SteamVR's requested adapter index to a name so the
			// comparison is readable rather than a bare number.
			IDXGIFactory *factory = nullptr;
			if (SUCCEEDED(CreateDXGIFactory(__uuidof(IDXGIFactory), (void **)&factory)) && factory) {
				IDXGIAdapter *vrAdapter = nullptr;
				if (vrAdapterIndex >= 0 && SUCCEEDED(factory->EnumAdapters((UINT)vrAdapterIndex, &vrAdapter)) && vrAdapter) {
					DXGI_ADAPTER_DESC vrDesc;
					if (SUCCEEDED(vrAdapter->GetDesc(&vrDesc))) {
						bool sameAdapter = haveGameAdapter &&
							vrDesc.AdapterLuid.HighPart == gameDesc.AdapterLuid.HighPart &&
							vrDesc.AdapterLuid.LowPart == gameDesc.AdapterLuid.LowPart;
						LogInfo("VRPose: SteamVR wants adapter index %d = \"%S\" (LUID %08x:%08x) - SAME AS GAME: %s\n",
							vrAdapterIndex, vrDesc.Description,
							vrDesc.AdapterLuid.HighPart, vrDesc.AdapterLuid.LowPart,
							sameAdapter ? "YES" : "NO - cross-adapter sharing is not possible, this explains error 106");
						CompatibilityLog("openvr_adapter_index=%d adapter=\"%S\" "
							"luid=%08x:%08x same_as_game=%d\n",
							vrAdapterIndex, vrDesc.Description,
							vrDesc.AdapterLuid.HighPart, vrDesc.AdapterLuid.LowPart,
							sameAdapter ? 1 : 0);
					}
					vrAdapter->Release();
				} else {
					LogInfo("VRPose: SteamVR reported adapter index %d, which could not be enumerated\n", vrAdapterIndex);
				}
				factory->Release();
			}
			if (gameAdapter)
				gameAdapter->Release();
		}

		ID3D11Texture2D *backBuffer = nullptr;
		if (FAILED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&backBuffer)) || !backBuffer)
			return;

	D3D11_TEXTURE2D_DESC bbDesc;
	backBuffer->GetDesc(&bbDesc);
	VRMenu::SetCurrentResolution(bbDesc.Width, bbDesc.Height);

		// True stereo: the back buffer comes from DXGI rather than
		// CreateTexture2D, so it is the one render target the twinning
		// hook never sees - and without a twin the second eye's final
		// image has nowhere to land. Registering it here also means the
		// engine's own last blit gets doubled like every other draw.
		StereoTwin::RegisterBackBuffer(device, backBuffer);

		// (Re)create our private copies whenever the back buffer's shape
		// changes - resolution changes and alt-tab can both do this.
		if (!sCompositorTexture[0] || !sCompositorTexture[1] ||
		    sCompositorTexWidth != bbDesc.Width ||
		    sCompositorTexHeight != bbDesc.Height ||
		    sCompositorTexFormat != bbDesc.Format) {
			ReleaseCompositorResources();

			D3D11_TEXTURE2D_DESC desc;
			memset(&desc, 0, sizeof(desc));
			desc.Width = bbDesc.Width;
			desc.Height = bbDesc.Height;
			desc.MipLevels = 1;
			desc.ArraySize = 1;
			desc.Format = bbDesc.Format;
			// Always single-sampled: the compositor can't take an MSAA
			// texture, and ResolveSubresource below handles the case
			// where the game's back buffer is multisampled.
			desc.SampleDesc.Count = 1;
			desc.SampleDesc.Quality = 0;
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
			desc.CPUAccessFlags = 0;
			// No MiscFlags - OpenVR handles cross-process sharing
			// itself and the official D3D11 samples submit plain
			// textures. (The real cause of the earlier
			// SharedTexturesNotSupported wall was the game's DXGI 1.0
			// factory, not this - see Notes/12.)
			desc.MiscFlags = 0;

			bool created = true;
			for (int eye = 0; eye < 2 && created; eye++) {
				HRESULT hr = device->CreateTexture2D(&desc, NULL, &sCompositorTexture[eye]);
				if (FAILED(hr)) {
					LogInfo("VRPose: failed to create eye %d submission texture (%ux%u fmt=%d): %x\n",
						eye, desc.Width, desc.Height, desc.Format, hr);
					sCompositorTexture[eye] = nullptr;
					created = false;
				}
			}
			if (!created) {
				ReleaseCompositorResources();
				backBuffer->Release();
				return;
			}
			sCompositorTexWidth = desc.Width;
			sCompositorTexHeight = desc.Height;
			sCompositorTexFormat = desc.Format;
			LogInfo("VRPose: created 2 eye submission textures %ux%u fmt=%d (backbuffer samples=%u)\n",
				desc.Width, desc.Height, desc.Format, bbDesc.SampleDesc.Count);
		}

		// The frame that was just rendered belongs to whichever eye was
		// current while it was being drawn, so capture it into that
		// eye's texture. The other eye keeps the image it was given on
		// the previous frame - that one-frame staleness is inherent to
		// alternate-eye rendering.
		// With true stereo both eyes were rendered in THIS frame, from the
		// same instant: the left in the game's own targets, the right in
		// the twin set. That is the whole point of the exercise - under
		// alternate-eye rendering one eye was always a frame behind, which
		// is what made moving objects double and the image flicker.
		ID3D11Texture2D *rightBackBuffer = (ID3D11Texture2D *)StereoTwin::TwinResource(backBuffer);
		const int renderedEye = G->vrCurrentEye & 1;

		// TEMPORARY isolation test. Send the SAME image to both eyes, taken
		// from whichever set is named here.
		//
		// Everything measurable says stereo should be working: both eyes'
		// matrices are correct to the interpupillary offset, every doubled
		// draw receives them, no target failed to twin and no render path is
		// unhandled. Yet the eyes do not fuse - and under alternate-eye
		// rendering, one image shown to both eyes always did. So the question
		// is no longer "is the pairing right" but "is the twin image itself a
		// faithful frame". Showing it to both eyes answers that directly: a
		// good twin looks like ordinary flat VR, a bad one shows its defect
		// full-screen instead of being masked by the good eye.
		//
		//   0 = both eyes get the LEFT image (known good; sanity check)
		//   1 = both eyes get the TWIN image (the one under suspicion)
		//  -1 = off, real stereo
		//   0 = both eyes get the LEFT image (known good; sanity check)
		//   1 = both eyes get the TWIN image (the one under suspicion)
		//  -1 = off, real stereo
		//
		// Looking straight at the twin is what identified the viewmodel bug:
		// no gun there either, so the draw was producing nothing rather than
		// landing out of view.
		static const int kBothEyesFrom = -1;

		// Do not flatten the complete startup/main-menu scene. The old broad
		// frame<900 fallback submitted one shared left-eye texture, then snapped
		// abruptly to real stereo at its cutoff. Screen-space handling must be
		// driven only by a confirmed fullscreen-video draw; the loading and pause
		// overlays already have their own render paths.
		// Cinema frames (main menu, loading screens) take the same route as
		// movies: one flat image on the room-fixed theatre screen.
		// The cinema decision was made one Present ago; a frame that filled the
		// G-buffer after all (the first frame of a level) is a real 3D scene.
		const bool monoPresentation = IsFullscreenVideoFrame(G->frame_no) ||
			(IsCinemaFrame() && (StampAge(&sGBufferFrame) > 1 ||
				InterlockedCompareExchange(&sCinemaPanelHold, 0, 0) != 0));
		if (!monoPresentation)
			HideVideoOverlay();
		if (!VRMenu::IsOpen())
			HideMenuOverlay();
		static bool loggedMonoPresentation = false;
		if (monoPresentation && !loggedMonoPresentation) {
			LogInfo("VRPose: mono presentation active (fullscreen video, frame=%u)\n",
				G->frame_no);
			loggedMonoPresentation = true;
		}
		if (monoPresentation) {
			// Full-screen intro/video frames and the pause menu are screen-space
			// presentations. Alternate-eye capture makes moving mono content
			// appear to slide because one eye is a frame behind. Feed the same
			// current image to both compositor eyes, without changing gameplay.
			if (bbDesc.SampleDesc.Count > 1) {
				context->ResolveSubresource(sCompositorTexture[0], 0, backBuffer, 0, bbDesc.Format);
				context->ResolveSubresource(sCompositorTexture[1], 0, backBuffer, 0, bbDesc.Format);
			} else {
				context->CopyResource(sCompositorTexture[0], backBuffer);
				context->CopyResource(sCompositorTexture[1], backBuffer);
			}
			sEyeTextureValid[0] = sEyeTextureValid[1] = true;
			if (sLastPolledPoseValid)
				sEyeTexturePose[0] = sEyeTexturePose[1] = sLastPolledPose;
		} else if (StereoTwin::gDoubleDraw && rightBackBuffer) {
			ID3D11Texture2D *srcL = backBuffer, *srcR = rightBackBuffer;
			if (kBothEyesFrom == 0)      srcR = backBuffer;
			else if (kBothEyesFrom == 1) srcL = rightBackBuffer;

			if (bbDesc.SampleDesc.Count > 1) {
				context->ResolveSubresource(sCompositorTexture[0], 0, srcL, 0, bbDesc.Format);
				context->ResolveSubresource(sCompositorTexture[1], 0, srcR, 0, bbDesc.Format);
			} else {
				context->CopyResource(sCompositorTexture[0], srcL);
				// Bisection: leave the second eye's texture stale rather than
				// refreshing it, so the copy and everything the compositor
				// does with a changed texture drops out of the frame. The
				// right eye freezes while this phase runs; that is the test.
				if (StereoTwin::gBisectSkip != StereoTwin::kSkipSecondSubmit)
					context->CopyResource(sCompositorTexture[1], srcR);
			}
			sEyeTextureValid[0] = sEyeTextureValid[1] = true;

			// Write both eyes to disk, once, so the comparison can be made
			// by looking at the actual pixels instead of asking someone to
			// judge a subtle difference inside a headset. Whatever is or is
			// not offset between these two files answers the question
			// outright.
			DumpBothEyesOnce(device, context);
			if (sLastPolledPoseValid) {
				sEyeTexturePose[0] = sLastPolledPose;
				sEyeTexturePose[1] = sLastPolledPose;
			}
		} else {
			if (bbDesc.SampleDesc.Count > 1)
				context->ResolveSubresource(sCompositorTexture[renderedEye], 0, backBuffer, 0, bbDesc.Format);
			else
				context->CopyResource(sCompositorTexture[renderedEye], backBuffer);
			sEyeTextureValid[renderedEye] = true;
			if (sLastPolledPoseValid)
				sEyeTexturePose[renderedEye] = sLastPolledPose;
		}

		// Until both eyes have been rendered at least once, send the
		// fresh image to both so the first frames aren't half black.
		const int leftSource = sEyeTextureValid[0] ? 0 : renderedEye;
		const int rightSource = sEyeTextureValid[1] ? 1 : renderedEye;
		// Above 1x, prefer the isolated copy of Metro's completed scene target.
		// It has Metro's final sRGB decoding but none of the backbuffer mutation
		// that caused the stretched vertical-band failure.  If the exact final
		// pass was not seen this frame, fall back safely to the native images.
		const bool useHighResolutionScene = !monoPresentation &&
			StereoTwin::gDoubleDraw &&
			VRMenu::EffectiveSceneResolutionScale() > 1.0001f &&
			// RunFrameActions increments G->frame_no immediately before this
			// submission call, so a scene captured during rendering is labelled
			// with the preceding value even though it belongs to this Present.
			sHighResolutionFrame + 1u == G->frame_no &&
			sHighResolutionEyeTexture[0] && sHighResolutionEyeTexture[1];
		ID3D11Texture2D *submissionSourceLeft = useHighResolutionScene
			? sHighResolutionEyeTexture[0]
			: sCompositorTexture[leftSource];
		ID3D11Texture2D *submissionSourceRight = useHighResolutionScene
			? sHighResolutionEyeTexture[1]
			: sCompositorTexture[rightSource];
		const bool legacyPresentation = !VRMenu::GetSettings().brightnessCorrection;
		ID3D11Texture2D *submittedLeft = PrepareSubmissionTexture(device,
			context, 0, submissionSourceLeft, useHighResolutionScene,
			legacyPresentation);
		// A mono presentation must use one identical texture handle for both
		// eyes. Rendering two nominally identical copies can still expose a
		// one-frame transition difference to the compositor.
		ID3D11Texture2D *submittedRight = monoPresentation
			? submittedLeft
			: PrepareSubmissionTexture(device, context, 1,
				submissionSourceRight, useHighResolutionScene,
				legacyPresentation);
		if (monoPresentation) {
			// A scene-eye texture is interpreted through two eye projections even
			// when both handles are identical. Put the movie on OpenVR's actual
			// quad-overlay path and submit opaque black scene eyes behind it so no
			// copy of Metro's doubled fullscreen draw remains visible at the edges.
			ID3D11RenderTargetView *blackView = nullptr;
			const float black[4] = { 0, 0, 0, 1 };
			const bool blackReady = sCompositorTexture[1] &&
				SUCCEEDED(device->CreateRenderTargetView(sCompositorTexture[1],
					nullptr, &blackView)) && blackView;
			if (blackReady) {
				context->ClearRenderTargetView(blackView, black);
				blackView->Release();
			}
			// The overlay is a physical widescreen quad and must retain the movie's
			// original aspect. The runtime-shaped scene conversion is correct for
			// projected eye images, but feeding that near-square texture to the quad
			// stretches a 16:9 movie vertically.
			if (blackReady && ShowVideoOverlay(submissionSourceLeft)) {
				submittedLeft = sCompositorTexture[1];
				submittedRight = sCompositorTexture[1];
			} else {
				HideVideoOverlay();
			}
		}
		static bool loggedHighResolutionScene = false;
		if (useHighResolutionScene && !loggedHighResolutionScene) {
			LogInfo("VR resolution: submitting isolated high-resolution final-colour eyes\n");
			loggedHighResolutionScene = true;
		}

		// Submit each eye tagged with the pose it was actually rendered
		// at. Alternate-eye rendering means one eye is always a frame
		// behind the other, so without this the compositor assumes both
		// were rendered at the current pose and the mismatch shows up as
		// the whole image splitting in two whenever the head turns.
		// Given the poses, it can reproject each eye onto a common one.
		// Pose-tagged submission is for alternate-eye rendering ONLY.
		//
		// It exists because AER left one eye a frame behind the other, and
		// tagging each texture with the pose it was drawn at let the
		// compositor reproject them onto a common one. With both eyes now
		// rendered in the same frame from the same instant there is no lag
		// left to correct - and the correction is actively harmful, because
		// the pose we can supply is a headset pose while the image was
		// rendered from the GAME's camera, which lives in the game's world
		// and is driven by a delta from a startup reference. The two frames
		// are unrelated, so the compositor computes a meaningless correction
		// and applies it per eye, using each eye's own position and
		// asymmetric frustum. That warps the two eyes DIFFERENTLY.
		//
		// Proven, not guessed: with the identical texture submitted to both
		// eyes, the headset still showed two different images - one warping
		// the room, the other warping the props. Nothing upstream of the
		// compositor can do that.
		vr::EVRCompositorError errL, errR;
		// Screen-space presentations are already identical in both eyes. Do
		// not ask OpenVR to reproject them from a headset pose, since that
		// creates a temporary eye separation while the image settles. Gameplay
		// stereo retains the pose-tagged path.
		if (sLastPolledPoseValid && !StereoTwin::gDoubleDraw && !monoPresentation) {
			vr::VRTextureWithPose_t eyeTexture;
			eyeTexture.eType = vr::TextureType_DirectX;
			eyeTexture.eColorSpace = vr::ColorSpace_Auto;

			eyeTexture.handle = submittedLeft;
			eyeTexture.mDeviceToAbsoluteTracking = sEyeTexturePose[leftSource];
			errL = compositor->Submit(vr::Eye_Left, &eyeTexture, NULL, vr::Submit_TextureWithPose);

			eyeTexture.handle = submittedRight;
			eyeTexture.mDeviceToAbsoluteTracking = sEyeTexturePose[rightSource];
			errR = compositor->Submit(vr::Eye_Right, &eyeTexture, NULL, vr::Submit_TextureWithPose);
		} else {
			vr::Texture_t eyeTexture;
			eyeTexture.eType = vr::TextureType_DirectX;
			eyeTexture.eColorSpace = vr::ColorSpace_Auto;

			eyeTexture.handle = submittedLeft;
			errL = compositor->Submit(vr::Eye_Left, &eyeTexture);
			eyeTexture.handle = submittedRight;
			errR = compositor->Submit(vr::Eye_Right, &eyeTexture);
		}

		backBuffer->Release();

		static int sSubmitErrorLogCount = 0;
		if ((errL != vr::VRCompositorError_None || errR != vr::VRCompositorError_None) && sSubmitErrorLogCount < 30) {
			LogInfo("VRPose: compositor Submit failed (left=%d right=%d) - 101=DoNotHaveFocus, "
				"102=InvalidTexture, 103=IsNotSceneApplication, 104=TextureOnWrongDevice, "
				"105=UnsupportedFormat, 106=SharedTexturesNotSupported. canRenderScene=%d\n",
				errL, errR, compositor->CanRenderScene());
			CompatibilityLog("submit_error left=%d right=%d can_render_scene=%d\n",
				(int)errL, (int)errR, compositor->CanRenderScene() ? 1 : 0);
			sSubmitErrorLogCount++;
		}

		sLastSubmitAccepted = errL == vr::VRCompositorError_None &&
			errR == vr::VRCompositorError_None;
		if (sLastSubmitAccepted) {
			sConsecutiveFailures = 0;
			InterlockedIncrement(&sSuccessfulSubmits);
		}
		else
			sConsecutiveFailures++;

		static bool sLoggedFirstSubmit = false;
		if (!sLoggedFirstSubmit && errL == vr::VRCompositorError_None) {
			LogInfo("VRPose: first successful frame submission to OpenVR compositor\n");
			CompatibilityLog("submission=first-success\n");
			sLoggedFirstSubmit = true;
		}
	}

	void WaitForCompositorFrame()
	{
		if (!sVRSystem || !sCompositorFramePacingEnabled)
			return;
		vr::IVRCompositor *compositor = vr::VRCompositor();
		if (!compositor)
			return;

		LARGE_INTEGER frequency = {}, before = {}, after = {};
		QueryPerformanceFrequency(&frequency);
		QueryPerformanceCounter(&before);
		static bool sLoggedFirstWaitEntry = false;
		if (!sLoggedFirstWaitEntry) {
			CompatibilityLog("wait_get_poses=enter-after-present\n");
			sLoggedFirstWaitEntry = true;
		}

		vr::TrackedDevicePose_t renderPoses[vr::k_unMaxTrackedDeviceCount] = {};
		const vr::EVRCompositorError waitErr = compositor->WaitGetPoses(
			renderPoses, vr::k_unMaxTrackedDeviceCount, NULL, 0);
		QueryPerformanceCounter(&after);
		const double waitMs = frequency.QuadPart
			? (double)(after.QuadPart - before.QuadPart) * 1000.0 /
				(double)frequency.QuadPart
			: 0.0;

		if (waitErr == vr::VRCompositorError_None &&
			renderPoses[vr::k_unTrackedDeviceIndex_Hmd].bPoseIsValid) {
			memcpy(sFrameRenderPoses, renderPoses, sizeof(sFrameRenderPoses));
			sFrameRenderPosesValid = true;
		} else {
			sFrameRenderPosesValid = false;
		}

		static bool sLoggedFirstWaitReturn = false;
		if (!sLoggedFirstWaitReturn) {
			CompatibilityLog("wait_get_poses=first-return error=%d elapsed_ms=%.3f hmd_pose_valid=%d\n",
				(int)waitErr, waitMs, sFrameRenderPosesValid ? 1 : 0);
			sLoggedFirstWaitReturn = true;
		}
		static int sSlowWaitLogCount = 0;
		if ((waitMs > 250.0 || waitErr != vr::VRCompositorError_None) &&
			sSlowWaitLogCount < 20) {
			CompatibilityLog("wait_get_poses=slow-or-error error=%d elapsed_ms=%.3f\n",
				(int)waitErr, waitMs);
			sSlowWaitLogCount++;
		}
	}

}
