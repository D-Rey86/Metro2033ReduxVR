#include <windows.h>
#include <tlhelp32.h>

#include "VRPerf.h"

#include <d3d11_1.h>
#include <dxgi1_4.h>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>
#include <openvr.h>

#include "StereoTwin.h"
#include "StereoSinglePass.h"
#include "VRPose.h"

namespace VRPerf {

	unsigned long gRenderThreadId = 0;
	thread_local int HookScope::tDepth = 0;
	volatile bool gHookTiming = false;

	namespace {

		const int kSlots = 6;
		const int kMaxMarks = 768;
		// frame begin + one per pass mark + present enter + after flush + after submit
		const int kMaxTs = kMaxMarks + 4;
		const double kSummarySeconds = 5.0;
		const double kLiveSeconds = 1.0;
		const long long kCsvByteLimit = 512LL * 1024 * 1024;
		const unsigned long long kKeyFrameStart = 1;

		// ---------------------------------------------------------------
		// Columns of the per-frame CSV. The same table drives the interval
		// sums, so a column added here is automatically averaged.
		enum Col {
			cFrame, cTimeS, cTrueStereo,
			cWallMs, cPresentThreadBusyMs, cD3DThreadBusyMs, cGameMs, cD3DFirstCallMs,
			cHookDrawMs, cHookRenderTargetMs, cHookConstantsMs, cHookShadersMs,
			cHookClearCopyMs, cHookStateMs, cHooksTotalMs, cOtherThreadHookMs,
			cDrawCalls, cRenderTargetCalls, cConstantsCalls, cShadersCalls,
			cClearCopyCalls, cStateCalls, cOtherThreadHookCalls,
			cReadbackMs, cReadbackCalls,
			cDoubledDraws, cSharedDraws, cEyeCBSwaps, cFoldedDraws, cPasses, cRTBinds,
			cPresFrameActionsMs, cPresVRMenuMs, cPresFlushSecondEyeMs, cPresSubmitMs,
			cPresDxgiPresentMs, cPresWaitGetPosesMs, cPostStereoParamsMs, cPostUpdatePoseMs,
			cPostCullingMs, cPostInputAimMs, cPostTailMs, cPresentTotalMs,
			cGpuFrameMs, cGpuSceneMs, cGpuPresentWorkMs, cGpuSubmitMs,
			cGpuMaxPassMs, cGpuMaxPassId,
			cVrTotalGpuMs, cVrPreSubmitGpuMs, cVrPostSubmitGpuMs, cVrCompositorGpuMs,
			cVrCompositorCpuMs, cVrCompositorIdleMs, cVrClientIntervalMs, cVrSubmitMs,
			cVrReprojFlags, cVrPresents, cVrMispresented, cVrDropped,
			cColCount
		};

		const char *kColNames[cColCount] = {
			"frame", "time_s", "true_stereo",
			"wall_ms", "present_thread_busy_ms", "d3d_thread_busy_ms", "game_ms", "d3d_first_call_ms",
			"hook_draw_ms", "hook_rt_ms", "hook_cb_ms", "hook_shader_ms",
			"hook_clearcopy_ms", "hook_state_ms", "hooks_total_ms", "other_thread_hook_ms",
			"draw_calls", "rt_calls", "cb_calls", "shader_calls",
			"clearcopy_calls", "state_calls", "other_thread_hook_calls",
			"readback_ms", "readback_calls",
			"doubled_draws", "shared_draws", "eye_cb_swaps", "folded_draws", "passes", "rt_binds",
			"pres_frame_actions_ms", "pres_vr_menu_ms", "pres_flush_2nd_eye_ms", "pres_submit_ms",
			"pres_dxgi_present_ms", "pres_wait_get_poses_ms", "post_stereo_params_ms", "post_update_pose_ms",
			"post_culling_ms", "post_input_aim_ms", "post_tail_ms", "present_total_ms",
			"gpu_frame_ms", "gpu_scene_ms", "gpu_present_work_ms", "gpu_submit_ms",
			"gpu_max_pass_ms", "gpu_max_pass_id",
			"vr_total_gpu_ms", "vr_presubmit_gpu_ms", "vr_postsubmit_gpu_ms", "vr_compositor_gpu_ms",
			"vr_compositor_cpu_ms", "vr_compositor_idle_ms", "vr_client_interval_ms", "vr_submit_ms",
			"vr_reproj_flags", "vr_presents", "vr_mispresented", "vr_dropped",
		};

		// Columns that also get percentiles in the summary.
		const int kPctCols[] = {
			cWallMs, cPresentThreadBusyMs, cD3DThreadBusyMs, cGameMs, cHooksTotalMs,
			cPresentTotalMs, cPresWaitGetPosesMs, cGpuFrameMs, cGpuSceneMs, cVrTotalGpuMs,
		};
		const int kPctColCount = sizeof(kPctCols) / sizeof(kPctCols[0]);

		// ---------------------------------------------------------------
		// Hook accumulators, by who called:
		//   D3D thread     - issues draws on the immediate context (identified
		//                    by MarkPass); this is the game's rendering work
		//   Present thread - when it is a different thread, hooks it runs
		//                    outside Present
		//   anything else  - deferred contexts / loaders
		std::atomic<unsigned long> sD3DThreadId(0);
		HANDLE sD3DThreadHandle = NULL;
		std::atomic<unsigned long long> sD3DTicks[kHookCount];
		std::atomic<unsigned> sD3DCalls[kHookCount];
		unsigned long long sPresentTicks[kHookCount];
		unsigned sPresentCalls[kHookCount];
		std::atomic<unsigned long long> sOtherTicks[kHookCount];
		std::atomic<unsigned> sOtherCalls[kHookCount];
		std::atomic<unsigned> sPassDraws(0);
		bool sInsidePresent = false;
		// First hooked call the D3D thread makes after our Present returned:
		// splits Metro's zero-draw "frame start" into waiting vs. its own work.
		std::atomic<unsigned long long> sD3DFirstCallTsc(0);

		// Synchronous readbacks, per frame and per call site (source line).
		const int kReadbackSites = 64;
		struct ReadbackSite {
			std::atomic<int> line;
			std::atomic<unsigned long long> ticks;
			std::atomic<unsigned> calls;
			unsigned long long reportedTicks;
			unsigned reportedCalls;
		};
		ReadbackSite sReadbackSites[kReadbackSites];
		std::atomic<unsigned long long> sReadbackTicks(0);
		std::atomic<unsigned> sReadbackCalls(0);
		std::atomic<unsigned long long> sShadowReads(0);
		unsigned long long sShadowReadsReported = 0;

		double sTscPerMs = 0.0;
		LARGE_INTEGER sQpcFreq;
		unsigned long long sStartTsc = 0;

		inline double TscMs(unsigned long long t)
		{
			return sTscPerMs > 0.0 ? (double)t / sTscPerMs : 0.0;
		}

		// ---------------------------------------------------------------
		struct PassInfo {
			std::string label;
			unsigned id;
		};
		struct PassAgg {
			double gpuMs, cpuMs, gpuMax;
			unsigned long long draws;
			unsigned segs;
		};

		struct Slot {
			ID3D11Query *disjoint;
			ID3D11Query *ts[kMaxTs];
			int nTs;
			int nSegs;
			unsigned long long segKey[kMaxMarks + 1];
			unsigned long long segCpu[kMaxMarks + 2];
			unsigned segDraws[kMaxMarks + 1];
			int tsPresentEnter, tsAfterFlush, tsAfterSubmit;
			bool open, closed, pending;
			double row[cColCount];
		};

		Slot sSlots[kSlots];
		int sCur = -1;
		int sQueue[kSlots];
		int sQHead = 0, sQCount = 0;
		UINT64 sTsVals[kMaxTs];

		bool sInitialised = false;
		bool sFailed = false;
		bool sGpuOk = false;
		ID3D11Device *sDevice = nullptr;
		ID3D11DeviceContext *sContext = nullptr;
		IDXGIAdapter3 *sAdapter3 = nullptr;

		FILE *sCsv = nullptr;
		FILE *sSummary = nullptr;
		long long sCsvBytes = 0;

		unsigned sFrame = 0;
		unsigned long long sStamps[kPointCount];
		unsigned long long sPrevExit = 0;
		ULONG64 sPrevCycles = 0;
		ULONG64 sPrevD3DCycles = 0;
		unsigned long long sGameHookTicks[kHookCount];
		unsigned sGameHookCalls[kHookCount];
		std::atomic<unsigned> sBinds(0);
		unsigned sPrevDoubled = 0, sPrevShared = 0, sPrevEyeCB = 0, sPrevFolded = 0;

		// Inserted by MarkPass on the D3D thread, read on the Present thread.
		std::unordered_map<unsigned long long, PassInfo> sPassInfo;
		SRWLOCK sPassInfoLock = SRWLOCK_INIT;
		// A newly opened D3D-thread handle, adopted (and the old one closed) by
		// EndPresent on the Present thread.
		std::atomic<HANDLE> sPendingD3DThreadHandle(NULL);
		std::unordered_map<unsigned long long, PassAgg> sPassAgg;

