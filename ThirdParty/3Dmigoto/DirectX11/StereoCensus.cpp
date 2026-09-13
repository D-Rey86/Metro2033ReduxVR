#include "StereoCensus.h"

#include <d3d11_1.h>
#include <cstring>

#include "log.h"
#include "Globals.h"

namespace StereoCensus {

	bool gActive = false;
	bool gArmedOnce = false;

	namespace {

		// Long enough to see any frame-to-frame variation (some passes only
		// run on alternate frames, and alternate-eye rendering itself means
		// consecutive frames are different eyes), short enough that the dump
		// stays readable.
		const int kCaptureFrames = 4;

		const int kMaxPasses = 1024;
		const int kMaxResources = 128;

		struct PassRecord {
			void *rtv;
			void *dsv;
			float vpW, vpH, vpMinZ, vpMaxZ;
			int draws;
			int dispatches;
			int clears;
			unsigned long long indices;
		};

		struct ResourceRecord {
			void *view;          // the RTV/DSV pointer, which is what draws quote
			void *resource;      // the underlying texture, shared between views
			bool depth;
			UINT width, height;
			UINT mips, arraySize, samples;
			DXGI_FORMAT format;
			int passesUsing;
			unsigned long long draws;
		};

		bool sArmed = false;
		bool sFinished = false;
		int sFramesCaptured = 0;

		PassRecord sPasses[kMaxPasses];
		int sPassCount = 0;

		ResourceRecord sResources[kMaxResources];
		int sResourceCount = 0;

		// Live pass state - a "pass" here is a run of draws sharing one render
		// target set and viewport, which is the granularity at which the
		// second eye would have to be redirected.
		void *sCurRTV = NULL;
		void *sCurDSV = NULL;
		float sCurVpW = 0.0f, sCurVpH = 0.0f, sCurMinZ = 0.0f, sCurMaxZ = 1.0f;
		bool sPassOpen = false;

		int sFrameDraws = 0, sFrameDispatches = 0, sFrameClears = 0;
		int sFrameCopies = 0, sFrameMapDiscards = 0;

		int BytesPerPixel(DXGI_FORMAT f)
		{
			switch (f) {
			case DXGI_FORMAT_R32G32B32A32_TYPELESS:
			case DXGI_FORMAT_R32G32B32A32_FLOAT:
			case DXGI_FORMAT_R32G32B32A32_UINT:
				return 16;
			case DXGI_FORMAT_R32G32B32_FLOAT:
				return 12;
			case DXGI_FORMAT_R16G16B16A16_TYPELESS:
			case DXGI_FORMAT_R16G16B16A16_FLOAT:
			case DXGI_FORMAT_R16G16B16A16_UNORM:
			case DXGI_FORMAT_R32G32_FLOAT:
				return 8;
			case DXGI_FORMAT_R8_UNORM:
			case DXGI_FORMAT_A8_UNORM:
			case DXGI_FORMAT_R8_TYPELESS:
				return 1;
			case DXGI_FORMAT_R16_FLOAT:
			case DXGI_FORMAT_R16_UNORM:
			case DXGI_FORMAT_R16_TYPELESS:
			case DXGI_FORMAT_D16_UNORM:
			case DXGI_FORMAT_R8G8_UNORM:
				return 2;
			default:
				return 4;   // the great majority: RGBA8, R11G11B10, D24S8, R32
			}
		}

		const char *FormatName(DXGI_FORMAT f)
		{
			switch (f) {
			case DXGI_FORMAT_R8G8B8A8_UNORM:          return "RGBA8";
			case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:     return "RGBA8_sRGB";
			case DXGI_FORMAT_R8G8B8A8_TYPELESS:       return "RGBA8_TL";
			case DXGI_FORMAT_B8G8R8A8_UNORM:          return "BGRA8";
			case DXGI_FORMAT_R10G10B10A2_UNORM:       return "RGB10A2";
			case DXGI_FORMAT_R11G11B10_FLOAT:         return "R11G11B10";
			case DXGI_FORMAT_R16G16B16A16_FLOAT:      return "RGBA16F";
			case DXGI_FORMAT_R16G16_FLOAT:            return "RG16F";
			case DXGI_FORMAT_R32_FLOAT:               return "R32F";
			case DXGI_FORMAT_R16_FLOAT:               return "R16F";
			case DXGI_FORMAT_R8_UNORM:                return "R8";
			case DXGI_FORMAT_R24G8_TYPELESS:          return "R24G8_TL";
			case DXGI_FORMAT_D24_UNORM_S8_UINT:       return "D24S8";
			case DXGI_FORMAT_R32_TYPELESS:            return "R32_TL";
			case DXGI_FORMAT_D32_FLOAT:               return "D32F";
			case DXGI_FORMAT_R16_TYPELESS:            return "R16_TL";
			case DXGI_FORMAT_D16_UNORM:               return "D16";
			default:                                  return "other";
			}
		}

