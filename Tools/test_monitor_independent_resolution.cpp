#include "../ThirdParty/3Dmigoto/DirectX11/VRCompatibility.h"

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
	const auto native = VRCompatibility::SelectMonitorIndependentSceneTarget(
		2560, 1421, 2560, 1440, 1.0f);
	ok &= Check(native.monitorIndependent && native.baseWidth == 2560 &&
		native.baseHeight == 1421 && native.targetWidth == 2560 &&
		native.targetHeight == 1421,
		"the tested 1440p SSAA-Off canvas must remain byte-for-byte native");

	const auto fullHd = VRCompatibility::SelectMonitorIndependentSceneTarget(
		1920, 1066, 1920, 1080, 1.0f);
	ok &= Check(fullHd.monitorIndependent && fullHd.baseWidth == 2560 &&
		fullHd.baseHeight == 1421 && fullHd.targetWidth == 2560 &&
		fullHd.targetHeight == 1421,
		"a 1080p desktop must normalize to the tested VR source canvas");

	const auto alreadyNormalized =
		VRCompatibility::SelectMonitorIndependentSceneTarget(
			2560, 1421, 1920, 1080, 1.0f);
	ok &= Check(alreadyNormalized.baseWidth == 2560 &&
		alreadyNormalized.baseHeight == 1421 &&
		alreadyNormalized.targetWidth == 2560 &&
		alreadyNormalized.targetHeight == 1421,
		"a scene already derived from the fixed presentation surface must not "
		"be normalized twice");

	const auto fullHdScaled =
		VRCompatibility::SelectMonitorIndependentSceneTarget(
			1920, 1066, 1920, 1080, 1.5f);
	ok &= Check(fullHdScaled.targetWidth == 3840 &&
		fullHdScaled.targetHeight == 2132,
		"VR supersampling must apply after monitor normalization");

	const auto lowVrScale =
		VRCompatibility::SelectMonitorIndependentSceneTarget(
			1920, 1066, 1920, 1080, 0.75f);
	ok &= Check(lowVrScale.targetWidth == 2560 &&
		lowVrScale.targetHeight == 1421,
		"sub-1x must preserve the established compositor-only reduction path");

	const auto x2 = VRCompatibility::SelectMonitorIndependentSceneTarget(
		2715, 1507, 1920, 1080, 1.0f);
	ok &= Check(x2.baseWidth == 3620 && x2.baseHeight == 2009,
		"lower-monitor X2 SSAA must normalize to its exact verified canvas");

	const auto half = VRCompatibility::SelectMonitorIndependentSceneTarget(
		1358, 753, 1920, 1080, 1.0f);
	ok &= Check(half.baseWidth == 1810 && half.baseHeight == 1004,
		"lower-monitor 0.5x SSAA must normalize to its exact verified canvas");

	const auto x3 = VRCompatibility::SelectMonitorIndependentSceneTarget(
		3326, 1846, 1920, 1080, 1.0f);
	ok &= Check(x3.baseWidth == 4434 && x3.baseHeight == 2461,
		"lower-monitor 3x SSAA must normalize to its exact verified canvas");

	const auto x4 = VRCompatibility::SelectMonitorIndependentSceneTarget(
		3840, 2132, 1920, 1080, 1.0f);
	ok &= Check(x4.baseWidth == 5120 && x4.baseHeight == 2842,
		"lower-monitor 4x SSAA must normalize to its exact verified canvas");

	const auto ultrawide = VRCompatibility::SelectMonitorIndependentSceneTarget(
		3440, 1421, 3440, 1440, 1.0f);
	ok &= Check(ultrawide.baseWidth == 2560 && ultrawide.baseHeight == 1421,
		"desktop aspect ratio must not leak into the normalized VR scene canvas");

	const auto unavailableOutput =
		VRCompatibility::SelectMonitorIndependentSceneTarget(
			1920, 1066, 0, 0, 1.0f);
	ok &= Check(!unavailableOutput.monitorIndependent &&
		unavailableOutput.targetWidth == 1920 &&
		unavailableOutput.targetHeight == 1066,
		"missing output dimensions must fail closed to Metro's request");

	if (!ok)
		return 1;
	std::puts("Monitor-independent resolution policy tests passed.");
	return 0;
}
