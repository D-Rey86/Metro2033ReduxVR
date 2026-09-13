# Milestone 2: real VR output (compositor submission → stereo) — 2026-07-30

## Where milestone 1 landed

Head *rotation* tracking works. World geometry, most props, NPCs and
level architecture all track correctly via the `cb_main_matrices0`
(per-object `m_WV`/`m_WVP`) and `cb_main_matrices1` (per-frame `m_V`)
constant buffer patches. Yaw and pitch are both confirmed correct in
the headset; roll is still unvalidated.

Known gap carried forward: the weapon viewmodel and player arms do
*not* track, because their view transform is baked into a shared
dynamic per-instance vertex buffer that cannot be patched without
corrupting hundreds of other objects using the same buffer. Fully
diagnosed and deliberately disabled - see Notes/10 Update 6. Revisit
after milestone 2.

## What's still missing for this to be "VR"

Everything so far only changes *what the game renders*. The image is
still a single flat mono frame presented to a normal desktop window,
which the user sees because Virtual Desktop mirrors the desktop into
the headset. That means:

- No stereo depth (both eyes see the identical image).
- No per-eye projection - the game's own FOV/aspect is used, so the
  view doesn't fill the headset correctly.
- Frame timing goes through Virtual Desktop's desktop capture rather
  than the VR compositor, adding latency and skipping reprojection.

## Step 1 (implemented, untested in headset): submit frames to the OpenVR compositor

`VRPose::SubmitFrameToCompositor`, called from
`HackerSwapChain::Present` *before* the real `Present` (the back buffer
only reliably holds the finished frame up to that point):

1. `GetBuffer(0)` the swap chain's back buffer.
2. Lazily create a private `ID3D11Texture2D` matching its
   width/height/format, single-sampled, `D3D11_RESOURCE_MISC_SHARED`.
   The shared flag is what lets the compositor open the texture on its
   own D3D device; swap chain buffers generally aren't shared, so
   submitting them directly is unreliable across drivers. Recreated
   automatically if the back buffer's shape changes.
3. `CopyResource` (or `ResolveSubresource` if the back buffer turns out
   to be MSAA) into it.
4. `Submit` it to `Eye_Left` and `Eye_Right`.

Also changed `VR_Init` from `VRApplication_Background` to
`VRApplication_Scene` - Background apps may query poses but are not
permitted to submit frames. The existing `VR_IsHmdPresent()` guard
still prevents any of this from firing (or from launching SteamVR) when
playing flat.

**This step deliberately submits the same mono image to both eyes.**
There is no stereo separation yet. The point is to establish and
validate the submission path on its own: if the image shows up in the
headset, compositor-timed and reprojected, the plumbing is correct and
everything after it is about *what* we render into that texture.

Expected result when tested: the game appears in the headset as a large
view driven by SteamVR itself rather than a mirrored desktop window,
still flat/mono, still head-tracked via the milestone 1 camera
rotation. Possible issues to watch for: aspect/FOV looking wrong or
stretched (the game renders ~16:9 while each eye is nearer 1:1 - see
step 3), and SteamVR contention if Virtual Desktop is also acting as a
scene app.

## Step 2 (next): stereo separation via alternate-eye rendering

We can't easily make a closed-source engine render its scene twice per
frame from a wrapper. The established workaround - and notably the one
Luke Ross's R.E.A.L. mods use (AER, "alternate eye rendering") - is to
render one eye per frame and hold the other:

- Frame N: offset the patched view matrix by `-IPD/2` along the view's
  right axis, capture the result, submit as the left eye (and re-submit
  the previous right-eye texture).
- Frame N+1: offset by `+IPD/2`, submit as the right eye (and re-submit
  the previous left).

We already own the view matrix in `PatchMappedVRCameraData`, so the
eye offset is a small change there - the real work is holding two
textures and submitting the correct pair each frame.

Trade-off: stereo costs half the effective framerate, and the two eyes
are one frame apart in time, which some people find uncomfortable. It
is, however, proven to work and ship. Worth measuring the game's flat
framerate first to see what we'd be halving.

## Step 3: per-eye projection / FOV

