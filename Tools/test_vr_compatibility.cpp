#include "../ThirdParty/3Dmigoto/DirectX11/VRCompatibility.h"

#include <cmath>
#include <cstdint>
#include <cstdio>

static bool Check(bool condition, const char *message)
{
	if (!condition)
		std::fprintf(stderr, "FAIL: %s\n", message);
	return condition;
}

int main()
{
	bool ok = true;
	const auto runtimeSubmission =
		VRCompatibility::SelectSubmissionTargetSize(3160, 3360, 3840, 2132);
	ok &= Check(runtimeSubmission.width == 3160 &&
		runtimeSubmission.height == 3360,
		"final submission must exactly match the valid OpenVR recommendation");
	ok &= Check(runtimeSubmission.usedRuntimeRecommendation,
		"valid OpenVR dimensions must be identified as runtime-derived");

	const auto sourceFallback =
		VRCompatibility::SelectSubmissionTargetSize(0, 0, 2560, 1421);
	ok &= Check(sourceFallback.width == 2560 && sourceFallback.height == 1421,
		"invalid OpenVR dimensions must preserve the source submission size");
	ok &= Check(!sourceFallback.usedRuntimeRecommendation,
		"source fallback must not be identified as runtime-derived");

	const auto oversizedFallback =
		VRCompatibility::SelectSubmissionTargetSize(20000, 20000, 1920, 1080);
	ok &= Check(oversizedFallback.width == 1920 &&
		oversizedFallback.height == 1080,
		"an unsupported D3D11 recommendation must safely retain the source");

	if (!ok)
		return 1;
	std::puts("VR compatibility target tests passed.");
	return 0;
}
