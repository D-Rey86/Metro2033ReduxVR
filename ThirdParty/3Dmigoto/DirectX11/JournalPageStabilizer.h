#pragma once
#include <cmath>
#include <cstring>

// Text-only native-page reference. Controller, calibration and eye transforms
// remain outside this helper. Never learn a reference from the draw-in pose.
class JournalPageStabilizer {
public:
	bool Update(unsigned frame, unsigned buttonFrame, float page[16]) {
		if (seen && frame == lastFrame) {
			if (locked) std::memcpy(page, reference, sizeof(reference));
			return locked;
		}
		if (!seen || frame - lastFrame > 12) {
			locked = closing = false;
			count = 0;
			lastButton = buttonFrame;
		}
		seen = true;
		lastFrame = frame;
		// Any new journal press after reference acquisition is a close request;
		// do not rely on the inferred open flag that is under investigation.
		if (buttonFrame != lastButton) {
			if (locked) closing = true;
			locked = false;
			count = 0;
			lastButton = buttonFrame;
		}
		float centre[3], axes[6];
		bool valid = true;
		for (unsigned i = 0; i < 16; ++i)
			valid = valid && std::isfinite(page[i]);
		for (unsigned r = 0; r < 3; ++r)
			centre[r] = page[r * 4] * 496.14f + page[r * 4 + 1] * 413.06f + page[r * 4 + 3];
		for (unsigned c = 0; c < 2; ++c) {
			float length = 0;
			for (unsigned r = 0; r < 3; ++r) length += page[r * 4 + c] * page[r * 4 + c];
			length = std::sqrt(length);
			valid = valid && length > 1e-7f;
			for (unsigned r = 0; r < 3; ++r)
				axes[c * 3 + r] = length > 1e-7f ? page[r * 4 + c] / length : 0;
		}
		if (!valid) {
			locked = false;
			count = 0;
		} else if (!locked && !closing) {
			bool stable = count != 0;
			for (unsigned r = 0; r < 3 && stable; ++r) {
				const float lo = centre[r] < lower[r] ? centre[r] : lower[r];
				const float hi = centre[r] > upper[r] ? centre[r] : upper[r];
				stable = hi - lo <= 0.008f;
			}
			for (unsigned c = 0; c < 2 && stable; ++c) {
				float dot = 0;
				for (unsigned r = 0; r < 3; ++r) dot += axes[c * 3 + r] * firstAxes[c * 3 + r];
				stable = dot >= 0.9998f;
			}
			if (!stable) {
				count = 0;
				firstFrame = frame;
				std::memset(sum, 0, sizeof(sum));
				std::memcpy(lower, centre, sizeof(lower));
				std::memcpy(upper, centre, sizeof(upper));
				std::memcpy(firstAxes, axes, sizeof(firstAxes));
			}
			for (unsigned r = 0; r < 3; ++r) {
				if (centre[r] < lower[r]) lower[r] = centre[r];
				if (centre[r] > upper[r]) upper[r] = centre[r];
			}
			for (unsigned i = 0; i < 16; ++i) sum[i] += page[i];
			++count;
			if (count >= 12 && frame - firstFrame >= 90) {
				for (unsigned i = 0; i < 16; ++i) reference[i] = static_cast<float>(sum[i] / count);
				locked = true;
			}
		}
		if (locked) std::memcpy(page, reference, sizeof(reference));
		return locked;
	}

private:
	bool seen = false, locked = false, closing = false;
	unsigned lastFrame = 0, lastButton = 0, firstFrame = 0, count = 0;
	float reference[16] = {}, lower[3] = {}, upper[3] = {}, firstAxes[6] = {};
	double sum[16] = {};
};
