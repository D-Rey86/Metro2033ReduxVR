#include "StereoTwin.h"

#include <unordered_map>
#include <vector>
#include <cmath>

#include "log.h"
#include "Globals.h"
#include "StereoSinglePass.h"
#include "HackerContext.h"

namespace StereoTwin {

	bool gHaveTwins = false;
	bool gRedirectToTwins = false;   // bring-up test; passed, see Notes/16
	// BISECTION. Second-eye draws OFF, everything else left exactly as it is:
	// the twin targets still exist, still get cleared, still get copied and
	// submitted, the matrices are still patched per eye.
	//
	// We now issue a THIRD of the draws per second that alternate-eye did and
	// run three times slower, so "rendering twice is expensive" does not
	// explain it - something here is pathological. This says which half:
	//
	//   framerate returns to AER levels -> the doubled draws are the cost
	//   framerate stays ~21             -> the twin RESOURCES are the cost,
	//                                      and the draws are innocent
	//
	// The second outcome would point at the 479 MB of extra render targets
	// thrashing something, which is a completely different problem from the
	// one I have been trying to solve all night.
	bool gDoubleDraw = true;
	// Twin targets are array SLICES of one texture rather than separate
	// textures, which is what lets a single draw fill both eyes.
	//
	// Measured against the alternative, mess hall, standing still: slices plus
	// single-pass folding 16.83 ms, separate textures with plain double-draw
	// 20.1-20.6 ms. Worth about 3.4 ms, comfortably more than the ~1.5 ms
	// slicing costs by itself.
	bool gSliceTwins = true;
	int gBisectSkip = kSkipNothing;
	bool gBisectRunning = false;   // sweeps done; see Notes/18 for the results

	const char *SkipCategoryName(int category)
	{
		switch (category) {
		case kSkipNothing:       return "baseline, nothing skipped";
		case kSkipPost:          return "second eye skips POST (reads an eye-dependent texture)";
		case kSkipInstanced:     return "second eye skips INSTANCED draws";
		case kSkipTessellated:   return "second eye skips TESSELLATED draws";
		case kSkipPlainGeometry: return "second eye skips PLAIN GEOMETRY";
		case kSkipEverything:    return "second eye skipped ENTIRELY (upper bound)";
		case kSkipTwinClears:    return "no twin CLEARS (draws still doubled)";
		case kSkipSecondSubmit:  return "no SECOND SUBMIT to the compositor";
		case kSkipTwinLookups:   return "no per-draw TWIN LOOKUPS (all our draw-path CPU work)";
		default:                 return "?";
		}
	}
	unsigned gDoubledDraws = 0;
	unsigned gSharedDraws = 0;
	unsigned gEyeCBSwaps = 0;

	namespace {

		CRITICAL_SECTION sLock;
		bool sLockReady = false;

		// Same pattern as VRPose's scan lock: initialise on first use rather
		// than in a constructor, because entering an uninitialised
		// CRITICAL_SECTION has crashed this project before when a reader ran
		// ahead of a disabled initialiser.
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

		struct TextureTwin {
			ID3D11Texture2D *twin;
			D3D11_TEXTURE2D_DESC desc;
			// The twin is array slice 1 of `twin`, which is the original
			// texture itself, rather than a separate allocation. The back
			// buffer can never be sliced - DXGI owns its description - so
			// both kinds have to coexist.
			bool sliced;
		};

		std::unordered_map<ID3D11Resource *, TextureTwin> sTextures;
		std::unordered_map<ID3D11RenderTargetView *, ID3D11RenderTargetView *> sRTVs;
		std::unordered_map<ID3D11DepthStencilView *, ID3D11DepthStencilView *> sDSVs;
		std::unordered_map<ID3D11ShaderResourceView *, ID3D11ShaderResourceView *> sSRVs;

		// Views covering both slices, keyed by the game's own view.
		std::unordered_map<ID3D11RenderTargetView *, ID3D11RenderTargetView *> sBothRTVs;
		std::unordered_map<ID3D11DepthStencilView *, ID3D11DepthStencilView *> sBothDSVs;

		// Targets deliberately NOT twinned, kept only so the inventory dump
		// can show what was shared and why - a wrong call here is invisible
		// in the picture (the second eye just gets the first eye's shadows,
		// which is nearly right) so it needs to be readable in the log.
		struct SharedRecord {
			UINT width, height;
			UINT bindFlags;
		};
		std::unordered_map<ID3D11Resource *, SharedRecord> sShared;

		int sTwinBytesMB = 0;
		bool sDumped = false;

