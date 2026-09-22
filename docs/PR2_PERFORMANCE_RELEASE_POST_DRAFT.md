# Draft GitHub post — PR2 performance test release

## Suggested title

Metro 2033 Redux VR `v0.1.0-test.4` — performance preview

## Ready-to-edit post

This test release integrates the performance work from community contributor
[@dubrovskiy-yevhen-stakelogic](https://github.com/dubrovskiy-yevhen-stakelogic)
in [pull request #2](https://github.com/D-Rey86/Metro2033ReduxVR/pull/2), with an
additional culling regression fix based on controlled headset testing.

### What changed

- Significantly reduced CPU and GPU frame time in the tested scene.
- Added the PR's safe stereo batching/folding work, shader and hot-path caching,
  and removal of several synchronous GPU readbacks.
- Retained the VR-safe CPU screen-occlusion bypass. The original PR default
  caused visible world objects to pop in and out in one repeatable test area;
  keeping only this bypass removed the regression while preserving nearly all
  of the measured performance gain.
- Disabled broad depth/G-buffer folding by default after extended multi-level
  testing exposed right-eye-only surface flicker. The generic folded shader can
  correct clip position but cannot rebuild every view-dependent shader output.
  Returning those passes to the proven two-eye path removed the flicker while
  retaining similar measured performance. The unsafe broad mode remains an
  unshipped developer opt-in only.
- Includes the PR's corrections for floor/bloom reflections, lamp flares,
  light-culling coverage, and the chapter-select television picture.
- Keeps the existing Test 3 monitor-independent presentation behavior and
  Steam-only installer safeguards.
- Removes development-only performance and compatibility diagnostics from the
  release runtime. The normal concise compatibility report remains available
  for support.

### Local comparison

The same save and exact location were tested at Medium quality on a Quest 3
through Virtual Desktop, SteamVR at 72 Hz and 2880x3060 per eye, using an RTX
5070 Ti and Ryzen 7 7800X3D:

- Public Test 3: 50.4 effective FPS, with no pop-in.
- Exact original PR2 build: 65.8 effective FPS, but repeatable object pop-in.
- Final built-in culling policy: 64.04 average / 71 median FPS, with no pop-in
  observed in the regression location.

A later 4.5-minute multi-level validation of the final safe-fold build averaged
67.70 FPS at the same resolution and refresh rate. It showed neither the
right-eye flicker nor renewed object pop-in. Because it covered different level
content, it is supporting regression evidence rather than a direct A/B against
the controlled scene above.

These results describe one system and one controlled scene. They are not a
guarantee of the same improvement on every headset, GPU, level, or runtime.

### Compatibility status

- Steam version only. The Epic Games Store executable is not currently
  supported.
- NVIDIA/Quest 3/Virtual Desktop received the final controlled validation.
- AMD GPUs, additional headsets, Steam Link, wired SteamVR configurations and
  more levels still need broader community coverage.
- This remains a test release. Please keep Test 3 available as a rollback.

### Installation

1. Download the attached tester ZIP. `[ADD FINAL ZIP NAME AND SHA-256]`
2. Run the included installer and select the Steam Metro 2033 Redux folder that
   contains `metro.exe`.
3. Start SteamVR before launching Metro.
4. Do not copy development marker files, PDBs, logs, shader caches, captures, or
   old DLLs into the game folder.

The installer configures the validated Metro settings: Medium quality, Very
High tessellation, VSync off, SSAA off, and 16x texture filtering. Headset render
resolution and the mod's VR resolution scale remain separate controls.

### Please include this with reports

- GPU and driver version
- CPU
- Headset and connection/runtime (Virtual Desktop, Steam Link, wired SteamVR,
  etc.)
- SteamVR refresh rate and per-eye resolution
- Mod VR resolution scale
- Level/save location and exact symptom
- `vr_compatibility_log.txt`
- fpsVR or equivalent frame-time information when reporting performance

Known deferred issues from earlier testing include the shooting-range-only aim
offset, an isolated decal-flicker location, and bottom-of-view disappearance for
some sewer water. Please report if any of these appear elsewhere.

Thanks to everyone testing and especially to the PR author for the substantial
performance work. This preview is intended to gather enough NVIDIA, AMD,
headset, and level coverage to decide whether the performance path is ready to
replace Test 3 as the default release.