Replace the game's `m_P` with `IVRSystem::GetProjectionMatrix(eye, ...)`
so the rendered frustum actually matches the headset's optics, instead
of the game's own ~16:9 desktop projection. `PatchMappedVRCameraData`
already caches the projection and feeds it into the recomputed
`m_WVP`, so patching it there propagates consistently to all the
geometry we already fix.

Complication worth thinking about before starting: the game renders
into a 16:9-ish target while each eye wants something closer to square.
Options are cropping via `VRTextureBounds_t` on submit, forcing a
different game resolution, or accepting some distortion initially.

## Interlude: the instance-buffer redesign is bigger than "the weapon fix"

First live test of step 1 corrected an assumption from Notes/10 Update 6.
Disabling instance-xform patching was described there as restoring a
state where "props track correctly and only the weapon doesn't." The
live test showed otherwise: with patching fully off
(`patchCalls=0` confirmed in the log for the whole session), **only the
level architecture moves**. Props were only ever tracking *because*
that patching was running, in its intermittent/corrupting form.

So props depend on the shared instance buffer exactly like the weapon
does. The private-copy redesign isn't a weapon-only nicety - it's what
props need too, and it's the single blocker behind most of what still
doesn't track.

**Feasibility of the no-readback design (checked, viable):** the plan
is to keep a CPU-side shadow of the buffer instead of doing a GPU
readback per corrected object (the ~300k-stalls-per-session cost the
user independently flagged). That only works if we can observe every
write the game makes to it. We can:

- Buffer 3461 is `D3D11_USAGE_DYNAMIC` (confirmed live: `Usage=2`).
- D3D11 forbids `UpdateSubresource`, `CopyResource` and
  `CopySubresourceRegion` from targeting a DYNAMIC resource.
- Therefore `Map`/`Unmap` is the only legal way the game can populate
  it - and those are calls `HackerContext` already intercepts. The
  captured frame contains 843 `Map`/`Unmap` pairs, consistent with
  this being the game's normal buffer-filling path.

Worth confirming cheaply at runtime before building on it (count Maps
whose resource matches the tracked xform buffer pointer), but the
deduction is sound: there is no other legal mechanism available to the
game.

Shape of the fix, combining correctness and performance:

1. Shadow the game's writes CPU-side by intercepting its own
   `Map`/`Unmap` of this buffer - no GPU readback, no stalls.
2. Never write into the game's buffer. Allocate a private
   `D3D11_USAGE_DEFAULT` vertex buffer, fill it with corrected data
   (`UpdateSubresource` is legal on DEFAULT), and substitute it in
   `IASetVertexBuffers` for the draws we want corrected.
3. The game's memory is then never touched, so neighbouring objects
   can't be corrupted - which was the root cause in Notes/10 Update 6.

One caveat to measure rather than assume: reading back from
write-combined mapped memory is slow, so if the shadow copy turns out
to be expensive, only copy the ranges that are actually about to be
bound rather than the whole 1MB.

## Step 4 (later): positional tracking

Milestone 1 is rotation-only - `PatchMappedVRCameraData` deliberately
recomputes the translation to keep the camera's world *position* fixed
(`T_new = -R_new * cameraPos`). Feeding real HMD translation in is a
comparatively small change to that same function once stereo is
working, but it interacts with the game's collision/clipping, so it
belongs after the visuals are right.

## Order of work / rationale

Compositor submission first because it's independent of the others and
validates the whole output path with one testable change. Stereo next
because it's the single biggest perceptual difference. FOV after that,
since getting it right is easier to judge once there's real stereo to
look at. Position last.

Performance is a known outstanding issue (the user has flagged it
independently) - milestone 1's per-object GPU readback stalls are gone
now that instance-xform patching is disabled, so it's worth
re-measuring in the headset before optimising anything further.

## Update — residual flicker is most likely temporal accumulation, not culling

Distant AND medium-range flicker persisted after the clip-plane fix. Two
things rule out the earlier "the game culls to its narrower frustum"
theory:

- The game has no FOV setting to widen it with, and
- more decisively, **we never touch the game's camera**. Only the GPU-side
  matrices in the constant buffer are patched, long after the engine has
  done its CPU-side culling - so culling decisions are identical on every
  frame regardless of which eye is being rendered.

