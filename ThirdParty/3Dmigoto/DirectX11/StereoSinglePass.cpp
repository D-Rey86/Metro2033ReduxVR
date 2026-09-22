#include "StereoSinglePass.h"

#include <d3dcompiler.h>
#include <dxgi1_4.h>   // IDXGIAdapter3, for the video memory budget
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <regex>
#include <cmath>

#include "log.h"
#include "Globals.h"
#include "DecompileHLSL.h"
#include "util.h"
#include "Overlay.h"
#include "StereoTwin.h"
#include "D3D_Shaders\stdafx.h"

namespace StereoSinglePass {

	// Permanent default. The small set of shaders whose view-dependent outputs
	// cannot be folded are kept on double-draw by ForceDoubleDrawForShader.
	// Single-pass is restored after the all-double-draw diagnostic. The known
	// unsafe shaders are kept on the conventional twin-pass path below.
	bool gEnabled = true;
	bool gVerifyByRedraw = false;
	bool ShadowMatrixAutoEnabled()
	{
		return false;
	}

	bool ShadowCasterIsolationEnabled()
	{
		// Preserve Metro's original light-space object matrices for depth-only
		// shadow-caster passes. This is intentionally narrow: regular scene
		// geometry and deferred lighting remain on the current VR path.
		return true;
	}

	bool LegacyDeferredSpaceEnabled()
	{
		return true;
	}

	bool ForceDoubleDrawForShader(UINT64 hash)
	{
		// First exact candidate from the live pig-pen flicker bisection. Bucket
		// 5 removed the artifact and contained only this textured world pass and
		// one position-only/depth pass. Test the visible textured pass first so
		// the higher-volume depth shader can remain folded if it is innocent.
		static const UINT64 kUnsafeTexturedWorldVS = 0x320C272616C11623ull;
		// A second live bisection isolated another artifact to bucket 5. With
		// the textured pass above already excluded, this was the sole remaining
		// bucket-5 scene shader: Metro's position/depth-only world variant.
		static const UINT64 kUnsafeWorldDepthVS = 0x4E9D4E63C6F1477Eull;
		// Bucket 2 isolated a localized artifact to Metro's small textured/decal
		// geometry pass. The other shader in that bucket is the global sky pass,
		// so leave it folded unless a separate test proves it unsafe.
		static const UINT64 kUnsafeTexturedDecalVS = 0x6FC1713CCCC530EEull;
		// The corridor bisection isolated bucket 7. Its dominant world-surface
		// variant exports left-eye view position and normals that the folded
		// right-eye draw cannot reconstruct. Keep that pass and its closely
		// related corridor variant on double-draw.
	static const UINT64 kUnsafeCorridorWorldVS = 0x67B434E01A3150AFull;
	static const UINT64 kUnsafeCorridorWorldVariantVS = 0x367328457692E6B2ull;
	// The remaining water/reflection artifact: this VS is paired with Metro's
	// cube-map reflection pixel shader in the affected scene. Its folded
	// right-eye output is not stable, so keep this surface on twin-pass.
	static const UINT64 kUnsafeWaterReflectionVS = 0x85F6741920989B53ull;
		// Metro's pre-rendered intro/splash video is a normalized fullscreen
		// quad with no camera matrix. Single-pass eye correction shifts the two
		// copies apart, so keep this screen-space video on the ordinary per-eye
		// path instead.
		static const UINT64 kFullscreenVideoVS = 0xD2B663AAD70298CEull;
		// Lamp lens flare: may be drawn from a private vertex buffer with its ghost
		// quads collapsed (HackerContext LampFlareBegin).
		static const UINT64 kLampGlowVS = 0x77840E246BBF6E55ull;
		if (hash == kUnsafeTexturedWorldVS ||
			hash == kLampGlowVS ||
			hash == kUnsafeWorldDepthVS ||
			hash == kUnsafeTexturedDecalVS ||
			hash == kUnsafeCorridorWorldVS ||
			hash == kUnsafeCorridorWorldVariantVS ||
			hash == kUnsafeWaterReflectionVS ||
			hash == kFullscreenVideoVS)
			return true;
		return false;
	}

	unsigned gDraws = 0;
	unsigned gDeclinedNoVariant = 0;
	unsigned gDeclinedReadsEye = 0;
	unsigned gDeclinedTessellated = 0;
	unsigned gDeclinedDisabled = 0;
	unsigned gDeclinedPrivateCB = 0;
	unsigned gDeclinedInstanced = 0;
	unsigned gDeclinedShaderList = 0;
	unsigned gDeclinedNoObjectCB = 0;
	unsigned gDeclinedEyeIndependent = 0;
	unsigned gDeclinedDepthTested = 0;
	unsigned gDeclinedSceneBuffer = 0;
	unsigned gDeclinedNoBothSliceView = 0;
	unsigned gFoldableIfInstanced = 0;
	unsigned gFoldableIfDepth = 0;
	bool gVPRTSupported = false;

	// Resolves a marker file name beside this DLL; the process working directory
	// is not the game folder.
	static bool FoldFlagPath(const wchar_t *name, wchar_t *out, size_t outCount)
	{
		// A path that does not fit is treated as "no such file". The _s string
		// functions would fast-fail the whole process instead: no invalid
		// parameter handler is installed anywhere in 3Dmigoto.
		const DWORD n = GetModuleFileNameW(migoto_handle, out, (DWORD)outCount);
		if (!n || n >= outCount)
			return false;
		wchar_t *slash = wcsrchr(out, L'\\');
		if (!slash)
			return false;
		const size_t dirLen = (size_t)(slash + 1 - out);
		const size_t nameLen = wcslen(name);
		if (dirLen + nameLen >= outCount)
			return false;
		memcpy(slash + 1, name, (nameLen + 1) * sizeof(wchar_t));
		return true;
	}

	static bool FoldFlagPresent(const wchar_t *name)
	{
		wchar_t path[MAX_PATH];
		if (!FoldFlagPath(name, path, MAX_PATH))
			return false;
		return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
	}

