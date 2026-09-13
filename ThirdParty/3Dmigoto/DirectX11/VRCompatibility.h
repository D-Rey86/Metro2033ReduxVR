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
	return { std::max(1u, sourceWidth), std::max(1u, sourceHeight), false };
}

}
