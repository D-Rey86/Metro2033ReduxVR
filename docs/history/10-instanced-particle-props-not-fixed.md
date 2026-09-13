# Some props still don't track head rotation: a fourth, unexplored rendering path — 2026-07-29

## Context

After the Map(WRITE_NO_OVERWRITE) fix (Notes/09 Update 4), most of the
scene - including the weapon and player's own arms - started correctly
responding to head rotation for the first time in this project. But the
user reported inconsistent results on a live headset test: many props
now move correctly, some don't, and *the same physical object* was
observed moving when viewed from one spot and not moving from another
(screenshots supplied showing circled static objects across several
different rooms - ceiling lamps, wall signs/posters, ammo crates, ceiling
scaffolding, ropes, and notably a chain-link fence panel that moved from
one player position but not another).

## Investigation

Standard live-log diagnostics (index-count/offset correlation, as used
for the weapon) proved unreliable here - repeat pixel-history queries
against different-looking screen locations kept returning the same
mesh/offset signature, indicating the mesh-identity heuristic
(`5202`/`5204`, 480/540 indices) that worked for the weapon is not a
reliable fingerprint in general - it's likely a small set of generic,
widely-reused low-poly meshes, not something unique to any one object
(the same lesson already learned once with `IndexCount==336` in
Notes/09).

Given the limits of blind screenshot-coordinate pixel history without
live visual feedback, switched to having the user drive RenderDoc's own
interactive UI directly (`qrenderdoc.exe`, opened on
`GameReferences/fence_id_live_frame3708.rdc` - a fresh flatscreen
capture taken specifically to investigate a non-moving fence panel in
the gunsmith/armory room). This was far more effective - within a couple
of steps the user found and correctly identified the fence's actual
draw call.

## Finding: fences (and likely other repeating/decorative geometry) use the particle-sprite system, not the instance-xform buffer