Much better fit: Metro Redux uses temporal accumulation (TAA, and
temporally filtered SSAO/shadows), which blends each frame with the
previous one. Under alternate-eye rendering the previous frame is *the
other eye* - a different camera position with a mirrored asymmetric
frustum - so every temporal effect is blending against a history buffer
that does not correspond to the current view. That matches the observed
behaviour: shimmering on fine detail at distance and mid-range, settling
when close where features are large in screen space, and unaffected by
clip planes.

First thing to try (free, no code): disable anti-aliasing and motion blur
in the game's video settings. A sharp drop in flicker confirms it.

If confirmed and the settings can't fully disable it, options are to
neutralise the temporal history from our side, or to stop alternating
eyes - the underlying conflict is inherent to alternate-eye rendering
interacting with frame-to-frame history, and it is the same root cause as
the residual NPC doubling.

### Follow-up: motion blur off did not help, and there is no AA setting

Tested. Motion blur disabled made no difference, and the game exposes no
anti-aliasing option to turn off. This weakens the temporal theory but
does not refute it: motion blur is a different mechanism from TAA and
temporally-filtered AO, and Metro Redux applies temporal filtering
internally where there is no user-facing switch for it.

**Decisive experiment to run first next session** - stop alternating eyes
and render the same eye every frame (skip the `VRPose::AdvanceStereoEye`
call in `HackerSwapChain::Present`, and submit the one fresh texture to
both eyes). That sacrifices stereo for the duration of the test, but it
is a one-line change and it splits the problem cleanly:

- **Flicker disappears** -> it is caused by alternating eyes, i.e. the
  frame-to-frame history mismatch. Same root cause as the residual NPC
  doubling, and the fix has to address the alternation itself rather
  than chase individual effects.
- **Flicker persists** -> the alternation is innocent and something
  about our projection substitution is at fault. Next step there would
  be reverting to the game's own projection while keeping stereo, to
  see whether the flicker follows the projection or the eye offset.

Either answer eliminates half the search space, which is worth more than
another round of guessing at settings.

## Flicker/pop-in: SOLVED 2026-08-09 (see section below for the answer)

## Flicker/pop-in: state of the investigation (historical)

Confirmed facts:
- **Only occurs with the mod.** Running stock (DLL renamed away) shows no
  flicker and no pop-in in the same spot. So this is ours.
- **Fires at a consistent distance** every time, and affects both flicker
  and pop-in, which suggests one threshold rather than two problems.
- Settles once within roughly 5-6 feet in game.

Hypotheses tested and ELIMINATED, each by measurement rather than argument:
1. **Far-plane clipping from frozen clip planes** - fixed (rebuild whenever
   the game changes them); no change.
2. **Temporal accumulation across alternating eyes** - disabled eye
   alternation entirely; flicker persisted. Also proves the flicker and the
   residual NPC doubling are separate problems.
3. **Stale global cached projection** - the diagnostic confirmed the game
   really does use several projections per frame (~220 writes, a handful
   with far=100 rather than 250), but excluding the minority ones caused
   visible doubling, proving those passes draw real scene geometry, and the
   flicker was unaffected.
4. **Per-object depth mismatch** - recovered each object's own depth mapping
   (E,F) from the m_WVP the game already wrote for it, so depth always
   matches the object's own pass. No change.
5. **Wider FOV breaking the engine's pixel-counting occlusion queries** -
   the game does use GPU queries (CreateQuery/Begin/End in the frame
   census), and a wider frustum makes objects cover fewer pixels, which
   would explain pop-in as well as flicker. Rendered with the game's own
   projection while keeping the correction fold; flicker persisted.
   CAVEAT: that run also showed doubling, so the test was not clean and
   this one deserves re-testing before being fully written off.

Next step when picking this up - stop theorising and observe directly.
RenderDoc cannot attach to an already-running process (it must hook before
device creation, which is why the exe never appears in its list), and
removing our DLL to let it hook at launch also removes the flicker. So use
**3Dmigoto's own frame analysis** (FrameAnalysis.cpp, driven from the
[Hunting] section of d3dx.ini) - it is already inside the process and works
with the mod active. Dump two consecutive frames while the flicker is
visible and diff the draws for one flickering object. That shows what
actually differs frame to frame instead of guessing.