		bool EligibleForTwinning(const D3D11_TEXTURE2D_DESC *d)
		{
			if (!d)
				return false;
			if (!(d->BindFlags & (D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL)))
				return false;
			if (d->Usage != D3D11_USAGE_DEFAULT)
				return false;
			// Square means light space here - the 3072x3072 shadow atlas is
			// the only square render target in the frame, and it is 67% of
			// all draws. Sharing it is where the performance comes from.
			//
			// TEMPORARY: that rule is on trial. Some props and NPCs render
			// identically in both eyes - no parallax at all, even at four
			// times the interpupillary distance - while every doubled draw
			// demonstrably receives per-eye matrices. An object CAN still
			// come out identical if it is drawn into a target we chose to
			// share, because then both eyes composite from the same pixels.
			// Two of the three shared targets are colour, not depth, which
			// is not what a plain shadow atlas looks like.
			//
			// Twinning everything settles it: if the doubling stops, the
			// heuristic is wrong and needs to be narrowed to genuine shadow
			// maps rather than "square". Costs framerate while it is on,
			// since the shadow work stops being shared.
			// Answered: twinning everything changed nothing, so the shared
			// targets are innocent and sharing them costs nothing in
			// correctness. Back on, for the framerate.
			static const bool kShareSquareTargets = true;
			if (kShareSquareTargets && d->Width == d->Height)
				return false;
			// Cube maps and array targets are environment/light rendering,
			// not the eye's view.
			if (d->ArraySize != 1)
				return false;
			if (d->MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE)
				return false;
			// Below this the target cannot be part of the eye's image chain;
			// the smallest real one in the census was 56x31.
			if (d->Width < 16 || d->Height < 16)
				return false;
			return true;
		}

		int ApproxBytesPerPixel(DXGI_FORMAT f)
		{
			switch (f) {
			case DXGI_FORMAT_R32G32B32A32_TYPELESS:
			case DXGI_FORMAT_R32G32B32A32_FLOAT:   return 16;
			case DXGI_FORMAT_R16G16B16A16_TYPELESS:
			case DXGI_FORMAT_R16G16B16A16_FLOAT:
			case DXGI_FORMAT_R32G32_FLOAT:         return 8;
			case DXGI_FORMAT_R8_UNORM:
			case DXGI_FORMAT_R8_TYPELESS:          return 1;
			case DXGI_FORMAT_R16_FLOAT:
			case DXGI_FORMAT_R16_TYPELESS:
			case DXGI_FORMAT_D16_UNORM:
			case DXGI_FORMAT_R8G8_UNORM:           return 2;
			default:                               return 4;
			}
		}
	}