		// Interval accumulation (summary file).
		struct Window {
			double sum[cColCount];
			unsigned cnt[cColCount];
			double max[cColCount];
			unsigned frames, firstFrame, gpuDropped;
			unsigned reprojCpu, reprojGpu, reprojMotion, throttled, vrFrames;
			unsigned long long startTsc;
			std::unordered_map<unsigned long long, PassAgg> passes;

			void Reset()
			{
				for (int c = 0; c < cColCount; c++) {
					sum[c] = 0.0;
					cnt[c] = 0;
					max[c] = 0.0;
				}
				frames = firstFrame = gpuDropped = 0;
				reprojCpu = reprojGpu = reprojMotion = throttled = vrFrames = 0;
				startTsc = __rdtsc();
				passes.clear();
			}
			double Avg(int c) const { return cnt[c] ? sum[c] / cnt[c] : NAN; }
		};
		Window sInterval, sLive;
		std::vector<float> sPct[kPctColCount];
		unsigned sMarkOverflow = 0;

		float sDisplayHz = 0.0f;
		bool sHmdLogged = false;

		// HUD
		bool sOverlayVisible = false;
		std::string sOverlayText;

		// Thread sampler hand-off.
		CRITICAL_SECTION sThreadLock;
		std::string sThreadReport;
		volatile bool sSamplerStarted = false;

		// ---------------------------------------------------------------
		const char *FormatName(unsigned f)
		{
			switch (f) {
			case 2: return "RGBA32F";
			case 10: return "RGBA16F";
			case 11: return "RGBA16";
			case 19: return "R32G8X24";
			case 20: return "D32S8";
			case 24: return "RGB10A2";
			case 26: return "R11G11B10F";
			case 27: return "RGBA8_TYPELESS";
			case 28: return "RGBA8";
			case 29: return "RGBA8_SRGB";
			case 34: return "RG16F";
			case 35: return "RG16";
			case 39: return "R32_TYPELESS";
			case 40: return "D32F";
			case 41: return "R32F";
			case 44: return "R24G8_TYPELESS";
			case 45: return "D24S8";
			case 54: return "R16F";
			case 55: return "D16";
			case 56: return "R16";
			case 61: return "R8";
			case 87: return "BGRA8";
			case 91: return "BGRA8_SRGB";
			}
			static char buf[16];
			sprintf_s(buf, "fmt%u", f);
			return buf;
		}

		std::string Narrow(const wchar_t *w)
		{
			char buf[512] = {};
			WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, sizeof(buf) - 1, NULL, NULL);
			return buf;
		}

		void CalibrateTsc()
		{
			QueryPerformanceFrequency(&sQpcFreq);
			LARGE_INTEGER q0, q1;
			QueryPerformanceCounter(&q0);
			const unsigned long long t0 = __rdtsc();
			Sleep(25);
			QueryPerformanceCounter(&q1);
			const unsigned long long t1 = __rdtsc();
			const double ms = (double)(q1.QuadPart - q0.QuadPart) * 1000.0 / (double)sQpcFreq.QuadPart;
			sTscPerMs = ms > 0.0 ? (double)(t1 - t0) / ms : 0.0;
		}

