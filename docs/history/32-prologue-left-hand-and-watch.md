# Prologue left hand and watch — active implementation handoff

Last updated: 2026-08-23

Branch: `codex/left-hand-controller-attachment`

This is separate from the later-game 12,132-index hand architecture documented
in `Notes/31-separated-left-hand-and-watch.md`. The prologue has a different
combined arms/hands mesh and a different bone layout. Do not feed it through
the embedded later-game preferred-pose rows.

## Final verified implementation at a glance

The verified recovery point is commit `e34708e` (`Attach prologue left hand and
watch to controller`) on branch `codex/left-hand-controller-attachment`.

The deployed and archived DLL is:

- artifact: `Builds/prologue-left-hand-deterministic-basis-20260823/d3d11.dll`
- SHA-256: `8BE785D2F4F6B9B6936327B3220753613C08A997DC45B253E754B419A395B6CA`
- build: Release x64, 0 warnings, 0 errors

The user verified this build after both a full game restart and a brand-new
game. The hand retained its saved placement and the complete watch, including
the time/timer, retained its placement and rotated perfectly with the hand.

The later defaults-only distribution build preserves that implementation and
embeds the exact values from the verified calibration files:

- artifact: `Builds/embedded-final-hand-watch-defaults-20260823/d3d11.dll`
- SHA-256: `FF3BF895EDA29630C0CE994724FF9856721EDBC77742350600063B154AF004E0`
- source commit: the commit containing the "Embed final hand and watch
  calibration defaults" change
- build: Release x64 succeeded with 0 errors; deployed to Metro

The original `8BE...` DLL remains the headset-tested behavioral baseline. The
later DLL changes only missing/malformed calibration-file defaults, using the
same values that were active during that successful headset test.

The final release-input candidate additionally disables the three temporary
hand/watch adjustment chords and restores weapon calibration:

- artifact: `Builds/release-hand-watch-controls-cleanup-20260823/d3d11.dll`
- SHA-256: `6AFBEB92B5CA62D8DA59D66DCFD7ACE03960EF647D96DE551477CB2B5900358A`
- build: Release x64 succeeded; deployed to Metro

The final architecture is:

1. Detect the exact 21,252-index prologue arms submission.
2. Omit both sleeve ranges, as the earlier arm-removal feature did.
3. Draw the native right-hand range through the existing right-hand path.
4. Substitute a private left-controller instance for both left-hand ranges.
5. Rebuild only the prologue left-hand bone group into a private `b8` palette.
6. Solve translation from the evaluated live wrist, so the anatomical wrist
   lands at the calibrated point on the controller.
7. Use one completed, view-correction-pre-cancelled hand instance for both
   left-hand ranges and both eyes.
8. Publish that completed instance as the prologue watch parent.
9. Route every physical watch piece to a rigid wrist-local watch baseline plus
   the independent prologue watch calibration.
10. Suppress Metro's flat clock glyphs, decode their live values, and render
    the same custom stereoscopic five-glyph display used by the later game.

This is deliberately separate from the normal-game preferred-pose system. The
prologue has only one gun, so Metro's live prologue hand pose is preserved; the
mod changes its root placement and controller attachment, not its authored
finger pose.

## Source-code map

The render interception and parenting implementation is in:

- `ThirdParty/3Dmigoto/DirectX11/HackerContext.cpp`
- `IsOpeningSequenceArmMesh`
- `BeginLeftHandBonePalette(bool rigidInstanceOnly, bool prologueSkeleton)`
- the 21,252-index special draw path
- the prologue branches of the physical-watch and custom-clock routing

The calibration state, persistence, activity tracking, and input chords are in:

- `ThirdParty/3Dmigoto/DirectX11/VRPose.cpp`
- `ThirdParty/3Dmigoto/DirectX11/VRPose.h`
- `LoadPrologueLeftHandAdjust` / `SavePrologueLeftHandAdjust`
- `MarkPrologueLeftHandActive`
- `GetPrologueLeftHandOffsetAdjust`
- `GetPrologueLeftHandRotationAdjust`
- `LoadPrologueWatchAdjust` / `SavePrologueWatchAdjust`
- `GetPrologueWatchAssemblyCalibration`