	// The generic folded vertex shader can rebuild only SV_Position for eye 1.
	// It cannot rebuild arbitrary view-dependent interpolants consumed by Metro's
	// depth/G-buffer pixel shaders. Enabling these broad scene folds therefore
	// produced right-eye-only flicker across unrelated levels (confirmed in two
	// recordings and removed by the conventional twin path on 2026-09-21).
	// Keep scene/depth folding experimental and opt-in. The validated batching,
	// depth-free folds, and the rest of PR2's performance path remain active.
	// An explicit off marker wins if both markers are present so old diagnostic
	// installs fail safe.
	bool FoldSceneBufferEnabled()
	{
		static int state = -1;
		if (state < 0) {
			const bool forcedOff = FoldFlagPresent(L"vr_fold_scene_off.txt");
			const bool experimentalOn = FoldFlagPresent(L"vr_fold_scene_on.txt");
			state = experimentalOn && !forcedOff ? 1 : 0;
		}
		return state != 0;
	}

	// Scene-buffer geometry is depth-tested by definition, so the two gates open
	// together.
	bool FoldDepthTestedEnabled()
	{
		return FoldSceneBufferEnabled();
	}

	bool FlagFilePresent(const wchar_t *name)
	{
		return FoldFlagPresent(name);
	}

	// Vertex-shader hashes the player has taken off the folded path.
	bool ShaderFoldExcluded(UINT64 vsHash)
	{
		static std::unordered_set<UINT64> excluded;
		static bool loaded = false;
		if (!loaded) {
			loaded = true;
			wchar_t path[MAX_PATH];
			FILE *f = NULL;
			if (FoldFlagPath(L"vr_fold_exclude.txt", path, MAX_PATH) &&
				_wfopen_s(&f, path, L"r") == 0 && f) {
				char line[128];
				while (fgets(line, sizeof(line), f)) {
					unsigned long long hash = 0;
					if (sscanf_s(line, "%llx", &hash) == 1 && hash)
						excluded.insert((UINT64)hash);
				}
				fclose(f);
			}
		}
		return !excluded.empty() && excluded.count(vsHash) != 0;
	}

	const char *ReportFoldPolicy()
	{
		static bool reported = false;
		if (reported)
			return NULL;
		reported = true;
		// Include the resolved directory so a misplaced marker file is visible.
		wchar_t path[MAX_PATH] = L"";
		char dir[MAX_PATH] = "";
		if (FoldFlagPath(L"", path, MAX_PATH))
			WideCharToMultiByte(CP_ACP, 0, path, -1, dir, MAX_PATH, NULL, NULL);
		static char line[512];
		sprintf_s(line, "fold policy: scene buffers %s, depth-tested %s, vprt=%d | switches looked for in %s "
			"(vr_fold_scene_on.txt / vr_fold_scene_off.txt / vr_fold_exclude.txt)",
			FoldSceneBufferEnabled() ? "ON" : "off",
			FoldDepthTestedEnabled() ? "ON" : "off", gVPRTSupported ? 1 : 0,
			dir[0] ? dir : "(unknown)");
		return line;
	}

	const char *ReportFrameStats()
	{
		static unsigned lastLogged = 0;
		static unsigned accFrames = 0;
		static unsigned acc[15] = { 0 };
		// gDraws is a running total also read by VRPerf: take a delta, never zero it.
		unsigned *counters[14] = {
			&gDeclinedNoVariant, &gDeclinedReadsEye, &gDeclinedTessellated,
			&gDeclinedDisabled, &gDeclinedPrivateCB, &gDeclinedInstanced,
			&gDeclinedShaderList, &gDeclinedNoObjectCB, &gDeclinedEyeIndependent,
			&gDeclinedDepthTested, &gDeclinedSceneBuffer, &gDeclinedNoBothSliceView,
			&gFoldableIfInstanced, &gFoldableIfDepth
		};
		for (int i = 0; i < 14; i++) {
			acc[i + 1] += *counters[i];
			*counters[i] = 0;
		}
		static unsigned prevFolded = 0;
		acc[0] += gDraws - prevFolded;
		prevFolded = gDraws;
		accFrames++;

		if (G->frame_no - lastLogged < 600 || !accFrames)
			return NULL;
		lastLogged = G->frame_no;
		const double f = (double)accFrames;
		static char line[520];
		sprintf_s(line, "single-pass per frame: folded %.0f | WOULD fold if the gate opened: "
			"instanced %.0f, depth-tested %.0f | vprt=%d | not folded (first match): "
			"depth-tested %.0f, scene buffer %.0f, eye-independent %.0f, no object CB %.0f, "
			"tessellated %.0f, instanced %.0f, shader list %.0f, no patched shader %.0f, "
			"reads other eye %.0f, private CB %.0f, no both-slice view %.0f, disabled %.0f",
			acc[0] / f, acc[13] / f, acc[14] / f, gVPRTSupported ? 1 : 0,
			acc[10] / f, acc[11] / f, acc[9] / f, acc[8] / f, acc[3] / f,
			acc[6] / f, acc[7] / f, acc[1] / f, acc[2] / f, acc[5] / f, acc[12] / f,
			acc[4] / f);
		for (int i = 0; i < 15; i++)
			acc[i] = 0;
		accFrames = 0;
		return line;
	}

	namespace {
		struct ShadowDiagnosticVariants {
			ID3D11PixelShader *noSpecular;
			ID3D11PixelShader *noShadowMap;
		};

		std::unordered_map<ID3D11VertexShader *, ID3D11VertexShader *> sVariants;
		std::unordered_map<ID3D11PixelShader *, unsigned> sPSSlots;
		std::unordered_set<ID3D11PixelShader *> sDeferredLightPS;
		// Bumped on every insert into sDeferredLightPS (see IsDeferredLightShader).
		volatile LONG sDeferredLightGeneration = 0;
		std::unordered_map<ID3D11PixelShader *, ShadowDiagnosticVariants> sShadowDiagnosticVariants;
		CRITICAL_SECTION sLock;
		bool sLockReady = false;
		// Bumped under sLock on every change to sVariants or sPSSlots, so the
		// per-thread memos in VariantOf/SampledSlotsOf never serve a stale answer.
		// Bumped BEFORE the change is visible: a reader that races sees a new
		// generation and re-queries.
		volatile LONG sShaderTableGeneration = 0;

