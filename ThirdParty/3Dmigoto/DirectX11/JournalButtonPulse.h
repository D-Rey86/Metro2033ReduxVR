#pragma once

// Metro needs a hold to open the journal, but closing must not continue holding
// BACK long enough to open it again. One gesture owns one pulse; releasing just
// half the chord must not turn that gesture into another press.
class JournalButtonPulse {
public:
	bool Update(bool modifier, bool stickClick, bool presented) {
		if (!modifier && !stickClick && remaining == 0)
			armed = true;
		if (modifier && stickClick && armed && remaining == 0) {
			remaining = presented ? kCloseFrames : kOpenFrames;
			armed = false;
		}
		const bool down = remaining > 0;
		if (down)
			--remaining;
		return down;
	}

	void Cancel() {
		remaining = 0;
		armed = false;
	}

	static const int kOpenFrames = 108; // Preserve the existing opening hold.
	static const int kCloseFrames = 6;  // ~83 ms at 72 Hz, not a second hold.

private:
	int remaining = 0;
	bool armed = true;
};