`MarkPrologueLeftHandActive` updates a timestamp. The calibration input path
treats the prologue as active for 1,000 ms after the most recent prologue hand
draw. Once a calibration mode is entered, it remembers whether it is editing
the prologue or normal-game target until that mode is turned off. This prevents
a brief missing draw during a transition from saving values into the wrong
file.

## Exact final hand draw flow

When the exact prologue mesh is encountered, the special path performs these
draws:

- right hand: count 7,161 at relative start 3,453
- left hand first run: count 6,474 at relative start 10,614
- left hand second run: count 510 at relative start 20,742

The right sleeve at relative start 0/count 3,453 and left sleeve at relative
start 17,088/count 3,654 are never drawn.

For the left hand, the path calls
`SubstituteWeaponInstanceBuffer(21252, false, true)` and
`BeginLeftHandBonePalette(true, true)`, then issues both left-hand ranges. The
second-eye twin pass explicitly rebinds the same private left-hand `b8` constant
buffer and the no-scissor rasterizer before issuing the same two ranges. It
then restores the original state and instance buffer.

If instance substitution is unavailable, the bounded fallback draws the two
left-hand ranges through Metro's native recursive route. The sleeves still do
not return. Diagnostics use the bounded status labels `WRIST_LOCKED`,
`PIVOT_FALLBACK`, and `UNAVAILABLE` rather than logging every draw forever.

The following invariants are required to avoid the flat/warped/one-eye failures
encountered during the normal-hand work:

- both left index runs use the exact same completed instance;
- both eyes use the exact same completed instance and private bone palette;
- `G->vrViewCorrection3x4` is pre-cancelled before the instance is submitted;
- head motion is not included in the controller-local hand transform;
- scissoring is disabled only around these hand draws and then restored.

## Prologue skeleton and wrist lock

The prologue private palette selects bones:

`0, 3, 6, 9, 12, 15, 18, 21, 24, 27, 30, 33, 36, 39, 42, 45, 48, 51, 54`

It does not load or apply `kEmbeddedPreferredLeftBoneRows`. Those rows belong
to the normal 12,132-index gameplay hand and applying them here would corrupt
the prologue skeleton/pose.

The anatomical wrist is evaluated from 121 captured wrist vertices with model
X greater than or equal to -0.525. Their weights sum to 1.0. The reduced wrist
coefficients used at runtime are:

| Bone | X | Y | Z | W |
|---:|---:|---:|---:|---:|
| 0 | -0.0182267376 | -0.0006567624 | 0.0008045128 | 0.0350996598 |
| 48 | -0.0036231641 | 0.0000894510 | -0.0000817758 | 0.0071625346 |
| 51 | -0.3743235038 | -0.0010010518 | -0.0010801531 | 0.7321017664 |
| 54 | -0.1165461193 | -0.0033326085 | 0.0022621166 | 0.2256360392 |

They were derived by:

`Builds/prologue-left-hand-instance-20260823/derive_prologue_wrist_coeffs.py`

At runtime the code evaluates that live wrist from Metro's current prologue
bone rows. The desired instance rotation is the absolute left-controller
rotation multiplied by the calibrated local hand rotation. Translation is then
solved so the evaluated live wrist lands on the absolute controller position
plus the saved controller-local offset. This is why the hand rotates around the
wrist instead of swinging around the mesh origin.

The final prologue authored basis is fixed identity. Do not restore the earlier
startup-captured basis: its meaning changed with the tracking anchor on every
process launch, which made an unchanged calibration file produce a different
visible hand location after restarting the game.

The stable model-space wrist landmark published for watch parenting is:

`(-0.51268, -0.00885, 0.00092)`

Using this fixed landmark instead of the first observed animation frame removes
another source of launch-to-launch variance.

## Final calibration files, values, and controls

The player's final verified values after the last positioning pass are:

`vr_prologue_left_hand_offset.txt`

```text
-0.192000 0.166000 -0.436001 0.190000 -1.009999 0.000000
```

The six values are translation X/Y/Z followed by rotation X/Y/Z.

`vr_prologue_watch_assembly_offset.txt`

```text
-0.450301 0.247900 -0.205200 0.522923 -0.332777 -0.148408
```

The prologue uses the normal shared casing-local time-face calibration:

`vr_watch_time_offset.txt`

```text
-0.000318 0.000303 -0.002759 -0.001678 -0.029611 -0.014000 0.956011
```

Those seven values are translation X/Y/Z, rotation X/Y/Z, and uniform scale.