		bool sReprojectionDirty = false;
		unsigned sPatched = 0;
		unsigned sSkippedNoPosition = 0;
		unsigned sSkippedAlreadyStereo = 0;
		unsigned sPatchedInstanced = 0;
		unsigned sFailedDecompile = 0;
		unsigned sFailedCompile = 0;
		LONGLONG sPatchTicks = 0;

		void EnsureLock()
		{
			static LONG init = 0;
			if (InterlockedCompareExchange(&init, 1, 0) == 0) {
				InitializeCriticalSection(&sLock);
				sLockReady = true;
			}
			while (!sLockReady)
				Sleep(0);
		}

		// Appended to every patched shader. Deliberately inert when the draw
		// is not instanced for stereo: with one instance, SV_InstanceID is
		// always 0, so the eye is 0, the array index is 0 and the position is
		// untouched. That means a patched shader renders identically to the
		// original until single-pass is actually switched on, which makes the
		// patch safe to apply unconditionally.
		const char *kEyeCode =
			"\n"
			"  // --- single-pass stereo ---\n"
			"  vr_rt_index = VR_EYE;\n"
			"  if (VR_EYE) {\n"
			"    float4 vr_c = o0;\n"
			"    o0.x = dot(IniParams.Load(int2(8,0)), vr_c);\n"
			"    o0.y = dot(IniParams.Load(int2(9,0)), vr_c);\n"
			"    o0.z = dot(IniParams.Load(int2(10,0)), vr_c);\n"
			"    o0.w = dot(IniParams.Load(int2(11,0)), vr_c);\n"
			"  }\n";

		// Adds the instance input and array-index output to main()'s
		// signature, and the eye code before its final return.
		bool PatchHLSL(const std::string &in, std::string *out)
		{
			// Needs a clip position to move.
			if (in.find("o0 : SV_Position0") == std::string::npos)
				return false;
			if (in.find("SV_RenderTargetArrayIndex") != std::string::npos)
				return false;

			const size_t mainPos = in.find("void main(");
			if (mainPos == std::string::npos)
				return false;
			const size_t openParen = in.find('(', mainPos);
			if (openParen == std::string::npos)
				return false;

			// The body's opening brace, and from it the real end of the
			// parameter list. Worth locating properly rather than taking the
			// first ')' after main( - the new output has to go at the very
			// end of the list, see below.
			const size_t bodyBrace = in.find('{', openParen);
			if (bodyBrace == std::string::npos)
				return false;
			const size_t closeParen = in.rfind(')', bodyBrace);
			if (closeParen == std::string::npos || closeParen < openParen)
				return false;

			// Does this shader already take an instance id?
			//
			// 75 of them do - the instanced geometry, which is most of the
			// props. Refusing those was wrong: they are exactly the draws
			// worth making single-pass. But doubling the instance count
			// changes what their instance id MEANS, so it has to be mapped
			// back to the original range before their own code runs, or every
			// prop reads the wrong per-instance data.
			std::smatch m;
			const bool hasInstanceId = std::regex_search(in, m,
				std::regex("(\\w+)\\s*:\\s*SV_InstanceID"));

			std::string s = in;
			std::string eyeExpr;

			// SV_RenderTargetArrayIndex must be declared AFTER every other
			// output, not before them. Measured, not assumed: a standalone
			// D3D11 program on this GPU compiled the same shader both ways,
			// and with the array index first the compiler silently discarded
			// the outputs that followed it - correct slice routing, but the
			// pixel shader receiving nothing. Declared last, all interpolants
			// survive at their own registers and both slices render exactly
			// as intended. Since we are appending to shaders we did not write,
			// getting this backwards would have corrupted all 160 of them in a
			// way that looks like a matrix bug.
			//
			// Both insertions below are therefore made at the END of the
			// parameter list, and in descending offset order so that the
			// earlier edit does not move the later one's target.
			if (hasInstanceId) {
				const std::string id = m[1].str();

				// Doubling the instance count changes what the shader's own
				// instance id means, so it is mapped back into the original
				// range before any of its code runs.
				//
				// The eye is the LOW bit rather than the upper half of the
				// range. Splitting it as "first N are the left eye" would mean
				// every shader needing to know N, and N changes per draw -
				// which would cost a GPU resource update on every draw call,
				// thousands of times a frame, to save the doubled geometry
				// this is all meant to avoid. As the low bit it is pure
				// arithmetic, nothing has to be uploaded, and the two eyes of
				// one instance stay adjacent.
				const std::string remap =
					"\n  uint vr_eye = " + id + " & 1;\n"
					"  " + id + " = " + id + " >> 1;\n";

				s.insert(bodyBrace + 1, remap);
				s.insert(closeParen,
					",\n  out uint vr_rt_index : SV_RenderTargetArrayIndex");
				eyeExpr = "vr_eye";
			} else {
				s.insert(closeParen,
					",\n  uint vr_instance_id : SV_InstanceID"
					",\n  out uint vr_rt_index : SV_RenderTargetArrayIndex");
				eyeExpr = "(vr_instance_id & 1)";
			}

			const size_t ret = s.rfind("  return;");
			if (ret == std::string::npos)
				return false;

			std::string eye = kEyeCode;
			for (size_t at = eye.find("VR_EYE"); at != std::string::npos; at = eye.find("VR_EYE"))
				eye.replace(at, 6, eyeExpr);
			s.insert(ret, eye);

			*out = s;
			return true;
		}
	}

	// Shader-creation cost, per frame, for the profiler. Written from whichever
	// thread creates shaders, so everything here is atomic.
	volatile LONG64 gCreateVSTicks = 0, gCreatePSTicks = 0, gBuildTicks = 0;
	volatile LONG gCreateVSCount = 0, gCreatePSCount = 0, gBuildCount = 0, gCacheHits = 0;
	volatile LONG64 gCreateMaxTicks = 0;

