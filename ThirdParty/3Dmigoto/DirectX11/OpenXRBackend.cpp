#include "OpenXRBackend.h"

#include <windows.h>

#include <cstring>
#include <vector>
#include <d3d11.h>

namespace VRBackend {

	OpenXRRuntime::OpenXRRuntime()
		: m_loader(nullptr), m_getInstanceProcAddr(nullptr),
		m_createInstance(nullptr), m_destroyInstance(nullptr),
		m_getSystem(nullptr), m_getD3D11Requirements(nullptr),
		m_createSession(nullptr), m_destroySession(nullptr),
		m_pollEvent(nullptr), m_enumerateViewConfigurationViews(nullptr),
		m_enumerateSwapchainFormats(nullptr), m_createSwapchain(nullptr),
		m_destroySwapchain(nullptr), m_enumerateSwapchainImages(nullptr),
		m_acquireSwapchainImage(nullptr), m_waitSwapchainImage(nullptr),
		m_releaseSwapchainImage(nullptr), m_waitFrame(nullptr),
		m_beginFrame(nullptr), m_endFrame(nullptr),
		m_beginSession(nullptr), m_endSession(nullptr),
		m_createReferenceSpace(nullptr), m_locateViews(nullptr),
		m_instance(XR_NULL_HANDLE), m_system(XR_NULL_SYSTEM_ID),
		m_session(XR_NULL_HANDLE), m_stageSpace(XR_NULL_HANDLE),
		m_sessionState(XR_SESSION_STATE_UNKNOWN), m_sessionRunning(false),
		m_viewConfig{}, m_swapchains{}, m_device(nullptr), m_lastError("none"), m_lastResult(XR_SUCCESS),
		m_headPose{}, m_headPoseValid(false)
	{
	}

	OpenXRRuntime::~OpenXRRuntime()
	{
		Shutdown();
	}

	bool OpenXRRuntime::Load()
	{
		if (m_loader)
			return true;
		m_loader = LoadLibraryW(L"openxr_loader.dll");
		if (!m_loader) {
			m_lastError = "LoadLibrary(openxr_loader.dll)";
			return false;
		}
		m_getInstanceProcAddr = reinterpret_cast<PFN_xrGetInstanceProcAddr>(
			GetProcAddress(m_loader, "xrGetInstanceProcAddr"));
		if (!m_getInstanceProcAddr) {
			m_lastError = "xrGetInstanceProcAddr export";
			FreeLibrary(m_loader);
			m_loader = nullptr;
			return false;
		}
		return true;
	}