The following prologue-hand setup controls are retained in the source for
development reference but disabled in the release input path. When temporarily
enabled, left trigger plus left grip held for about one second enters/exits the
mode:

- left stick X/Y: translate sideways/up-down
- left grip plus left stick Y: translate in/out
- right stick Y: pitch
- right stick X: yaw
- right grip plus right stick X: wrist-axis twist/roll
- repeat the entry chord to exit and save

The following complete-prologue-watch setup controls are likewise disabled.
When temporarily enabled, left trigger plus right grip held for about one
second enters/exits the mode:

- left stick X/Y: translate sideways/up-down
- left grip plus left stick Y: translate in/out
- right stick Y: rotate X
- right stick X: rotate Y
- left grip plus right stick X: rotate Z/roll
- repeat the entry chord to exit and save

The time-face setup chord is also disabled for release. Its development chord
is left trigger plus both grips held for about one second. Its detailed
translation, rotation, and scale controls are documented in
`Notes/31-separated-left-hand-and-watch.md` and save to the shared
`vr_watch_time_offset.txt`.

Weapon calibration is re-enabled on its established left trigger plus left-grip
chord. Journal calibration remains unchanged and still takes precedence over
its overlapping chord while the journal is visible.

### Distribution defaults

The exact final values above are embedded in `VRPose.cpp` as both the initial
values and malformed-file fallbacks. A clean installation therefore receives
the verified prologue hand, watch, and shared time-face placement without
shipping any calibration files. User-created calibration files continue to
override those defaults normally. The values are deterministic and do not need
the preferred gun to appear first or any four-second capture period.

## Final watch and live time/timer routing

The prologue watch is not one draw. Its physical family is the same identified
cluster used later in the game:

- 6,108-index primary casing/parent
- 2,628-index secondary casing piece
- two 750-index pieces
- exact 60-index cyan light/shadow bar with VS
  `65C62A5148831C58` and PS `94ED7704592CAC87`

Keep the existing count, shader, matrix, and frame-proximity guards. Do not
loosen the 60-index classification globally; unrelated 60-index effects exist.
`sOpeningSequenceArmFrame` and `sPrologueLeftWatchParentDeltaFrame` distinguish
the prologue cluster from ordinary later-game watch submissions.

The final prologue watch does not use Metro's animated native hand-to-watch
pivot. It uses the proven normal-game `fixedWatchLocal` rigid baseline under
the completed prologue hand instance, followed by the independent prologue
watch calibration. Every physical piece inherits that same completed 6,108
parent, so the casing, orange parts, cyan bar, and display remain one rigid
assembly.

The native flat watch glyphs remain the source of truth but are not the visible
display. The mod captures all five glyphs—four digits and the colon—decodes the
actual values Metro supplies, suppresses the flat glyph draws, and renders the
custom stereoscopic 3D display on the final 6,108 casing parent. Consequently:

- normal clock mode displays Metro's real-world time;
- wearing a filter automatically displays Metro's filter countdown;
- countdown transitions and tens boundaries use the same corrected atomic
  update path as the normal-game watch;
- prologue-specific fake time logic is neither needed nor present;
- depth testing and eye placement match the normal custom watch display.

The prologue capture decoded `19:23`, proving all five native glyphs and the
same decoder path are present in the opening sequence.

## Failed approaches and why not to retry them

| Attempt | Result | Root cause |
|---|---|---|
| Treat the combined mesh as one object | Left hand stayed tied to the right/controller-native root | The left hand needed its own instance and bone palette |
| Rotate the mesh around its submission origin | Hand swung in an arc | The origin is not the anatomical wrist |
| Reuse normal-game embedded preferred bone rows | Incorrect/corrupt prologue pose | Different mesh and skeleton layout |
| Capture the controller-local authored basis at startup | Saved placement changed after every restart | Startup tracking anchor changed the captured basis |
| Use the first animated wrist frame as the watch landmark | Placement varied between launches | First-frame animation state was not deterministic |
| Use the gameplay watch calibration unchanged | Watch moved well away from the hand | Prologue and normal hand roots/local frames differ |
| Parent watch with `completedStableWrist * inverse(nativePrologueHand) * nativeWatch` | Pieces followed left but casing swung on rotation | Metro's animated watch pivot retained a lever arm |
| Cancel the native lever-arm translation only | Placement became adjustable but swing remained | Native rotation/pivot was still inherited |
| Draw native flat time on the moved watch | Eye mismatch, sliding, and insufficient depth | The source glyph matrices were view/HUD-oriented |