	void NoteShaderCreate(bool pixel, LONGLONG ticks)
	{
		InterlockedAdd64(pixel ? &gCreatePSTicks : &gCreateVSTicks, ticks);
		InterlockedIncrement(pixel ? &gCreatePSCount : &gCreateVSCount);
		LONG64 seen = gCreateMaxTicks;
		while (ticks > seen &&
			InterlockedCompareExchange64(&gCreateMaxTicks, ticks, seen) != seen)
			seen = gCreateMaxTicks;
	}

	static bool ShaderSyncEnabled();

	const char *ReportShaderCreation()
	{
		const LONG vs = InterlockedExchange(&gCreateVSCount, 0);
		const LONG ps = InterlockedExchange(&gCreatePSCount, 0);
		const LONG built = InterlockedExchange(&gBuildCount, 0);
		const LONG hits = InterlockedExchange(&gCacheHits, 0);
		const LONG64 vsTicks = InterlockedExchange64(&gCreateVSTicks, 0);
		const LONG64 psTicks = InterlockedExchange64(&gCreatePSTicks, 0);
		const LONG64 buildTicks = InterlockedExchange64(&gBuildTicks, 0);
		const LONG64 maxTicks = InterlockedExchange64(&gCreateMaxTicks, 0);
		if (!vs && !ps && !built)
			return NULL;
		// At most 3000 lines per session.
		static unsigned lines = 0;
		if (++lines > 3000)
			return NULL;
		LARGE_INTEGER freq;
		QueryPerformanceFrequency(&freq);
		const double toMs = 1000.0 / (double)freq.QuadPart;
		static char line[300];
		sprintf_s(line, "shader creation: vs %ld (%.2f ms on game thread), ps %ld (%.2f ms), "
			"slowest %.2f ms | stereo variants built %ld (%.2f ms, %s), from disk cache %ld",
			vs, (double)vsTicks * toMs, ps, (double)psTicks * toMs, (double)maxTicks * toMs,
			built, (double)buildTicks * toMs,
			ShaderSyncEnabled() ? "sync" : "background", hits);
		return line;
	}

	// Stereo variants on disk, keyed by the original bytecode plus the patch
	// itself, so a changed kEyeCode or PatchHLSL invalidates every entry.
	// An empty file records "this shader cannot be patched".
	static const unsigned kVariantCacheVersion = 1;

	static UINT64 Fnv1a64(const unsigned char *p, size_t n, UINT64 h = 1469598103934665603ull)
	{
		for (size_t i = 0; i < n; i++) {
			h ^= p[i];
			h *= 1099511628211ull;
		}
		return h;
	}

