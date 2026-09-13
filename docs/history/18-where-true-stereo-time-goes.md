# Where true stereo's time actually goes

Every performance number in this project before tonight came from the framerate
Virtual Desktop reports. The headset runs at **72 Hz**, and the compositor locks
to a half or a third of refresh when a frame misses its budget, so that figure
could only ever read about 72, 36 or 24. Several correct changes in a row
appeared to do nothing because they moved the frame time without crossing a
threshold, and two figures the whole cost model rested on turned out to be
artefacts:

- "the game's entire frame costs 13.9 ms" was the **72 Hz frame interval** -
  AER sitting vsync-locked at full rate. Not a cost at all.
- "the second eye costs 33.7 ms" was the difference between two such quantised
  readings. It happened to be about right, for the wrong reasons.

`StereoSinglePass::FrameTiming` now measures GPU time directly with timestamp
queries at Present. Note it is Present-to-Present on the GPU timeline, so it
includes the GPU idling while the CPU catches up - CPU cost shows up in it
exactly like GPU cost.

## The measurements

Mess hall, standing still, same spot:

| | GPU ms |
|---|---|
| alternate-eye | **15.4** |
| true stereo | **45.7** |
| 72 Hz budget | 13.9 |

AER is already 1.5 ms over budget, which is why it now reads 65 fps where it
used to hold 72. That regression arrived with the array-slice targets and has
not been chased down.

## The self-bisecting sweep

`StereoTwin::gBisectRunning` cycles configurations every ~300 frames and logs
real milliseconds for each, so one standing-still run produces a whole
breakdown instead of one bit per run. Results, averaged over several sweeps:

| removed from the frame | saves |
|---|---|
| second eye's TESSELLATED draws | 3.7 ms |
| second eye's POST draws (sample an eye-dependent texture) | 3.0 ms |
| second eye's PLAIN GEOMETRY draws | 1.0 ms |
| second eye's INSTANCED draws | 0.2 ms |
| **the entire second eye's drawing** | **6.5 ms** |
| twin CLEARS | 1.0 ms |
| second compositor submit copy | 0.0 ms |
| CPU building the second eye's constant buffers | 0.34 ms |

**Removing every second-eye draw saves only 6.5 ms of a 30 ms gap.** Roughly
24 ms of true stereo's cost is not the second eye's rendering, is not the
clears, is not the submit, and is not CPU work in the constant-buffer path. It
is insensitive to how much geometry is drawn, which is the signature of
something per-frame and fixed. Still unidentified. The next phase to try is
`kSkipTwinLookups`, which exits the twin path on its first line exactly as
alternate-eye does, isolating our own per-draw bookkeeping.

## What this means for single-pass stereo

Single-pass works and is correct: 467 draws a frame fold into one draw with two
instances, both eyes render properly, and the framerate did not move.

That is not a bug, it is the premise being wrong. **Instanced single-pass does
not transform geometry once.** One draw with two instances still runs the
vertex shader once per vertex per eye. What it removes is draw-call overhead,
render-target switches and constant-buffer rewrites - and the bisection puts
all of those together at a few milliseconds. It was built to solve a 31 ms
problem and the problem it addresses is 6 ms.

Worth keeping - it is correct, it is free, and it removes real work - but it
was never going to be the answer, and the numbers said so before it was
written.

## RESOLVED: it was the internal render resolution

Metro's **QUALITY** setting bundles an internal resolution scale. At VERY HIGH
the game renders the scene at 1.41x linear - 2x area - and resolves down to the
back buffer. Dropping it to HIGH changed the scene targets from **3620x2009 to
2560x1421**, and the bloom chain from 905x502 to 640x355.

| | VERY HIGH | HIGH |
|---|---|---|
| alternate-eye | 15.4 ms | **13.92 ms** (exactly the 72 Hz budget) |
| true stereo | 45.7 ms | **~16.2 ms** |

This is also the answer to the missing 24 ms above. It was pixel and bandwidth
bound, so it scaled with resolution and was attached to no category of draw at
all - which is precisely why nine bisection phases could not find it. The
lesson is that the phases could only ever test things the wrapper controls, and
the cost lived in something the wrapper merely inherited.