The discarded animated-pivot watch calibration was backed up as:

`vr_prologue_watch_assembly_offset.native-pivot-backup.txt`

The rigid-baseline migration went from the old headset-space value
`-0.091700 -0.061500 -0.427999 0 0 0` to the intermediate value
`-0.159300 -0.019100 -0.271200 0.292923 -0.282777 -0.218408`. That intermediate
value remains useful for archaeology but has been superseded by the final
verified watch calibration listed above.

## Future original-hands / separated-hands option

Use the same global runtime setting proposed in
`Notes/31-separated-left-hand-and-watch.md`; do not create a prologue-only
preference unless the UI explicitly needs one.

When separated hands are enabled, retain the complete final flow documented
above.

When original/combined hands are selected:

- still intercept `IsOpeningSequenceArmMesh`;
- still omit both sleeve ranges, because arm removal predates this feature and
  bypassing the entire special block would restore the unwanted arms;
- draw the right hand and both left-hand ranges through the original/native
  combined right-controller path;
- do not call `BeginLeftHandBonePalette(true, true)`;
- do not publish the prologue completed-left-hand parent;
- do not reparent the physical watch family to the left controller;
- disable prologue custom-clock suppression/routing so the original presentation
  is restored consistently with that mode;
- leave all calibration files untouched so switching back restores the user's
  separated-hand placement immediately.

The branch must therefore be inside the exact 21,252-index interception, not
around it. Test both modes in a fresh prologue/new game and in ordinary gameplay
chapters, including a filter-equipped countdown and both-eye capture.

## Recovery and verification checklist

To restore the proven implementation:

1. Check out commit `e34708e` or recover the archived DLL whose SHA-256 is listed
   above.
2. Preserve or restore the three final calibration files and values in this
   note.
3. Confirm the 21,252-index draw is classified and reports `WRIST_LOCKED` rather
   than `PIVOT_FALLBACK` or `UNAVAILABLE`.
4. Confirm sleeves remain absent.
5. Confirm both left-hand ranges are present, stereo-correct, and controller
   locked.
6. Physically turn and rotate the controller; the hand must neither head-drag
   nor swing around a distant origin.
7. Confirm all watch casing pieces, cyan bar, colon, and four digits remain one
   rigid assembly.
8. Test ordinary time and filter countdown behavior.
9. Restart the entire game and start a brand-new game; placement must persist.

## Proven prologue mesh identity

`IsOpeningSequenceArmMesh` identifies one exact G-buffer submission:

- index count: 21,252
- instance count: 1
- start index: 966255
- base vertex: 223462
- vertex shader: `79BA9F1ECFAF0CA1`
- pixel shader: `32B3ADE9DE302235`

The saved capture is:

`<METRO_INSTALL>/FrameAnalysis-PrologueArmsFull-20260822-185006-922`

The analysis script is:

`Builds/prologue-arm-identification-20260822/analyze_21252_mesh.py`

Connectivity and bone analysis proves four islands and five contiguous index
runs:

| Relative start | Count | Component |
|---:|---:|---|
| 0 | 3453 | right sleeve |
| 3453 | 7161 | right hand |
| 10614 | 6474 | left hand, first run |
| 17088 | 3654 | left sleeve |
| 20742 | 510 | left hand, second run |

The left hand is one connected component split into two index runs only because
of submission order. Both runs must always receive the same instance and eye
state. The left component has negative-X model bounds and primarily references
the prologue skeleton's bone group from 3 through 54. The regular gameplay hand
uses different geometry, coefficients, and preferred bone rows.

## Existing sleeve removal

Before the current work, `DrawIndexedInstanced` intercepted the exact 21,252
draw, recursively rendered the right hand and both left-hand runs, and omitted
the two sleeve runs. This was headset-verified. Preserve that exact signature
and fallback behavior.

## First left-controller candidate

Candidate:

`Builds/prologue-left-hand-instance-20260823/d3d11.dll`

SHA-256:

`A1040885397253F80828F50F1EFB081983798825E48CA0BFF1845905A762B5D3`

The exact prologue interception now:

