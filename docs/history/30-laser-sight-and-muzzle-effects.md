# Note 30 — Laser sight and muzzle effects

Date: 2026-08-19

This note records the completed muzzle-flash/laser-beam work, the red laser-dot
investigation, every important failed approach, and the exact state left for a
later session.

## 1. Starting symptoms

- The visible laser beam moved with head/body/thumbstick turning instead of
  remaining rigidly attached to the weapon.
- The projected red dot on the contacted wall was HMD-locked.
- The muzzle flash was HMD-locked. During intermediate tests two distinct flash
  pieces were visible; both appeared in both eyes.
- Once the continuous flash was attached to the barrel, releasing the trigger
  still made it jump to the HMD for one frame.

## 2. Visible beam — fixed

The beam draw is identified by:

- VS hash `6C14CA9DCD06FE85`
- PS hash `339689302CA37E17`
- 24 indexed vertices in the clean RenderDoc capture

It is folded through the same controller/weapon affine used for attached weapon
effects. The final corrected world-view transform is cached in
`sLaserBeamCorrectedWV`, together with the eye view in which it was captured.

Player result: the visible beam stays attached to the weapon during physical
turning and thumbstick turning.

RenderDoc later proved the beam mesh itself spans local Z approximately
`[0, 1]`, with its centred geometric tip at `(0, 0, 1)`.

## 3. Muzzle flash — fixed

The flash was not a single object. The work had to cover both the primary flash
and the associated particle/release-tail path. Intermediate builds proved this
when one flash followed the barrel while another remained on the HMD, and then
when both were attached during sustained fire but jumped on trigger release.

The final correction keeps both pieces on the barrel through sustained fire and
through the release frame. Player-confirmed working on 2026-08-19.

Do not collapse this back to a one-draw fix: the release-tail handling is needed
to prevent the one-frame HMD jump.

## 4. Native projected red dot — identified but not successfully corrected

Capture:

`GameReferences/red_dot.rdc`

Analysis output:

`.codex-video-analysis/red-dot-rdc/probe.json`

Exact deferred projected-light draw in the capture:

- event 603
- `DrawIndexed(36)`
- VS hash `98CD6A5E2980BAE1`
- pixel constant buffer b12 (`cb_light`)
- captured color `(9, 0, 0, 6)`
- captured position w about `0.043403`
- inverse light-transform scales about `(69.444, 69.444, 0.2083)`

The runtime classifier uses those structural cb-light values rather than one PS
hash because Metro has multiple deferred-light shader variants. It logs the live
PS hash in `VRPose laser dot: paired projected-light correction active ... ps=`.

Attempts to rebuild or correct the native light-volume and projector matrices
all left the visible dot HMD-locked. Depending on the attempted transform, head
motion changed from horizontal-only to both axes and back, but attachment never
became weapon-relative. This demonstrated that the visible native result is
authored/reconstructed in another camera-relative layer downstream.

The native draw is positively classified and suppressed by both setting
`data.call_info.skip` and binding a null PS at the final pre-submit boundary.
Despite that, the original HMD-locked red mark is still visible in player tests.
Therefore either another draw/layer creates the surviving mark or the event-603
classification is not the final visible variant in this runtime. Do not claim
the old dot is removed; it is still an open issue.

Three 200-instance particle batches immediately before the beam (capture events
882, 893 and 904) were dumped and compared. Suppressing them produced no useful
visible difference, so they were left untouched to avoid damaging unrelated
particles.

## 5. Custom stereo dot — current replacement

`HackerContext::DrawVRLaserDot()` draws a dedicated stereo red marker when the
native laser-light draw is identified.

Appearance:

- distance-scaled from Metro's scene-position texture (`t0`);
- current radius expression: `clamp(2.10 / distance, 1.20, 3.20)` pixels;
- sharpened alpha edge and brighter red core to approach the native mark;
- player judged the size acceptable after an initial too-large version and a
  second too-small version.

Position history:

1. **Canonical shot/controller ray:** exact bullet-aligned position. This was
   player-confirmed perfect, but did not include visual walking sway.
2. **Native projector local point** `(-0.069992505, 0.104999369,
   0.432075130)`: followed sway but sat beside the laser. Those X/Y values are
   projector-light offsets, not the visible beam centre.
3. **Centred `(0,0,0.432075130)`:** appeared in the middle of the beam. RenderDoc
   proved 0.432 is internal to a mesh whose local Z endpoint is 1.0.
4. **Centred beam tip `(0,0,1)`:** reached roughly the end, but still did not
   coincide with the proven bullet-impact position. The visible weapon beam and
   the redirected damage ray are not the same absolute ray.
5. **Current:** restore the canonical bullet-aligned position, then add only the
   changing component of the beam-tip displacement. A slowly moving baseline
   removes the beam's fixed mismatch; the residual adds cosmetic sway.

Current sway-filter constants:

- baseline adaptation: `0.012` per frame;
- animated offset clamp: `±0.060` NDC per axis;
- discontinuities above `0.08` NDC reset the baseline instead of being treated
  as sway;
- stale beam data after four frames resets the baseline.

Player result: resting dot is back at the exact bullet-aligned position and
some sway is present. During running it still does **not** follow the laser as
far sideways as the beam moves. The player accepted this as good enough for now.

Important design fact: Metro's walking/running weapon sway is visual, while the
VR damage ray is generated from the canonical controller/barrel direction.
Making the dot follow all visual sway while also remaining an exact bullet
indicator is impossible unless the same sway is deliberately added to the shot
ray. Do not silently change shooting behavior merely to align this cosmetic
effect.

## 6. Current shipped/test build

The last build in this session compiled with zero errors and zero warnings and
was deployed byte-for-byte to the Metro folder.

SHA-256:

`DBF42B2001E9307C39EFB488BCB518D9593E133A6C908731A1733975CC5D5C7D`

Current user-visible status:

- laser beam attachment: fixed;
- muzzle flash, including trigger release: fixed;
- custom red dot resting aim: accurately aligned with bullets;
- custom red dot appearance/size: acceptable;
- custom red dot walking sway: improved;
- custom red dot running sway: incomplete, accepted for now;
- old native HMD-locked dot: still visible, unresolved.
