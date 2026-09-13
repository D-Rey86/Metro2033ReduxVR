# Weapon Viewmodel Vertex-Buffer Fix Implemented — 2026-07-29

## Identifying the weapon draw

Used `Tools/identify_weapon_draws.py` against the existing RenderDoc
capture to pull render-target thumbnails + draw metadata for the
candidate eids from Notes/07/08 (96, 205, 207, 213):

- **eid=96**: numIndices=336, numInstances=1. Thumbnail (a normals/G-buffer
  render, false-colored) clearly shows a weapon silhouette (barrel/stock
  shape) in the lower-left, matching first-person weapon framing.
  **Confirmed weapon geometry.**
- eid=205: numIndices=48, numInstances=9 - tiny mesh, heavily instanced.
  Almost certainly debris/casings, not the weapon.
- eid=207: numIndices=48, numInstances=1 - same tiny mesh as 205, single
  instance.
- eid=213: numIndices=7548, numInstances=2 - thumbnail shows only wall/
  fence geometry, no weapon visible. Structural prop, not the weapon.

## The actual buffer layout (via `Tools/inspect_weapon_vbuffers.py`)

For eid=96's vertex input state:

- **IA slot 0** (resource 5198, stride 32): the ordinary static mesh
  buffer - POSITION, NORMAL, TANGENT, BINORMAL, BONES, WEIGHTS, TEXCOORD.
  Not touched.
- **IA slot 1** (resource 3461, byteOffset=128, stride=64): a **separate,
  small per-instance buffer** - `xform0`/`xform1`/`xform2`/`xform3`,
  `perInstance=True`, `instanceRate=1`.