1. marks the current frame as a prologue-arms frame;
2. draws the right-hand range through the previous path;
3. calls `SubstituteWeaponInstanceBuffer(21252, false, true)` once;
4. draws both left-hand runs with that one private left-controller instance;
5. draws those same two runs in the twin eye without recomputing their matrix;
6. restores Metro's original instance buffer; and
7. falls back to the previous native recursive draws if substitution is not
   available during loading.

No prologue bones are frozen or replaced. Metro's live prologue pose remains
active. The existing normal left-hand calibration is temporarily inherited by
this first diagnostic candidate; add a separate prologue calibration only after
the basic transform is visually validated.

Expected bounded log records:

- `VRPose prologue: split opening mesh...`
- `VRPose prologue: left controller attachment ACTIVE...`
- or `...UNAVAILABLE...` if the required instance/pose state was absent
- `VRPose prologue watch: idx=...` for familiar physical watch passes

## Watch and time evidence

The saved prologue capture proves this is not a completely separate display
system. It contains:

- a 6108-index physical draw;
- 2628 and two 750-index physical draws;
- conditional 60-index draws, including the known cyan shader pair
  `65C62A5148831C58 / 94ED7704592CAC87`;
- the exact watch UI anchor pair
  `A9037683D2AF5BC0 / B468E0743796D214`; and
- immediately following six-index glyph quads through the existing generic
  native text pair `7B3EB7275D556B14 / 4A48D5D4C49DCDF2`.

Therefore the existing native HH:MM/filter decode and custom stereo 3D display
should be reusable. The missing element is a completed prologue wrist/watch
parent. The first candidate intentionally does not guess that parent.

## Test and next decision

Load the playable prologue and check only the left hand first:

1. Is it visible in both eyes?
2. Does it translate with the physical left controller?
3. Does it rotate with the left controller?
4. Does it remain one intact hand, including the small second index run?
5. Does it drift with head movement or swing around the controller?
6. Where do the physical watch and native/custom time appear relative to it?

After the run, inspect `d3d11_log.txt` for the bounded prologue records. If the
hand is rigid and controller-relative, derive a prologue wrist landmark and
publish a completed prologue watch parent. If the hand swings or drifts, do not
adjust random pivots: generate prologue-specific wrist/centre coefficients from
the saved vertex/bone capture and use the same exact anatomical constraint as
the proven gameplay hand, while retaining the live prologue bone rows.

## First headset result and measured correction

Video: `<PRIVATE_VIDEO>/2026-08-23 19-19-48.mp4`.

Result:

- hand visible and geometrically correct in both eyes;
- translation path active;
- no apparent drift during physical body turning;
- controller rotation produced a large orbit/swing;
- calibration translation could not bring the hand to the controller because
  rotation continued around the wrong point; and
- the complete watch remained on the right controller.

The video shows a clean rigid orbit, not hand deformation. The runtime log
repeatedly reports `left controller attachment ACTIVE`, proving the fallback
was not responsible. This confirms the first candidate was rotating the
prologue geometry around the later-game hand's fixed model pivot.

The runtime log also proves the complete prologue watch family in one live
frame, including:

- 6108 at start 442770/base 85259;
- 2628 at start 450876/base 87325;
- two 750 material passes at start 450078/base 87185; and
- the exact cyan 60 pass with shaders
  `65C62A5148831C58 / 94ED7704592CAC87`.

The native clock decoder captured `19:23` from the exact icon-anchored glyph
sequence. No separate prologue time decoder is required.

## Wrist-locked candidate

Candidate:

`Builds/prologue-left-hand-wrist-locked-20260823/d3d11.dll`

SHA-256:

`4109187947ECFB0A67D50CB54B2CBC5A95B1A4C93C66EFBDC7596BCF85E3C15D`

`derive_prologue_wrist_coeffs.py` reconstructs the negative-X hand component
from the saved index buffer, decodes positions at 1/4096 model scale, and
expresses selected vertex centroids as weighted functions of the live bone
palette. The anatomical wrist selection uses 121 vertices at the sleeve-facing
end (`model X >= -0.525`). Its weights sum to one across bones 0, 48, 51 and
54, so evaluating those coefficients each frame yields the live skinned wrist
without a GPU vertex readback.