The fence's draw call is `DrawIndexedInstanced(6, 45)` - a single 6-index
quad (2 triangles) stamped 45 times via GPU hardware instancing
(`SV_InstanceID`-driven), a completely different mechanism from the
weapon's per-object `DrawIndexed` calls. Its Vertex Shader stage binds
**four** constant buffers (via RenderDoc's Pipeline State panel):

- **Slot 0: `cb_main_matrices0`** → Buffer 80 (208 bytes) - the buffer we
  already patch via `PatchMappedVRObjectData`. Presumably supplies the
  overall panel's base world transform.
- **Slot 4: `cb_misc_1`** → Buffer 84 (128 bytes) - **not examined, not
  patched.**
- **Slot 5: `cb_surface`** → Buffer 85 (64 bytes) - **not examined, not
  patched.**
- **Slot 10: `cb_particles`** → Buffer 90 (112 bytes) - **not examined,
  not patched.**

The `cb_particles` name strongly suggests this is the engine's
particle/instanced-sprite renderer (used for dust, sparks, foliage,
etc. in the normal course of the game) repurposed here to stamp a
repeating chain-link texture across a grid of instances. The per-tile
placement (each of the 45 instances' individual position within the
panel) is almost certainly computed from `cb_particles`/`cb_surface`
data combined with `SV_InstanceID` in the shader, entirely independent
of the already-patched `cb_main_matrices0`. Patching cb0 alone likely
only moves whatever base/anchor transform the whole instanced batch
inherits, not the individual tile positions - consistent with the
observed "moves from some angles, not others, sometimes looks doubled"
symptom (different draws/instances responding inconsistently depending
on which ones happen to be visible and how their positions compose).

This is very likely why several of the still-static objects in the
user's screenshots are exactly the kind of thing this rendering
technique suits: ceiling lamp light shafts, hanging ropes/cloth,
wall-mounted paper/posters, chain-link fencing - decorative/repeating
detail geometry, as opposed to large hero props like the weapon.

## Status: not fixed, follow-up scoped for a future session

This is a genuinely new, fourth rendering path (in addition to: (1)
precomputed cb0 WVP for plain static geometry, (2) per-instance vertex
stream data for one shader family per Notes/07, (3) the instance-xform
buffer fixed in Notes/09) - not a quick extension of tonight's fix.
Reverse-engineering `cb_misc_1`/`cb_surface`/`cb_particles`'s actual
layout (most likely via vertex shader disassembly against
`Vertex Shader 5116`/`Pixel Shader 5117`, similar to the approach used
in Notes/07 for the camera buffers) is the natural next step, but was
explicitly deferred - this session already delivered a major, confirmed
fix (Notes/09 Update 4) and the user opted to stop here rather than
open a new deep investigation tonight.

## Update 1 — 2026-07-29: disassembly shows the fence didn't need a new fix - it needed us to stop breaking it

Got the full vertex shader disassembly for `Vertex Shader 5116` (the
fence's VS, `Tools/inspect_fence_shader.py` against
`GameReferences/fence_id_live_frame3708.rdc`, following the same
approach as `Tools/inspect_vs_shader.py` from Notes/07). Full input
signature and constant block layout also captured (see
`GameReferences/fence_shader_dump.txt`).

**The key finding**: the `inst0..inst7` per-instance data (from the
same shared buffer we've been correcting, IA slot 1, 64-byte stride) is
used *only* to compute a local-space billboard corner offset - a whole
lot of vector math involving `particles_T`/`particles_R`/`particles_U`/
`particles_angle`/`eye_position` to orient and size each of the 45
tiles. That local offset is added to a local base position (itself
partly derived from `inst1`), producing a purely local-space vertex
position (`r1.xyz`). **Only then**, at the very end of the shader, is
that local position transformed to clip space via `m_WVP` -
`cb_main_matrices0`, the exact per-object buffer
`PatchMappedVRObjectData` already recomputes for every draw, fence
included:

```
188: dp4 o0.x, m_WVP[0].xyzw, r1.xyzw
189: dp4 o0.y, m_WVP[1].xyzw, r1.xyzw
190: dp4 r0.w, m_WVP[2].xyzw, r1.xyzw
191: dp4 o0.w, m_WVP[3].xyzw, r1.xyzw
```

The `inst0..inst7` data is **never** combined with any view matrix in
this shader - it has nothing to do with the camera at all. It's real
particle placement/orientation data, consumed as-is.

This flips the whole hypothesis from Notes/09/10: the fence's overall
position was *already* correctly responding to head rotation the whole
time, via the same cb0 mechanism that fixed the weapon. The actual bug
is that `PatchInstanceXformAtOffset` was unconditionally treating the
first 48 bytes of *any* IA-slot-1/64-byte-stride buffer as a "local-to-
view" 3x4 matrix and overwriting it with a rotation-corrected version -
including this buffer's `inst0`/`inst1`, which are real position/
orientation floats a completely different shader actually depends on.
We weren't failing to fix the fence; we were actively corrupting its
per-instance data on top of an already-correct base transform, which
lines up much better with the reported symptoms (inconsistent,
angle-dependent, occasionally doubled) than "nothing happens" would.

**Fix**: distinguish the weapon's `xform0..xform3` layout (a genuine
matrix, 4 float4 elements at IA slot 1) from anything else sharing the
same slot+stride signature, and only apply the correction for the
former.

- `HackerDevice::CreateInputLayout` (`HackerDevice.cpp`) now
  unconditionally (not gated by `G->hunting` like the existing frame-
  analysis blob-caching below it) scans `pInputElementDescs` for any
  IA-slot-1 element whose semantic name starts with `"xform"`, and if
  found, registers that `ID3D11InputLayout*` via the new
  `VRPose::RegisterKnownWeaponXformInputLayout`.
- `VRPose.h`/`.cpp` gained a small opaque `void*`-keyed
  registry (`RegisterKnownWeaponXformInputLayout` /
  `IsKnownWeaponXformInputLayout`) - kept `void*`-typed rather than
  `ID3D11InputLayout*` to avoid a `d3d11.h` dependency in `VRPose.h`.
- `HackerContext::IASetInputLayout` (previously an untouched
  passthrough) now records the bound layout in a new file-scope static,
  `sCurrentInputLayout`.
- `HackerContext::IASetVertexBuffers`'s existing slot-1/stride-64 match
  now additionally requires
  `VRPose::IsKnownWeaponXformInputLayout(sCurrentInputLayout)` before
  calling `PatchInstanceXformAtOffset` - anything else (the particle
  system, and any other future shader family sharing this signature) is
  now left completely untouched, exactly as it should have been from
  the start.

Rebuilt clean (0 errors), deployed, autonomous crash-free load-check
performed (launched directly, confirmed process still running after
~25s, closed) since the user had stepped away. **Not yet confirmed in
headset** - next session should verify: (1) the fence (and other
previously-inconsistent props) now hold still relative to the world
exactly like static geometry should (no more corruption/doubling), and
(2) the weapon and arms still move correctly (regression check - the
weapon's own layout must still be correctly recognized as
`xform0..xform3`).

## Update 2 — 2026-07-29: live test found a real regression, root-caused and fixed (unconfirmed)

Live headset test of Update 1's build: props are genuinely fixed - the
shooting range fence area (previously the whole reason for this
investigation) now moves correctly, and a separate underground-town
area with lots of props (no weapon allowed there) was reported as
"everything was moving like it should." Confirms the core theory from
Update 1 was right.

But: **the weapon and arms stopped moving** - a direct regression from
tonight's earlier confirmed-working state (Notes/09 Update 4). Given
props got fixed and only the weapon broke, right after adding the
`xform0..xform3` layout allowlist, the allowlist itself was the prime
suspect - specifically, `IsKnownWeaponXformInputLayout` returning false
for the weapon's *actual* layout, meaning the weapon's own corrections
are now being skipped, same as everything else that isn't on the list.

Leading hypothesis: the semantic-name check
(`HackerDevice::CreateInputLayout` in `HackerDevice.cpp`) used
`strncmp` - case-sensitive - but D3D11 semantic names are matched
case-insensitively by convention, and the exact casing RenderDoc
displayed when we inspected this offline (lowercase `"xform0"`) isn't
guaranteed to match the actual bytes in the live
`D3D11_INPUT_ELEMENT_DESC` array at runtime.

**Fix**: switched to `_strnicmp` (case-insensitive). Also added a
throttled diagnostic log in `CreateInputLayout` that prints every
slot-1 element's exact semantic name string and whether it matched, so
the *next* test gives direct confirmation either way instead of another
guess. Rebuilt (0 errors), deployed, autonomous crash-free load-check
passed - but the diagnostic log stayed empty during that check, because
the main menu alone never creates the weapon's or particle system's
input layouts (they only get created once real gameplay loads those
shaders). **Genuinely unconfirmed** - needs a live test that actually
draws the weapon, after which `d3d11_log.txt` should be checked for
`CreateInputLayout layout=... matched=` lines to verify the fix
directly, independent of just observing in-headset behavior.

## Next steps for a future session

1. **Verify in headset** - the main open item, now specifically: does
   the weapon move again, and do props still hold correctly? Check
   `d3d11_log.txt` for `CreateInputLayout layout=... slot1 semantic=...
   matched=...` lines either way to see exactly what's being compared,
   rather than relying purely on visual observation.
2. If the case-fix resolves it, this note's job is basically done - the
   particle system doesn't need its own correction, it needed to be
   left alone, and the weapon's layout just needed a robust
   case-insensitive match. Fold this into Notes/09 as the final
   resolution if confirmed.
3. If the diagnostic log shows the weapon's layout *does* say
   `"xform0"` (or a case-insensitive match) and `matched=1` but the
   weapon still doesn't move, the bug is elsewhere (worth checking:
   is `sCurrentInputLayout` actually up to date at the moment
   `IASetVertexBuffers` fires for the weapon's draw - e.g. thread
   safety, since `CreateInputLayout`/resource creation can happen on a
   loading thread while `IASetInputLayout`/`IASetVertexBuffers` fire on
   the render thread, and `std::unordered_set`/a raw pointer aren't
   synchronized between them).