Reverted to the last known-good build for now.

---

# Pop-in / flicker: ROOT CAUSE AND FIX (2026-08-09)

## The cause

Metro runs its **own CPU-side software occlusion culling** at
`metro.exe+0x826B50`. That routine:

- projects an object's bounding box into screen space,
- tests it against an engine-maintained CPU depth/visibility buffer,
- returns "hidden" when it believes the object is occluded,
- and so the object **never reaches D3D11 at all**.

In VR that buffer still represents the game's original narrow view, not the
much wider headset view. Objects plainly visible through the headset were
rejected against a buffer that does not describe what the player can see,
which is what produced the angle- and distance-dependent pop-in.

## The fix

Patch the routine to return "visible" unconditionally:

    mov eax, 1
    ret

Implemented as `InstallCpuScreenOcclusionBypass()` in VRPose.cpp. It
signature-checks the prologue before writing, so a game update makes it
fail safe and log rather than corrupt an unrelated function. All six engine
callers treat nonzero as visible, so patching the shared routine covers
every render-list category at once.

It touches no camera, transform or input state - which is why it does not
disturb the menu, movement, weapon placement, ballistics or shadows, unlike
the camera-based approaches tried earlier.

## Why every earlier attempt failed

**The decisive point: our vantage point is a D3D11 wrapper, so we can only
observe what reaches D3D.** This culling happens upstream of submission, so
every measurement available to us was downstream of the decision and blind
to it.

The specific trap: draws-per-frame was steady (~1650-1900), and that was
read as "nothing is being removed". A count that is steady *but permanently
reduced* looks identical to one that is complete, because there was no
baseline to compare against. That reading is what sent the investigation
into frustum geometry for hours.

## Eliminated by measurement (do not re-derive)

| Hypothesis | How it was eliminated |
|---|---|
| Frozen/stale clip planes | Rebuilt on change. No effect. |
| Temporal accumulation across eyes | Alternation disabled. Persisted. |
| Stale global cached projection | Excluding minority projections caused doubling; flicker unaffected. |
| Per-object depth mismatch | Recovered each object's own depth mapping. No effect. |
| **GPU occlusion queries** | 70,000 `D3D11_QUERY_OCCLUSION` readbacks intercepted and forced to "fully visible". No change, no framerate cost. Engine uses **zero predication** (`SetPredication` called 0 times), so there is no second GPU path. |
| Gross frustum culling | Draw counts steady. *(Weak - see trap above.)* |
| LOD mesh swapping | Per-frame index-count churn 0-4 of ~365 meshes. |
| Light sleep (`r_light_frames2sleep`) | Set 10 -> 0, verified persisted. No change. |
| Culling frustum **width** | `r_base_fov` clamps at 121.9 deg h / 90 deg v; 160 and 220 identical. No change. |
| Culling frustum **aim** | Rendered view vs engine camera measured directly in world space: **0.3-6.5 deg**. Culling was aimed correctly all along. |
| Aim injection steering the camera at the gun | Swinging the gun with the head still does not change what renders. |
| Portal/sector visibility | 38,256 calls with every sector forced visible produced no visual change. |
| Twin pass / stereo mode | Identical in true stereo and alternate-eye. |

## Fixed along the way (real bugs, not the cause)

- **Culling follow lag.** The follow was clamped to `kMaxStep = 40`
  (~3.6 deg/frame, ~260 deg/sec) - below head-turn speed, so it saturated
  and fell **40-52 degrees** behind during turns, collapsing to 0.6 deg at
  rest. Raised to 250 (~22 deg/frame); worst error now 2-4 deg. Sampling it
  every 600 frames had always caught it at rest and reported it healthy.
- **`m_iP` was not the inverse of `m_P`.** The forward projection included
  the view correction, the inverse did not. In a deferred renderer that
  corrupts position reconstruction from depth. Now inverted from the
  corrected matrix directly, so the two cannot drift apart.

## Watch item

The bypassed routine existed to save CPU work. Every object it used to
reject is now submitted, so **framerate in large open areas** is the thing
to watch. If it costs too much, the refinement is to make the occluder's
buffer represent the VR view rather than to disable the test - but there is
no reason to do that work unless it measurably bites.
