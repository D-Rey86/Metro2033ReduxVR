#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace VRCompatibility {

struct SubmissionTargetSize {
	unsigned width;
	unsigned height;
	bool usedRuntimeRecommendation;
};

struct SceneRenderTargetSize {
	unsigned baseWidth;
	unsigned baseHeight;
	unsigned targetWidth;
	unsigned targetHeight;
	bool monitorIndependent;
};

// OpenVR's recommendation is the contract for the final per-eye image. Metro
// may render internally at a different, engine-safe aspect ratio, but the
// completed eye must be converted to this shape before Submit. Fall back to
// the source only when the runtime did not provide a usable D3D11 texture size.
inline SubmissionTargetSize SelectSubmissionTargetSize(
	unsigned recommendedWidth, unsigned recommendedHeight,
	unsigned sourceWidth, unsigned sourceHeight)
{
	const unsigned maximumD3D11Dimension = 16384;
	const bool validRecommendation = recommendedWidth >= 640 &&
		recommendedHeight >= 360 &&
		recommendedWidth <= maximumD3D11Dimension &&
		recommendedHeight <= maximumD3D11Dimension;
	if (validRecommendation)
		return { recommendedWidth, recommendedHeight, true };
	return { (std::max)(1u, sourceWidth),
		(std::max)(1u, sourceHeight), false };
}

// Metro derives its scene canvas from the desktop backbuffer before applying
// its SSAA tier. Normalize that canvas to the tested 2560x1440 output
// reference, then apply only the VR scale above 1x. This keeps headset source
// quality independent of the physical monitor while preserving Metro's exact
// odd-height canvases and its established sub-1x compositor behavior.
inline SceneRenderTargetSize SelectMonitorIndependentSceneTarget(
	unsigned requestedSceneWidth, unsigned requestedSceneHeight,
	unsigned desktopOutputWidth, unsigned desktopOutputHeight,
	float vrScale)
{
	const unsigned referenceOutputWidth = 2560;
	const unsigned referenceOutputHeight = 1440;
	const unsigned maximumD3D11Dimension = 16384;
	if (requestedSceneWidth < 640 || requestedSceneHeight < 360 ||
		desktopOutputWidth < 640 || desktopOutputHeight < 360)
		return { requestedSceneWidth, requestedSceneHeight,
			requestedSceneWidth, requestedSceneHeight, false };

	// These are the five canvases verified in-headset at a 2560x1440 desktop
	// output. Snapping a proportionally equivalent lower-monitor request back
	// to the exact dimensions avoids one-pixel resolve-chain disagreements.
	static const unsigned knownCanvases[][2] = {
		{ 1810, 1004 }, // Metro SSAA 0.5x
		{ 2560, 1421 }, // Metro SSAA Off
		{ 3620, 2009 }, // Metro SSAA X2
		{ 4434, 2461 }, // Metro SSAA 3x
		{ 5120, 2842 }, // Metro SSAA 4x
	};
	// The forced presentation surface may already have caused Metro to build
	// the reference canvas. Accept that exact result before applying the ratio
	// from the user's requested desktop mode, otherwise it would be scaled twice.
	unsigned baseWidth = requestedSceneWidth;
	unsigned baseHeight = requestedSceneHeight;
	double bestRelativeError = 1.0;
	for (const auto &canvas : knownCanvases) {
		const double widthError = std::fabs((double)requestedSceneWidth - canvas[0]) /
			canvas[0];
		const double heightError = std::fabs((double)requestedSceneHeight - canvas[1]) /
			canvas[1];
		const double error = (std::max)(widthError, heightError);
		if (error < bestRelativeError) {
			bestRelativeError = error;
			baseWidth = canvas[0];
			baseHeight = canvas[1];
		}
	}
	if (bestRelativeError > 0.015) {
		const double normalizedWidth = (double)requestedSceneWidth *
			referenceOutputWidth / desktopOutputWidth;
		const double normalizedHeight = (double)requestedSceneHeight *
			referenceOutputHeight / desktopOutputHeight;
		baseWidth = (unsigned)std::floor(normalizedWidth + 0.5);
		baseHeight = (unsigned)std::floor(normalizedHeight + 0.5);
		bestRelativeError = 1.0;
		for (const auto &canvas : knownCanvases) {
			const double widthError = std::fabs(normalizedWidth - canvas[0]) /
				canvas[0];
			const double heightError = std::fabs(normalizedHeight - canvas[1]) /
				canvas[1];
			const double error = (std::max)(widthError, heightError);
			if (error < bestRelativeError) {
				bestRelativeError = error;
				baseWidth = canvas[0];
				baseHeight = canvas[1];
			}
		}
		if (bestRelativeError > 0.015) {
			baseWidth = (std::max)(640u,
				(unsigned)std::floor(normalizedWidth + 0.5));
			baseHeight = (std::max)(360u,
				(unsigned)std::floor(normalizedHeight + 0.5));
		}
	}
	// Unknown render modes retain their measured proportional dimensions.
	// Known modes tolerate only small rounding/display-aspect differences.

	const float internalScale = (std::max)(1.0f,
		(std::min)(2.0f, vrScale));
	const unsigned targetWidth = (unsigned)std::floor(
		baseWidth * internalScale + 0.5f);
	const unsigned targetHeight = (unsigned)std::floor(
		baseHeight * internalScale + 0.5f);
	if (baseWidth > maximumD3D11Dimension ||
		baseHeight > maximumD3D11Dimension ||
		targetWidth > maximumD3D11Dimension ||
		targetHeight > maximumD3D11Dimension)
		return { requestedSceneWidth, requestedSceneHeight,
			requestedSceneWidth, requestedSceneHeight, false };

	return { baseWidth, baseHeight, targetWidth, targetHeight, true };
}

}