	// Everything else that shapes a variant: PatchHLSL, the decompiler and the
	// D3DCompile call are all code in this DLL, so its link timestamp stands in
	// for them (no version to remember to bump); the IniParams register is
	// configuration the decompiler writes into the HLSL.
	static UINT64 VariantCacheSalt()
	{
		static UINT64 build = 0;
		static bool haveBuild = false;
		if (!haveBuild) {
			DWORD stamp = 0;
			const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)migoto_handle;
			if (dos && dos->e_magic == IMAGE_DOS_SIGNATURE) {
				const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)((const BYTE *)dos + dos->e_lfanew);
				if (nt->Signature == IMAGE_NT_SIGNATURE)
					stamp = nt->FileHeader.TimeDateStamp;
			}
			build = Fnv1a64((const unsigned char *)&stamp, sizeof(stamp));
			haveBuild = true;   // benign race: every thread computes the same value
		}
		const int reg = G->decompiler_settings.IniParamsReg;
		return Fnv1a64((const unsigned char *)&reg, sizeof(reg), build);
	}

	static bool VariantCachePath(const unsigned char *bytecode, size_t length,
		wchar_t *out, size_t outCount)
	{
		UINT64 key = Fnv1a64(bytecode, length, VariantCacheSalt());
		key = Fnv1a64((const unsigned char *)kEyeCode, strlen(kEyeCode), key);
		key ^= kVariantCacheVersion * 0x9E3779B97F4A7C15ull;
		wchar_t dir[MAX_PATH];
		if (!FoldFlagPath(L"vr_singlepass_cache", dir, MAX_PATH))
			return false;
		if (_snwprintf_s(out, outCount, _TRUNCATE, L"%s\\%016llX_%zu.bin", dir, key, length) <= 0)
			return false;
		CreateDirectoryW(dir, NULL);
		return true;
	}

	static void WriteVariantCache(const wchar_t *path, const void *data, size_t bytes)
	{
		// Written to a temporary name and renamed, so a crash mid-write can never
		// leave a truncated shader that loads as garbage next session.
		wchar_t tmp[MAX_PATH + 8];
		if (_snwprintf_s(tmp, _TRUNCATE, L"%s.tmp", path) <= 0)
			return;
		FILE *f = NULL;
		if (_wfopen_s(&f, tmp, L"wb") != 0 || !f)
			return;
		const bool ok = bytes == 0 || fwrite(data, 1, bytes, f) == bytes;
		fclose(f);
		if (!ok || !MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING))
			DeleteFileW(tmp);
	}

	static unsigned sVariantGeneration = 0;   // under sLock; bumped by ReleaseAll

	static void InsertVariant(ID3D11VertexShader *created, ID3D11VertexShader *variant,
		unsigned generation)
	{
		EnsureLock();
		EnterCriticalSection(&sLock);
		if (generation == sVariantGeneration) {
			InterlockedIncrement(&sShaderTableGeneration);
			ID3D11VertexShader *&slot = sVariants[created];
			std::swap(slot, variant);   // variant now holds any stale entry
			sPatched++;
		}
		LeaveCriticalSection(&sLock);
		if (variant)
			variant->Release();
	}

	static void BuildVariant(ID3D11Device *device, const void *bytecode,
		SIZE_T length, ID3D11VertexShader *created, unsigned generation)
	{
		LARGE_INTEGER t0, t1;
		QueryPerformanceCounter(&t0);
		struct BuildTimer {
			LARGE_INTEGER start;
			~BuildTimer() {
				LARGE_INTEGER end;
				QueryPerformanceCounter(&end);
				InterlockedAdd64(&gBuildTicks, end.QuadPart - start.QuadPart);
				InterlockedIncrement(&gBuildCount);
			}
		} buildTimer{ t0 };

		wchar_t cachePath[MAX_PATH];
		const bool cacheable = VariantCachePath((const unsigned char *)bytecode, length,
			cachePath, MAX_PATH);
		if (cacheable) {
			FILE *f = NULL;
			if (_wfopen_s(&f, cachePath, L"rb") == 0 && f) {
				std::vector<unsigned char> blob;
				fseek(f, 0, SEEK_END);
				const long size = ftell(f);
				fseek(f, 0, SEEK_SET);
				if (size > 0) {
					blob.resize((size_t)size);
					if (fread(blob.data(), 1, blob.size(), f) != blob.size())
						blob.clear();
				}
				fclose(f);
				if (size == 0) {
					InterlockedIncrement(&gCacheHits);
					return;   // known unpatchable
				}
				ID3D11VertexShader *variant = NULL;
				if (!blob.empty() && SUCCEEDED(device->CreateVertexShader(blob.data(),
						blob.size(), NULL, &variant)) && variant) {
					InterlockedIncrement(&gCacheHits);
					InsertVariant(created, variant, generation);
					return;
				}
				// Unreadable entry: fall through and rebuild it.
			}
		}

		// Failures are deterministic for the same bytecode and are cached as empty
		// files so they are not retried; device errors are not cached.
		struct NegativeCache {
			const wchar_t *path;
			bool armed;
			~NegativeCache() { if (armed && path) WriteVariantCache(path, NULL, 0); }
		} negative{ cacheable ? cachePath : NULL, true };

		std::string asmText = BinaryToAsmText(bytecode, length, false);
		if (asmText.empty()) {
			sFailedDecompile++;
			return;
		}

		ParseParameters p;
		p.bytecode = bytecode;
		p.decompiled = asmText.c_str();
		p.decompiledSize = asmText.size();
		p.ZeroOutput = false;
		p.G = &G->decompiler_settings;

		bool patchedByDecompiler = false, errorOccurred = false;
		std::string model;
		const std::string hlsl = DecompileBinaryHLSL(p, patchedByDecompiler, model, errorOccurred);
		if (hlsl.empty() || errorOccurred) {
			sFailedDecompile++;
			return;
		}

		std::string patched;
		if (!PatchHLSL(hlsl, &patched)) {
			if (hlsl.find("o0 : SV_Position0") == std::string::npos)
				sSkippedNoPosition++;
			else
				sSkippedAlreadyStereo++;
			return;
		}

		ID3DBlob *code = NULL, *errors = NULL;
		HRESULT hr = D3DCompile(patched.c_str(), patched.size(), "vr_stereo_vs",
			NULL, NULL, "main", model.empty() ? "vs_5_0" : model.c_str(),
			D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
		if (FAILED(hr) || !code) {
			sFailedCompile++;
			// Only the first few, or a broken shader family floods the log.
			if (sFailedCompile <= 5 && errors) {
				LogInfo("SinglePass: compile failed: %s\n",
					(const char *)errors->GetBufferPointer());
			}
			if (errors) errors->Release();
			if (code) code->Release();
			return;
		}
		if (errors) errors->Release();

		ID3D11VertexShader *variant = NULL;
		hr = device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), NULL, &variant);
		if (FAILED(hr) || !variant) {
			// A device failure says nothing about the bytecode; do not cache it.
			negative.armed = false;
			code->Release();
			sFailedCompile++;
			return;
		}
		negative.armed = false;
		if (cacheable)
			WriteVariantCache(cachePath, code->GetBufferPointer(), code->GetBufferSize());
		code->Release();

		InsertVariant(created, variant, generation);

		QueryPerformanceCounter(&t1);
		sPatchTicks += t1.QuadPart - t0.QuadPart;
	}

	// Variants (decompile plus D3DCompile, tens of milliseconds) are built on a
	// worker so Metro's CreateVertexShader during play does not miss a frame;
	// draws use double-draw until their variant exists. vr_shader_sync.txt beside
	// the DLL builds them inline instead.
	static bool ShaderSyncEnabled()
	{
		static int state = -1;
		if (state < 0)
			state = FoldFlagPresent(L"vr_shader_sync.txt") ? 1 : 0;
		return state != 0;
	}

	namespace {
		struct VariantJob {
			ID3D11Device *device;
			ID3D11VertexShader *created;
			std::vector<unsigned char> bytecode;
			unsigned generation;
		};
		// Statically initialised, so no thread can ever reach an uninitialised
		// lock however the first shader creations interleave.
		SRWLOCK sJobLock = SRWLOCK_INIT;
		CONDITION_VARIABLE sJobReady = CONDITION_VARIABLE_INIT;
		std::vector<VariantJob> sJobs;
		// 0 not started, 3 starting, 1 worker running, 2 no worker (build inline).
		volatile LONG sWorkerStarted = 0;

		DWORD WINAPI VariantWorker(LPVOID)
		{
			for (;;) {
				AcquireSRWLockExclusive(&sJobLock);
				while (sJobs.empty())
					SleepConditionVariableSRW(&sJobReady, &sJobLock, INFINITE, 0);
				VariantJob job = std::move(sJobs.front());
				sJobs.erase(sJobs.begin());
				ReleaseSRWLockExclusive(&sJobLock);

				BuildVariant(job.device, job.bytecode.data(), job.bytecode.size(),
					job.created, job.generation);
				job.created->Release();
				job.device->Release();
			}
		}
	}

	void OnCreateVertexShader(ID3D11Device *device, const void *bytecode,
		SIZE_T length, ID3D11VertexShader *created)
	{
		if (!device || !bytecode || !length || !created)
			return;

		// A new shader can be handed the address of one the game has released.
		// Drop that one's variant now, or it would be used for this shader until
		// the worker replaces it.
		EnsureLock();
		EnterCriticalSection(&sLock);
		const unsigned generation = sVariantGeneration;
		ID3D11VertexShader *stale = NULL;
		auto existing = sVariants.find(created);
		if (existing != sVariants.end()) {
			stale = existing->second;
			InterlockedIncrement(&sShaderTableGeneration);
			sVariants.erase(existing);
		}
		LeaveCriticalSection(&sLock);
		if (stale)
			stale->Release();

		if (ShaderSyncEnabled()) {
			BuildVariant(device, bytecode, length, created, generation);
			return;
		}

		if (InterlockedCompareExchange(&sWorkerStarted, 3, 0) == 0) {
			HANDLE thread = CreateThread(NULL, 0, VariantWorker, NULL, 0, NULL);
			if (thread) {
				CloseHandle(thread);
				InterlockedExchange(&sWorkerStarted, 1);
			} else {
				InterlockedExchange(&sWorkerStarted, 2);   // no worker: stay inline
			}
		}
		// Another thread is creating the worker: wait for the outcome, so no
		// job is queued for a worker that then fails to start.
		while (sWorkerStarted == 3)
			Sleep(0);
		if (sWorkerStarted != 1) {
			BuildVariant(device, bytecode, length, created, generation);
			return;
		}

		// Both are kept alive until the worker is done: the key must not be
		// freed and its address reused while its variant is still being built.
		VariantJob job;
		job.device = device;
		job.created = created;
		job.bytecode.assign((const unsigned char *)bytecode,
			(const unsigned char *)bytecode + length);
		job.generation = generation;
		device->AddRef();
		created->AddRef();
		AcquireSRWLockExclusive(&sJobLock);
		sJobs.push_back(std::move(job));
		ReleaseSRWLockExclusive(&sJobLock);
		WakeConditionVariable(&sJobReady);
	}

	ID3D11VertexShader *VariantOf(ID3D11VertexShader *original)
	{
		if (!original || sVariants.empty())
			return NULL;
		thread_local ID3D11VertexShader *tLastShader = NULL;
		thread_local LONG tLastGeneration = -1;
		thread_local ID3D11VertexShader *tLastVariant = NULL;
		const LONG generation = sShaderTableGeneration;
		if (original == tLastShader && generation == tLastGeneration)
			return tLastVariant;
		EnterCriticalSection(&sLock);
		auto it = sVariants.find(original);
		ID3D11VertexShader *v = (it != sVariants.end()) ? it->second : NULL;
		LeaveCriticalSection(&sLock);
		tLastShader = original;
		tLastGeneration = generation;
		tLastVariant = v;
		return v;
	}


	static ID3D11PixelShader *CreateAssemblyPatchedPS(ID3D11Device *device,
		const std::string &originalAssembly, const char *from, const char *to)
	{
		std::string patched = originalAssembly;
		const size_t at = patched.find(from);
		if (at == std::string::npos)
			return NULL;
		patched.replace(at, strlen(from), to);

		std::vector<char> assembly(patched.begin(), patched.end());
		std::vector<byte> bytecode;
		try {
			std::vector<AssemblerParseError> parseErrors;
			if (FAILED(AssembleFluganWithSignatureParsing(&assembly, &bytecode,
				&parseErrors)))
				return NULL;
			for (const auto &error : parseErrors)
				LogInfo("Shadow diagnostic assembler: %s\n", error.what());
		} catch (const std::exception &error) {
			LogInfo("Shadow diagnostic assembler exception: %s\n", error.what());
			return NULL;
		}

		ID3D11PixelShader *shader = NULL;
		if (FAILED(device->CreatePixelShader(bytecode.data(), bytecode.size(),
			NULL, &shader)))
			return NULL;
		return shader;
	}

	void OnCreatePixelShader(ID3D11Device *device, const void *bytecode,
		SIZE_T length, ID3D11PixelShader *created)
	{
		if (!device || !bytecode || !length || !created)
			return;

		ID3D11ShaderReflection *refl = NULL;
		if (FAILED(D3DReflect(bytecode, length, IID_ID3D11ShaderReflection, (void **)&refl)) || !refl)
			return;

		D3D11_SHADER_DESC sd;
		unsigned mask = 0;
		bool deferredLight = false;
		if (SUCCEEDED(refl->GetDesc(&sd))) {
			for (UINT i = 0; i < sd.BoundResources; i++) {
				D3D11_SHADER_INPUT_BIND_DESC bd;
				if (FAILED(refl->GetResourceBindingDesc(i, &bd)))
					continue;
				if (bd.Type == D3D_SIT_CBUFFER && bd.BindPoint == 12 &&
					bd.Name && strcmp(bd.Name, "cb_light") == 0)
					deferredLight = true;
				if (bd.Type != D3D_SIT_TEXTURE)
					continue;
				for (UINT s = 0; s < bd.BindCount; s++)
					if (bd.BindPoint + s < 32)
						mask |= 1u << (bd.BindPoint + s);
			}
		}
		refl->Release();

		EnsureLock();
		EnterCriticalSection(&sLock);
		InterlockedIncrement(&sShaderTableGeneration);
		sPSSlots[created] = mask;
		if (deferredLight)
		{
			sDeferredLightPS.insert(created);
			InterlockedIncrement(&sDeferredLightGeneration);
		}
		LeaveCriticalSection(&sLock);

		// This exact instruction sequence identifies Metro's shadowed deferred
		// point-light shader (3CB9F025E61AC94E). Build two temporary variants
		// from its original DXBC so F6 can distinguish a moving specular lobe
		// from a moving shadow projection without suppressing the light draw.
		if (!deferredLight)
			return;
		const std::string assembly = BinaryToAsmText(bytecode, length, false);
		static const char *kShadowAverage =
			"mul r0.w, r0.w, l(0.0833333358)";
		static const char *kSpecularExponent =
			"exp r2.w, r0.x";
		if (assembly.find(kShadowAverage) == std::string::npos ||
			assembly.find(kSpecularExponent) == std::string::npos ||
			assembly.find("sample_c_lz_indexable(texture2d)(float,float,float,float)") == std::string::npos)
			return;

		ID3D11PixelShader *noSpecular = CreateAssemblyPatchedPS(device, assembly,
			kSpecularExponent, "mov r2.w, l(0.000000)");
		ID3D11PixelShader *noShadowMap = CreateAssemblyPatchedPS(device, assembly,
			kShadowAverage, "mov r0.w, l(1.000000)");
		if (!noSpecular || !noShadowMap) {
			if (noSpecular) noSpecular->Release();
			if (noShadowMap) noShadowMap->Release();
			LogInfo("Shadow diagnostic: failed to build one or more PS 3CB9 variants\n");
			return;
		}

		EnterCriticalSection(&sLock);
		sShadowDiagnosticVariants[created] = { noSpecular, noShadowMap };
		LeaveCriticalSection(&sLock);
		LogInfo("Shadow diagnostic: built component variants for PS 3CB9F025E61AC94E\n");
	}

	ID3D11PixelShader *ShadowDiagnosticVariantOf(ID3D11PixelShader *original)
	{
		// The component-removal variants were useful to prove that the movement
		// is in shadow projection. The live F6 control now performs per-light
		// matrix bisection, so no replacement pixel shader is selected here.
		if (!original || !sLockReady)
			return NULL;
		return NULL;
	}

	unsigned SampledSlotsOf(ID3D11PixelShader *ps)
	{
		if (!ps)
			return 0;
		if (!sLockReady)
			return 0xFFFFFFFFu;   // unknown: assume it reads everything
		// Asked on every draw; answer repeats until the shader or the table
		// changes, so skip the lock and lookup when neither has.
		thread_local ID3D11PixelShader *tLastShader = NULL;
		thread_local LONG tLastGeneration = -1;
		thread_local unsigned tLastMask = 0;
		const LONG generation = sShaderTableGeneration;
		if (ps == tLastShader && generation == tLastGeneration)
			return tLastMask;
		EnterCriticalSection(&sLock);
		auto it = sPSSlots.find(ps);
		const unsigned mask = (it != sPSSlots.end()) ? it->second : 0xFFFFFFFFu;
		LeaveCriticalSection(&sLock);
		tLastShader = ps;
		tLastGeneration = generation;
		tLastMask = mask;
		return mask;
	}

	bool IsDeferredLightShader(ID3D11PixelShader *ps)
	{
		if (!ps || !sLockReady)
			return false;
		// Asked on every draw, while the set only grows when a shader is
		// created: answer from a per-thread last-query cache and take the lock
		// only when the shader or the set has changed.
		thread_local ID3D11PixelShader *tLastShader = NULL;
		thread_local LONG tLastGeneration = -1;
		thread_local bool tLastFound = false;
		const LONG generation = InterlockedCompareExchange(&sDeferredLightGeneration, 0, 0);
		if (ps == tLastShader && generation == tLastGeneration)
			return tLastFound;
		EnterCriticalSection(&sLock);
		const bool found = sDeferredLightPS.count(ps) != 0;
		LeaveCriticalSection(&sLock);
		tLastShader = ps;
		tLastGeneration = generation;
		tLastFound = found;
		return found;
	}

	void SetReprojection(const float m[16])
	{
		// One row per entry, starting at kReprojectionParam - which is where
		// the appended shader code reads them from.
		//
		// Never resize here. The GPU texture is created once, at the size the
		// ini reserved, and growing the vector afterwards would leave the two
		// disagreeing - the upload would then write past the end of a smaller
		// mapping. HackerDevice reserves the room up front instead.
		if ((int)G->iniParams.size() < kReprojectionParam + 4)
			return;
		// Mark dirty only when the rows change. Compared against G->iniParams, so
		// another writer of those rows still forces an upload.
		bool changed = false;
		for (int row = 0; row < 4; row++) {
			DirectX::XMFLOAT4 &p = G->iniParams[kReprojectionParam + row];
			const float *r = m + row * 4;
			if (p.x != r[0] || p.y != r[1] || p.z != r[2] || p.w != r[3]) {
				p.x = r[0]; p.y = r[1]; p.z = r[2]; p.w = r[3];
				changed = true;
			}
		}
		if (changed)
			sReprojectionDirty = true;
	}

	void UploadIfDirty(ID3D11DeviceContext1 *context, ID3D11Resource *iniTexture)
	{
		if (!sReprojectionDirty || !context || !iniTexture || G->iniParams.empty())
			return;

		D3D11_MAPPED_SUBRESOURCE m;
		if (SUCCEEDED(context->Map(iniTexture, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
			memcpy(m.pData, G->iniParams.data(),
				sizeof(DirectX::XMFLOAT4) * G->iniParams.size());
			context->Unmap(iniTexture, 0);
			// Cleared only once uploaded: the rows are pose-independent, so a failed
			// upload would otherwise never be retried.
			sReprojectionDirty = false;
		}
	}

	// How much video memory the process is using against what the system is
	// willing to give it.
	//
	// The twin render targets add 479 MB. If that takes the process past its
	// budget - with SteamVR, Virtual Desktop's encoder and the game all
	// resident on the same card - the driver pages memory across PCIe every
	// frame to make room. That would cost tens of milliseconds, would be
	// completely indifferent to how many draws we issue, and would be free in
	// alternate-eye mode where the twins are allocated but never touched. All
	// three match what the timings show, so it is worth checking rather than
	// reasoning about: CurrentUsage above Budget is the whole answer.
	static void ReportVideoMemory(ID3D11Device *device)
	{
		if (!device)
			return;

		IDXGIDevice *dxgiDevice = NULL;
		if (FAILED(device->QueryInterface(__uuidof(IDXGIDevice), (void **)&dxgiDevice)) || !dxgiDevice)
			return;

		IDXGIAdapter *adapter = NULL;
		dxgiDevice->GetAdapter(&adapter);
		dxgiDevice->Release();
		if (!adapter)
			return;

		IDXGIAdapter3 *adapter3 = NULL;
		if (SUCCEEDED(adapter->QueryInterface(__uuidof(IDXGIAdapter3), (void **)&adapter3)) && adapter3) {
			DXGI_QUERY_VIDEO_MEMORY_INFO info;
			if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) {
				const double usedMB = info.CurrentUsage / (1024.0 * 1024.0);
				const double budgetMB = info.Budget / (1024.0 * 1024.0);
				LogInfo("VRAM: using %.0f MB of a %.0f MB budget%s | %.0f MB reserved, twins are 479 MB\n",
					usedMB, budgetMB,
					(info.CurrentUsage > info.Budget)
						? "  *** OVER BUDGET - the driver is paging every frame ***" : "",
					info.CurrentReservation / (1024.0 * 1024.0));
			}
			adapter3->Release();
		}
		adapter->Release();
	}

	void FrameTiming(ID3D11Device *device, ID3D11DeviceContext1 *context)
	{
		if (!device || !context)
			return;

		// Three sets in rotation, so a result is only collected once the GPU
		// has long finished with it. Reading the set we just closed would
		// stall the CPU on the GPU and change the very thing being measured.
		struct Slot {
			ID3D11Query *disjoint, *start, *end;
			bool pending;
		};
		static Slot slots[3] = {};
		static int open = -1;
		static int next = 0;
		static bool ready = false;

		if (!ready) {
			D3D11_QUERY_DESC qd = {};
			for (int i = 0; i < 3; i++) {
				qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
				if (FAILED(device->CreateQuery(&qd, &slots[i].disjoint)))
					return;
				qd.Query = D3D11_QUERY_TIMESTAMP;
				if (FAILED(device->CreateQuery(&qd, &slots[i].start)))
					return;
				if (FAILED(device->CreateQuery(&qd, &slots[i].end)))
					return;
			}
			ready = true;
		}

		// Close the frame that was open, then open the next one. Present to
		// Present is exactly the interval we care about.
		if (open >= 0) {
			context->End(slots[open].end);
			context->End(slots[open].disjoint);
			slots[open].pending = true;
		}

		open = next;
		next = (next + 1) % 3;
		context->Begin(slots[open].disjoint);
		context->End(slots[open].start);

		// Collect whatever has finished, without flushing.
		static double accMs = 0.0;
		static unsigned accFrames = 0;
		static unsigned lastLogged = 0;

		for (int i = 0; i < 3; i++) {
			if (!slots[i].pending || i == open)
				continue;

			D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj;
			if (context->GetData(slots[i].disjoint, &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
				continue;

			UINT64 t0 = 0, t1 = 0;
			if (context->GetData(slots[i].start, &t0, sizeof(t0), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
				continue;
			if (context->GetData(slots[i].end, &t1, sizeof(t1), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
				continue;

			slots[i].pending = false;
			if (dj.Disjoint || !dj.Frequency || t1 <= t0)
				continue;   // the clock changed under us; that frame is void

			accMs += 1000.0 * (double)(t1 - t0) / (double)dj.Frequency;
			accFrames++;
		}

		if (G->frame_no - lastLogged < 300 || accFrames < 30)
			return;
		lastLogged = G->frame_no;

		// 13.9 ms is the 72 Hz budget. Over it and the compositor drops to a
		// half or a third of refresh, which is what the framerate readings
		// have been showing all along.
		const double ms = accMs / accFrames;

		if (StereoTwin::gBisectRunning && StereoTwin::gDoubleDraw) {
			static double baseline = 0.0;
			const int phase = StereoTwin::gBisectSkip;
			if (phase == StereoTwin::kSkipNothing)
				baseline = ms;

			LogInfo("BISECT %d/%d  %6.2f ms  %s%*.2f ms | %s\n",
				phase + 1, (int)StereoTwin::kSkipCategoryCount, ms,
				(phase == StereoTwin::kSkipNothing) ? "" : "saves ",
				(phase == StereoTwin::kSkipNothing) ? 0 : 6,
				(phase == StereoTwin::kSkipNothing) ? 0.0 : (baseline - ms),
				StereoTwin::SkipCategoryName(phase));

			ReportVideoMemory(device);

			StereoTwin::gBisectSkip++;
			if (StereoTwin::gBisectSkip >= StereoTwin::kSkipCategoryCount) {
				StereoTwin::gBisectSkip = StereoTwin::kSkipNothing;
				LogInfo("BISECT ---- sweep complete, repeating ----\n");
			}
		} else {
			LogInfo("GPU: %.2f ms/frame (%.0f fps if unthrottled) - 72Hz budget is 13.9 ms, "
				"%s by %.2f ms | mode=%s, %u draws folded last frame\n",
				ms, 1000.0 / ms, (ms > 13.9) ? "OVER" : "under", fabs(ms - 13.9),
				StereoTwin::gDoubleDraw ? "true stereo" : "alternate-eye", gDraws);
			ReportVideoMemory(device);
		}

		accMs = 0.0;
		accFrames = 0;
	}

	void ReportPatchStats()
	{
		static bool done = false;
		if (done || G->frame_no < 600)
			return;
		done = true;

		LARGE_INTEGER freq;
		QueryPerformanceFrequency(&freq);
		LogInfo("\n=========== single-pass shader patching ===========\n");
		LogInfo("  patched successfully : %u\n", sPatched);
		LogInfo("  no clip position     : %u\n", sSkippedNoPosition);
		LogInfo("  already stereo-ish   : %u\n", sSkippedAlreadyStereo);
		LogInfo("  decompile failed     : %u\n", sFailedDecompile);
		LogInfo("  recompile failed     : %u\n", sFailedCompile);
		LogInfo("  total time patching  : %.2f s\n",
			(double)sPatchTicks / (double)freq.QuadPart);
		LogInfo("==================================================\n\n");
	}

	void ReleaseAll()
	{
		if (!sLockReady)
			return;
		EnterCriticalSection(&sLock);
		InterlockedIncrement(&sShaderTableGeneration);
		for (auto &kv : sVariants)
			if (kv.second) kv.second->Release();
		sVariants.clear();
		// Only called when the device is replaced (StereoTwin::NoteDevice): a
		// build still in flight belongs to the old device.
		sVariantGeneration++;
		LeaveCriticalSection(&sLock);
	}
}