Note the earlier characterisation of the 3620x2009 rendering as "throwing half
the pixels away" was wrong: downsampling 3620 to 2560 IS the supersampling
resolve, so it was genuine anti-aliasing, just extremely expensive. Turning it
off costs some edge quality. At these numbers it is obviously the right trade,
but it is a trade, not free.

Caveats on the numbers above: the 16.2 ms reading was taken with ~300 draws
folding, where the 45.7 ms mess-hall reading was at 400-470, so the worst case
still needs measuring in the mess hall specifically. And 13.92 ms for AER is
exactly at budget with no margin.

Remaining to close 2.3 ms, in order of confidence: the array-slice regression
(ours, ~1.5 ms at the old resolution), the twin clears (1.0 ms at the old
resolution), and single-pass folding, which finally operates at a scale where
it is visible rather than lost in noise.

## Single-pass, measured against its own absence

Slicing and single-pass stand or fall together - one draw can fill two slices
of one resource but never two separate resources - so the fair comparison is
all of the work against none of it. Mess hall, standing still, QUALITY on HIGH:

| | GPU ms |
|---|---|
| array slices + single-pass folding ~335 draws | **16.83** |
| separate twin textures + plain double-draw | **20.1 - 20.6** |

Worth about **3.4 ms, 17%**, comfortably more than the ~1.5 ms slicing costs by
itself. It stays.

Note this only became visible after the resolution dropped. At 3620x2009 the
same change measured as nothing at all, because a 3 ms saving inside a 45 ms
frame that the compositor rounds to 24 fps is invisible. The work was right;
the conditions for seeing it were not.

## The video-settings crash was never about tessellation

Changing TESSELLATION crashed the game, at OFF and at HIGH alike. The log ends
mid-word immediately after the swap chain was recreated and the render targets
re-twinned, which names the real cause: **Metro tears down its D3D device and
builds a new one whenever a video setting changes.** Every cache we keep -
sTextures, sRTVs, sBothRTVs/sBothDSVs, the shader variants, the tracked
constant buffers and their eye1CB - then holds pointers into the destroyed
device, and the allocator hands the new one the same addresses, so a lookup
returns a view belonging to a device that no longer exists.

So it is not a tessellation bug. ANY video setting change could crash; the
QUALITY change earlier survived by luck. `StereoTwin::NoteDevice` now drops
every cached object when the device it was recorded for is replaced.

This matters more for release than for us: players change settings.

## A caution about the alternate-eye figures

AER readings come back as 13.89, 13.91, 13.92 - pinned to three decimals. That
is vsync lock, not cost: AER finishes early and waits for the compositor, so a
Present-to-Present timer reports the 72 Hz interval. AER's true GPU cost is
some unknown amount BELOW 13.9 ms and it has headroom that cannot be measured
this way. "AER costs 13.9 ms" was never a cost figure.

## The lever that has not been pulled

The game renders its scene at **3620x2009** (7.3 Mpix) and the back buffer
handed to the compositor is **2560x1440** (3.7 Mpix). It renders roughly twice
the pixels that ever reach the headset and discards half of them in a
downscale. 3620x2009 is 1.41x linear on 2560x1440 - 2x area, the signature of
2x supersampling - and nothing in `d3dx.ini` sets it (`upscaling = 0`, no
width/height override), so it is the game's own SSAA setting.

Pixel work is what the base frame costs, and half of it is currently wasted.
This is also the prerequisite for the VR menu's resolution option: today the
render resolution and the submitted resolution are DECOUPLED, so every point on
such a slider would inherit the same waste and trade quality for speed far
worse than it should. Couple them first, then expose the matched number.

Arithmetic, if the mismatch is resolved: base frame ~8-9 ms, plus the measured
6.5 ms for a second eye, puts true stereo near 15 ms against a 13.9 ms budget -
with the 24 ms mystery and the game's tessellation still to find. Close enough
that 72 Hz stereo is plausible, which it is not on today's numbers.
