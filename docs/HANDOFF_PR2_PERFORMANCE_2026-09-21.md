# Metro 2033 Redux VR — PR2 performance integration handoff

Date: 2026-09-21

## Accepted state

Community pull request 2 is integrated locally. Its world-object pop-in and
right-eye-only scene-flicker regressions are fixed. The accepted policy keeps
Metro's CPU screen-occlusion bypass enabled whenever Expanded Visibility is
active, leaves the PR's object- and cluster-frustum bypasses disabled, and
keeps broad depth/G-buffer single-pass folding off by default. Safe batching,
depth-free folding, shader caching and the rest of the PR performance path
remain active. This retained the measured performance improvement in the
validated runs.

The accepted integration has been fast-forwarded into the clean local `main`
branch in `C:\Projects\Metro2033ReduxVR-Public`. It is seven commits ahead of
`origin/main`. Nothing from this integration has been pushed to GitHub and no
public release containing PR2 exists yet.

Primary source worktree:

`C:\Projects\Metro2033ReduxVR-PR2-Review`

Accepted integration branch:

`codex/pr2-performance-integration`

Active test installation:

`H:\Games\Steam\steamapps\common\Metro 2033 Redux`

Preserved public Test 3 baseline installation:

`H:\Games\Steam\steamapps\common\Metro 2033 Redux Dev Before PR2`

## Integrated upstream work

The local integration contains community PR #2 commits:

- `8be8779` — performance work
- `b27cc38` — visual corrections
- `467945f` — local merge of PR #2 into the public Test 3 line

The contributor's major changes include scene/depth single-pass folding,
removal of synchronous viewmodel readback, shader-variant and hot-path caches,
CPU occlusion-depth reconstruction, wider light-culling coverage, floor/bloom
reflection correction, lamp-flare correction, and chapter-select picture
correction. Do not describe every path as universally validated: the final
local headset runs concentrated on performance and the known pop-in location.

## Controlled performance and regression evidence

All comparison runs used the same save, exact location, Medium quality, Quest 3
through Virtual Desktop, SteamVR at 72 Hz, and a 2880x3060 per-eye target on an
RTX 5070 Ti / Ryzen 7 7800X3D system. The user repeated approximately the same
head motion in the same heavy area.

| Run | Runtime | Effective/average FPS | CPU | GPU | Reprojection | Pop-in |
| --- | --- | ---: | ---: | ---: | ---: | --- |
| A | Public Test 3 DLL | 50.4 | 18.83 ms average | 13.35 ms average | 23.4% | No |
| B | Exact original PR2 DLL | 65.8 | 10.21 ms average | 10.36 ms average | 6.4% | Yes |
| C | PR2 plus only `vr_vis_occlusion_bypass_on.txt` | 63.5 | 11.32 ms average | 10.30 ms average | 9.1% | No |
| D | Built-in occlusion default, no active `_on` marker | 64.04 average / 71 median | 8.9 ms median | 10.6 ms median | 10.0% | No |

Run D used fpsVR's text-session summary because fpsVR did not rotate that run
into a new numbered JSON file. Do not compare its median CPU/GPU fields directly
against the average fields from A-C. The user perceived D as at least as fast as
C. The small C-D difference is normal variation, not proof of a further speedup.

Complete A-C fpsVR JSON/BIN captures and compatibility reports are preserved at:

`C:\Projects\Metro2033ReduxVR-PR2-Review\dist\Controlled-AB-20260921`

The built-in candidate, symbols, final fpsVR text log, compatibility report,
and exact rollback DLL are preserved at:

`C:\Projects\Metro2033ReduxVR-PR2-CPUOcclusionDefault\dist\PR2-CPUOcclusionDefault-20260921`

Important hashes:

- Public Test 3 DLL: `D73F2FFB86CC77888146C5C818480393208C6A5944CF9C36CC3E7F1787509A59`
- Exact original PR2 candidate: `E8076C1A6113D5FD17217B941F1E1B9BD00485FB1A3AE4C412AE5C4D163FD92A`
- Headset-validated built-in-policy candidate before diagnostic cleanup: `1DEC4C2445B71B595B09D64F787A2A5D44DB8E0AA8D067AE9835A26B7DBC83F0`
- Final diagnostic-cleaned candidate: `C56DFEF8D57CEB5694F232624E472E057C0049C128DE6810EE87866F19A6A5C7`
- Final right-eye-safe candidate: `33DE6EA987F64F1770EAD4CE8957885E5CE665D33894FE7044EE96BC37CA44D3`

## What the A/B proves

The observed pop-in is caused by the PR's re-enabled CPU screen-occlusion route.
Enabling only the existing CPU screen-occlusion bypass removed the regression;
the object- and cluster-frustum bypasses remained disabled. The evidence does
not distinguish an error in the remapped-depth producer from an error in
Metro's CPU consumer, so do not claim the depth-reconstruction math itself is
proven to be the sole root cause.

The accepted source policy is in `VRPose.cpp`:

