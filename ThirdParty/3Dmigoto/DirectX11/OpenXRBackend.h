#pragma once

#include <windows.h>
#include <d3d11.h>
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include "../../openxr/include/openxr/openxr.h"
#include "../../openxr/include/openxr/openxr_platform.h"
#include <vector>

namespace VRBackend {

	// OpenXR runtime bootstrap.  This is deliberately not called by the
	// existing OpenVR path yet; it can be enabled after pose and graphics
	// interop are implemented and tested independently.
	class OpenXRRuntime {
	public:
		OpenXRRuntime();
		~OpenXRRuntime();

		bool Load();
		bool CreateInstance();
		bool SelectHmdSystem();
		bool CreateD3D11Session(struct ID3D11Device *device);
		bool SubmitD3D11Frame(struct ID3D11DeviceContext *context,
			struct ID3D11Texture2D *leftTexture,
			struct ID3D11Texture2D *rightTexture);
		bool GetHeadPose(XrPosef *pose) const;
		void Shutdown();

		bool IsLoaded() const { return m_loader != nullptr; }
		bool IsInstanceReady() const { return m_instance != XR_NULL_HANDLE; }
		bool IsSystemReady() const { return m_system != XR_NULL_SYSTEM_ID; }
		bool IsSessionReady() const { return m_session != XR_NULL_HANDLE; }
		const char *LastError() const { return m_lastError; }
		int LastResult() const { return m_lastResult; }
		XrInstance Instance() const { return m_instance; }
		XrSystemId System() const { return m_system; }

	private:
		HMODULE m_loader;
		PFN_xrGetInstanceProcAddr m_getInstanceProcAddr;
		PFN_xrCreateInstance m_createInstance;
		PFN_xrDestroyInstance m_destroyInstance;
		PFN_xrGetSystem m_getSystem;
		PFN_xrGetD3D11GraphicsRequirementsKHR m_getD3D11Requirements;
		PFN_xrCreateSession m_createSession;
		PFN_xrDestroySession m_destroySession;
		PFN_xrPollEvent m_pollEvent;
		PFN_xrEnumerateViewConfigurationViews m_enumerateViewConfigurationViews;
		PFN_xrEnumerateSwapchainFormats m_enumerateSwapchainFormats;
		PFN_xrCreateSwapchain m_createSwapchain;
		PFN_xrDestroySwapchain m_destroySwapchain;
		PFN_xrEnumerateSwapchainImages m_enumerateSwapchainImages;
		PFN_xrAcquireSwapchainImage m_acquireSwapchainImage;
		PFN_xrWaitSwapchainImage m_waitSwapchainImage;
		PFN_xrReleaseSwapchainImage m_releaseSwapchainImage;
		PFN_xrWaitFrame m_waitFrame;
		PFN_xrBeginFrame m_beginFrame;
		PFN_xrEndFrame m_endFrame;
		PFN_xrBeginSession m_beginSession;
		PFN_xrEndSession m_endSession;
		PFN_xrCreateReferenceSpace m_createReferenceSpace;
		PFN_xrLocateViews m_locateViews;
		XrInstance m_instance;
		XrSystemId m_system;
		XrSession m_session;
		XrSpace m_stageSpace;
		XrSessionState m_sessionState;
		bool m_sessionRunning;
		XrViewConfigurationView m_viewConfig[2];
		XrSwapchain m_swapchains[2];
		std::vector<XrSwapchainImageD3D11KHR> m_swapchainImages[2];
		struct ID3D11Device *m_device;
		const char *m_lastError;
		int m_lastResult;
		XrPosef m_headPose;
		bool m_headPoseValid;
	};

}