## Update 3 — 2026-07-29: allowlist was the wrong shape; switched to a blocklist

Live test of Update 2's case-insensitive fix: the weapon now moves and
*stays* moving - that regression is resolved. But props regressed hard
in the other direction - far fewer things moving than even the very
first broken state, and the underground-town/shooting-range walk that
previously demonstrated everything working now shows most things
static again.

Checked the diagnostic log added in Update 2
(`grep "CreateInputLayout layout=" d3d11_log.txt`) and it told a clear
story: exactly 3 distinct layouts had been created with slot-1
elements in that whole session - one 4-element `"xform"` layout
(matched, correct - this is the weapon) and two `"inst"` layouts (4 and
8 elements, both correctly excluded). The *matching logic itself* was
working exactly as designed. So the regression wasn't a matching bug -
it was a **design bug**: building this as an *allowlist* ("only patch
layouts we've positively confirmed are the weapon's exact `xform0..3`
naming") is fundamentally too narrow. This engine plausibly has many
distinct per-object shader families, each with its own instance-data
semantic naming - we've only ever sampled two of them (`xform` from
the weapon, `inst` from the particle system). An allowlist requires
enumerating *every* legitimate matrix-consuming layout in the entire
game to work correctly; anything we haven't specifically captured and
named gets silently excluded, which is exactly what happened to
whatever other props use non-`xform`-named matrix layouts.