Cross-referencing against the shader disassembly (Notes/07's finding that
`v7`/`v8`/`v9` are dotted against the local position, then `m_P` applied):
input signature declares `v7.xyzw, v8.xyzw, v9.xyzw, v10.w` - meaning only
3 of the 4 xform rows (`xform0`/`1`/`2`, the first 48 bytes of each 64-byte
block) form the actual local-to-view 3x4 matrix; `xform3` (last 16 bytes)
is barely touched (only its `.w` used, for something unrelated to the
transform - alpha/fade/instance flag, not investigated further since it
doesn't need patching).

This confirms it's a **separate, small, presumably-dynamic buffer** -
not baked into the giant static level-geometry pool - so the same Map/
Unmap technique already built for cb0/cb1 should apply here too.

## The fix

Rather than trying to recover the object's un-viewed "local" transform in
isolation (which we don't have - the baked matrix mixes it with the OLD
view irreversibly unless we explicitly divide it back out), compute a
single **correction transform** once per camera-buffer patch:

```
Correction = V_new * V_old^-1
```

Left-multiplying this onto any already-baked `local-to-OLD-view` matrix
yields `local-to-NEW-view` directly:
`Correction * (V_old * Local) = V_new * V_old^-1 * V_old * Local = V_new * Local`.

Implementation (`HackerContext.cpp`):
- `PatchMappedVRCameraData` (cb1 patch) now also computes
  `G->vrViewCorrection3x4` alongside the existing `vrCachedView3x4`/
  `vrCachedProjection4x4` - translation is identical between V_old/V_new
  (we never touch it), so the inverse-of-old-view computation only needs
  `origR`/`T`, already on hand.
- New `IASetVertexBuffers` hook (previously untouched passthrough) tracks
  whatever buffer is bound at IA slot 1 with exactly a 64-byte stride
  (`sTrackedVRInstanceXformVB`) - stride is part of the identification
  since slot 1 gets reused for unrelated vertex data by other draws.
- New `HackerContext::PatchMappedVRInstanceXformData(mappedData, byteWidth)`:
  on Unmap, walks every 64-byte block in the mapped range and left-
  multiplies `vrViewCorrection3x4` onto the first 48 bytes (the matrix),
  leaving the last 16 untouched. Applied uniformly across the whole mapped
  buffer rather than trying to identify individual objects' byte ranges -
  every block follows the same baking convention, and correcting stale/
  unused blocks is harmless.

## Status

Builds clean (0 errors, full clean rebuild). Load-checked without a
headset (SteamVR not confirmed running during this pass) - process starts
and runs without crashing, consistent with the code being correctly gated
behind `vrHeadRotationValid`/`vrCachedViewValid` for pure-passthrough
behavior when no VR pose is available.

**Not yet visually verified** - needs the real in-headset test (task 5/6)
to confirm the weapon now moves consistently with the world instead of
staying fixed. Two things likely to need iteration once tested:
1. The `Correction` derivation assumes the same row-major/column-vector
   convention established for cb0/cb1 - should be consistent, but this is
   the first time it's been applied to a *third* data source, worth
   double-checking if the weapon moves in a sensible way at all (vs.
   corrupted/exploded geometry, which would indicate a math error here).
2. Whether IA slot 1 + 64-byte stride is a reliable-enough fingerprint in
   practice, or whether some other unrelated draw also happens to bind a
   64-byte-stride buffer at slot 1 and gets incorrectly patched. Watch for
   any *other* geometry breaking/glitching once this is tested live.

## Update 1 — 2026-07-29: first live test broke geometry, root cause found and fixed

First live test with the above (`PatchMappedVRInstanceXformData`, blindly
correcting every 64-byte block across the whole mapped buffer on Unmap)
produced visible corruption - streaking/exploded triangles across the
whole view (user-supplied screenshot confirmed this directly). Immediately
disabled (`kInstanceXformFixEnabled = false`) to restore the previous
working state, then diagnosed with non-destructive logging before trying
again.

**Root cause**, confirmed via diagnostic logging
(`Tools/identify_weapon_draws.py`-adjacent live logging, not a separate
script this time - logged directly from `IASetVertexBuffers`/`Unmap`):
the per-instance "xform" buffer is a **1MB shared pool** (16384 64-byte
slots, confirmed `ByteWidth=1048576`), bound with **widely varying
offsets** (0, 64, 192, 320, ... 6912+ observed across ~30 binds in a few
seconds) - i.e. many different objects share this one buffer, not just
the weapon. The old code corrected *all 16384 blocks* on *every* Unmap,
and Unmap fired 10+ times in the same short window. Since nothing
distinguished "already-corrected-by-us" data from fresh game data, each
pass re-applied the correction on top of the last one - the rotation
compounded and drifted further off with every Unmap. That's exactly what
runaway matrix drift looks like on screen.

**Fix**: abandoned the "patch the whole buffer on Unmap" approach
entirely. New design, entirely in `IASetVertexBuffers` (no more Map/Unmap
involvement for this buffer):
- `HackerContext::PatchInstanceXformAtOffset(buf, offset)`: for the
  *specific* offset a bind actually uses, reads back just those 48 bytes
  via a small (48-byte) STAGING buffer (`CopySubresourceRegion` +
  `Map(READ)` - `GetOrCreateVRInstanceXformReadbackStaging` on
  `HackerDevice`, same pattern as earlier removed staging helpers),
  corrects via `Multiply3x4Affine`, writes back with a boxed
  `UpdateSubresource` (unlike constant buffers, regular/vertex buffers
  don't have the "box must be NULL" restriction, so no need for
  `UpdateSubresource1`/D3D11.1 partial-update capability here).
- `sPatchedInstanceXformOffsets` (a `std::unordered_set<UINT>`): tracks
  which offsets have already been corrected *this frame*, so rebinding
  the same slot multiple times (e.g. across render passes) doesn't
  double-apply. Cleared once per frame via
  `HackerContext::ResetVRInstanceXformFrameTracking()`, called from
  `HackerSwapChain::Present` alongside the existing `VRPose::UpdateVRPose()`
  call.
- This does mean a GPU stall (`CopySubresourceRegion`+`Map(READ)`) per
  *unique* offset per frame, not just once like cb0/cb1 - acceptable for
  a correctness-first milestone, flagged as a perf item to revisit if it
  turns out to matter.

Rebuilt clean, deployed, crash-free load-checked without a headset.

**Second live test** (still same day): no more corruption, but the weapon
still didn't visibly move with the world - "nothing moving except the
building" per the user, with the building itself appearing to end up
oddly positioned (possibly just a session-specific reference-orientation
artifact of the "first valid pose = zero" design, not necessarily a new
bug - no explicit recenter exists yet). Added full diagnostic logging
(entry/skip/apply logging in `PatchInstanceXformAtOffset`, match-confirm
logging in `IASetVertexBuffers`, call-count logging in
`ResetVRInstanceXformFrameTracking`) *without* disabling the actual patch
this time, since it's no longer corrupting anything - just seemingly
having no effect. Rebuilt, deployed, crash-free load-checked. **Not yet
retested live** - next session should check the log for:
1. Does `IASetVertexBuffers` ever actually match (confirms the slot=1/
   stride=64 identification still holds against live gameplay, not just
   the one capture we based it on).
2. Does `PatchInstanceXformAtOffset` get reached, and does it skip due to
   `vrCachedViewValid` being false (would mean cb1 hasn't been patched
   yet when this fires) or due to already-patched-this-frame (would mean
   the reset isn't working as expected)?
3. If it applies, do the logged `current`/`patched` values look sane (unit
   -length rows, plausible translation) - would confirm the mechanism
   works but something else (maybe the axis-mapping/reference-orientation

## Update 2 — 2026-07-29: yaw axis fix confirmed correct; found and fixed a translation bug

Third live test (identification + full logging active, write still
enabled): confirmed `IASetVertexBuffers` matches correctly and
`PatchInstanceXformAtOffset` runs and applies to many offsets each frame -
mechanism is being reached fine. But the user reported the world moving
the *same* direction as head turn (turn left -> world slides left), which
is backwards - correct behavior is the opposite (turning left should
reveal what's to your left, meaning the previous view slides right).

This pointed at the OpenVR->game-space axis conversion
(`VRPose::ConvertOpenVRRotationToGameSpace`, previously an untested
identity copy) rather than the patching mechanism. Fixed via a coordinate-
handedness conjugation `R' = S*R*S` with `S=diag(-1,1,1)`, which inverts
yaw (X-Z plane rotation) while leaving pitch unchanged (roll also inverts
as a side effect of this specific single-axis flip, not yet confirmed
right or wrong). **User confirmed this fixed the direction** - world now
moves correctly opposite to head turn.

Weapon still didn't move after this fix. Reviewing the diagnostic log
data (from `PatchInstanceXformAtOffset`'s `current=[...]`/`patched=[...]`
dumps) surfaced a second, unrelated bug: translation values were jumping
by huge amounts after "correction" (e.g. one offset's translation went
from `(-0.035,-0.021,-0.009)` to `(17.286,-8.623,3.597)`). Root cause: a
view matrix's translation is *not* independent of its rotation -
`T = -R * camera_position` - so keeping the game's original `T` unchanged
while swapping in a new `R` silently implies a different (wrong) camera
position, since that `T` was only ever valid paired with the *original*
`R`. World geometry absorbed this error well enough to look plausible
(large-scale level geometry hides a few units of drift), but it would
have been glaring for anything small and close to the camera - like the
weapon.

Fixed in `PatchMappedVRCameraData`: recover the camera's actual world
position from the original (un-patched) view matrix first
(`cameraPos = -origR^T * origT`, computed via the existing
`ComputeRigidInverse` helper), then compute the new translation as
`T_new = -R_new * cameraPos` - correctly keeping camera *position* fixed
(as intended for a rotation-only milestone) while being consistent with
the new rotation, instead of just leaving the old T in place. This also
required threading `cameraPos` through to the `vrViewCorrection3x4`
computation (previously recomputed `V_old^-1` a second time with the
same, now-fixed inputs - simplified to reuse the values already computed
for the m_V/m_iV write).

Rebuilt, deployed, crash-free load-checked. **Fourth live test**: log data
now sane (translation stays near original magnitude instead of jumping by
double digits). Yaw direction still correct per the earlier fix. Weapon
*still* didn't visibly move, though the user reported the world now
"looks a little better."

Added one more diagnostic before the next test: `DrawIndexed` now logs
whenever `IndexCount==336` (the confirmed weapon mesh's exact index count
from the RenderDoc capture, see Notes/09 above) alongside whatever xform
buffer offset was most recently bound (`sLastBoundXformOffset`) - this
will tell us definitively *which* offset is the weapon's in a live
session, rather than guessing across a dozen logged candidates. Not yet
tested live - next session should check for a
`DrawIndexed IndexCount=336 (weapon mesh) - last bound xform offset=N`
log line, then find that specific offset's `PatchInstanceXformAtOffset`
entry and sanity-check its `current`/`patched` values directly.

Possible remaining explanations if the weapon-specific offset's data also
looks sane and it *still* doesn't move visually:
- The weapon may be rendered via a genuinely different camera/view-space
  setup than the world (common technique: a dedicated near-origin "weapon
  view space" camera, distinct from the world camera's actual position) -
  if the weapon pass's own cb1 update uses a different reference frame
  than what `vrViewCorrection3x4` was computed from, the correction
  wouldn't transfer correctly even though the math is individually sound.
- Multiple draw calls might share the mesh's IndexCount=336 signature
  without all being the actual first-person weapon (e.g. a holstered/
  world-space copy of the same weapon model) - would need the offset
  correlation to disambiguate, or a fresh RenderDoc capture of this exact
  session if the live logging isn't conclusive enough.
   issue already flagged in `VRPose.cpp`) is masking the visual result.

## Update 3 — 2026-07-29: `IndexCount==336` identification was wrong from the start

Pitch fix deployed (`VRPose.cpp` extended to `S=diag(-1,-1,1)`), plus a
sharper diagnostic: `PatchInstanceXformAtOffset` now always caches its
most recent current/patched matrices (not throttled), and `DrawIndexed`
logs them specifically when it sees `IndexCount==336`, tagged with the
translation's distance from the origin (the buffer stores a
local-to-view matrix, so anything genuinely close to the camera - the
weapon - should have a small translation; distant background objects
should not).

Live test: gun still didn't move, and pitch direction unconfirmed
(user didn't report on it this round). But the log revealed the real
problem - `IndexCount==336` draws had translations ranging from 0.03 to
135 units, wildly inconsistent with all being the same on-screen object.
One specific signature recurred identically across many different ring-
buffer offsets (`current=[-0.990 -0.029 0.141 -0.797 / 0.011 -0.991
-0.132 -0.055 / 0.144 -0.129 0.981 -0.031]`, dist=0.80) - stable and
close to origin, looked like a strong weapon candidate.

**Stress test** (same technique as the earlier 180-degree camera
override that isolated the cb1 dead-end in Notes/07): added a forced
+200 unit shove to the translation of any `IndexCount==336` draw with
distance-from-origin < 10, on top of the normal correction. If this is
really the weapon buffer, the gun should fly off-screen/into geometry -
unmissable either way. **Result: zero visible change.** This is
conclusive - the buffer being patched at this offset is not what
determines the weapon's on-screen position, regardless of what we write
to it.

**Root cause, confirmed via a fresh static analysis of the existing
capture** (`Tools/identify_336_buffers.py`, replacing the old
`identify_weapon_draws.py`): searched the *entire* capture (not just
the 4 pre-selected eids from Update 0) for every draw with
`numIndices==336` and found only 6 total. Of those, 5 (`eid=96, 1583,
1691, 1862, 1990`) share the *exact same* `indexOffset`/`baseVertex`
into the shared 92MB static-geometry buffer - meaning they are
**literally the same mesh**, drawn 5 times at different locations in
one frame. `eid=96`'s own thumbnail (re-saved and visually inspected)
shows a full-body NPC standing in a room with a chain-link fence in the
background - not a first-person weapon view at all. `eid=96` was
misidentified back in the original pass; the "weapon silhouette" seen
in the thumbnail was very likely a prop elsewhere in that same frame
(possibly a gun being held by the NPC, or lying nearby), not the
player's own viewmodel.

**Conclusion**: every offset we've patched under the "weapon" label
this whole time was never the actual first-person gun. The instance-
xform mechanism, correction math, and write pipeline may all be correct
- we have simply never patched the right object. `kForceWeaponOffsetStressTest`
has been reverted (it would otherwise visibly corrupt whatever unrelated
near-camera object happens to match during normal play). The
`NEAR-ORIGIN CANDIDATE` tagging in `DrawIndexed` is left in place since
it's informative and harmless.

**Next step (needs the user in-game, not solvable from the existing
capture)**: take a *fresh* RenderDoc capture during live gameplay with
the weapon actually visible on screen, then use RenderDoc's pixel-
history feature (right-click the on-screen gun pixel -> "Debug pixel" /
history) to get the *exact* draw call responsible, rather than
inferring from index count or thumbnails. Requires the established
workaround of temporarily removing 3Dmigoto's `d3d11.dll` so RenderDoc
can hook cleanly (they conflict), capturing a frame while the gun is on
screen (flatscreen is fine, no headset needed for identification), then
restoring 3Dmigoto's DLL afterward. Once the real draw call is known,
re-check whether it even uses this same "1MB shared xform pool at IA
slot 1" mechanism at all, or something else entirely (a dedicated per-
weapon constant buffer, a different vertex stream, etc.) - Notes/07
already found that different shader families in this engine bake view
data in genuinely different ways (precomputed cb0 WVP vs. per-instance
vertex stream vs. this xform pool), so the weapon may turn out to use a
fourth mechanism not yet seen.

## Update 4 — 2026-07-29: live RenderDoc pixel history finds the real weapon draws, and the real bug

Did the fresh live capture from Update 3's plan: launched the game
flatscreen with `rd.ExecuteAndInject` (3Dmigoto's `d3d11.dll` moved
aside first - they still conflict), got the gun on screen, captured via
the in-game hotkey. Ran `Tools/frame_timeline.py` to map render-target
changes across the frame and confirm which target holds actual 3D
geometry before post-processing (`2D Render Target 5334`, populated
roughly eid 2817-3618, before the post-process chain's fullscreen-
triangle passes take over). Ran `Tools/weapon_pixel_history.py`
(RenderDoc's `PixelHistory` API) against a pixel squarely on the gun's
receiver in that target - much more direct than inferring from index
counts or thumbnails.

**Result**: every real geometry draw touching that pixel - regardless
of mesh (`5202`/`5204`) or index count (**480 or 540, never 336**) -
bound the exact same per-instance xform offset (`28480` in this
capture) at IA slot 1. `IndexCount==336` was never the right signature;
480/540 is. This also matches Update 3's finding that eid=96 (336
indices) was a misidentified NPC/prop draw entirely.

Also dumped VS-stage constant buffers for these draws and found they
*also* bind `cb_main_matrices0` (same 208-byte per-object WVP buffer
already patched by `PatchMappedVRObjectData` for ordinary world
geometry) at slot 0 - raising the question of whether cb0 or the
instance-xform buffer actually determines the weapon's final position.
Redeployed the existing (unmodified) build to test cb0's effect in
isolation - **gun still didn't move**, which by itself was ambiguous
between "cb0 isn't what positions it" and "cb0 positions it correctly
but something else undoes it."

**The real fix**: reasoned that if misidentification were the whole
story, a UNIVERSAL stress test (shove every single instance-xform
offset by +200 units, no filtering by distance or index count at all)
should make *something* visibly break, since the shared pool covers
many world objects too, not just the weapon. Deployed it - **result:
literally nothing changed anywhere in the scene**, gun or otherwise.
That ruled out misidentification as the (sole) problem and pointed
straight at the write itself never reaching the renderer.

Root cause: `PatchInstanceXformAtOffset` read the buffer correctly (via
a staging-buffer `CopySubresourceRegion`/`Map(READ)`, unaffected) but
wrote back with `UpdateSubresource` - which is **illegal on
`D3D11_USAGE_DYNAMIC` resources** per the D3D11 spec. This buffer's own
usage pattern (CPU writes new instance data at ever-growing offsets
across hundreds of draws without ever being reset - confirmed live via
a one-time diagnostic log: `Usage=2` i.e. `DYNAMIC`) is the textbook
ring-buffer pattern, which requires `DYNAMIC` + `Map(WRITE_NO_OVERWRITE)`
to partially update, not `UpdateSubresource` (`DEFAULT`-usage only).
Without the debug layer active, an illegal `UpdateSubresource` call on
a `DYNAMIC` resource silently no-ops instead of erroring - exactly
matching the "write computes correctly, log shows it, nothing visibly
changes, even under a maximally obvious stress test" symptom that took
several rounds to pin down.

**Fix**: replaced the `UpdateSubresource` write with
`Map(buf, 0, D3D11_MAP_WRITE_NO_OVERWRITE, 0, &mapped)` +
`memcpy` into `mapped.pData + offset` + `Unmap` - the D3D11-legal way
to patch a sub-range of a `DYNAMIC` buffer without discarding the rest
of the ring (matches how the game itself must be writing to this same
buffer). Removed the now-superseded blanket stress test in the same
change. Rebuilt, deployed.

**Live test: confirmed working.** User: "The gun (and arms) and a lot
more things are moving now. Some things still aren't, but most things
are." First real motion on the weapon since this whole investigation
began. Whatever's still not moving is presumably geometry using a
mechanism this project hasn't covered yet (a fourth path, per Notes/07's
finding that different shader families bake view data differently) -
next step is identifying which specific things are still static and
whether they're worth chasing for milestone 1, or acceptable to leave
for later polish.
