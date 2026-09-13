# AER left-eye head-motion jitter — paused investigation (2026-08-13)

## Symptom

Alternate-eye rendering (AER) is stable while the headset is still, but the
world doubles/jitters in the physical left eye while the head is moving. The
right eye appears stable. True stereo does not show this defect. Controller,
weapon, and 6DOF tracking remain correct.

The custom reticle and VR menu initially doubled in AER for a separate,
understood reason: both overlays always used eye 0's projection even when the
active AER frame belonged to eye 1. They now use `G->vrCurrentEye` in AER and
remain fused. Do not mix that resolved overlay defect with the world jitter.

## Current clean runtime path

- AER renders one eye per game frame and toggles `G->vrCurrentEye` after
  submission.
- The newly rendered backbuffer is copied into that eye's persistent
  compositor texture; the other eye retains its preceding image.
- Both textures are submitted every game frame with `Submit_TextureWithPose`,
  using the HMD pose from which each texture was rendered.
- The compositor is explicitly set to `TrackingUniverseStanding`.
- The camera and controllers use the compositor-predicted pose set returned by
  `WaitGetPoses` rather than a separate unpredicted pose query. This did not
  remove the jitter but is retained because it is the standard, internally
  consistent OpenVR timing path and did not regress tracking.

## Tests completed

| Test | Result / conclusion |
|---|---|
| Active-eye projection for injected reticle/menu | Fixed both overlays completely; world jitter unchanged. |
| Remove `Submit_TextureWithPose` and use ordinary texture submission | Jitter/doubling appeared in **both** eyes. Pose-tagged submission is beneficial and must remain. |
| Add `Submit_FrameDiscontinuity` to suppress SteamVR motion smoothing | No change; removed. |
| Explicit D3D11 `Flush()` after the active-eye copy | No change; submission was not racing the queued copy. Removed. |
| Reverse compositor submit order from left/right to right/left | Jitter stayed in the physical left eye. Submission order eliminated. Restored left/right. |
| Log active eye, texture age, per-eye pose delta, compositor frame index, frame presents, reprojection flags, and submit errors for 1,186 AER frames | Eye ages were symmetric: one eye age 0 and the other age 1, alternating every frame. Retained-eye pose deltas were symmetric and reached about 3 degrees during faster turns. Both submits returned success. Async reprojection was active. SteamVR commonly predicted one or two additional display frames. |
| Invert eye/render phase relative to compositor frame parity | Trace confirmed the phase really inverted; jitter still stayed in the physical left eye. Game/compositor parity eliminated. Removed. |
| Use compositor-predicted `WaitGetPoses` HMD/controller poses and explicitly set standing tracking space | Tracking stayed correct; jitter unchanged. Kept as the cleaner OpenVR pose path. |
| Route right-eye AER frames through the complete twin RTV/DSV/SRV/copy/clear chain to preserve separate per-eye post-process/history resources | No change. Cross-eye render-target or temporal-history contamination is not sufficient to explain the defect. Reverted to avoid needless routing complexity. |
| Hold one common predicted HMD/controller pose across the two game frames that form an AER stereo pair | Tracking stayed correct; jitter unchanged. Different render poses inside a pair eliminated. Reverted because it halves pose updates without benefit. |

## What the evidence rules out

- The reticle/menu overlay projection bug.
- Failed or stale left-eye texture copies.
- D3D command submission order.
- Left-first versus right-first compositor submission.
- Which eye is fresh on a given compositor-frame parity.
- Unequal texture age or unequal recorded pose age.
- OpenVR submit errors or absence of async reprojection.
- Unpredicted-vs-predicted HMD/controller sampling.
- Standing/seated tracking-space ambiguity.
- Shared per-eye post-processing/history targets.
- Rendering the two halves of a stereo pair from different tracked poses.

## Important historical distinction

`Notes/11-milestone2-stereo-plan.md` records an older test where disabling eye
alternation did not fix distance-dependent flicker/pop-in. That symptom was
later proven to be Metro's CPU software occlusion culling and fixed by the
occlusion bypass. It is not the current head-motion-only, physical-left-eye
AER jitter and must not be cited as disposing of this issue.

## Best next diagnostics when resumed

1. Run a deliberately monoscopic A/B inside the AER submission path: render
   one fixed eye every frame and submit that exact fresh image/pose to both
   physical eyes. Then repeat with the other rendered eye. This distinguishes
   physical-left compositor/display behavior from left-projection content and
   from retained-frame behavior. It will look non-stereo and is diagnostic
   only.
2. Record the SteamVR mirror with both eyes plus an injected active-eye/frame
   marker, so the visible event can be correlated to the exact rendered eye
   and compositor frame rather than reported by perception alone.
3. Inspect SteamVR's application-specific frame-throttling/reprojection state
   and headset refresh rate. The captured flags show one-to-two-frame-ahead
   prediction even though most compositor frames report one present.
4. If the fixed-eye test implicates retained frames, test pair-at-a-time
   submission (submit only after both eyes have been freshly rendered) versus
   the current submit-both-every-game-frame design.

## Menu implication

`AER Phase Sync` currently stores and displays a choice but does not improve
the defect. Left-first, right-first, and compositor-parity/auto phase behavior
were tested without moving or removing the physical-left-eye jitter. Unless a
future pair-at-a-time design gives this setting a real purpose, remove it from
the release menu rather than expose a nonfunctional control.