**Fix**: flipped to a **blocklist**. Patch every slot-1/64-byte-stride
buffer bind by default (restoring the original inclusive behavior that
made the weapon and most props work after Notes/09 Update 4), and only
skip the one specific case disassembly has confirmed is *not* a matrix
- layouts with an `"inst"`-prefixed semantic at slot 1 (case-
insensitive `_strnicmp` match). This only requires correctly
identifying what to exclude, not enumerating every legitimate case, and
should behave identically to the already-confirmed-working weapon fix
for every shader family we haven't specifically looked at, while still
protecting the particle system's real per-instance data from
corruption.

Renamed the VRPose registry accordingly:
`RegisterKnownWeaponXformInputLayout`/`IsKnownWeaponXformInputLayout` →
`RegisterKnownNonMatrixInputLayout`/`IsKnownNonMatrixInputLayout`, with
`HackerContext::IASetVertexBuffers`'s gate flipped to
`!IsKnownNonMatrixInputLayout(...)`. Also raised the `CreateInputLayout`
diagnostic's log cap from 60 to 500 (per-call, not per-unique-layout -
the 60 cap could plausibly have been exhausted by repeat creations of
the same few layouts before ever seeing a distinct one from later in a
session, silently hiding the exact kind of case this fix now needs to
catch broadly).

Rebuilt (0 errors), deployed, autonomous crash-free load-check passed.
**Not yet confirmed in headset** - needs the user to redo the same
underground-town/shooting-range walk and confirm both the weapon AND
props move correctly together this time.

## Next steps for a future session

1. **Verify in headset** - does the weapon still move, and do props
   move again (ideally back to "everything moving" like the very first
   post-Notes/09-Update-4 success, before any of today's layout-
   filtering work)?
2. If props are still incomplete, get the full (uncapped-at-60)
   `CreateInputLayout` log from an actual full-length play session and
   look for additional distinct `"inst"`-prefixed (or similarly-shaped
   non-matrix) layouts this project hasn't seen yet - the blocklist can
   only exclude what it's been told about.
3. Once both weapon and props are confirmed stable together, this
   effectively closes out the whole "props don't track head rotation"
   investigation from Notes/09/10 - worth a final consolidated summary
   at that point.

## Update 4 — 2026-07-29: blocklist test showed IDENTICAL layout classification to the case-insensitive allowlist test - the bug isn't here

Live test of Update 3's blocklist build, redoing the same underground-
town/shooting-range walk: user reported "same thing" - still regressed
props, no different from Update 2's allowlist result.

Checked the diagnostic log: **exactly the same 3 distinct layouts** as
Update 2's test (one 4-element `"xform"` layout, two `"inst"` layouts
of 4 and 8 elements), classified identically (`xform` included,
`inst` excluded) in both builds. This is a genuinely important negative
result: since there are only ever 3 layouts with slot-1 elements in
this whole play session, and both the allowlist and blocklist versions
classify all 3 of them the same way, **the allowlist-vs-blocklist
change could not possibly have altered any patching decision this
session**. The "other unnamed matrix layouts are being missed" theory
from Update 3 is wrong - there aren't any.

This means the actual prop regression has nothing to do with input-
layout classification at all. The only variable that has changed
across the three most recent tests is whether the *weapon's own*
`PatchInstanceXformAtOffset` calls are active:
- Case-sensitive allowlist (weapon excluded, bug): props worked, weapon
  broken.
- Case-insensitive allowlist / blocklist (weapon included, fixed):
  props broken, weapon works.

