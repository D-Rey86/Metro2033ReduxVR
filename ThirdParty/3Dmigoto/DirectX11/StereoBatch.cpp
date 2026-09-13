#include "StereoBatch.h"

#include <vector>
#include <cstring>

#include "log.h"
#include "Globals.h"

namespace StereoBatch {

	// Start disabled. The per-draw path works and is shippable; this only
	// turns on once it has been shown to produce an identical picture.
	// OFF. Batching works and the picture is correct with it, but it does not
	// pay: 21fps against 22-24 for the simple per-draw path, even after the
	// replay was reduced to setting only what changed between draws.
	//
	// The reason is a flaw in the test that motivated it. Skipping the
	// render-target switches took the town from 22 to the low 30s, and that
	// was read as "switches cost a third of the frame". But that test also
	// stopped the second eye writing to a SECOND SET of full-resolution
	// targets - it drew into the first eye's. So it measured switching plus
	// the bandwidth of the extra targets together, and attributed all of it
	// to switching. Batching can only ever recover the first part, which
	// turns out to be the smaller one.
	//
	// Left in place, off, behind this flag: it is correct, and if the second
	// eye's cost is ever reduced some other way the switch saving may matter
	// again.
	bool gEnabled = false;

	namespace {
		// Longest run measured was 529, so this covers the real distribution
		// with room to spare. Anything beyond it falls back to inline drawing
		// rather than being dropped - a slow frame is acceptable, a missing
		// object is not.
		const int kMaxHeld = 1024;

		Held sHeld[kMaxHeld];
		int sCount = 0;

		// Per-draw copies of the matrices, because the engine recycles the
		// constant buffer between draws and the originals are gone by the
		// time we replay.
		std::vector<unsigned char> sPool;
	}

	void Reset()
	{
		sCount = 0;
		sPool.clear();
	}

	bool Empty()
	{
		return sCount == 0;
	}

	int Count()
	{
		return sCount;
	}

	bool Hold(const Held &h, const unsigned char *cb0, UINT cb0Size,
		const unsigned char *cb1, UINT cb1Size)
	{
		if (sCount >= kMaxHeld)
			return false;

		Held &dst = sHeld[sCount];
		dst = h;

		dst.cb0Offset = -1;
		dst.cb1Offset = -1;
		if (cb0 && cb0Size) {
			dst.cb0Offset = (int)sPool.size();
			sPool.insert(sPool.end(), cb0, cb0 + cb0Size);
		}
		if (cb1 && cb1Size) {
			dst.cb1Offset = (int)sPool.size();
			sPool.insert(sPool.end(), cb1, cb1 + cb1Size);
		}

		sCount++;
		return true;
	}

	const Held *At(int i)
	{
		if (i < 0 || i >= sCount)
			return NULL;
		return &sHeld[i];
	}

	const unsigned char *Bytes(int offset)
	{
		if (offset < 0 || (size_t)offset >= sPool.size())
			return NULL;
		return sPool.data() + offset;
	}
}