	bool OpenXRRuntime::CreateInstance()
	{
		if (!Load() || m_instance != XR_NULL_HANDLE)
			return m_instance != XR_NULL_HANDLE;

		if (XR_FAILED(m_getInstanceProcAddr(XR_NULL_HANDLE,
			"xrCreateInstance", reinterpret_cast<PFN_xrVoidFunction *>(&m_createInstance))))
			{ m_lastError = "xrCreateInstance proc"; return false; }

		uint32_t extensionCount = 0;
		PFN_xrEnumerateInstanceExtensionProperties enumerateExtensions = nullptr;
		if (XR_FAILED(m_getInstanceProcAddr(XR_NULL_HANDLE,
			"xrEnumerateInstanceExtensionProperties", reinterpret_cast<PFN_xrVoidFunction *>(&enumerateExtensions))))
			{ m_lastError = "xrEnumerateInstanceExtensionProperties proc"; return false; }
		if (XR_FAILED(enumerateExtensions(nullptr, 0, &extensionCount, nullptr)))
			{ m_lastError = "xrEnumerateInstanceExtensionProperties count"; return false; }
		std::vector<XrExtensionProperties> extensions(extensionCount,
			XrExtensionProperties{ XR_TYPE_EXTENSION_PROPERTIES });
		if (XR_FAILED(enumerateExtensions(nullptr, extensionCount, &extensionCount, extensions.data())))
			{ m_lastError = "xrEnumerateInstanceExtensionProperties data"; return false; }
		bool haveD3D11 = false;
		for (const auto &extension : extensions)
			if (std::strcmp(extension.extensionName, XR_KHR_D3D11_ENABLE_EXTENSION_NAME) == 0)
				haveD3D11 = true;
		if (!haveD3D11)
			{ m_lastError = "XR_KHR_D3D11_enable missing"; return false; }
		const char *enabledExtensions[] = { XR_KHR_D3D11_ENABLE_EXTENSION_NAME };

		XrInstanceCreateInfo ci{ XR_TYPE_INSTANCE_CREATE_INFO };
		std::strncpy(ci.applicationInfo.applicationName, "Metro 2033 Redux VR",
			XR_MAX_APPLICATION_NAME_SIZE - 1);
		std::strncpy(ci.applicationInfo.engineName, "3Dmigoto",
			XR_MAX_ENGINE_NAME_SIZE - 1);
		ci.applicationInfo.applicationVersion = 1;
		ci.applicationInfo.engineVersion = 1;
		// VDXR currently exposes the OpenXR 1.0 contract. Requesting the
		// header's 1.1 current version causes xrCreateInstance to fail on
		// runtimes that have not adopted 1.1 yet.
		ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
		ci.enabledExtensionCount = 1;
		ci.enabledExtensionNames = enabledExtensions;
		XrResult createResult = m_createInstance(&ci, &m_instance);
		m_lastResult = static_cast<int>(createResult);
		if (XR_FAILED(createResult))
			{ m_lastError = "xrCreateInstance"; return false; }

		m_getInstanceProcAddr(m_instance, "xrDestroyInstance",
			reinterpret_cast<PFN_xrVoidFunction *>(&m_destroyInstance));
		m_getInstanceProcAddr(m_instance, "xrGetSystem",
			reinterpret_cast<PFN_xrVoidFunction *>(&m_getSystem));
		if (!m_getSystem) {
			m_lastError = "xrGetSystem proc after instance";
			return false;
		}
		m_getInstanceProcAddr(m_instance, "xrGetD3D11GraphicsRequirementsKHR",
			reinterpret_cast<PFN_xrVoidFunction *>(&m_getD3D11Requirements));
		m_getInstanceProcAddr(m_instance, "xrCreateSession",
			reinterpret_cast<PFN_xrVoidFunction *>(&m_createSession));
		m_getInstanceProcAddr(m_instance, "xrDestroySession",
			reinterpret_cast<PFN_xrVoidFunction *>(&m_destroySession));
		m_getInstanceProcAddr(m_instance, "xrPollEvent",
			reinterpret_cast<PFN_xrVoidFunction *>(&m_pollEvent));
		m_getInstanceProcAddr(m_instance, "xrEnumerateViewConfigurationViews",
			reinterpret_cast<PFN_xrVoidFunction *>(&m_enumerateViewConfigurationViews));
		m_getInstanceProcAddr(m_instance, "xrEnumerateSwapchainFormats",
			reinterpret_cast<PFN_xrVoidFunction *>(&m_enumerateSwapchainFormats));
		m_getInstanceProcAddr(m_instance, "xrCreateSwapchain",
			reinterpret_cast<PFN_xrVoidFunction *>(&m_createSwapchain));
		m_getInstanceProcAddr(m_instance, "xrDestroySwapchain",
			reinterpret_cast<PFN_xrVoidFunction *>(&m_destroySwapchain));
		m_getInstanceProcAddr(m_instance, "xrEnumerateSwapchainImages",
			reinterpret_cast<PFN_xrVoidFunction *>(&m_enumerateSwapchainImages));
		m_getInstanceProcAddr(m_instance, "xrAcquireSwapchainImage",
			reinterpret_cast<PFN_xrVoidFunction *>(&m_acquireSwapchainImage));
		m_getInstanceProcAddr(m_instance, "xrWaitSwapchainImage",
			reinterpret_cast<PFN_xrVoidFunction *>(&m_waitSwapchainImage));
		m_getInstanceProcAddr(m_instance, "xrReleaseSwapchainImage",
			reinterpret_cast<PFN_xrVoidFunction *>(&m_releaseSwapchainImage));
		m_getInstanceProcAddr(m_instance, "xrWaitFrame",
			reinterpret_cast<PFN_xrVoidFunction *>(&m_waitFrame));
		m_getInstanceProcAddr(m_instance, "xrBeginFrame",
			reinterpret_cast<PFN_xrVoidFunction *>(&m_beginFrame));
		m_getInstanceProcAddr(m_instance, "xrEndFrame",
			reinterpret_cast<PFN_xrVoidFunction *>(&m_endFrame));
		m_getInstanceProcAddr(m_instance, "xrBeginSession",
			reinterpret_cast<PFN_xrVoidFunction *>(&m_beginSession));
		m_getInstanceProcAddr(m_instance, "xrEndSession",
			reinterpret_cast<PFN_xrVoidFunction *>(&m_endSession));
		m_getInstanceProcAddr(m_instance, "xrCreateReferenceSpace",
			reinterpret_cast<PFN_xrVoidFunction *>(&m_createReferenceSpace));
		m_getInstanceProcAddr(m_instance, "xrLocateViews",
			reinterpret_cast<PFN_xrVoidFunction *>(&m_locateViews));
		return m_destroyInstance != nullptr && m_getSystem != nullptr;
	}

