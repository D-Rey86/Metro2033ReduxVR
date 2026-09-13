#pragma once

// Backend-neutral VR data.  Metro-specific camera, weapon and shot code
// should consume these types rather than OpenVR/OpenXR structs directly.
namespace VRBackend {

	enum class Runtime {
		OpenVR,
		OpenXR,
	};

	struct Pose {
		float matrix[12] = {};
		bool valid = false;
	};

	struct ControllerState {
		float stickX = 0.0f;
		float stickY = 0.0f;
		float trigger = 0.0f;
		unsigned long long buttons = 0;
		bool valid = false;
	};

}
