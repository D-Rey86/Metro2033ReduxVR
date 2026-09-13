# Maintainer and AI handoff

This is the shortest reliable path to resume work without losing the project's hard-won context.

## Read in this order

1. `README.md` for scope, supported runtime, and compatibility boundaries.
2. `docs/STATUS.md` for the current accepted behavior and unresolved issues.
3. `docs/ARCHITECTURE.md` for ownership boundaries in the renderer.
4. `docs/PERFORMANCE.md` for the current performance evidence and requirements for a stereo optimization.
5. `docs/DEVELOPMENT_LOG.md` when investigating a specific subsystem or hypothesis.
6. `docs/history/` when the log points to a detailed numbered engineering note.
7. The current source and tests; they override prose when documentation is stale.

`DEVELOPMENT_LOG.md` is deliberately comprehensive and chronological. Search it by subsystem, shader hash, function name, checkpoint, or symptom instead of reading all of it linearly. Later dated entries supersede earlier candidates. Anything labeled rejected, rolled back, diagnostic, candidate, or awaiting headset validation is not an accepted fix.

## Verified public baseline

- Public root commit: `6896f82` (`Initial public source release`)
- Exported private integration baseline: `cd66f2b` (`Merge expanded visibility toggle fix`)
- Supported target: `Release | x64`
- Verified Metro executable SHA-256: `183EF65212E351C55A2832C3F3C8B04616B153697913D33B1E27683163F14E15`
- Supported runtime: OpenVR/SteamVR; OpenXR is experimental
- Clean-clone build and all self-contained tests passed at export time

The public `main` branch intentionally excludes an uncommitted single-pass performance experiment that existed in the private working directory. That experiment was not a verified replacement for the accepted baseline and must not be reconstructed by guessing. The existing `StereoSinglePass.*`, `StereoTwin.*`, and performance documentation provide the committed starting point for a measured implementation.

## Before making a change

- Confirm the repository is clean and preserve a known-good build.
- Reproduce the issue and identify the owning state, draw, shader, or native path.
- Read the relevant history in `DEVELOPMENT_LOG.md`, including failed approaches.
- State what is proven versus assumed before deploying a candidate.
- Do not broaden a menu/script/gameplay classifier without checking the other states it can affect.
- Do not treat a compile, static test, or desktop mirror as proof of headset correctness.

## Minimum verification

Run:

```powershell
.\Tools\run_tests.ps1
```

Build `ThirdParty/3Dmigoto/StereovisionHacks.sln`, target `DirectX11`, configuration `Release | x64`. For runtime changes, verify both eyes and the relevant checkpoint, then check normal gameplay, main menu, a scripted scene, hands/weapons, HUD, decals, particles, and lighting paths likely to share the changed state.

## Current best next project

Performance is the highest-value unfinished area. Measure CPU and GPU ownership before changing stereo execution. The accepted baseline is multi-pass/twin rendering where required; earlier broad single-pass attempts produced shader flicker without a verified large gain. A new candidate should be narrow, disabled by default while experimental, and evaluated with stable frame-time captures on more than one runtime/GPU when possible.

## Missing historical artifacts

The development log and historical notes name private `Builds/`, `GameReferences/`, videos, captures, and memory dumps. Those artifacts are intentionally absent because they include generated binaries, copyrighted observations, large captures, or local-machine details. Their absence does not affect a clean build. If an investigation genuinely requires one, reproduce the evidence from a legally owned game copy and document the method rather than committing the artifact.