- CPU screen-occlusion bypass: on by default when Expanded Visibility is active
- Object-frustum bypass: marker-only, off by default
- Cluster-frustum bypass: marker-only, off by default
- `vr_vis_occlusion_bypass_off.txt`: diagnostic rollback only

The previously used `_on` marker has been renamed in the game directory to
`vr_vis_occlusion_bypass_on.tested-run-c.txt`; it is preserved evidence and is
not recognized as an active flag.

## Right-eye flicker isolation and accepted policy

After extended play across multiple levels, two recordings showed repeated
surface/effect details flickering only in the right eye. A marker-only A/B on
the exact `C56DFE...` DLL changed only `vr_fold_scene_off.txt`; it removed the
flicker. fpsVR recorded 65.22 average fps, 8.46 ms average GPU time, 7.38 ms
average CPU time and 9.5% reprojection over 131.5 seconds at 72 Hz and
2880x3060. This was an extended-level check, not the same-save controlled area
used for A-D, so use it to show absence of an obvious performance loss rather
than as a direct numeric comparison.

Source inspection explains the eye specificity: the generic folded vertex
shader rewrites only `SV_Position` for eye 1. It cannot reconstruct arbitrary
view-dependent interpolants consumed by Metro's depth/G-buffer passes. A
deny-list of individual shader hashes had already accumulated and still missed
common surfaces, so adding another hash would not be a robust cross-level or
cross-GPU fix.

The accepted source makes broad scene/depth folding opt-in through
`vr_fold_scene_on.txt`. `vr_fold_scene_off.txt` wins if both markers exist.
Neither marker is shipped or active in the validated game folder; release
behavior is safe by default. The final built-in-policy run removed the flicker
and retained no-pop-in behavior. The user reported similar performance; fpsVR
recorded 67.70 average fps, 9.48 ms average GPU time, 8.20 ms average CPU time
and 5.3% reprojection over 272.8 seconds at the same 72 Hz and 2880x3060.

The marker A/B, final candidate, PDB, rollback DLL, compatibility reports and
fpsVR JSON captures are preserved at:

`C:\Projects\Metro2033ReduxVR-PR2-Review\dist\PR2-RightEyeFlicker-20260921`

## Diagnostic cleanup

Temporary compatibility diagnostics added locally in commit `54ec609` were
removed before final integration. Removed items include extended adapter/device
fields, DXGI video-memory budget snapshots, stereo-policy snapshots, draw-path
counter summaries, and the one-shot OpenVR compositor timing snapshot.

The original concise `vr_compatibility_log.txt` remains because it is part of
the tester compatibility path. The contributor's `VRPerf` profiler remains
compiled but inactive unless its explicit marker files are present. Release
packaging must exclude all profiler and rollback marker files, PDBs, caches,
logs, captures, and dumps.

## Verification already completed

- All eight self-contained C++ tests passed.
- All public Python and PowerShell tests passed.
- VR-menu policy verification passed.
- Camera-ownership verification passed.
- Release-binary diagnostic guard passed.
- Clean `Release | x64` build passed.
- Headset validation passed with the built-in policy and no active `_on` marker.
- Extended multi-level testing exposed the broad-fold right-eye flicker.
- Marker-only isolation removed the flicker without an obvious performance loss.
- The final `33DE6EA9...` built-in safe-fold candidate passed a 4.5-minute
  headset run with no flicker, no renewed object pop-in and similar perceived
  performance.

Static tests do not replace the completed headset runs. Additional levels,
headsets, runtimes and AMD GPUs still require broader community coverage.

## Do not use these as the accepted source

Two rejected occlusion-neighborhood experiments are preserved only in the named
Git stash made during final integration. Do not restore, merge or deploy them.

Do not return to the original PR default (CPU screen occlusion enabled), even
though its comments claim the remap can only under-cull. The controlled headset
test disproved that claim for at least one real scene/runtime combination.

## Next-chat priorities

1. Test additional levels, scripted scenes, menus, pre-rendered video,
   hands/weapons, lights, decals, water, and reflection-heavy areas.
2. Obtain at least one AMD GPU report and additional OpenVR runtime/headset
   coverage. Do not claim universal compatibility from the NVIDIA Quest 3 run.
3. Confirm no profiler/rollback marker files exist in the release payload.
4. Build a new tester archive from the clean public-source line, run installer,
   uninstaller, manifest and clean-install tests, then publish only after the
   resulting ZIP is downloaded back and hash-verified.
5. Keep the release labeled as a test/preview until broader reports arrive.

Suggested prompt for the next chat:

> Continue the Metro 2033 Redux VR PR2 performance integration from
> `docs/HANDOFF_PR2_PERFORMANCE_2026-09-21.md`. Start by reading that file and
> the development log. Continue compatibility and regression validation from
> the headset-accepted `33DE6EA9...` DLL. Preserve the accepted CPU
> screen-occlusion bypass and safe scene-fold defaults; do not restore the
> rejected occlusion-neighborhood experiments or enable broad scene/depth folds.
