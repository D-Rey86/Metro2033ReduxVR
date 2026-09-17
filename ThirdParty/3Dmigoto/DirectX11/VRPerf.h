#pragma once

// Metro2033ReduxVR frame profiler.
//
// Attributes every frame's time to its owner instead of reading one
// quantised framerate:
//
//   * the Present thread's CPU, split into each step of Present;
//   * the D3D submission thread (the one issuing draws on the immediate
//     context - in Metro this is NOT the Present thread), with time inside
//     the mod's hooked D3D11 calls by category;
//   * GPU time per render pass, from timestamp queries at every render-target
//     change (read back several frames later, never stalling);
//   * SteamVR's own frame timing (GPU, compositor, reprojection, drops);
//   * a background sampler naming the busiest threads in the process.
//
// Output goes to <game>\vr_perf\<timestamp>_frames.csv (one row per frame)
// and <game>\vr_perf\<timestamp>_summary.txt (a report every few seconds).
// Off unless vr_perf.txt sits beside d3d11.dll (vr_perf_hud.txt also shows the
// in-headset HUD). Independent of d3dx.ini [Logging].

#include <intrin.h>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11RenderTargetView;
struct ID3D11DepthStencilView;

namespace VRPerf {

	// Compile-time kill switch. At run time the profiler is also off unless
	// vr_perf.txt sits beside d3d11.dll; see Active().
	static const bool kEnabled = true;

	// Decided once. While false no file, thread, GPU query or hook timer is created.
	bool Active();

	enum Hook {
		kHookDraw,        // Draw*/Dispatch* including second-eye doubling
		kHookRenderTarget,// OMSetRenderTargets* including second-eye replay
		kHookConstants,   // *SetConstantBuffers, Map/Unmap, UpdateSubresource
		kHookShaders,     // *SetShader, *SetShaderResources, input layout
		kHookClearCopy,   // Clear*, Copy*, Resolve, GenerateMips
		kHookState,       // viewports, blend/depth/raster state, IA buffers, samplers
		kHookCount
	};

	enum Point {
		kPresentEnter,
		kAfterFrameActions,
		kAfterVRMenu,
		kAfterFlushSecondEye,
		kAfterSubmit,
		kAfterDxgiPresent,
		kAfterWaitGetPoses,
		kAfterStereoParams,
		kAfterUpdatePose,
		kAfterCulling,
		kAfterInputAndAim,
		kPointCount
	};

	extern unsigned long gRenderThreadId;

	void HookEnd(unsigned hook, unsigned site, unsigned long long ticks);

	// One entry per hooked function (__FUNCTION__), so the D3D thread's hook
	// time can be attributed to exact calls rather than six buckets.
	unsigned RegisterSite(const char *name);
	// Every 600 frames, the busiest sites on the D3D thread; NULL in between.
	const char *ReportSites();

	// Per-call hook timing: on while Active() and vr_perf_lite.txt is absent
	// (re-checked every 120 frames). draw_calls, per-pass draw counts and
	// d3d_first_call_ms come from hook timing and read 0 while it is off.
	extern volatile bool gHookTiming;

	struct HookScope {
		unsigned hook;
		unsigned site;
		unsigned long long start;
		bool active;
		static thread_local int tDepth;

		HookScope(unsigned h, unsigned s) : hook(h), site(s), start(0), active(false)
		{
			if (!kEnabled || !gHookTiming)
				return;
			// Remembered, so a toggle between here and the destructor cannot
			// unbalance the nesting depth.
			active = true;
			if (tDepth++ == 0)
				start = __rdtsc();
		}
		~HookScope()
		{
			if (!active)
				return;
			if (--tDepth == 0)
				HookEnd(hook, site, __rdtsc() - start);
		}
	};

	// Present boundary. BeginPresent/EndPresent bracket the hooked Present on
	// the thread that presents; Stamp records the named points in between.
	void BeginPresent(ID3D11Device *device, ID3D11DeviceContext *context);
	void Stamp(Point point);
	void EndPresent();

	// A render-target binding just changed on `context`. Called after the
	// previous pass's second eye has been flushed, so the timestamp closes
	// the previous pass including its replay.
	void MarkPass(ID3D11DeviceContext *context, unsigned numViews,
		unsigned width, unsigned height, unsigned format,
		ID3D11RenderTargetView *rtv0, ID3D11DepthStencilView *depth);

	// In-headset HUD. The text is rebuilt about once a second from the same
	// data that feeds the CSV; empty until the first second has elapsed.
	bool OverlayVisible();
	void ToggleOverlay();
	const char *OverlayText();
	// Appends one line to the summary file; ignored while the profiler is inactive.
	void Note(const char *text);

	// A synchronous GPU->CPU readback (staging copy + Map READ) took `ticks`
	// at source line `site`. Each one drains the GPU queue, serialising CPU
	// and GPU for the rest of the frame.
	void NoteReadback(int site, unsigned long long ticks);
	// The same data was served from a CPU shadow instead (no GPU stall).
	void NoteShadowRead();
}

#define VRPERF_HOOK(bucket) \
	static const unsigned vrPerfSite_ = VRPerf::RegisterSite(__FUNCTION__); \
	VRPerf::HookScope vrPerfHookScope_(VRPerf::bucket, vrPerfSite_)