Since the "inst" (particle) layouts are excluded identically in every
version tested so far, props relying on cb0 alone (ordinary static
geometry, per Notes/07) should be completely unaffected by whether the
weapon's xform corrections run - there's no code path connecting them.
Yet the correlation across three tests is exact. Leading hypothesis
now: some side effect of *performing* the weapon's
`Map(D3D11_MAP_WRITE_NO_OVERWRITE)`/`Unmap` cycles on the large shared
1MB ring buffer - not which specific layout gets classified - is
disrupting something else, possibly a timing/synchronization issue
(repeatedly re-mapping a large DYNAMIC buffer many times per frame is
an unusual usage pattern; the driver may not handle it exactly like the
game's own single-map-per-frame writes do) rather than a
classification/correctness bug in this project's code at all.

Added uncapped running-total counters (`sCumMatched`,
`sCumSkippedNonMatrix`, `sCumPatchCalls`, `sCumSkippedNoCachedView`,
`sCumSkippedAlreadyPatched`, `sCumApplied`), logged as a periodic
summary line (`VRPose diag: xform summary - ...`) every 180 frames from
`ResetVRInstanceXformFrameTracking`, instead of the old first-40-calls-
only diagnostics which get exhausted in the first few seconds and give
zero visibility into later gameplay (exactly when this regression
becomes visible - after walking to a different area). No functional/
correction-logic changes in this build - deliberately data-gathering
only, to avoid another blind guess-and-redeploy cycle. Rebuilt (0
errors), deployed, crash-free load-checked.

## Next steps for a future session

1. **Get the summary log from a real play session** covering the same
   underground-town/shooting-range walk - `grep "xform summary"
   d3d11_log.txt` - and look at how `applied` and the various
   `skipped*` counters trend over the course of the session,
   especially whether `skippedNoCachedView` climbs unexpectedly (would
   mean cb1's own patch, and thus `vrCachedViewValid`, is somehow
   becoming unreliable once the weapon is active - a very different
   bug from anything investigated so far).
2. If counts look sane (mostly `applied`, few skips) but props are
   still visibly broken, the bug is downstream of this whole
   patch-decision system - likely either GPU-side timing/
   synchronization from the repeated `Map(NO_OVERWRITE)` calls, or
   something entirely unrelated to the instance-xform mechanism that
   just happens to correlate with weapon draws (e.g. does drawing the
   weapon change something about draw ORDER or STATE for subsequent
   objects that render after it in the frame?).
3. Consider a controlled experiment: temporarily hard-code
   `PatchInstanceXformAtOffset` to still perform its read/Map/Unmap
   cycle for the weapon's offset but WITHOUT actually modifying the
   data (write back `current` unchanged instead of `patched`) - if
   props are STILL broken with this "inert" version, that would
   conclusively prove the *mechanism* of repeatedly touching this
   buffer (not the correction math) is the problem, independent of
   layout classification entirely.

## Update 5 — 2026-07-29: user pushback correctly ruled out raw performance volume; running the inert-data experiment

User correctly pushed back on the Update 4 "it's raw GPU-stall volume"
theory: the ~679k total corrections/session figure isn't a fair
comparison, since performance was *already* similarly heavy back when
props worked fine (Notes/09 Update 4's very first success, before any
layout filtering existed at all) - the weapon's own contribution to
that total is a small fraction. Re-examined all four tests side by
side instead of assuming:

- Update 1 (case-sensitive bug, weapon layout accidentally excluded
  from correction entirely): props worked.
- Update 2 (case-insensitive fix, weapon layout correctly included,
  *allowlist* scope - meaning literally nothing else was being touched,
  same as Update 1, since the allowlist only ever called
  `PatchInstanceXformAtOffset` for the one confirmed weapon layout):
  props broke.
- Update 3/4 (blocklist - broader scope, everything except confirmed
  particle "inst" layouts patched for real): props broke, identically
  to Update 2.

The critical realization: Update 2 already isolated the weapon as the
*sole* variable - its allowlist touched nothing else, at all, and props
broke anyway. That rules out "classification scope" (allowlist vs.
blocklist) as the cause entirely, and rules out "total volume of
patches across all objects" too, since Update 2 had far fewer total
patches than Updates 3/4 yet showed the identical prop regression.
The regression tracks one and only one variable across every test so
far: whether the weapon's own correction runs for real.

**Experiment**: narrowed `IASetVertexBuffers`'s gate back to exactly
Update 2's scope (only call `PatchInstanceXformAtOffset` for the
confirmed weapon layout, via a temporarily-reintroduced
`VRPose::IsKnownWeaponXformInputLayout`, kept alongside the
non-matrix blocklist for later restoration), but added a debug switch
inside `PatchInstanceXformAtOffset`
(`kWriteInertWeaponData`, currently `true`) that performs the
*identical* read/`Map(WRITE_NO_OVERWRITE)`/`Unmap` cycle - same cost,
same timing, same offset - but writes back the byte-for-byte
*unchanged* `current` value instead of the rotated `patched` one. This
isolates the last remaining explanation: is it the *act* of touching
this specific buffer region for the weapon (a timing/mechanism issue),
or the *specific rotated values* written there (a data/side-effect
issue)? The weapon itself is expected to look static/disconnected from
head tracking in this build - that's intentional, not a bug, for the
duration of this one test.