		// Records a view the first time it is seen, resolving its underlying
		// texture. Only ever called on a pass change during a capture, so the
		// COM traffic here is a few hundred calls per session, not per draw.
		int InternView(void *view, bool depth)
		{
			if (!view)
				return -1;
			for (int i = 0; i < sResourceCount; i++) {
				if (sResources[i].view == view)
					return i;
			}
			if (sResourceCount >= kMaxResources)
				return -1;

			ResourceRecord &r = sResources[sResourceCount];
			memset(&r, 0, sizeof(r));
			r.view = view;
			r.depth = depth;

			ID3D11Resource *res = NULL;
			if (depth)
				((ID3D11DepthStencilView *)view)->GetResource(&res);
			else
				((ID3D11RenderTargetView *)view)->GetResource(&res);
			if (res) {
				r.resource = res;
				ID3D11Texture2D *tex = NULL;
				if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), (void **)&tex)) && tex) {
					D3D11_TEXTURE2D_DESC d;
					tex->GetDesc(&d);
					r.width = d.Width;
					r.height = d.Height;
					r.mips = d.MipLevels;
					r.arraySize = d.ArraySize;
					r.samples = d.SampleDesc.Count;
					r.format = d.Format;
					tex->Release();
				}
				res->Release();   // we only wanted the pointer as an identity
			}
			return sResourceCount++;
		}

		void ClosePass()
		{
			if (!sPassOpen)
				return;
			sPassOpen = false;
			// Passes that issued no work at all are state churn, not render
			// passes - dropping them keeps the dump about the actual graph.
			if (sPasses[sPassCount].draws == 0 &&
			    sPasses[sPassCount].dispatches == 0 &&
			    sPasses[sPassCount].clears == 0)
				return;
			if (sPassCount < kMaxPasses - 1)
				sPassCount++;
		}

		void OpenPass()
		{
			if (sPassCount >= kMaxPasses - 1)
				return;
			PassRecord &p = sPasses[sPassCount];
			memset(&p, 0, sizeof(p));
			p.rtv = sCurRTV;
			p.dsv = sCurDSV;
			p.vpW = sCurVpW;
			p.vpH = sCurVpH;
			p.vpMinZ = sCurMinZ;
			p.vpMaxZ = sCurMaxZ;
			sPassOpen = true;

			int ri = InternView(sCurRTV, false);
			if (ri >= 0) sResources[ri].passesUsing++;
			int di = InternView(sCurDSV, true);
			if (di >= 0) sResources[di].passesUsing++;
		}

		void Dump()
		{
			LogInfo("\n");
			LogInfo("=========== StereoCensus: render graph over %d frames ===========\n", sFramesCaptured);

			LogInfo("--- render targets and depth buffers actually used ---\n");
			unsigned long long totalBytes = 0;
			for (int i = 0; i < sResourceCount; i++) {
				const ResourceRecord &r = sResources[i];
				unsigned long long bytes = (unsigned long long)r.width * r.height *
					BytesPerPixel(r.format) * (r.arraySize ? r.arraySize : 1) *
					(r.samples ? r.samples : 1);
				// Mip chains add about a third.
				if (r.mips != 1) bytes = bytes * 4 / 3;
				totalBytes += bytes;
				LogInfo("  %-5s view=%p res=%p %4ux%-4u %-11s mips=%u arr=%u msaa=%u "
					"passes=%d draws=%llu  %.2f MB\n",
					r.depth ? "DEPTH" : "COLOR", r.view, r.resource,
					r.width, r.height, FormatName(r.format),
					r.mips, r.arraySize, r.samples,
					r.passesUsing, r.draws, bytes / (1024.0 * 1024.0));
			}
			LogInfo("  TOTAL render-target memory in use: %.1f MB "
				"(true stereo needs a second set of the eye-dependent ones)\n",
				totalBytes / (1024.0 * 1024.0));

			LogInfo("--- pass sequence (%d passes over %d frames) ---\n", sPassCount, sFramesCaptured);
			for (int i = 0; i < sPassCount; i++) {
				const PassRecord &p = sPasses[i];
				LogInfo("  [%4d] rtv=%p dsv=%p vp=%.0fx%.0f z=[%.2f,%.2f] "
					"draws=%d disp=%d clears=%d idx=%llu\n",
					i, p.rtv, p.dsv, p.vpW, p.vpH, p.vpMinZ, p.vpMaxZ,
					p.draws, p.dispatches, p.clears, p.indices);
			}

			LogInfo("--- per-frame averages (this is the doubling cost) ---\n");
			const double f = sFramesCaptured ? (double)sFramesCaptured : 1.0;
			LogInfo("  passes/frame      %.1f\n", sPassCount / f);
			LogInfo("  draws/frame       %.1f\n", sFrameDraws / f);
			LogInfo("  dispatches/frame  %.1f\n", sFrameDispatches / f);
			LogInfo("  clears/frame      %.1f\n", sFrameClears / f);
			LogInfo("  copies/frame      %.1f\n", sFrameCopies / f);
			LogInfo("  CB map-discards/frame %.1f\n", sFrameMapDiscards / f);
			LogInfo("================ StereoCensus: end ================\n\n");
		}
	}

	// Arming mid-frame would make the first captured frame a partial one -
	// everything before the first weapon draw missing, which is most of the
	// graph. So arming only takes effect at the next frame boundary.
	void Arm()
	{
		if (sFinished || sArmed)
			return;
		sArmed = true;
		gArmedOnce = true;
		LogInfo("StereoCensus: armed at frame %u - capturing %d whole frames from the next boundary\n",
			G->frame_no, kCaptureFrames);
	}

	void NoteRenderTargets(ID3D11RenderTargetView *rtv, ID3D11DepthStencilView *dsv)
	{
		if ((void *)rtv == sCurRTV && (void *)dsv == sCurDSV)
			return;
		ClosePass();
		sCurRTV = (void *)rtv;
		sCurDSV = (void *)dsv;
		OpenPass();
	}

	void NoteViewport(float width, float height, float minZ, float maxZ)
	{
		if (width == sCurVpW && height == sCurVpH && minZ == sCurMinZ && maxZ == sCurMaxZ)
			return;
		ClosePass();
		sCurVpW = width;
		sCurVpH = height;
		sCurMinZ = minZ;
		sCurMaxZ = maxZ;
		OpenPass();
	}

	void NoteDraw(unsigned indexCount, unsigned instanceCount)
	{
		if (!sPassOpen)
			OpenPass();
		if (sPassOpen) {
			sPasses[sPassCount].draws++;
			sPasses[sPassCount].indices += (unsigned long long)indexCount *
				(instanceCount ? instanceCount : 1);
		}
		sFrameDraws++;

		int ri = InternView(sCurRTV, false);
		if (ri >= 0) sResources[ri].draws++;
		int di = InternView(sCurDSV, true);
		if (di >= 0) sResources[di].draws++;
	}

	void NoteDispatch()
	{
		if (sPassOpen) sPasses[sPassCount].dispatches++;
		sFrameDispatches++;
	}

	void NoteClear(bool depth)
	{
		UNREFERENCED_PARAMETER(depth);
		if (sPassOpen) sPasses[sPassCount].clears++;
		sFrameClears++;
	}

	void NoteCopy()      { sFrameCopies++; }
	void NoteMapDiscard(){ sFrameMapDiscards++; }

	void EndFrame()
	{
		if (!gActive) {
			if (sArmed && !sFinished)
				gActive = true;   // start clean at the top of the next frame
			return;
		}
		ClosePass();
		sPassOpen = false;
		sCurRTV = NULL;
		sCurDSV = NULL;

		sFramesCaptured++;
		if (sFramesCaptured >= kCaptureFrames || sPassCount >= kMaxPasses - 1) {
			gActive = false;
			sFinished = true;
			Dump();
		}
	}
}