`BeginLeftHandBonePalette(true, true)` now selects the prologue bone group and
these prologue-specific centre/wrist coefficients. It does not load or apply
the later-game embedded preferred pose. It constructs an absolute desired
instance whose evaluated wrist centroid lands on the physical left controller,
pre-cancels the view correction, publishes the completed hand/watch parent,
and binds an otherwise unchanged copy of Metro's live prologue palette. Both
left-hand index runs and both eyes consume that same completed instance.

The completed parent also makes the existing physical-watch routing eligible.
Because the prologue reuses the same watch geometry but has a different hand
skeleton, its watch may initially be misplaced even if it moves to the left
controller. Validate hand rotation first, then calibrate or derive a separate
prologue watch-local transform rather than disturbing the proven gameplay
watch calibration.

The first test changed the temporarily shared `vr_left_hand_offset.txt` from
the proven gameplay value to `-0.234000 -0.216000 -0.220000 1.139999 -1.500000
0.000000`. That test value is preserved in the wrist-locked candidate folder as
`vr_left_hand_offset.first-prologue-test.txt`. The game directory was restored
to the pre-test gameplay value `-0.018000 0.060000 -0.220000 0.999999 -1.049999
0.000000`. Do not calibrate the prologue hand again until it has a separate
calibration store.
# Independent prologue calibration and native watch parenting (2026-08-23, 19:39)

The wrist-locked candidate proved the anatomical constraint: the prologue hand
rotated correctly without physical-turn drift. It was still using the normal
game's `vr_left_hand_offset.txt`, however, so the user correctly did not move it.
The same run showed no visible watch/time. The log proved this was placement,
not missing game data:

- all five native time glyphs were captured (`1`, `9`, `3`, `4`, colon),
- they were intentionally suppressed for the custom rigid 3D display,
- the 6108 casing was routed, but its translation changed from native
  `(0.057, -0.165, 0.551)` to the off-wrist normal-game result
  `(-0.329, -0.225, 0.151)`.

The next candidate makes two focused changes:

1. Prologue hand placement now reads/writes
   `vr_prologue_left_hand_offset.txt`. Its missing-file defaults equal the
   proven normal-game calibration
   `-0.018 0.060 -0.220 0.999999 -1.049999 0`, so the first run starts from
   the same successful wrist-lock orientation without touching
   `vr_left_hand_offset.txt`. The opening mesh marks itself active each frame;
   the existing LT + left-grip calibration chord then selects and saves the
   prologue file. The mode remembers which calibration it owns until toggled
   off, even if a transient draw disappears.
2. The prologue watch no longer consumes gameplay's `fixedWatchLocal` or
   `vr_watch_assembly_offset.txt`. Since the prologue has one hand/watch pose,
   its final transform is computed as
   `completedStablePrologueWrist * inverse(nativePrologueHand) * nativeWatch`.
   Thus 6108, 2628, both 750 passes, the exact cyan 60 pass, and the custom time
   inherit the prologue's own authored hand-to-watch relationship under the
   same stable controller parent. Normal gameplay retains its existing fixed
   local matrix and calibrated assembly path.

Build/deploy:

- artifact: `Builds/prologue-left-hand-independent-calibration-watch-parent-20260823/d3d11.dll`
- SHA-256: `D79C09867E629383BE35A17A8E99C1760CD6BFE8A3647BEC9BDF9DEC9FD65C5A`
- Release x64: 0 errors, 1 pre-existing C4250 warning
- deployed to the Metro directory; the normal calibration file was re-read
  afterward and remained exactly unchanged.
# Prologue watch placement calibration (2026-08-23, 19:49)

Video `2026-08-23 19-44-39.mp4` confirmed that the independently calibrated
hand is correctly placed and rotates correctly. The entire physical watch and
custom time are present and remain grouped, but they orbit far in front of the
wrist. Runtime `watchrel` logs make the cause unambiguous: the watch relative
rotation is essentially constant, while the native prologue child translation
is approximately `(0.0572, -0.1650, 0.5840)`, a 0.61-unit lever arm. This is a
wrong authored child origin, not loss of controller rotation or a split watch.

Added independent prologue watch assembly calibration:

- file: `vr_prologue_watch_assembly_offset.txt`
- missing-file baseline translation: `(-0.0572, 0.1650, -0.5840)`, cancelling
  the measured native lever arm so the first run starts near the wrist
- rotation baseline: identity, preserving the prologue watch's authored face
- existing LT + right-grip watch calibration automatically selects this file
  while the prologue draw is live