	// Can a vertex shader choose which slice of a render-target array it draws
	// into? That single capability decides whether single-pass stereo is
	// possible here.
	//
	// Single-pass means issuing each draw ONCE with two instances, and having
	// the vertex shader pick its eye - transforming geometry once instead of
	// twice while still filling both eyes. The measurements say that is the
	// whole prize: the second geometry pass costs 33.7ms while the game's
	// entire frame costs 13.9ms, and removing it took the mess hall from 21
	// to 72fps.
	//
	// Writing SV_RenderTargetArrayIndex from the vertex shader needs the
	// D3D11.3 feature below. Without it the only routes are a geometry shader
	// (which amplifies every triangle and would likely cost more than it
	// saves) or NVIDIA's NVAPI path (which would exclude other vendors).
	static void ReportSinglePassSupport(ID3D11Device *device)
	{
		static bool reported = false;
		if (reported || !device)
			return;
		reported = true;

		D3D11_FEATURE_DATA_D3D11_OPTIONS3 o3;
		memset(&o3, 0, sizeof(o3));
		HRESULT hr = device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS3, &o3, sizeof(o3));
		if (FAILED(hr)) {
			LogInfo("SinglePass: could not query D3D11_OPTIONS3 (hr=0x%x) - assume unsupported\n", hr);
			return;
		}
		LogInfo("SinglePass: VPAndRTArrayIndexFromAnyShaderFeedingRasterizer = %d  (%s)\n",
			o3.VPAndRTArrayIndexFromAnyShaderFeedingRasterizer,
			o3.VPAndRTArrayIndexFromAnyShaderFeedingRasterizer
				? "supported - the portable single-pass route is open"
				: "NOT supported - would need a geometry shader or NVAPI");
		LogInfo("SinglePass: feature level %d, 191 vertex shaders would need patching\n",
			(int)device->GetFeatureLevel());
	}

	void NoteDevice(ID3D11Device *device)
	{
		if (!device)
			return;
		EnsureLock();

		static ID3D11Device *sDevice = NULL;
		EnterCriticalSection(&sLock);
		const bool changed = (sDevice != NULL && sDevice != device);
		sDevice = device;
		LeaveCriticalSection(&sLock);

		if (!changed)
			return;

		LogInfo("StereoTwin: the device was replaced - dropping every twin and view "
			"cached for the old one\n");
		ReleaseAll();
		StereoSinglePass::ReleaseAll();
		ResetVRDeviceState();
	}

	void ResetForNewSwapChain()
	{
		EnsureLock();

		EnterCriticalSection(&sLock);
		const bool anything = !sTextures.empty() || !sRTVs.empty() || !sSRVs.empty();
		LeaveCriticalSection(&sLock);
		if (!anything)
			return;   // first swap chain of the session; nothing stale yet

		LogInfo("StereoTwin: new swap chain - dropping every twin and view cached "
			"for the old render targets\n");
		ReleaseAll();
		StereoSinglePass::ReleaseAll();
		ResetVRDeviceState();

		sTwinBytesMB = 0;
		sDumped = false;
	}

	bool WidenDescForStereo(D3D11_TEXTURE2D_DESC *desc)
	{
		if (!gSliceTwins || !desc)
			return false;
		if (!EligibleForTwinning(desc))
			return false;
		desc->ArraySize = 2;
		return true;
	}

	bool IsSliced(ID3D11Resource *resource)
	{
		if (!gHaveTwins || !resource)
			return false;
		EnterCriticalSection(&sLock);
		auto it = sTextures.find(resource);
		const bool sliced = (it != sTextures.end()) && it->second.sliced;
		LeaveCriticalSection(&sLock);
		return sliced;
	}

	void OnCreateTexture2D(ID3D11Device *device, const D3D11_TEXTURE2D_DESC *desc, ID3D11Texture2D *created, bool widened)
	{
		if (!device || !desc || !created)
			return;
		NoteDevice(device);
		ReportSinglePassSupport(device);

		if (!(desc->BindFlags & (D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL)))
			return;   // the overwhelming majority: ordinary art assets

		EnsureLock();

		// `desc` has already been widened by the time it gets here, so it no
		// longer passes the eligibility test - `widened` is the answer to that
		// question, decided before creation.
		if (widened) {
			EnterCriticalSection(&sLock);
			TextureTwin t;
			t.twin = created;      // slice 1 of the same texture
			t.desc = *desc;
			t.sliced = true;
			sTextures[created] = t;
			sTwinBytesMB += (int)(((unsigned long long)desc->Width * desc->Height *
				ApproxBytesPerPixel(desc->Format)) / (1024 * 1024));
			gHaveTwins = true;
			LeaveCriticalSection(&sLock);
			return;
		}

		if (!EligibleForTwinning(desc)) {
			EnterCriticalSection(&sLock);
			SharedRecord rec = { desc->Width, desc->Height, desc->BindFlags };
			sShared[created] = rec;
			LeaveCriticalSection(&sLock);
			return;
		}

		ID3D11Texture2D *twin = NULL;
		HRESULT hr = device->CreateTexture2D(desc, NULL, &twin);
		if (FAILED(hr) || !twin) {
			LogInfo("StereoTwin: FAILED to twin %ux%u fmt=%d bind=0x%x (hr=0x%x) - "
				"second eye will be missing this target\n",
				desc->Width, desc->Height, desc->Format, desc->BindFlags, hr);
			return;
		}

		EnterCriticalSection(&sLock);
		TextureTwin t;
		t.twin = twin;
		t.desc = *desc;
		t.sliced = false;
		sTextures[created] = t;
		sTwinBytesMB += (int)(((unsigned long long)desc->Width * desc->Height *
			ApproxBytesPerPixel(desc->Format)) / (1024 * 1024));
		gHaveTwins = true;
		LeaveCriticalSection(&sLock);
	}

	// ---- narrowing the game's own views to slice 0 -----------------------
	//
	// Each builds a description for ONE slice of a sliced resource. Called
	// twice per view: once with slice 0 for the game's own view, once with
	// slice 1 to make its twin, so both are guaranteed the same shape.

	static bool BuildRTVSlice(ID3D11Resource *resource, const D3D11_RENDER_TARGET_VIEW_DESC *in,
		UINT slice, D3D11_RENDER_TARGET_VIEW_DESC *out)
	{
		ID3D11Texture2D *tex = NULL;
		if (FAILED(resource->QueryInterface(__uuidof(ID3D11Texture2D), (void **)&tex)) || !tex)
			return false;
		D3D11_TEXTURE2D_DESC td;
		tex->GetDesc(&td);
		tex->Release();

		memset(out, 0, sizeof(*out));
		out->Format = in ? in->Format : td.Format;

		const bool multisampled = td.SampleDesc.Count > 1;
		if (multisampled) {
			out->ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY;
			out->Texture2DMSArray.FirstArraySlice = slice;
			out->Texture2DMSArray.ArraySize = 1;
		} else {
			out->ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
			out->Texture2DArray.MipSlice =
				(in && in->ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2D) ? in->Texture2D.MipSlice :
				(in && in->ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2DARRAY) ? in->Texture2DArray.MipSlice : 0;
			out->Texture2DArray.FirstArraySlice = slice;
			out->Texture2DArray.ArraySize = 1;
		}
		return true;
	}

	static bool BuildDSVSlice(ID3D11Resource *resource, const D3D11_DEPTH_STENCIL_VIEW_DESC *in,
		UINT slice, D3D11_DEPTH_STENCIL_VIEW_DESC *out)
	{
		ID3D11Texture2D *tex = NULL;
		if (FAILED(resource->QueryInterface(__uuidof(ID3D11Texture2D), (void **)&tex)) || !tex)
			return false;
		D3D11_TEXTURE2D_DESC td;
		tex->GetDesc(&td);
		tex->Release();

		memset(out, 0, sizeof(*out));
		out->Format = in ? in->Format : td.Format;
		out->Flags = in ? in->Flags : 0;

		if (td.SampleDesc.Count > 1) {
			out->ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DMSARRAY;
			out->Texture2DMSArray.FirstArraySlice = slice;
			out->Texture2DMSArray.ArraySize = 1;
		} else {
			out->ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
			out->Texture2DArray.MipSlice =
				(in && in->ViewDimension == D3D11_DSV_DIMENSION_TEXTURE2D) ? in->Texture2D.MipSlice :
				(in && in->ViewDimension == D3D11_DSV_DIMENSION_TEXTURE2DARRAY) ? in->Texture2DArray.MipSlice : 0;
			out->Texture2DArray.FirstArraySlice = slice;
			out->Texture2DArray.ArraySize = 1;
		}
		return true;
	}

	static bool BuildSRVSlice(ID3D11Resource *resource, const D3D11_SHADER_RESOURCE_VIEW_DESC *in,
		UINT slice, D3D11_SHADER_RESOURCE_VIEW_DESC *out)
	{
		ID3D11Texture2D *tex = NULL;
		if (FAILED(resource->QueryInterface(__uuidof(ID3D11Texture2D), (void **)&tex)) || !tex)
			return false;
		D3D11_TEXTURE2D_DESC td;
		tex->GetDesc(&td);
		tex->Release();

		memset(out, 0, sizeof(*out));
		out->Format = in ? in->Format : td.Format;

		if (td.SampleDesc.Count > 1) {
			out->ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY;
			out->Texture2DMSArray.FirstArraySlice = slice;
			out->Texture2DMSArray.ArraySize = 1;
		} else {
			out->ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
			out->Texture2DArray.MostDetailedMip =
				(in && in->ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D) ? in->Texture2D.MostDetailedMip :
				(in && in->ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2DARRAY) ? in->Texture2DArray.MostDetailedMip : 0;
			out->Texture2DArray.MipLevels =
				(in && in->ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D) ? in->Texture2D.MipLevels :
				(in && in->ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2DARRAY) ? in->Texture2DArray.MipLevels : td.MipLevels;
			out->Texture2DArray.FirstArraySlice = slice;
			out->Texture2DArray.ArraySize = 1;
		}
		return true;
	}

	const D3D11_RENDER_TARGET_VIEW_DESC *NarrowRTVDesc(ID3D11Resource *resource,
		const D3D11_RENDER_TARGET_VIEW_DESC *desc, D3D11_RENDER_TARGET_VIEW_DESC *storage)
	{
		if (!IsSliced(resource) || !BuildRTVSlice(resource, desc, 0, storage))
			return desc;
		return storage;
	}

	const D3D11_DEPTH_STENCIL_VIEW_DESC *NarrowDSVDesc(ID3D11Resource *resource,
		const D3D11_DEPTH_STENCIL_VIEW_DESC *desc, D3D11_DEPTH_STENCIL_VIEW_DESC *storage)
	{
		if (!IsSliced(resource) || !BuildDSVSlice(resource, desc, 0, storage))
			return desc;
		return storage;
	}

	const D3D11_SHADER_RESOURCE_VIEW_DESC *NarrowSRVDesc(ID3D11Resource *resource,
		const D3D11_SHADER_RESOURCE_VIEW_DESC *desc, D3D11_SHADER_RESOURCE_VIEW_DESC *storage)
	{
		if (!IsSliced(resource) || !BuildSRVSlice(resource, desc, 0, storage))
			return desc;
		return storage;
	}

	void OnCreateRTV(ID3D11Device *device, ID3D11Resource *resource, const D3D11_RENDER_TARGET_VIEW_DESC *desc, ID3D11RenderTargetView *created)
	{
		if (!gHaveTwins || !device || !resource || !created)
			return;
		EnsureLock();

		EnterCriticalSection(&sLock);
		auto it = sTextures.find(resource);
		ID3D11Texture2D *twinTex = (it != sTextures.end()) ? it->second.twin : NULL;
		const bool sliced = (it != sTextures.end()) && it->second.sliced;
		LeaveCriticalSection(&sLock);
		if (!twinTex)
			return;

		// Same shape as the view the game just got, one slice along.
		D3D11_RENDER_TARGET_VIEW_DESC sliceDesc;
		if (sliced) {
			if (!BuildRTVSlice(resource, desc, 1, &sliceDesc))
				return;
			desc = &sliceDesc;
		}

		ID3D11RenderTargetView *twinView = NULL;
		if (SUCCEEDED(device->CreateRenderTargetView(twinTex, desc, &twinView)) && twinView) {
			EnterCriticalSection(&sLock);
			sRTVs[created] = twinView;
			LeaveCriticalSection(&sLock);

			// Name every twinned render-target view as it is made.
			//
			// A forced quarter-screen shift on the right eye came back
			// completely absent from the captured image, so the second eye's
			// render is not reaching the back buffer twin. The first thing
			// to rule out is whether the back buffer's own view ever got
			// twinned - without it the engine's final blit is never doubled,
			// and the twin keeps whatever it last held.
			D3D11_TEXTURE2D_DESC td;
			twinTex->GetDesc(&td);
			LogInfo("StereoTwin: twinned RTV %p -> %p over %ux%u\n",
				created, twinView, td.Width, td.Height);
		} else {
			LogInfo("StereoTwin: FAILED to twin an RTV - that pass will never "
				"reach the second eye\n");
		}
	}

	void OnCreateDSV(ID3D11Device *device, ID3D11Resource *resource, const D3D11_DEPTH_STENCIL_VIEW_DESC *desc, ID3D11DepthStencilView *created)
	{
		if (!gHaveTwins || !device || !resource || !created)
			return;
		EnsureLock();

		EnterCriticalSection(&sLock);
		auto it = sTextures.find(resource);
		ID3D11Texture2D *twinTex = (it != sTextures.end()) ? it->second.twin : NULL;
		const bool sliced = (it != sTextures.end()) && it->second.sliced;
		LeaveCriticalSection(&sLock);
		if (!twinTex)
			return;

		D3D11_DEPTH_STENCIL_VIEW_DESC sliceDesc;
		if (sliced) {
			if (!BuildDSVSlice(resource, desc, 1, &sliceDesc))
				return;
			desc = &sliceDesc;
		}

		ID3D11DepthStencilView *twinView = NULL;
		if (SUCCEEDED(device->CreateDepthStencilView(twinTex, desc, &twinView)) && twinView) {
			EnterCriticalSection(&sLock);
			sDSVs[created] = twinView;
			LeaveCriticalSection(&sLock);
		}
	}

	void OnCreateSRV(ID3D11Device *device, ID3D11Resource *resource, const D3D11_SHADER_RESOURCE_VIEW_DESC *desc, ID3D11ShaderResourceView *created)
	{
		if (!gHaveTwins || !device || !resource || !created)
			return;
		EnsureLock();

		EnterCriticalSection(&sLock);
		auto it = sTextures.find(resource);
		ID3D11Texture2D *twinTex = (it != sTextures.end()) ? it->second.twin : NULL;
		const bool sliced = (it != sTextures.end()) && it->second.sliced;
		LeaveCriticalSection(&sLock);
		if (!twinTex)
			return;

		D3D11_SHADER_RESOURCE_VIEW_DESC sliceDesc;
		if (sliced) {
			if (!BuildSRVSlice(resource, desc, 1, &sliceDesc))
				return;
			desc = &sliceDesc;
		}

		ID3D11ShaderResourceView *twinView = NULL;
		if (SUCCEEDED(device->CreateShaderResourceView(twinTex, desc, &twinView)) && twinView) {
			EnterCriticalSection(&sLock);
			sSRVs[created] = twinView;
			LeaveCriticalSection(&sLock);
		}
	}

	ID3D11RenderTargetView *BothSlicesRTV(ID3D11Device *device, ID3D11RenderTargetView *gameView)
	{
		if (!gSliceTwins || !device || !gameView)
			return NULL;
		EnsureLock();

		EnterCriticalSection(&sLock);
		auto cached = sBothRTVs.find(gameView);
		ID3D11RenderTargetView *hit = (cached != sBothRTVs.end()) ? cached->second : NULL;
		LeaveCriticalSection(&sLock);
		if (hit)
			return hit;

		ID3D11Resource *res = NULL;
		gameView->GetResource(&res);
		if (!res)
			return NULL;
		if (!IsSliced(res)) {
			res->Release();
			return NULL;
		}

		// Same shape as the game's own view, widened from one slice to two.
		D3D11_RENDER_TARGET_VIEW_DESC d;
		gameView->GetDesc(&d);
		if (d.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2DARRAY)
			d.Texture2DArray.ArraySize = 2, d.Texture2DArray.FirstArraySlice = 0;
		else if (d.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY)
			d.Texture2DMSArray.ArraySize = 2, d.Texture2DMSArray.FirstArraySlice = 0;
		else {
			res->Release();
			return NULL;
		}

		ID3D11RenderTargetView *both = NULL;
		HRESULT hr = device->CreateRenderTargetView(res, &d, &both);
		res->Release();
		if (FAILED(hr) || !both)
			return NULL;

		EnterCriticalSection(&sLock);
		sBothRTVs[gameView] = both;
		LeaveCriticalSection(&sLock);
		return both;
	}

	ID3D11DepthStencilView *BothSlicesDSV(ID3D11Device *device, ID3D11DepthStencilView *gameView)
	{
		if (!gSliceTwins || !device || !gameView)
			return NULL;
		EnsureLock();

		EnterCriticalSection(&sLock);
		auto cached = sBothDSVs.find(gameView);
		ID3D11DepthStencilView *hit = (cached != sBothDSVs.end()) ? cached->second : NULL;
		LeaveCriticalSection(&sLock);
		if (hit)
			return hit;

		ID3D11Resource *res = NULL;
		gameView->GetResource(&res);
		if (!res)
			return NULL;
		if (!IsSliced(res)) {
			res->Release();
			return NULL;
		}

		D3D11_DEPTH_STENCIL_VIEW_DESC d;
		gameView->GetDesc(&d);
		if (d.ViewDimension == D3D11_DSV_DIMENSION_TEXTURE2DARRAY)
			d.Texture2DArray.ArraySize = 2, d.Texture2DArray.FirstArraySlice = 0;
		else if (d.ViewDimension == D3D11_DSV_DIMENSION_TEXTURE2DMSARRAY)
			d.Texture2DMSArray.ArraySize = 2, d.Texture2DMSArray.FirstArraySlice = 0;
		else {
			res->Release();
			return NULL;
		}

		ID3D11DepthStencilView *both = NULL;
		HRESULT hr = device->CreateDepthStencilView(res, &d, &both);
		res->Release();
		if (FAILED(hr) || !both)
			return NULL;

		EnterCriticalSection(&sLock);
		sBothDSVs[gameView] = both;
		LeaveCriticalSection(&sLock);
		return both;
	}

	ID3D11RenderTargetView *TwinRTV(ID3D11RenderTargetView *v)
	{
		if (!v) return NULL;
		EnterCriticalSection(&sLock);
		auto it = sRTVs.find(v);
		ID3D11RenderTargetView *r = (it != sRTVs.end()) ? it->second : NULL;
		LeaveCriticalSection(&sLock);
		return r;
	}

	ID3D11DepthStencilView *TwinDSV(ID3D11DepthStencilView *v)
	{
		if (!v) return NULL;
		EnterCriticalSection(&sLock);
		auto it = sDSVs.find(v);
		ID3D11DepthStencilView *r = (it != sDSVs.end()) ? it->second : NULL;
		LeaveCriticalSection(&sLock);
		return r;
	}

	ID3D11ShaderResourceView *TwinSRV(ID3D11ShaderResourceView *v)
	{
		if (!v) return NULL;
		EnterCriticalSection(&sLock);
		auto it = sSRVs.find(v);
		ID3D11ShaderResourceView *r = (it != sSRVs.end()) ? it->second : NULL;
		LeaveCriticalSection(&sLock);
		return r;
	}

	ID3D11Resource *TwinResource(ID3D11Resource *r)
	{
		if (!r) return NULL;
		EnterCriticalSection(&sLock);
		auto it = sTextures.find(r);
		ID3D11Resource *t = (it != sTextures.end()) ? (ID3D11Resource *)it->second.twin : NULL;
		LeaveCriticalSection(&sLock);
		return t;
	}

	void RegisterBackBuffer(ID3D11Device *device, ID3D11Texture2D *backBuffer)
	{
		if (!device || !backBuffer)
			return;
		EnsureLock();
		NoteDevice(device);

		EnterCriticalSection(&sLock);
		bool known = sTextures.count(backBuffer) != 0;
		LeaveCriticalSection(&sLock);
		if (known)
			return;

		D3D11_TEXTURE2D_DESC d;
		backBuffer->GetDesc(&d);

		// A plain off-screen copy of it: the twin is never presented, only
		// read back and handed to the compositor as the second eye, so it
		// must not carry the swap chain's own usage flags.
		D3D11_TEXTURE2D_DESC td = d;
		td.Usage = D3D11_USAGE_DEFAULT;
		td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
		td.CPUAccessFlags = 0;
		td.MiscFlags = 0;

		ID3D11Texture2D *twin = NULL;
		HRESULT hr = device->CreateTexture2D(&td, NULL, &twin);
		if (FAILED(hr) || !twin) {
			LogInfo("StereoTwin: FAILED to twin the back buffer %ux%u fmt=%d (hr=0x%x) - "
				"the second eye has nowhere to land\n", d.Width, d.Height, d.Format, hr);
			return;
		}

		EnterCriticalSection(&sLock);
		TextureTwin t;
		t.twin = twin;
		t.desc = td;
		t.sliced = false;   // DXGI owns the back buffer's description
		sTextures[backBuffer] = t;
		gHaveTwins = true;
		LeaveCriticalSection(&sLock);

		LogInfo("StereoTwin: back buffer %p twinned as %p (%ux%u fmt=%d)\n",
			backBuffer, twin, d.Width, d.Height, d.Format);
	}

	void ReportFrameStats()
	{
		static unsigned lastLogged = 0;
		static unsigned accDoubled = 0, accShared = 0, accFrames = 0, accSwaps = 0;
		accDoubled += gDoubledDraws;
		accShared += gSharedDraws;
		accSwaps += gEyeCBSwaps;
		accFrames++;
		gDoubledDraws = gSharedDraws = gEyeCBSwaps = 0;

		if (G->frame_no - lastLogged < 600 || !accFrames)
			return;
		lastLogged = G->frame_no;
		LogInfo("StereoTwin: %.0f draws/frame doubled, %.0f shared (eye-independent), "
			"%.0f of the doubled draws got the right eye's matrices\n",
			(double)accDoubled / accFrames, (double)accShared / accFrames,
			(double)accSwaps / accFrames);
		accDoubled = accShared = accFrames = accSwaps = 0;
	}

	bool GetTwinPair(unsigned width, unsigned height, int format, int index,
		ID3D11Texture2D **outOriginal, ID3D11Texture2D **outTwin)
	{
		if (!gHaveTwins)
			return false;
		EnterCriticalSection(&sLock);
		int n = 0;
		bool found = false;
		for (auto &kv : sTextures) {
			if (kv.second.desc.Width != width || kv.second.desc.Height != height)
				continue;
			if (format >= 0 && (int)kv.second.desc.Format != format)
				continue;
			if (n++ != index)
				continue;
			*outOriginal = (ID3D11Texture2D *)kv.first;
			*outTwin = kv.second.twin;
			found = true;
			break;
		}
		LeaveCriticalSection(&sLock);
		return found;
	}

	// Where does the second eye stop being a second eye?
	//
	// Photographing one arbitrary target of the nine at scene resolution was
	// a bad sample - it may well have been a buffer both eyes write
	// identically. This walks EVERY twinned target instead and reports how
	// far its twin has drifted from the original. Targets the two eyes agree
	// on read ~0; targets carrying genuine parallax read clearly above it.
	// The boundary between those two groups is the answer.
	void CompareAllTwins(ID3D11Device *device, ID3D11DeviceContext *context)
	{
		if (!gHaveTwins || !device || !context)
			return;

		EnterCriticalSection(&sLock);
		std::vector<std::pair<ID3D11Resource *, TextureTwin>> pairs(sTextures.begin(), sTextures.end());
		LeaveCriticalSection(&sLock);

		LogInfo("\n=========== twin divergence, target by target ===========\n");
		for (auto &kv : pairs) {
			const D3D11_TEXTURE2D_DESC &d = kv.second.desc;
			if (d.SampleDesc.Count > 1)
				continue;

			D3D11_TEXTURE2D_DESC sd = d;
			sd.Usage = D3D11_USAGE_STAGING;
			sd.BindFlags = 0;
			sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			sd.MiscFlags = 0;

			ID3D11Texture2D *sa = NULL, *sb = NULL;
			if (FAILED(device->CreateTexture2D(&sd, NULL, &sa)) || !sa)
				continue;
			if (FAILED(device->CreateTexture2D(&sd, NULL, &sb)) || !sb) {
				sa->Release();
				continue;
			}
			context->CopyResource(sa, kv.first);
			context->CopyResource(sb, kv.second.twin);

			D3D11_MAPPED_SUBRESOURCE ma, mb;
			double acc = 0.0;
			unsigned long long n = 0;
			if (SUCCEEDED(context->Map(sa, 0, D3D11_MAP_READ, 0, &ma)) &&
			    SUCCEEDED(context->Map(sb, 0, D3D11_MAP_READ, 0, &mb))) {
				// Sample rows sparsely - this only needs a magnitude.
				for (UINT y = 0; y < d.Height; y += 8) {
					const unsigned char *pa = (const unsigned char *)ma.pData + (size_t)y * ma.RowPitch;
					const unsigned char *pb = (const unsigned char *)mb.pData + (size_t)y * mb.RowPitch;
					for (UINT x = 0; x < d.Width * 4; x += 32) {
						acc += fabs((double)pa[x] - (double)pb[x]);
						n++;
					}
				}
				context->Unmap(sb, 0);
				context->Unmap(sa, 0);
			}
			sa->Release();
			sb->Release();

			LogInfo("  %4ux%-4u fmt=%-3d  mean|orig-twin| = %8.3f  %s\n",
				d.Width, d.Height, d.Format, n ? acc / n : 0.0,
				(n && acc / n < 0.05) ? "IDENTICAL - eyes agree here" : "differs");
		}
		LogInfo("=========================================================\n\n");
	}

	void MaybeDumpInventory()
	{
		if (sDumped || !gHaveTwins || G->frame_no < 600)
			return;
		sDumped = true;

		EnterCriticalSection(&sLock);
		LogInfo("\n=========== StereoTwin inventory (frame %u) ===========\n", G->frame_no);
		LogInfo("--- twinned (eye-dependent) ---\n");
		for (auto &kv : sTextures) {
			const D3D11_TEXTURE2D_DESC &d = kv.second.desc;
			LogInfo("  %p -> %p  %4ux%-4u fmt=%-3d bind=0x%02x mips=%u\n",
				kv.first, kv.second.twin, d.Width, d.Height, d.Format,
				d.BindFlags, d.MipLevels);
		}
		LogInfo("--- shared (eye-independent: square = light space) ---\n");
		for (auto &kv : sShared) {
			LogInfo("  %p  %4ux%-4u bind=0x%02x\n", kv.first,
				kv.second.width, kv.second.height, kv.second.bindFlags);
		}
		size_t slicedCount = 0;
		for (auto &kv : sTextures)
			if (kv.second.sliced) slicedCount++;
		LogInfo("  textures twinned : %zu (+%d MB), of which %zu carry both eyes "
			"as array slices\n", sTextures.size(), sTwinBytesMB, slicedCount);
		LogInfo("  textures shared  : %zu\n", sShared.size());
		LogInfo("  views twinned    : %zu RTV, %zu DSV, %zu SRV\n",
			sRTVs.size(), sDSVs.size(), sSRVs.size());
		LogInfo("  redirect-to-twins: %s\n", gRedirectToTwins ? "ON (bring-up test)" : "off");
		LogInfo("======================================================\n\n");
		LeaveCriticalSection(&sLock);
	}

	static volatile LONG sModeTogglePending = 0;

	void RequestStereoModeToggle()
	{
		InterlockedExchange(&sModeTogglePending, 1);
	}

	bool ApplyPendingStereoModeToggle()
	{
		if (InterlockedExchange(&sModeTogglePending, 0) == 0)
			return false;

		gDoubleDraw = !gDoubleDraw;

		// The twin targets stay allocated in both modes. They measured free
		// when nothing draws into them - 72fps with the second eye's draws
		// off and every twin still created, cleared and submitted - so there
		// is nothing to gain by tearing them down and rebuilding on a toggle.
		LogInfo("StereoTwin: switched to %s\n",
			gDoubleDraw ? "TRUE STEREO (both eyes per frame)"
			            : "ALTERNATE-EYE (one eye per frame, faster)");
		return true;
	}

	void ReleaseAll()
	{
		if (!sLockReady)
			return;
		EnterCriticalSection(&sLock);
		for (auto &kv : sRTVs) if (kv.second) kv.second->Release();
		for (auto &kv : sDSVs) if (kv.second) kv.second->Release();
		for (auto &kv : sSRVs) if (kv.second) kv.second->Release();
		for (auto &kv : sBothRTVs) if (kv.second) kv.second->Release();
		for (auto &kv : sBothDSVs) if (kv.second) kv.second->Release();
		sBothRTVs.clear();
		sBothDSVs.clear();
		// A sliced twin is the game's own texture, which we never took a
		// reference on - only the separately allocated ones are ours to free.
		for (auto &kv : sTextures)
			if (kv.second.twin && !kv.second.sliced) kv.second.twin->Release();
		sRTVs.clear();
		sDSVs.clear();
		sSRVs.clear();
		sTextures.clear();
		sShared.clear();
		gHaveTwins = false;
		LeaveCriticalSection(&sLock);
	}
}