		// ---------------------------------------------------------------
		// Background thread sampler: which threads in the process are
		// actually burning CPU.
		DWORD WINAPI SamplerThread(LPVOID)
		{
			typedef LONG (NTAPI *NtQueryInformationThreadFn)(HANDLE, int, PVOID, ULONG, PULONG);
			typedef HRESULT (WINAPI *GetThreadDescriptionFn)(HANDLE, PWSTR *);
			NtQueryInformationThreadFn ntQuery = (NtQueryInformationThreadFn)
				GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread");
			GetThreadDescriptionFn getDesc = (GetThreadDescriptionFn)
				GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription");

			const DWORD pid = GetCurrentProcessId();
			std::map<DWORD, ULONG64> prevCycles;
			std::map<DWORD, std::string> names;
			LARGE_INTEGER prevQpc = {};
			ULONGLONG prevProcTime = 0;

			for (;;) {
				Sleep((DWORD)(kSummarySeconds * 1000.0));

				LARGE_INTEGER now;
				QueryPerformanceCounter(&now);
				const double intervalMs = prevQpc.QuadPart
					? (double)(now.QuadPart - prevQpc.QuadPart) * 1000.0 / (double)sQpcFreq.QuadPart : 0.0;
				prevQpc = now;

				FILETIME ct, et, kt, ut;
				ULONGLONG procTime = 0;
				if (GetProcessTimes(GetCurrentProcess(), &ct, &et, &kt, &ut))
					procTime = (((ULONGLONG)kt.dwHighDateTime << 32) | kt.dwLowDateTime) +
						(((ULONGLONG)ut.dwHighDateTime << 32) | ut.dwLowDateTime);
				const double procCores = (intervalMs > 0.0 && prevProcTime)
					? (double)(procTime - prevProcTime) / 10000.0 / intervalMs : 0.0;
				prevProcTime = procTime;

				struct Row { double pct; DWORD tid; };
				std::vector<Row> rows;
				std::map<DWORD, ULONG64> cycles;

				HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
				if (snap == INVALID_HANDLE_VALUE)
					continue;
				THREADENTRY32 te;
				te.dwSize = sizeof(te);
				for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
					if (te.th32OwnerProcessID != pid)
						continue;
					HANDLE th = OpenThread(THREAD_QUERY_INFORMATION | THREAD_QUERY_LIMITED_INFORMATION,
						FALSE, te.th32ThreadID);
					if (!th)
						continue;
					ULONG64 c = 0;
					QueryThreadCycleTime(th, &c);
					cycles[te.th32ThreadID] = c;

					if (names.find(te.th32ThreadID) == names.end()) {
						std::string name;
						void *start = nullptr;
						if (ntQuery && ntQuery(th, 9 /* ThreadQuerySetWin32StartAddress */,
							&start, sizeof(start), NULL) == 0 && start) {
							HMODULE mod = NULL;
							char buf[300];
							if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
								GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)start, &mod) && mod) {
								wchar_t path[MAX_PATH] = {};
								GetModuleFileNameW(mod, path, MAX_PATH);
								const wchar_t *base = wcsrchr(path, L'\\');
								sprintf_s(buf, "%s+0x%llx", Narrow(base ? base + 1 : path).c_str(),
									(unsigned long long)((char *)start - (char *)mod));
							} else {
								sprintf_s(buf, "0x%p", start);
							}
							name = buf;
						}
						if (getDesc) {
							PWSTR desc = nullptr;
							if (SUCCEEDED(getDesc(th, &desc)) && desc) {
								if (desc[0])
									name += " \"" + Narrow(desc) + "\"";
								LocalFree(desc);
							}
						}
						names[te.th32ThreadID] = name;
					}
					CloseHandle(th);

					auto p = prevCycles.find(te.th32ThreadID);
					if (p != prevCycles.end() && intervalMs > 0.0 && sTscPerMs > 0.0) {
						const double pct = (double)(c - p->second) / (intervalMs * sTscPerMs) * 100.0;
						rows.push_back({ pct, te.th32ThreadID });
					}
				}
				CloseHandle(snap);
				prevCycles.swap(cycles);

				std::sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) { return a.pct > b.pct; });
				std::string report;
				char line[512];
				sprintf_s(line, "THREADS (last %.1f s, %% of one core; process total %.2f cores, %u threads)\n",
					intervalMs / 1000.0, procCores, (unsigned)cycles.size());
				report += line;
				const unsigned long d3d = sD3DThreadId.load(std::memory_order_relaxed);
				for (size_t i = 0; i < rows.size() && i < 12; i++) {
					const DWORD tid = rows[i].tid;
					sprintf_s(line, "  %6.1f%%  tid %-6lu %s%s%s\n", rows[i].pct, tid, names[tid].c_str(),
						tid == gRenderThreadId ? "   <== Present thread" : "",
						tid == d3d ? "   <== D3D submission thread" : "");
					report += line;
				}

				EnterCriticalSection(&sThreadLock);
				sThreadReport.swap(report);
				LeaveCriticalSection(&sThreadLock);
			}
		}

		// ---------------------------------------------------------------
		void ReleaseQueries()
		{
			for (int i = 0; i < kSlots; i++) {
				Slot &s = sSlots[i];
				if (s.disjoint) { s.disjoint->Release(); s.disjoint = nullptr; }
				for (int j = 0; j < kMaxTs; j++)
					if (s.ts[j]) { s.ts[j]->Release(); s.ts[j] = nullptr; }
				s.open = s.closed = s.pending = false;
			}
			sCur = -1;
			sQHead = sQCount = 0;
			sGpuOk = false;
			if (sAdapter3) { sAdapter3->Release(); sAdapter3 = nullptr; }
		}

		bool CreateQueries(ID3D11Device *device)
		{
			D3D11_QUERY_DESC qd = {};
			for (int i = 0; i < kSlots; i++) {
				Slot &s = sSlots[i];
				qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
				if (FAILED(device->CreateQuery(&qd, &s.disjoint)))
					return false;
				qd.Query = D3D11_QUERY_TIMESTAMP;
				for (int j = 0; j < kMaxTs; j++)
					if (FAILED(device->CreateQuery(&qd, &s.ts[j])))
						return false;
			}
			return true;
		}

		std::string AdapterName(ID3D11Device *device)
		{
			std::string name = "unknown";
			IDXGIDevice *dxgiDevice = nullptr;
			if (FAILED(device->QueryInterface(__uuidof(IDXGIDevice), (void **)&dxgiDevice)) || !dxgiDevice)
				return name;
			IDXGIAdapter *adapter = nullptr;
			dxgiDevice->GetAdapter(&adapter);
			dxgiDevice->Release();
			if (!adapter)
				return name;
			DXGI_ADAPTER_DESC desc;
			if (SUCCEEDED(adapter->GetDesc(&desc)))
				name = Narrow(desc.Description);
			adapter->QueryInterface(__uuidof(IDXGIAdapter3), (void **)&sAdapter3);
			adapter->Release();
			return name;
		}

		void OpenFiles()
		{
			CreateDirectoryA("vr_perf", NULL);
			FILE *csv = NULL, *summary = NULL;
			SYSTEMTIME st;
			GetLocalTime(&st);
			char stamp[64], path[128];
			sprintf_s(stamp, "%04u%02u%02u_%02u%02u%02u", st.wYear, st.wMonth, st.wDay,
				st.wHour, st.wMinute, st.wSecond);

			sprintf_s(path, "vr_perf\\%s_frames.csv", stamp);
			if (fopen_s(&csv, path, "w") != 0 || !csv)
				return;
			sprintf_s(path, "vr_perf\\%s_summary.txt", stamp);
			if (fopen_s(&summary, path, "w") != 0 || !summary) {
				fclose(csv);
				return;
			}
			setvbuf(csv, NULL, _IOFBF, 1 << 20);
			for (int c = 0; c < cColCount; c++)
				sCsvBytes += fprintf(csv, c ? ",%s" : "%s", kColNames[c]);
			sCsvBytes += fprintf(csv, "\n");
			setvbuf(summary, NULL, _IOFBF, 1 << 16);
			// Published only once set up; other threads test these pointers.
			sCsv = csv;
			sSummary = summary;
		}

		void WriteHeader(const std::string &adapter)
		{
			if (!sSummary)
				return;
			SYSTEMTIME st;
			GetLocalTime(&st);
			fprintf(sSummary,
				"Metro2033ReduxVR frame profile - started %04u-%02u-%02u %02u:%02u:%02u\n"
				"GPU adapter: %s\n"
				"TSC: %.0f ticks/ms\n\n",
				st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
				adapter.c_str(), sTscPerMs);

			FILE *settings = nullptr;
			if (fopen_s(&settings, "vr_menu_settings.txt", "r") == 0 && settings) {
				fprintf(sSummary, "vr_menu_settings.txt:\n");
				char line[256];
				while (fgets(line, sizeof(line), settings))
					fprintf(sSummary, "  %s", line);
				fclose(settings);
				fprintf(sSummary, "\n");
			} else {
				fprintf(sSummary, "vr_menu_settings.txt: not present (defaults)\n\n");
			}

			fprintf(sSummary,
				"How to read this:\n"
				"  wall            Present-to-Present time (1000/wall = app fps)\n"
				"  present_thread  CPU the thread calling Present executed (QueryThreadCycleTime)\n"
				"  d3d_thread      CPU executed by the thread issuing draws on the immediate context;\n"
				"                  in Metro it is a separate thread from Present\n"
				"  game            Present thread from end of our Present to the next Present\n"
				"  hooks           D3D thread time inside hooked D3D11 calls (mod logic + driver)\n"
				"  present         everything the mod does inside Present, by step\n"
				"  gpu_*           GPU timestamps; span from frame start to Present - includes idle\n"
				"                  bubbles when the CPU does not feed the GPU fast enough\n"
				"  vr_*            SteamVR's own Compositor_FrameTiming (lags about one frame)\n"
				"  passes          one pass = one render-target binding; GPU and CPU ms per pass\n\n");
			fflush(sSummary);
		}

		void LogHmdOnce()
		{
			if (sHmdLogged)
				return;
			vr::IVRSystem *system = vr::VRSystem();
			if (!system)
				return;
			sHmdLogged = true;
			uint32_t w = 0, h = 0;
			system->GetRecommendedRenderTargetSize(&w, &h);
			vr::ETrackedPropertyError err;
			sDisplayHz = system->GetFloatTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd,
				vr::Prop_DisplayFrequency_Float, &err);
			char tracking[128] = {}, model[128] = {};
			system->GetStringTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd,
				vr::Prop_TrackingSystemName_String, tracking, sizeof(tracking), &err);
			system->GetStringTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd,
				vr::Prop_ModelNumber_String, model, sizeof(model), &err);
			if (sSummary)
				fprintf(sSummary, "HMD: %s / %s, %.1f Hz, SteamVR recommended per-eye %ux%u\n\n",
					tracking, model, sDisplayHz, w, h);
		}

		bool EnsureInit(ID3D11Device *device, ID3D11DeviceContext *context)
		{
			if (sFailed)
				return false;
			if (!sInitialised) {
				InitializeCriticalSection(&sThreadLock);
				CalibrateTsc();
				sStartTsc = __rdtsc();
				OpenFiles();
				if (!sCsv || !sSummary) {
					sFailed = true;
					return false;
				}
				sInterval.Reset();
				sLive.Reset();
				sOverlayVisible = StereoSinglePass::FlagFilePresent(L"vr_perf_hud.txt");
				sInitialised = true;
			}
			if (device != sDevice || context != sContext) {
				const bool recreated = sDevice != nullptr;
				ReleaseQueries();
				AcquireSRWLockExclusive(&sPassInfoLock);
				sPassInfo.clear();
				ReleaseSRWLockExclusive(&sPassInfoLock);
				sDevice = device;
				sContext = context;
				const std::string adapter = AdapterName(device);
				sGpuOk = CreateQueries(device);
				if (!sGpuOk)
					ReleaseQueries();
				if (recreated) {
					fprintf(sSummary, "\n*** D3D device recreated at frame %u (video settings change) ***\n\n", sFrame);
				} else {
					WriteHeader(adapter);
					if (!sGpuOk)
						fprintf(sSummary, "GPU timestamp queries unavailable - GPU columns will be empty\n\n");
				}
			}
			if (!sSamplerStarted) {
				sSamplerStarted = true;
				HANDLE t = CreateThread(NULL, 0, SamplerThread, NULL, 0, NULL);
				if (t)
					CloseHandle(t);
			}
			return true;
		}

		// ---------------------------------------------------------------
		void WriteRow(const double *row)
		{
			if (!sCsv || sCsvBytes > kCsvByteLimit)
				return;
			char buf[4096];
			int n = 0;
			for (int c = 0; c < cColCount && n < (int)sizeof(buf) - 32; c++) {
				if (c)
					buf[n++] = ',';
				const double v = row[c];
				if (std::isnan(v))
					continue;
				if (v == std::floor(v) && std::fabs(v) < 1e9)
					n += sprintf_s(buf + n, sizeof(buf) - n, "%.0f", v);
				else
					n += sprintf_s(buf + n, sizeof(buf) - n, "%.3f", v);
			}
			buf[n++] = '\n';
			fwrite(buf, 1, n, sCsv);
			sCsvBytes += n;
		}

		void AccumulateInto(Window &w, const double *row)
		{
			if (w.frames == 0)
				w.firstFrame = (unsigned)row[cFrame];
			w.frames++;
			for (int c = 0; c < cColCount; c++) {
				const double v = row[c];
				if (std::isnan(v))
					continue;
				w.sum[c] += v;
				w.cnt[c]++;
				if (v > w.max[c])
					w.max[c] = v;
			}
			if (!std::isnan(row[cVrReprojFlags])) {
				const unsigned flags = (unsigned)row[cVrReprojFlags];
				w.vrFrames++;
				if (flags & vr::VRCompositor_ReprojectionReason_Cpu) w.reprojCpu++;
				if (flags & vr::VRCompositor_ReprojectionReason_Gpu) w.reprojGpu++;
				if (flags & vr::VRCompositor_ReprojectionMotion) w.reprojMotion++;
				if (flags & vr::VRCompositor_ThrottleMask) w.throttled++;
			}
		}

		void Accumulate(const double *row)
		{
			AccumulateInto(sInterval, row);
			AccumulateInto(sLive, row);
			for (int i = 0; i < kPctColCount; i++)
				if (!std::isnan(row[kPctCols[i]]))
					sPct[i].push_back((float)row[kPctCols[i]]);
		}

		// Finalise one frame: GPU data (if any) is merged into its CPU row.
		void Finalize(Slot &s, const UINT64 *ts, UINT64 freq)
		{
			double *row = s.row;
			const bool gpu = ts && freq && s.tsPresentEnter >= 0;
			auto ms = [&](int a, int b) -> double {
				if (a < 0 || b < 0 || ts[b] < ts[a])
					return 0.0;
				return (double)(ts[b] - ts[a]) * 1000.0 / (double)freq;
			};

			if (gpu) {
				const int last = s.tsAfterSubmit >= 0 ? s.tsAfterSubmit : s.tsPresentEnter;
				row[cGpuFrameMs] = ms(0, last);
				row[cGpuSceneMs] = ms(0, s.tsPresentEnter);
				row[cGpuPresentWorkMs] = s.tsAfterFlush >= 0 ? ms(s.tsPresentEnter, s.tsAfterFlush) : 0.0;
				row[cGpuSubmitMs] = (s.tsAfterFlush >= 0 && s.tsAfterSubmit >= 0)
					? ms(s.tsAfterFlush, s.tsAfterSubmit) : 0.0;

				double maxMs = -1.0;
				unsigned long long maxKey = 0;
				for (int j = 0; j < s.nSegs; j++) {
					const int end = (j + 1 < s.nSegs) ? j + 1 : s.tsPresentEnter;
					const double g = ms(j, end);
					const double c = TscMs(s.segCpu[j + 1] - s.segCpu[j]);
					PassAgg *aggs[2] = { &sInterval.passes[s.segKey[j]], &sLive.passes[s.segKey[j]] };
					for (PassAgg *a : aggs) {
						a->gpuMs += g;
						a->cpuMs += c;
						a->draws += s.segDraws[j];
						a->segs++;
						if (g > a->gpuMax)
							a->gpuMax = g;
					}
					if (g > maxMs) {
						maxMs = g;
						maxKey = s.segKey[j];
					}
				}
				row[cGpuMaxPassMs] = maxMs;
				AcquireSRWLockShared(&sPassInfoLock);
				auto info = sPassInfo.find(maxKey);
				row[cGpuMaxPassId] = maxKey == kKeyFrameStart ? 0.0
					: (info != sPassInfo.end() ? (double)info->second.id : NAN);
				ReleaseSRWLockShared(&sPassInfoLock);
			} else {
				sInterval.gpuDropped++;
				sLive.gpuDropped++;
			}

			WriteRow(row);
			Accumulate(row);
		}

		bool TryCollect(Slot &s)
		{
			D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj;
			if (sContext->GetData(s.disjoint, &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
				return false;
			for (int i = 0; i < s.nTs; i++)
				if (sContext->GetData(s.ts[i], &sTsVals[i], sizeof(UINT64), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
					return false;
			Finalize(s, dj.Disjoint ? nullptr : sTsVals, dj.Frequency);
			return true;
		}

		void CollectReady()
		{
			while (sQCount > 0) {
				Slot &s = sSlots[sQueue[sQHead]];
				if (!TryCollect(s))
					break;
				s.pending = false;
				sQHead = (sQHead + 1) % kSlots;
				sQCount--;
			}
		}

		void OpenSlot(unsigned long long nowTsc)
		{
			const int idx = (sCur + 1) % kSlots;
			Slot &s = sSlots[idx];
			if (s.pending) {
				// The GPU has not caught up in kSlots frames. Keep the CPU
				// row, give up on this frame's GPU data.
				Finalize(s, nullptr, 0);
				s.pending = false;
				sQHead = (sQHead + 1) % kSlots;
				sQCount--;
			}
			sCur = idx;
			s.nTs = 0;
			s.nSegs = 1;
			s.segKey[0] = kKeyFrameStart;
			s.segCpu[0] = nowTsc;
			s.segDraws[0] = 0;
			s.tsPresentEnter = s.tsAfterFlush = s.tsAfterSubmit = -1;
			sContext->Begin(s.disjoint);
			sContext->End(s.ts[s.nTs++]);
			s.open = true;
			s.closed = false;
			s.pending = false;
		}

		// ---------------------------------------------------------------
		std::string PassLabel(unsigned long long key, unsigned *id)
		{
			*id = 0;
			if (key == kKeyFrameStart)
				return "(frame start)";
			AcquireSRWLockShared(&sPassInfoLock);
			auto info = sPassInfo.find(key);
			std::string label = "?";
			if (info != sPassInfo.end()) {
				*id = info->second.id;
				label = info->second.label;
			}
			ReleaseSRWLockShared(&sPassInfoLock);
			return label;
		}

		struct RankedPass { unsigned long long key; PassAgg a; };

		std::vector<RankedPass> RankPasses(const std::unordered_map<unsigned long long, PassAgg> &passes,
			double *total)
		{
			std::vector<RankedPass> out;
			*total = 0.0;
			for (const auto &kv : passes) {
				out.push_back({ kv.first, kv.second });
				*total += kv.second.gpuMs;
			}
			std::sort(out.begin(), out.end(),
				[](const RankedPass &a, const RankedPass &b) { return a.a.gpuMs > b.a.gpuMs; });
			return out;
		}

		// One-line diagnosis shared by the summary and the HUD.
		std::string Verdict(const Window &w, double target)
		{
			const double wall = w.Avg(cWallMs);
			const double vrGpu = w.Avg(cVrTotalGpuMs);
			const double d3d = w.Avg(cD3DThreadBusyMs);
			const double present = w.Avg(cPresentThreadBusyMs);
			std::string v;
			char buf[160];
			if (wall <= target * 1.05)
				v += "at headset rate";
			else {
				sprintf_s(buf, "BELOW rate (%.0f fps of %.0f)", 1000.0 / wall, 1000.0 / target);
				v += buf;
			}
			if (!std::isnan(vrGpu) && vrGpu > target * 0.9) {
				sprintf_s(buf, "; GPU-bound %.1f ms", vrGpu);
				v += buf;
			}
			if (!std::isnan(d3d) && d3d > target * 0.9) {
				sprintf_s(buf, "; D3D thread CPU-bound %.1f ms", d3d);
				v += buf;
			}
			if (!std::isnan(present) && present > target * 0.9) {
				sprintf_s(buf, "; Present thread CPU-bound %.1f ms", present);
				v += buf;
			}
			const double readback = w.Avg(cReadbackMs);
			if (!std::isnan(readback) && readback > 0.5) {
				sprintf_s(buf, "; GPU readback stalls %.1f ms", readback);
				v += buf;
			}
			if (w.vrFrames && w.reprojMotion * 2 > w.vrFrames)
				v += "; motion smoothing active";
			return v;
		}

		double TargetMs()
		{
			return sDisplayHz > 1.0f ? 1000.0 / sDisplayHz : 1000.0 / 72.0;
		}

		void BuildOverlayText()
		{
			LogHmdOnce();
			const Window &w = sLive;
			const double target = TargetMs();
			const double elapsedMs = TscMs(__rdtsc() - w.startTsc);
			char line[200];
			std::string t;

			sprintf_s(line, "VR PERF  %.1f app fps / %.0f Hz   frame %.1f ms   %s\n",
				elapsedMs > 0.0 ? w.frames * 1000.0 / elapsedMs : 0.0, 1000.0 / target,
				w.Avg(cWallMs), w.Avg(cTrueStereo) > 0.5 ? "TRUE STEREO" : "ALTERNATE EYE");
			t += line;
			sprintf_s(line, "GPU  app %.1f ms (budget %.1f)  compositor %.1f ms\n",
				w.Avg(cVrTotalGpuMs), target, w.Avg(cVrCompositorGpuMs));
			t += line;
			sprintf_s(line, "REPROJ  motion %u%%  throttled %u%%  dropped %.0f\n",
				w.vrFrames ? w.reprojMotion * 100 / w.vrFrames : 0,
				w.vrFrames ? w.throttled * 100 / w.vrFrames : 0, w.sum[cVrDropped]);
			t += line;
			sprintf_s(line, "CPU  d3d thread %.1f ms  present thread %.1f ms  wait %.1f ms\n",
				w.Avg(cD3DThreadBusyMs), w.Avg(cPresentThreadBusyMs), w.Avg(cPresWaitGetPosesMs));
			t += line;
			sprintf_s(line, "MOD  hooks %.1f ms (%.0f calls)  submit %.2f ms\n",
				w.Avg(cHooksTotalMs), w.Avg(cDrawCalls) + w.Avg(cRenderTargetCalls) + w.Avg(cConstantsCalls) +
				w.Avg(cShadersCalls) + w.Avg(cClearCopyCalls) + w.Avg(cStateCalls), w.Avg(cPresSubmitMs));
			t += line;
			sprintf_s(line, "READBACK  %.2f ms  %.1f calls/frame (GPU->CPU stalls)\n",
				w.Avg(cReadbackMs), w.Avg(cReadbackCalls));
			t += line;
			sprintf_s(line, "DRAWS  %.0f  doubled %.0f  shared %.0f  passes %.0f\n",
				w.Avg(cDrawCalls), w.Avg(cDoubledDraws), w.Avg(cSharedDraws), w.Avg(cPasses));
			t += line;

			const unsigned gpuFrames = w.frames > w.gpuDropped ? w.frames - w.gpuDropped : 0;
			if (gpuFrames) {
				double total = 0.0;
				std::vector<RankedPass> passes = RankPasses(w.passes, &total);
				t += "TOP GPU PASSES\n";
				for (size_t i = 0; i < passes.size() && i < 4; i++) {
					unsigned id;
					const std::string label = PassLabel(passes[i].key, &id);
					sprintf_s(line, " %5.2f ms  %s\n", passes[i].a.gpuMs / gpuFrames, label.c_str());
					t += line;
				}
			}
			t += Verdict(w, target);
			t += "\n";
			sOverlayText.swap(t);
		}

		void WriteSummary()
		{
			Window &w = sInterval;
			if (!sSummary || w.frames == 0)
				return;
			LogHmdOnce();

			const double elapsedMs = TscMs(__rdtsc() - w.startTsc);
			const double tSec = TscMs(__rdtsc() - sStartTsc) / 1000.0;
			const double target = TargetMs();
			const double stereoPct = w.cnt[cTrueStereo] ? 100.0 * w.sum[cTrueStereo] / w.cnt[cTrueStereo] : 0.0;

			fprintf(sSummary,
				"==================================================================================\n"
				"t=%.1fs  frames %u-%u  (%u frames, %.1f app fps)  true-stereo %.0f%%  budget %.2f ms (%.0f Hz)\n",
				tSec, w.firstFrame, w.firstFrame + w.frames - 1, w.frames,
				elapsedMs > 0.0 ? w.frames * 1000.0 / elapsedMs : 0.0, stereoPct, target, 1000.0 / target);

			fprintf(sSummary, "\n  %-26s %8s %8s %8s %8s %8s\n", "per frame (ms)", "avg", "p50", "p95", "p99", "max");
			for (int i = 0; i < kPctColCount; i++) {
				std::vector<float> &v = sPct[i];
				const int col = kPctCols[i];
				if (v.empty()) {
					fprintf(sSummary, "  %-26s %8s\n", kColNames[col], "-");
					continue;
				}
				auto pct = [&](double p) -> float {
					size_t k = (size_t)(p * (v.size() - 1) + 0.5);
					std::nth_element(v.begin(), v.begin() + k, v.end());
					return v[k];
				};
				const float p50 = pct(0.50), p95 = pct(0.95), p99 = pct(0.99);
				fprintf(sSummary, "  %-26s %8.2f %8.2f %8.2f %8.2f %8.2f\n", kColNames[col],
					w.Avg(col), p50, p95, p99, w.max[col]);
				v.clear();
			}

			fprintf(sSummary, "\n  VERDICT: %s\n", Verdict(w, target).c_str());

			fprintf(sSummary, "\n  D3D SUBMISSION THREAD (tid %lu, avg ms/frame)\n", sD3DThreadId.load());
			fprintf(sSummary, "    busy                                 %7.2f\n", w.Avg(cD3DThreadBusyMs));
			fprintf(sSummary, "    inside hooked D3D11 calls            %7.2f\n", w.Avg(cHooksTotalMs));
			static const struct { const char *name; int ms, calls; } hooks[] = {
				{ "draw/dispatch (+2nd eye)", cHookDrawMs, cDrawCalls },
				{ "OMSetRenderTargets (+replay)", cHookRenderTargetMs, cRenderTargetCalls },
				{ "constant buffers / Map", cHookConstantsMs, cConstantsCalls },
				{ "shaders / SRVs", cHookShadersMs, cShadersCalls },
				{ "clear / copy", cHookClearCopyMs, cClearCopyCalls },
				{ "state / IA / samplers", cHookStateMs, cStateCalls },
			};
			for (const auto &h : hooks) {
				const double m = w.Avg(h.ms), c = w.Avg(h.calls);
				fprintf(sSummary, "      %-32s %7.2f  %7.0f calls  %6.2f us/call\n", h.name, m, c,
					c > 0.0 ? m * 1000.0 / c : 0.0);
			}
			fprintf(sSummary, "    (other threads in hooks: %.2f ms, %.0f calls)\n",
				w.Avg(cOtherThreadHookMs), w.Avg(cOtherThreadHookCalls));

			fprintf(sSummary, "\n  SYNCHRONOUS READBACKS (staging copy + Map READ; each drains the GPU queue)\n");
			fprintf(sSummary, "    total %.3f ms/frame, %.2f calls/frame, max %.2f ms in one frame\n",
				w.Avg(cReadbackMs), w.Avg(cReadbackCalls), w.max[cReadbackMs]);
			{
				const unsigned long long shadowReads = sShadowReads.load(std::memory_order_relaxed);
				fprintf(sSummary, "    served from CPU shadows instead (no stall): %.2f reads/frame\n",
					w.frames ? (double)(shadowReads - sShadowReadsReported) / w.frames : 0.0);
				sShadowReadsReported = shadowReads;
			}
			for (int i = 0; i < kReadbackSites; i++) {
				ReadbackSite &site = sReadbackSites[i];
				const int lineNo = site.line.load(std::memory_order_relaxed);
				if (!lineNo)
					break;
				const unsigned long long ticks = site.ticks.load(std::memory_order_relaxed);
				const unsigned calls = site.calls.load(std::memory_order_relaxed);
				const unsigned long long dTicks = ticks - site.reportedTicks;
				const unsigned dCalls = calls - site.reportedCalls;
				site.reportedTicks = ticks;
				site.reportedCalls = calls;
				if (dCalls && w.frames)
					fprintf(sSummary, "      HackerContext.cpp:%-6d %8.3f ms/frame  %7.2f calls/frame  %7.3f ms/call\n",
						lineNo, TscMs(dTicks) / w.frames, (double)dCalls / w.frames, TscMs(dTicks) / dCalls);
			}

			fprintf(sSummary, "\n  PRESENT THREAD (tid %lu, avg ms/frame)\n", gRenderThreadId);
			fprintf(sSummary, "    busy / wall                          %7.2f / %.2f\n",
				w.Avg(cPresentThreadBusyMs), w.Avg(cWallMs));
			fprintf(sSummary, "    game (Present exit -> next Present)  %7.2f\n", w.Avg(cGameMs));
			fprintf(sSummary, "      first D3D-thread call after exit   %7.2f  (frame-start wait)\n",
				w.Avg(cD3DFirstCallMs));
			fprintf(sSummary, "    present (mod work inside Present)    %7.2f\n", w.Avg(cPresentTotalMs));
			static const struct { const char *name; int col; } steps[] = {
				{ "RunFrameActions (3Dmigoto)", cPresFrameActionsMs },
				{ "VR menu draw", cPresVRMenuMs },
				{ "flush 2nd eye", cPresFlushSecondEyeMs },
				{ "SubmitFrameToCompositor", cPresSubmitMs },
				{ "DXGI Present (desktop window)", cPresDxgiPresentMs },
				{ "WaitGetPoses", cPresWaitGetPosesMs },
				{ "stereo params", cPostStereoParamsMs },
				{ "UpdateVRPose", cPostUpdatePoseMs },
				{ "culling FOV/follow", cPostCullingMs },
				{ "controllers/aim", cPostInputAimMs },
				{ "post-present cmd list", cPostTailMs },
			};
			for (const auto &st : steps)
				fprintf(sSummary, "      %-32s %7.2f  (max %.2f)\n", st.name, w.Avg(st.col), w.max[st.col]);

			fprintf(sSummary, "\n  GPU (timestamps, avg ms/frame; %u frames without GPU data)\n", w.gpuDropped);
			fprintf(sSummary, "    frame span %.2f | scene %.2f | present-time work %.2f | compositor copy %.2f\n",
				w.Avg(cGpuFrameMs), w.Avg(cGpuSceneMs), w.Avg(cGpuPresentWorkMs), w.Avg(cGpuSubmitMs));

			fprintf(sSummary, "\n  STEAMVR (%u frames)\n", w.vrFrames);
			fprintf(sSummary, "    app GPU total %.2f (pre-submit %.2f, post-submit %.2f) | compositor GPU %.2f, CPU %.2f, idle %.2f\n",
				w.Avg(cVrTotalGpuMs), w.Avg(cVrPreSubmitGpuMs), w.Avg(cVrPostSubmitGpuMs), w.Avg(cVrCompositorGpuMs),
				w.Avg(cVrCompositorCpuMs), w.Avg(cVrCompositorIdleMs));
			fprintf(sSummary, "    client interval %.2f | Submit %.2f | reprojection: cpu-reason %u, gpu-reason %u, "
				"motion %u, throttled %u | dropped %.0f, mispresented %.0f\n",
				w.Avg(cVrClientIntervalMs), w.Avg(cVrSubmitMs), w.reprojCpu, w.reprojGpu, w.reprojMotion, w.throttled,
				w.sum[cVrDropped], w.sum[cVrMispresented]);

			fprintf(sSummary, "\n  WORKLOAD (avg/frame)  draws %.0f | doubled %.0f | shared %.0f | folded %.0f | "
				"eye-CB swaps %.0f | passes %.0f | RT binds %.0f\n",
				w.Avg(cDrawCalls), w.Avg(cDoubledDraws), w.Avg(cSharedDraws), w.Avg(cFoldedDraws),
				w.Avg(cEyeCBSwaps), w.Avg(cPasses), w.Avg(cRTBinds));
			if (sMarkOverflow)
				fprintf(sSummary, "  (pass list truncated at %d passes on %u frames)\n", kMaxMarks, sMarkOverflow);

			const unsigned gpuFrames = w.frames > w.gpuDropped ? w.frames - w.gpuDropped : 0;
			if (gpuFrames && !w.passes.empty()) {
				double total = 0.0;
				std::vector<RankedPass> passes = RankPasses(w.passes, &total);
				fprintf(sSummary, "\n  PASSES by GPU ms/frame (%zu distinct, total %.2f ms)\n", passes.size(), total / gpuFrames);
				fprintf(sSummary, "    %4s  %8s %8s %8s %7s %7s  %s\n", "id", "gpu ms", "gpu max", "cpu ms", "x/frame", "draws", "target");
				for (size_t i = 0; i < passes.size() && i < 30; i++) {
					const RankedPass &p = passes[i];
					unsigned id;
					const std::string label = PassLabel(p.key, &id);
					fprintf(sSummary, "    %4u  %8.3f %8.3f %8.3f %7.1f %7.1f  %s\n", id,
						p.a.gpuMs / gpuFrames, p.a.gpuMax, p.a.cpuMs / gpuFrames,
						(double)p.a.segs / gpuFrames, (double)p.a.draws / gpuFrames, label.c_str());
				}
			}

			if (sAdapter3) {
				DXGI_QUERY_VIDEO_MEMORY_INFO info;
				if (SUCCEEDED(sAdapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)))
					fprintf(sSummary, "\n  VRAM %.0f MB used of %.0f MB budget%s\n",
						info.CurrentUsage / 1048576.0, info.Budget / 1048576.0,
						info.CurrentUsage > info.Budget ? "  *** OVER BUDGET - driver is paging ***" : "");
			}

			EnterCriticalSection(&sThreadLock);
			const std::string threads = sThreadReport;
			LeaveCriticalSection(&sThreadLock);
			if (!threads.empty())
				fprintf(sSummary, "\n  %s", threads.c_str());
			fprintf(sSummary, "\n");

			fflush(sSummary);
			if (sCsv)
				fflush(sCsv);
			w.Reset();
			sMarkOverflow = 0;
		}
	}

	// -------------------------------------------------------------------
	static const unsigned kMaxSites = 160;
	static const char *sSiteNames[kMaxSites] = {};
	static unsigned sSiteCount = 0;
	static SRWLOCK sSiteLock = SRWLOCK_INIT;
	// D3D thread only, so plain counters are enough.
	static unsigned long long sSiteTicks[kMaxSites] = {};
	static unsigned long long sSiteCalls[kMaxSites] = {};

	unsigned RegisterSite(const char *name)
	{
		AcquireSRWLockExclusive(&sSiteLock);
		unsigned index = kMaxSites - 1;   // overflow bucket
		for (unsigned i = 0; i < sSiteCount; i++)
			if (sSiteNames[i] == name || strcmp(sSiteNames[i], name) == 0)
				index = i;
		if (index == kMaxSites - 1 && sSiteCount < kMaxSites - 1) {
			index = sSiteCount++;
			sSiteNames[index] = name;
		}
		if (!sSiteNames[kMaxSites - 1])
			sSiteNames[kMaxSites - 1] = "(other)";
		ReleaseSRWLockExclusive(&sSiteLock);
		return index;
	}

	bool Active()
	{
		static volatile LONG active = -1;   // -1 undecided, 0 off, 1 on
		LONG a = active;
		if (a < 0) {
			InterlockedCompareExchange(&active,
				(kEnabled && StereoSinglePass::FlagFilePresent(L"vr_perf.txt")) ? 1 : 0, -1);
			a = active;
		}
		return a > 0;
	}

	const char *ReportSites()
	{
		if (!sInitialised)
			return NULL;
		static unsigned frames = 0;
		static unsigned long long accTicks[kMaxSites] = {};
		static unsigned long long accCalls[kMaxSites] = {};
		for (unsigned i = 0; i < kMaxSites; i++) {
			accTicks[i] += sSiteTicks[i];
			accCalls[i] += sSiteCalls[i];
			sSiteTicks[i] = 0;
			sSiteCalls[i] = 0;
		}
		if (++frames < 600 || sTscPerMs <= 0.0)
			return NULL;

		unsigned order[kMaxSites];
		for (unsigned i = 0; i < kMaxSites; i++)
			order[i] = i;
		std::sort(order, order + kMaxSites, [](unsigned a, unsigned b) {
			return accTicks[a] > accTicks[b];
		});
		static std::string line;
		line = "hook sites on D3D thread (ms/frame, calls/frame, us/call):";
		double totalMs = 0.0;
		for (unsigned i = 0; i < kMaxSites; i++)
			totalMs += (double)accTicks[i] / sTscPerMs / frames;
		char part[160];
		sprintf_s(part, " total %.2f ms |", totalMs);
		line += part;
		for (unsigned n = 0; n < 16; n++) {
			const unsigned i = order[n];
			if (!accCalls[i])
				break;
			const double ms = (double)accTicks[i] / sTscPerMs / frames;
			const double calls = (double)accCalls[i] / frames;
			sprintf_s(part, " %s %.2f/%.0f/%.2f |", sSiteNames[i] ? sSiteNames[i] : "?",
				ms, calls, calls > 0.0 ? ms * 1000.0 / calls : 0.0);
			line += part;
		}
		for (unsigned i = 0; i < kMaxSites; i++) {
			accTicks[i] = 0;
			accCalls[i] = 0;
		}
		frames = 0;
		return line.c_str();
	}

	void HookEnd(unsigned hook, unsigned site, unsigned long long ticks)
	{
		// Cached per thread: this runs about 11000 times a frame, and the
		// thread cannot change underneath itself.
		static thread_local unsigned long tCachedTid = 0;
		if (!tCachedTid)
			tCachedTid = GetCurrentThreadId();
		const unsigned long tid = tCachedTid;
		if (tid == sD3DThreadId.load(std::memory_order_relaxed)) {
			if (!sD3DFirstCallTsc.load(std::memory_order_relaxed)) {
				unsigned long long expected = 0;
				sD3DFirstCallTsc.compare_exchange_strong(expected, __rdtsc() - ticks,
					std::memory_order_relaxed);
			}
			sD3DTicks[hook].fetch_add(ticks, std::memory_order_relaxed);
			sD3DCalls[hook].fetch_add(1, std::memory_order_relaxed);
			if (site < kMaxSites) {
				sSiteTicks[site] += ticks;
				sSiteCalls[site]++;
			}
			if (hook == kHookDraw)
				sPassDraws.fetch_add(1, std::memory_order_relaxed);
		} else if (tid == gRenderThreadId) {
			if (!sInsidePresent) {
				sPresentTicks[hook] += ticks;
				sPresentCalls[hook]++;
			}
		} else {
			sOtherTicks[hook].fetch_add(ticks, std::memory_order_relaxed);
			sOtherCalls[hook].fetch_add(1, std::memory_order_relaxed);
		}
	}

	bool OverlayVisible()
	{
		return sInitialised && sOverlayVisible;
	}

	void ToggleOverlay()
	{
		sOverlayVisible = !sOverlayVisible;
	}

	void NoteShadowRead()
	{
		if (sInitialised)
			sShadowReads.fetch_add(1, std::memory_order_relaxed);
	}

	void NoteReadback(int site, unsigned long long ticks)
	{
		if (!sInitialised)
			return;
		sReadbackTicks.fetch_add(ticks, std::memory_order_relaxed);
		sReadbackCalls.fetch_add(1, std::memory_order_relaxed);
		for (int i = 0; i < kReadbackSites; i++) {
			ReadbackSite &s = sReadbackSites[i];
			int line = s.line.load(std::memory_order_relaxed);
			if (line == 0) {
				int expected = 0;
				if (s.line.compare_exchange_strong(expected, site))
					line = site;
				else
					line = expected;
			}
			if (line == site) {
				s.ticks.fetch_add(ticks, std::memory_order_relaxed);
				s.calls.fetch_add(1, std::memory_order_relaxed);
				return;
			}
		}
	}

	void Note(const char *text)
	{
		if (sInitialised && sSummary && text)
			fprintf(sSummary, "NOTE frame %u: %s\n", sFrame, text);
	}

	const char *OverlayText()
	{
		return sOverlayText.empty() ? "VR PERF  collecting...\n" : sOverlayText.c_str();
	}

	// vr_pin_hot_threads.txt: keep Metro's D3D submission thread and Present
	// thread on the logical processors of one L3 cache (one Zen 2 CCX), so the
	// two threads that hand each frame to each other never do it across CCX or
	// CCD boundaries. Re-checked every 120 frames; removing the file gives both
	// threads the process affinity back.
	static KAFFINITY FirstL3CacheMask()
	{
		DWORD bytes = 0;
		GetLogicalProcessorInformationEx(RelationCache, NULL, &bytes);
		if (!bytes)
			return 0;
		std::vector<unsigned char> buffer(bytes);
		auto *info = (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *)buffer.data();
		if (!GetLogicalProcessorInformationEx(RelationCache, info, &bytes))
			return 0;
		for (DWORD offset = 0; offset < bytes;) {
			auto *entry = (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *)(buffer.data() + offset);
			if (entry->Relationship == RelationCache && entry->Cache.Level == 3
				&& entry->Cache.GroupMask.Group == 0 && (entry->Cache.GroupMask.Mask & 1))
				return entry->Cache.GroupMask.Mask;
			if (!entry->Size)
				break;
			offset += entry->Size;
		}
		return 0;
	}

	static void UpdateHotThreadPinning()
	{
		static bool pinned = false;
		static unsigned long pinnedD3D = 0, pinnedPresent = 0;
		const bool wanted = StereoSinglePass::FlagFilePresent(L"vr_pin_hot_threads.txt");
		const unsigned long d3d = sD3DThreadId.load(std::memory_order_relaxed);
		const unsigned long present = gRenderThreadId;
		if (wanted == pinned && (!pinned || (d3d == pinnedD3D && present == pinnedPresent)))
			return;

		DWORD_PTR processMask = 0, systemMask = 0;
		GetProcessAffinityMask(GetCurrentProcess(), &processMask, &systemMask);
		KAFFINITY mask = wanted ? FirstL3CacheMask() & processMask : processMask;
		if (!mask)
			return;
		auto setMask = [](unsigned long tid, KAFFINITY m) {
			if (!tid)
				return false;
			HANDLE thread = OpenThread(THREAD_SET_LIMITED_INFORMATION | THREAD_QUERY_LIMITED_INFORMATION,
				FALSE, tid);
			if (!thread)
				return false;
			// The id of a thread that has exited may belong to another process now.
			const bool ok = GetProcessIdOfThread(thread) == GetCurrentProcessId()
				&& SetThreadAffinityMask(thread, (DWORD_PTR)m) != 0;
			CloseHandle(thread);
			return ok;
		};
		auto apply = [&setMask, mask](unsigned long tid) { return setMask(tid, mask); };
		if (pinned && !wanted) {
			apply(pinnedD3D);
			if (pinnedPresent != pinnedD3D)
				apply(pinnedPresent);
			pinned = false;
			Note("hot-thread pinning OFF: D3D and Present threads back on the process affinity");
			return;
		}
		// A previously pinned thread that is no longer one of the two (Metro can
		// move rendering to another thread) gets the process mask back first.
		if (pinned) {
			if (pinnedD3D != d3d && pinnedD3D != present)
				setMask(pinnedD3D, processMask);
			if (pinnedPresent != pinnedD3D && pinnedPresent != d3d && pinnedPresent != present)
				setMask(pinnedPresent, processMask);
		}
		const bool okD3D = apply(d3d);
		const bool okPresent = present == d3d ? okD3D : apply(present);
		pinned = wanted;
		pinnedD3D = d3d;
		pinnedPresent = present;
		char note[200];
		sprintf_s(note, "hot-thread pinning ON (vr_pin_hot_threads.txt): mask 0x%llx, D3D tid %lu %s, "
			"Present tid %lu %s", (unsigned long long)mask, d3d, okD3D ? "ok" : "FAILED",
			present, okPresent ? "ok" : "FAILED");
		Note(note);
	}

	void BeginPresent(ID3D11Device *device, ID3D11DeviceContext *context)
	{
		VRPose::TraceStep("present-enter");
		if (!kEnabled || !device || !context || !Active())
			return;
		gRenderThreadId = GetCurrentThreadId();
		if (!EnsureInit(device, context))
			return;
		{
			static unsigned presents = 0;
			if ((presents % 120) == 60)
				UpdateHotThreadPinning();
			if ((presents++ % 120) == 0) {
				const bool timing = !StereoSinglePass::FlagFilePresent(L"vr_perf_lite.txt");
				if (timing != gHookTiming) {
					gHookTiming = timing;
					if (presents > 1)
						Note(timing ? "perf-lite OFF: per-hook timing resumed"
						: "perf-lite ON (vr_perf_lite.txt): per-hook timing, hook sites and draw sections paused");
				}
			}
		}

		const unsigned long long now = __rdtsc();
		for (int i = 0; i < kPointCount; i++)
			sStamps[i] = 0;
		sStamps[kPresentEnter] = now;
		sInsidePresent = true;

		// Hooked-call time of this frame: the D3D thread's, plus the Present
		// thread's own calls made outside Present when it is a separate thread.
		for (int h = 0; h < kHookCount; h++) {
			sGameHookTicks[h] = sD3DTicks[h].exchange(0, std::memory_order_relaxed) + sPresentTicks[h];
			sGameHookCalls[h] = sD3DCalls[h].exchange(0, std::memory_order_relaxed) + sPresentCalls[h];
			sPresentTicks[h] = 0;
			sPresentCalls[h] = 0;
		}

		if (sGpuOk && sCur >= 0) {
			Slot &s = sSlots[sCur];
			if (s.open) {
				s.segDraws[s.nSegs - 1] = sPassDraws.exchange(0, std::memory_order_relaxed);
				s.segCpu[s.nSegs] = now;
				s.tsPresentEnter = s.nTs;
				sContext->End(s.ts[s.nTs++]);
				s.open = false;
			}
		}
	}

	void Stamp(Point point)
	{
		static const char *kPointNames[kPointCount] = {
			"present-enter", "after-frame-actions", "after-vr-menu-and-hud",
			"after-flush-second-eye", "after-submit", "after-dxgi-present",
			"after-wait-get-poses", "after-stereo-params", "after-update-pose",
			"after-culling", "after-input-and-aim",
		};
		VRPose::TraceStep(kPointNames[point]);
		if (!kEnabled || !sInitialised || sFailed || GetCurrentThreadId() != gRenderThreadId)
			return;
		sStamps[point] = __rdtsc();
		if (!sGpuOk || sCur < 0)
			return;
		Slot &s = sSlots[sCur];
		if (s.closed || s.tsPresentEnter < 0)
			return;
		if (point == kAfterFlushSecondEye) {
			s.tsAfterFlush = s.nTs;
			sContext->End(s.ts[s.nTs++]);
		} else if (point == kAfterSubmit) {
			s.tsAfterSubmit = s.nTs;
			sContext->End(s.ts[s.nTs++]);
			sContext->End(s.disjoint);
			s.closed = true;
		}
	}

	void EndPresent()
	{
		VRPose::TraceStep("present-exit");
		VRPose::TraceFrameEnd();
		if (!kEnabled || !sInitialised || sFailed || GetCurrentThreadId() != gRenderThreadId)
			return;
		const unsigned long long exit = __rdtsc();
		sInsidePresent = false;
		ULONG64 cycles = 0, d3dCycles = 0;
		QueryThreadCycleTime(GetCurrentThread(), &cycles);
		if (HANDLE adopted = sPendingD3DThreadHandle.exchange(NULL)) {
			if (sD3DThreadHandle)
				CloseHandle(sD3DThreadHandle);
			sD3DThreadHandle = adopted;
			sPrevD3DCycles = 0;
		}
		if (sD3DThreadHandle)
			QueryThreadCycleTime(sD3DThreadHandle, &d3dCycles);

		if (sStamps[kPresentEnter] == 0) {
			sPrevExit = exit;
			sPrevCycles = cycles;
			sPrevD3DCycles = d3dCycles;
			return;
		}
		for (int i = 1; i < kPointCount; i++)
			if (sStamps[i] == 0)
				sStamps[i] = sStamps[i - 1];

		const bool haveFrame = sPrevExit != 0;
		double row[cColCount];
		for (int c = 0; c < cColCount; c++)
			row[c] = NAN;

		if (haveFrame) {
			row[cFrame] = sFrame;
			row[cTimeS] = TscMs(exit - sStartTsc) / 1000.0;
			row[cTrueStereo] = StereoTwin::gDoubleDraw ? 1.0 : 0.0;
			row[cWallMs] = TscMs(exit - sPrevExit);
			row[cPresentThreadBusyMs] = TscMs(cycles - sPrevCycles);
			if (sD3DThreadHandle && sPrevD3DCycles && d3dCycles >= sPrevD3DCycles)
				row[cD3DThreadBusyMs] = TscMs(d3dCycles - sPrevD3DCycles);
			row[cGameMs] = TscMs(sStamps[kPresentEnter] - sPrevExit);
			const unsigned long long firstCall =
				sD3DFirstCallTsc.exchange(0, std::memory_order_relaxed);
			if (firstCall > sPrevExit)
				row[cD3DFirstCallMs] = TscMs(firstCall - sPrevExit);

			double hooks = 0.0;
			static const int msCols[kHookCount] = { cHookDrawMs, cHookRenderTargetMs, cHookConstantsMs,
				cHookShadersMs, cHookClearCopyMs, cHookStateMs };
			static const int callCols[kHookCount] = { cDrawCalls, cRenderTargetCalls, cConstantsCalls,
				cShadersCalls, cClearCopyCalls, cStateCalls };
			double otherMs = 0.0;
			unsigned otherCalls = 0;
			for (int h = 0; h < kHookCount; h++) {
				row[msCols[h]] = TscMs(sGameHookTicks[h]);
				row[callCols[h]] = sGameHookCalls[h];
				hooks += row[msCols[h]];
				otherMs += TscMs(sOtherTicks[h].exchange(0, std::memory_order_relaxed));
				otherCalls += sOtherCalls[h].exchange(0, std::memory_order_relaxed);
			}
			row[cHooksTotalMs] = hooks;
			row[cOtherThreadHookMs] = otherMs;
			row[cOtherThreadHookCalls] = otherCalls;
			row[cReadbackMs] = TscMs(sReadbackTicks.exchange(0, std::memory_order_relaxed));
			row[cReadbackCalls] = sReadbackCalls.exchange(0, std::memory_order_relaxed);

			auto delta = [](unsigned cur, unsigned &prev) -> double {
				const unsigned d = cur >= prev ? cur - prev : cur;
				prev = cur;
				return (double)d;
			};
			row[cDoubledDraws] = delta(StereoTwin::gDoubledDraws, sPrevDoubled);
			row[cSharedDraws] = delta(StereoTwin::gSharedDraws, sPrevShared);
			row[cEyeCBSwaps] = delta(StereoTwin::gEyeCBSwaps, sPrevEyeCB);
			row[cFoldedDraws] = delta(StereoSinglePass::gDraws, sPrevFolded);
			row[cRTBinds] = sBinds.exchange(0, std::memory_order_relaxed);

			const unsigned long long *s = sStamps;
			row[cPresFrameActionsMs] = TscMs(s[kAfterFrameActions] - s[kPresentEnter]);
			row[cPresVRMenuMs] = TscMs(s[kAfterVRMenu] - s[kAfterFrameActions]);
			row[cPresFlushSecondEyeMs] = TscMs(s[kAfterFlushSecondEye] - s[kAfterVRMenu]);
			row[cPresSubmitMs] = TscMs(s[kAfterSubmit] - s[kAfterFlushSecondEye]);
			row[cPresDxgiPresentMs] = TscMs(s[kAfterDxgiPresent] - s[kAfterSubmit]);
			row[cPresWaitGetPosesMs] = TscMs(s[kAfterWaitGetPoses] - s[kAfterDxgiPresent]);
			row[cPostStereoParamsMs] = TscMs(s[kAfterStereoParams] - s[kAfterWaitGetPoses]);
			row[cPostUpdatePoseMs] = TscMs(s[kAfterUpdatePose] - s[kAfterStereoParams]);
			row[cPostCullingMs] = TscMs(s[kAfterCulling] - s[kAfterUpdatePose]);
			row[cPostInputAimMs] = TscMs(s[kAfterInputAndAim] - s[kAfterCulling]);
			row[cPostTailMs] = TscMs(exit - s[kAfterInputAndAim]);
			row[cPresentTotalMs] = TscMs(exit - s[kPresentEnter]);

			vr::IVRCompositor *compositor = vr::VRSystem() ? vr::VRCompositor() : nullptr;
			if (compositor) {
				vr::Compositor_FrameTiming t = {};
				t.m_nSize = sizeof(t);
				if (compositor->GetFrameTiming(&t, 1)) {
					row[cVrTotalGpuMs] = t.m_flTotalRenderGpuMs;
					row[cVrPreSubmitGpuMs] = t.m_flPreSubmitGpuMs;
					row[cVrPostSubmitGpuMs] = t.m_flPostSubmitGpuMs;
					row[cVrCompositorGpuMs] = t.m_flCompositorRenderGpuMs;
					row[cVrCompositorCpuMs] = t.m_flCompositorRenderCpuMs;
					row[cVrCompositorIdleMs] = t.m_flCompositorIdleCpuMs;
					row[cVrClientIntervalMs] = t.m_flClientFrameIntervalMs;
					row[cVrSubmitMs] = t.m_flSubmitFrameMs;
					row[cVrReprojFlags] = t.m_nReprojectionFlags;
					row[cVrPresents] = t.m_nNumFramePresents;
					row[cVrMispresented] = t.m_nNumMisPresented;
					row[cVrDropped] = t.m_nNumDroppedFrames;
				}
			}
			sFrame++;
		}
		sPrevExit = exit;
		sPrevCycles = cycles;
		sPrevD3DCycles = d3dCycles;

		if (!sGpuOk) {
			if (haveFrame) {
				WriteRow(row);
				Accumulate(row);
			}
		} else {
			if (sCur >= 0) {
				Slot &s = sSlots[sCur];
				if (!s.closed) {
					if (s.tsPresentEnter < 0) {
						s.tsPresentEnter = s.nTs;
						sContext->End(s.ts[s.nTs++]);
					}
					sContext->End(s.disjoint);
					s.closed = true;
				}
				s.open = false;
				if (haveFrame) {
					memcpy(s.row, row, sizeof(row));
					s.row[cPasses] = s.nSegs - 1;
					if (s.nSegs > kMaxMarks)
						sMarkOverflow++;
					s.pending = true;
					sQueue[(sQHead + sQCount) % kSlots] = sCur;
					sQCount++;
				}
			}
			OpenSlot(exit);
			CollectReady();
		}

		if (TscMs(__rdtsc() - sLive.startTsc) >= kLiveSeconds * 1000.0) {
			BuildOverlayText();
			sLive.Reset();
		}
		if (TscMs(__rdtsc() - sInterval.startTsc) >= kSummarySeconds * 1000.0)
			WriteSummary();
	}

	void MarkPass(ID3D11DeviceContext *context, unsigned numViews,
		unsigned width, unsigned height, unsigned format,
		ID3D11RenderTargetView *rtv0, ID3D11DepthStencilView *depth)
	{
		if (!kEnabled || context != sContext || !sContext)
			return;

		// Whoever binds render targets on the immediate context is the
		// thread doing Metro's rendering.
		const unsigned long tid = GetCurrentThreadId();
		if (tid != sD3DThreadId.load(std::memory_order_relaxed)) {
			sD3DThreadId.store(tid, std::memory_order_relaxed);
			if (HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid)) {
				if (HANDLE unused = sPendingD3DThreadHandle.exchange(h))
					CloseHandle(unused);
			}
		}

		if (!sGpuOk || sCur < 0)
			return;
		Slot &s = sSlots[sCur];
		if (!s.open)
			return;

		unsigned long long key = ((unsigned long long)(uintptr_t)rtv0 * 0x9E3779B97F4A7C15ULL) ^
			(((unsigned long long)(uintptr_t)depth + numViews) * 0xC2B2AE3D27D4EB4FULL);
		if (key <= kKeyFrameStart)
			key += 2;

		sBinds.fetch_add(1, std::memory_order_relaxed);
		if (key == s.segKey[s.nSegs - 1])
			return;
		if (s.nSegs > kMaxMarks)
			return;

		AcquireSRWLockShared(&sPassInfoLock);
		const bool known = sPassInfo.find(key) != sPassInfo.end();
		ReleaseSRWLockShared(&sPassInfoLock);
		if (!known) {
			char label[160];
			if (numViews > 0 && rtv0 && width) {
				sprintf_s(label, "%ux%u %s%s%s", width, height, FormatName(format),
					numViews > 1 ? (numViews == 2 ? " +1 MRT" : numViews == 3 ? " +2 MRT" : " +3 MRT") : "",
					depth ? " +depth" : "");
			} else if (depth) {
				D3D11_DEPTH_STENCIL_VIEW_DESC dd = {};
				depth->GetDesc(&dd);
				unsigned w = 0, h = 0;
				ID3D11Resource *res = nullptr;
				depth->GetResource(&res);
				ID3D11Texture2D *tex = nullptr;
				if (res && SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), (void **)&tex)) && tex) {
					D3D11_TEXTURE2D_DESC td;
					tex->GetDesc(&td);
					w = td.Width;
					h = td.Height;
					tex->Release();
				}
				if (res)
					res->Release();
				sprintf_s(label, "depth-only %ux%u %s", w, h, FormatName(dd.Format));
			} else {
				sprintf_s(label, "no target bound");
			}
			PassInfo info;
			info.label = label;
			AcquireSRWLockExclusive(&sPassInfoLock);
			info.id = (unsigned)sPassInfo.size() + 1;
			sPassInfo.emplace(key, info);
			ReleaseSRWLockExclusive(&sPassInfoLock);
		}

		s.segDraws[s.nSegs - 1] = sPassDraws.exchange(0, std::memory_order_relaxed);
		s.segKey[s.nSegs] = key;
		s.segCpu[s.nSegs] = __rdtsc();
		sContext->End(s.ts[s.nTs++]);
		s.nSegs++;
	}
}