	bool OpenXRRuntime::CreateD3D11Session(ID3D11Device *device)
	{
		if (!device || !SelectHmdSystem() || m_session != XR_NULL_HANDLE)
			return m_session != XR_NULL_HANDLE;
		if (!m_getD3D11Requirements || !m_createSession) {
			m_lastError = "D3D11/session proc";
			return false;
		}
		XrGraphicsRequirementsD3D11KHR requirements{
			XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR };
		if (XR_FAILED(m_getD3D11Requirements(m_instance, m_system, &requirements))) {
			m_lastError = "xrGetD3D11GraphicsRequirementsKHR";
			return false;
		}
		XrGraphicsBindingD3D11KHR binding{ XR_TYPE_GRAPHICS_BINDING_D3D11_KHR };
		binding.device = device;
		XrSessionCreateInfo ci{ XR_TYPE_SESSION_CREATE_INFO };
		ci.next = &binding;
		ci.systemId = m_system;
		if (XR_FAILED(m_createSession(m_instance, &ci, &m_session))) {
			m_lastError = "xrCreateSession";
			return false;
		}
		m_device = device;
		m_device->AddRef();
		return true;
	}

	bool OpenXRRuntime::SubmitD3D11Frame(ID3D11DeviceContext *context,
		ID3D11Texture2D *leftTexture, ID3D11Texture2D *rightTexture)
	{
		if (!context || !leftTexture || !rightTexture || !m_session ||
			!m_pollEvent || !m_waitFrame || !m_beginFrame || !m_endFrame ||
			!m_createSwapchain || !m_acquireSwapchainImage ||
			!m_waitSwapchainImage || !m_releaseSwapchainImage)
			return false;

		// OpenXR delivers session state asynchronously. Do not call waitFrame
		// until the runtime has sent READY and we have begun the session.
		XrEventDataBuffer event{ XR_TYPE_EVENT_DATA_BUFFER };
		while (m_pollEvent(m_instance, &event) == XR_SUCCESS) {
			if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
				const XrEventDataSessionStateChanged *changed =
					reinterpret_cast<const XrEventDataSessionStateChanged *>(&event);
				m_sessionState = changed->state;
				if (m_sessionState == XR_SESSION_STATE_READY && !m_sessionRunning && m_beginSession) {
					XrSessionBeginInfo begin{ XR_TYPE_SESSION_BEGIN_INFO };
					begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
					m_sessionRunning = XR_SUCCEEDED(m_beginSession(m_session, &begin));
				}
				if (m_sessionState == XR_SESSION_STATE_STOPPING && m_sessionRunning && m_endSession) {
					m_endSession(m_session);
					m_sessionRunning = false;
				}
			}
			event = XrEventDataBuffer{ XR_TYPE_EVENT_DATA_BUFFER };
		}
		if (!m_sessionRunning)
			return true;

