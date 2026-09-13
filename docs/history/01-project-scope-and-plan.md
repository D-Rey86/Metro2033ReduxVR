# Project Scope and Milestone 1 Plan — 2026-07-28

## Baseline: what already exists for this game

- **vorpX DirectVR**: partial support only. Per user reports on the vorpX
  forum, only rotational tracking registers ("1 of 3 categories" in the
  DirectVR scan) — no positional tracking, no automatic FOV, and visible
  warping when looking around. Confirms this game's camera isn't a clean
  fit for vorpX's generic injector. Steam version works better than GOG.
- **Helix Mod / 3Dmigoto stereo-3D fix**: unlocks separation/convergence for
  3D Vision-style stereo display, targets Metro 2033 Redux + Last Light
  Redux specifically. This is depth-only (fixed camera, two offset renders)
  — no head tracking at all. But it's valuable as a *starting point*: it
  already has working shader/constant-buffer hooks into this exact game's
  renderer via 3Dmigoto, which is the hard part we'd otherwise have to
  rediscover from scratch with RenderDoc.
  - Package not yet pulled into `GameReferences/` — hosted on a personal
    blog via third-party file host (not GitHub), so this should be fetched
    manually rather than auto-downloaded. Need: the ini + shaderfix folder
    from https://helixmod.blogspot.com/2019/03/metro-redux-2033-last-light-update.html,
    dropped into `GameReferences/HelixModShaderfix/`.
- **4A Engine**: closed-source, no SDK, only a handful of titles ever built
  on it. No UEVR-equivalent community framework exists — there is no
  generalized injector for this engine the way there is for Unreal.

## Milestone 1 target

Rotational-only head tracking, replacing/augmenting mouse-look, with the
existing Helix Mod stereo hooks providing the per-eye rendering. No
positional tracking, no controller input remap, no HUD work yet — those are
separate milestones. Success criteria: turning your head in the Galaxy XR
(via Virtual Desktop → SteamVR) turns the in-game camera 1:1, logged and
verified even before subjective in-headset testing.

## Why OpenVR, not raw OpenXR

Virtual Desktop drives PCVR content through SteamVR on the PC side for the
Galaxy XR (confirmed via Virtual Desktop's own docs — it registers as a
SteamVR driver). So the PC-side code should poll pose via OpenVR's
`IVRSystem::GetDeviceToAbsoluteTrackingPose`, same API most existing PCVR
mods/tools (including vorpX) build against. No need for a raw OpenXR loader.

## Concrete steps

1. Clone `bo3b/3Dmigoto` from GitHub into `ThirdParty/3Dmigoto`, build stock
   (unmodified) with Visual Studio, verify it loads against `metro.exe`
   (drops in as `d3d11.dll` next to the exe) with the Helix Mod shaderfix
   config — validates the build pipeline before any of our code changes.
2. Use RenderDoc (`Tools/`) to capture a frame and confirm/expand on which
   constant buffer the Helix Mod ini hooks for view/projection data — the
   ini tells us the shader hash but not necessarily the full matrix layout.
3. Integrate the OpenVR SDK into our 3Dmigoto fork. On each frame (3Dmigoto
   already hooks `Present`), poll HMD orientation and inject it into the
   view matrix construction, replacing whatever mouse-look currently
   supplies rotation.
4. Test in-game: confirm 1:1 head rotation, log pose data to a debug file
   for verification independent of subjective headset feel.

## Open items / blockers

- Need to fetch the Helix Mod shaderfix package manually (see above) —
  not auto-downloaded here since it's a third-party blog/file-host binary
  drop, not an official repo.
- Need go-ahead to clone `bo3b/3Dmigoto` from GitHub into `ThirdParty/`.