Rebuilt (0 errors), deployed, autonomous crash-free load-check passed.
**Awaiting live test.** Two clean outcomes:
- Props still broken with inert data → the mechanism itself (touching/
  remapping this offset) is the cause, independent of values - points
  toward GPU/driver timing specific to this buffer region or this point
  in the frame, not a data/math bug.
- Props fixed with inert data → the actual rotated values written to
  the weapon's offset are somehow responsible - would need to
  investigate what else might read this exact offset/region
  unexpectedly, or a subtler math issue.

## Update 6 — 2026-07-30: RESOLVED (diagnosis). Mapping the game's shared buffer is the cause; instance-xform patching disabled

**Inert-data experiment result: props still broken.** Confirmed from
the log that the build genuinely ran in inert mode (`inertMode=1` on
every logged apply) and did **299,076** `Map(WRITE_NO_OVERWRITE)`/
`Unmap` cycles writing **byte-for-byte unchanged data** - and the user
reported regression all the way back to "only the building moving."

This is conclusive. Every live test now fits one explanation:

| Build | Map/Unmap on game's buffer? | Props | Weapon |
|---|---|---|---|
| Update 1 (case-sensitive bug - lookups failed, so no patching) | no | work | broken |
| Update 2 (case-insensitive, weapon-only scope) | yes | broken | works |
| Update 3/4 (blocklist, broad scope) | yes | broken | works |
| Update 5 (weapon-only scope, **inert** writes) | yes | broken | broken |

Props break **whenever these Map/Unmap calls happen** and work whenever
they don't - completely independent of what is written. The correction
math, the offset identification, and the layout classification were all
red herrings; every one of them was fine.

**Root cause**: buffer 3461 is the game's shared ~1MB `D3D11_USAGE_DYNAMIC`
per-instance ring buffer, holding instance data for *hundreds* of
objects per frame. The game continuously maps it (`WRITE_NO_OVERWRITE`)
and appends data at advancing offsets throughout the frame. Interleaving
our own map/unmap cycles into that stream doesn't preserve other
objects' data - whether via driver-level buffer renaming, or our writes
racing GPU reads of neighbouring regions that `WRITE_NO_OVERWRITE`
explicitly promises we won't touch. We were corrupting hundreds of
neighbours to fix one object.

This also retroactively explains the very first symptom in this whole
investigation, which no earlier theory accounted for: *"there seems to
be no consistency in what is and isn't moving... this also seems to
change for some items depending on where I look at them from."* Which
objects get clobbered depends on draw order, which shifts with camera
angle and culling.

**Action taken**: gated the instance-xform patch off at its call site
in `HackerContext::IASetVertexBuffers`
(`kInstanceXformPatchEnabled = false`), restoring the known-good state
where cb0/cb1 patching makes world geometry and props track head
rotation correctly, and only the weapon viewmodel/arms don't. Chosen
deliberately over the reverse trade (one object working, hundreds
broken). All supporting machinery - offset tracking, the
`IsKnownNonMatrixInputLayout` blocklist, `PatchInstanceXformAtOffset`
itself, the correction math - is left intact and in its intended state
for the eventual proper fix. `kWriteInertWeaponData` restored to
`false`. Rebuilt (0 errors), deployed, crash-free load-checked.

**The proper fix, when revisited**: never touch the game's buffer.
Bind our *own* copy for the affected draws instead - allocate a private
`D3D11_USAGE_DEFAULT` vertex buffer, populate it with corrected data
(`UpdateSubresource` is legal on DEFAULT, unlike on DYNAMIC), and
substitute it in `IASetVertexBuffers` for draws we want corrected. The
game's memory then stays completely untouched, so no neighbour can be
disturbed. Open sub-problem: obtaining the *current* values to correct
still needs either the existing staging readback (a GPU stall, and the
source of the ~300k-stalls-per-session performance complaint) or a
CPU-side shadow maintained by intercepting the game's own Map/Unmap of
this buffer - the latter avoids the stall entirely and is likely the
better design, but reading back from write-combined mapped memory is
itself slow, so it needs measurement rather than assumption.

Milestone-2 (stereo rendering) work takes priority over this - see
Notes/11.