		// Create the eye swapchains from the game's actual back-buffer size and
		// format. This keeps the first OpenXR path compatible with the existing
		// stereo renderer, which already produces one texture per eye.
		D3D11_TEXTURE2D_DESC sourceDesc{};
		leftTexture->GetDesc(&sourceDesc);
		if (m_swapchains[0] == XR_NULL_HANDLE || m_swapchains[1] == XR_NULL_HANDLE) {
			if (!m_enumerateViewConfigurationViews || !m_enumerateSwapchainFormats)
				return false;
			uint32_t viewCount = 0;
			if (XR_FAILED(m_enumerateViewConfigurationViews(m_instance, m_system,
				XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &viewCount, nullptr)) || viewCount < 2)
				return false;
			if (viewCount > 2) viewCount = 2;
			std::vector<XrViewConfigurationView> views(viewCount,
				XrViewConfigurationView{ XR_TYPE_VIEW_CONFIGURATION_VIEW });
			if (XR_FAILED(m_enumerateViewConfigurationViews(m_instance, m_system,
				XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, viewCount, &viewCount, views.data())))
				return false;
			for (uint32_t i = 0; i < 2; ++i) m_viewConfig[i] = views[i];

			uint32_t formatCount = 0;
			if (XR_FAILED(m_enumerateSwapchainFormats(m_session, 0, &formatCount, nullptr)))
				return false;
			std::vector<int64_t> formats(formatCount);
			if (XR_FAILED(m_enumerateSwapchainFormats(m_session, formatCount, &formatCount, formats.data())))
				return false;
			int64_t format = static_cast<int64_t>(sourceDesc.Format);
			bool formatSupported = false;
			for (int64_t candidate : formats) if (candidate == format) formatSupported = true;
			if (!formatSupported) return false;

			for (uint32_t eye = 0; eye < 2; ++eye) {
				XrSwapchainCreateInfo create{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
				create.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
				create.format = format;
				create.sampleCount = 1;
				create.width = sourceDesc.Width;
				create.height = sourceDesc.Height;
				create.faceCount = 1;
				create.arraySize = 1;
				create.mipCount = 1;
				if (XR_FAILED(m_createSwapchain(m_session, &create, &m_swapchains[eye])))
					return false;
				uint32_t imageCount = 0;
				if (XR_FAILED(m_enumerateSwapchainImages(m_swapchains[eye], 0, &imageCount, nullptr)))
					return false;
				m_swapchainImages[eye].resize(imageCount,
					XrSwapchainImageD3D11KHR{ XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR });
				if (XR_FAILED(m_enumerateSwapchainImages(m_swapchains[eye], imageCount,
					&imageCount, reinterpret_cast<XrSwapchainImageBaseHeader *>(m_swapchainImages[eye].data()))))
					return false;
			}
			if (m_createReferenceSpace && m_stageSpace == XR_NULL_HANDLE) {
				XrReferenceSpaceCreateInfo space{ XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
				space.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
				space.poseInReferenceSpace.orientation.w = 1.0f;
				if (XR_FAILED(m_createReferenceSpace(m_session, &space, &m_stageSpace))) {
					space.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
					m_createReferenceSpace(m_session, &space, &m_stageSpace);
				}
			}
		}

		XrFrameWaitInfo waitInfo{ XR_TYPE_FRAME_WAIT_INFO };
		XrFrameState frameState{ XR_TYPE_FRAME_STATE };
		if (XR_FAILED(m_waitFrame(m_session, &waitInfo, &frameState))) return false;
		XrFrameBeginInfo beginInfo{ XR_TYPE_FRAME_BEGIN_INFO };
		if (XR_FAILED(m_beginFrame(m_session, &beginInfo))) return false;
		XrView views[2] = {
			{ XR_TYPE_VIEW }, { XR_TYPE_VIEW } };
		XrViewState viewState{ XR_TYPE_VIEW_STATE };
		XrViewLocateInfo locateInfo{ XR_TYPE_VIEW_LOCATE_INFO };
		locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
		locateInfo.displayTime = frameState.predictedDisplayTime;
		locateInfo.space = m_stageSpace;
		uint32_t locatedCount = 0;
		if (m_locateViews && XR_SUCCEEDED(m_locateViews(m_session, &locateInfo,
			&viewState, 2, &locatedCount, views)) && locatedCount >= 2) {
			m_headPose = views[0].pose;
			m_headPoseValid = (viewState.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) != 0;
		}

		ID3D11Texture2D *source[2] = { leftTexture, rightTexture };
		XrSwapchainSubImage subImages[2]{};
		XrCompositionLayerProjectionView projectionViews[2]{
			{ XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW },
			{ XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW } };
		for (uint32_t eye = 0; eye < 2; ++eye) {
			uint32_t index = 0;
			if (XR_FAILED(m_acquireSwapchainImage(m_swapchains[eye], nullptr, &index))) return false;
			XrSwapchainImageWaitInfo imageWait{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
			imageWait.timeout = XR_INFINITE_DURATION;
			if (XR_FAILED(m_waitSwapchainImage(m_swapchains[eye], &imageWait))) return false;
			// The game's back buffer may be multisampled. OpenXR swapchain
			// images are single-sampled, so a direct CopyResource would produce
			// undefined/corrupted eye images on that path.
			if (sourceDesc.SampleDesc.Count > 1)
				context->ResolveSubresource(m_swapchainImages[eye][index].texture,
					0, source[eye], 0, sourceDesc.Format);
			else
				context->CopyResource(m_swapchainImages[eye][index].texture, source[eye]);
			m_releaseSwapchainImage(m_swapchains[eye], nullptr);
			subImages[eye].swapchain = m_swapchains[eye];
			subImages[eye].imageRect.extent.width = static_cast<int32_t>(sourceDesc.Width);
			subImages[eye].imageRect.extent.height = static_cast<int32_t>(sourceDesc.Height);
			projectionViews[eye].subImage = subImages[eye];
			if (locatedCount >= 2) {
				projectionViews[eye].fov = views[eye].fov;
				projectionViews[eye].pose = views[eye].pose;
			} else {
				projectionViews[eye].fov.angleLeft = -0.8f;
				projectionViews[eye].fov.angleRight = 0.8f;
				projectionViews[eye].fov.angleUp = 0.8f;
				projectionViews[eye].fov.angleDown = -0.8f;
				projectionViews[eye].pose.orientation.w = 1.0f;
			}
		}
		XrCompositionLayerProjection layer{ XR_TYPE_COMPOSITION_LAYER_PROJECTION };
		layer.space = m_stageSpace;
		layer.viewCount = 2;
		layer.views = projectionViews;
		const XrCompositionLayerBaseHeader *layers[] = {
			reinterpret_cast<const XrCompositionLayerBaseHeader *>(&layer) };
		XrFrameEndInfo endInfo{ XR_TYPE_FRAME_END_INFO };
		endInfo.displayTime = frameState.predictedDisplayTime;
		endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
		endInfo.layerCount = 1;
		endInfo.layers = layers;
		return XR_SUCCEEDED(m_endFrame(m_session, &endInfo));
	}

	bool OpenXRRuntime::GetHeadPose(XrPosef *pose) const
	{
		if (!pose || !m_headPoseValid)
			return false;
		*pose = m_headPose;
		return true;
	}

	bool OpenXRRuntime::SelectHmdSystem()
	{
		if (!CreateInstance() || !m_getSystem)
		{
			if (m_lastError && std::strcmp(m_lastError, "none") == 0)
				m_lastError = "CreateInstance/xrGetSystem";
			return false;
		}
		XrSystemGetInfo si{ XR_TYPE_SYSTEM_GET_INFO };
		si.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
		if (XR_FAILED(m_getSystem(m_instance, &si, &m_system))) {
			m_lastError = "xrGetSystem(HMD)";
			return false;
		}
		return true;
	}

	void OpenXRRuntime::Shutdown()
	{
		for (int eye = 0; eye < 2; ++eye) {
			if (m_destroySwapchain && m_swapchains[eye] != XR_NULL_HANDLE)
				m_destroySwapchain(m_swapchains[eye]);
			m_swapchains[eye] = XR_NULL_HANDLE;
			m_swapchainImages[eye].clear();
		}
		if (m_device) {
			m_device->Release();
			m_device = nullptr;
		}
		if (m_destroySession && m_session != XR_NULL_HANDLE)
			m_destroySession(m_session);
		m_session = XR_NULL_HANDLE;
		if (m_destroyInstance && m_instance != XR_NULL_HANDLE)
			m_destroyInstance(m_instance);
		m_instance = XR_NULL_HANDLE;
		m_system = XR_NULL_SYSTEM_ID;
		m_destroyInstance = nullptr;
		m_getSystem = nullptr;
		m_getD3D11Requirements = nullptr;
		m_createSession = nullptr;
		m_destroySession = nullptr;
		m_beginSession = nullptr;
		m_endSession = nullptr;
		m_createInstance = nullptr;
		if (m_loader)
			FreeLibrary(m_loader);
		m_loader = nullptr;
		m_getInstanceProcAddr = nullptr;
	}

}