- casing, emissive parts, cyan bar and custom time all consume the same final
  calibrated matrix
- prologue hand calibration and both normal-game calibration files are not
  touched

Artifact: `Builds/prologue-left-watch-independent-calibration-20260823/d3d11.dll`
SHA-256: `5475E6EAD5465CDCB4F99D8BA0B39CE35193419482166943FBEADE22D2E682C5`
Release x64 succeeded with 0 errors and one pre-existing C4250 warning. Deployed
with the saved prologue hand calibration still exactly
`-0.088 0.208 -0.498001 0.999999 -1.049999 0`.
# Rigid prologue watch pivot (2026-08-23, 19:58)

Video `2026-08-23 19-53-21.mp4` proved the prior native-parent approach still
had a rotation defect after the user placed the watch on the wrist. The
instance-level `watchrel` orientation stayed constant, but the visible casing
swung away during controller rotation. This means Metro's native prologue
hand-to-watch matrix contains an animated model/viewmodel pivot below the
instance stage; calibrating its translation only hides the lever arm at one
orientation.

The prologue now uses the proven `fixedWatchLocal` rigid baseline already used
by normal gameplay. Its calibration remains completely independent. To avoid
throwing away the user's 19:53 headset placement, the saved old-baseline pose
was converted into the new coordinate system:

- old native-pivot calibration:
  `-0.091700 -0.061500 -0.427999 0 0 0`
- derived rigid-pivot calibration:
  `-0.159300 -0.019100 -0.271200 0.292923 -0.282777 -0.218408`
- the original was preserved recoverably as
  `vr_prologue_watch_assembly_offset.native-pivot-backup.txt`

The hand load trace also resolves the apparent lost-save report. This run did
load the previous hand file exactly as saved:
`(-0.088, 0.208, -0.498)`. The subsequent live calibration deliberately moved
X through zero and saved the new value `(0.058, 0.204, -0.498)`. The new value
is preserved unchanged in this deployment.

Artifact: `Builds/prologue-left-watch-rigid-pivot-20260823/d3d11.dll`
SHA-256: `80E1855301060433FB6CEDE7AA7A6A1EF03933E496102B99F7EF5A0B158FA2F8`
Release x64 succeeded with 0 warnings and 0 errors; deployed.
# Deterministic prologue hand basis (2026-08-23, 20:06)

The rigid-pivot headset run confirmed the watch now rotates perfectly with the
hand. The remaining repeated hand repositioning was not a file-load failure.
The log shows the previous file was loaded exactly as saved, but
`controllerLocalBase` was recaptured from the controller's startup tracking
anchor each process launch. Consequently the same saved Euler/translation
values were interpreted under a different rotated basis, and the user had to
move the mesh to compensate. The successive saved translations demonstrate
the compensation:

- prior: `(0.058, 0.204, -0.498)`
- latest: `(0.014, 0.388, -0.288)`

The prologue has one fixed hand mesh and pose, so it now uses a deterministic
identity authored basis and the absolute live controller rotation. Normal
gameplay retains its proven captured basis and is isolated from this change.
The prologue's stable watch wrist landmark is also fixed to the successful run's
captured model-space value `(-0.51268, -0.00885, 0.00092)` so a different first
animation frame cannot shift the otherwise rigid watch after restart.

This coordinate-system correction necessarily requires one last hand placement
(and possibly a watch touch-up) because all older saved prologue offsets were
authored in a startup-dependent basis. Values saved after this build are
controller-local and reproducible across launches.

Artifact: `Builds/prologue-left-hand-deterministic-basis-20260823/d3d11.dll`
SHA-256: `8BE785D2F4F6B9B6936327B3220753613C08A997DC45B253E754B419A395B6CA`
Release x64 succeeded with 0 warnings and 0 errors; deployed without modifying
either current calibration file.
# Verified final prologue persistence (2026-08-23)

User validation after the deterministic-basis build:

- existing game restarted: hand and watch stayed in their saved positions
- brand-new game: hand and watch also stayed in their saved positions
- watch continues to rotate perfectly with the hand

This confirms that the independent calibration files, deterministic prologue
controller-local basis, fixed wrist landmark, rigid watch pivot, complete watch
assembly routing, and custom live time/timer all survive both process restart
and new-campaign initialization. Treat
`prologue-left-hand-deterministic-basis-20260823` as the verified prologue
baseline.
