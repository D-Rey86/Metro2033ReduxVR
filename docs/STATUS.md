# Project status

Status at the initial public-source export.

For detailed subsystem history, exact rejected hypotheses, calibration evolution,
and shader/native-path evidence, see `DEVELOPMENT_LOG.md`. It is historical and
chronological; later entries and current source supersede earlier candidates.

## Verified baseline

- Windows x64 Metro 2033 Redux 1.0.0.3
- executable SHA-256: `183EF65212E351C55A2832C3F3C8B04616B153697913D33B1E27683163F14E15`
- SteamVR/OpenVR runtime path
- most development and visual verification performed on NVIDIA hardware
- Quest-class, PC-streamed headsets and PSVR2 have received hands-on testing, but the matrix is not exhaustive

## Implemented

- stereo HMD rendering and 6DOF head tracking
- tracked-controller aiming and game input routing
- VR hands, weapons, equipment, journal, watch, lighter, charger, scopes, reticle, and HUD treatment
- VR main menu and configuration UI
- resolution scaling, world/IPD controls, comfort turning, recentering, brightness, HUD, scope, and placement settings
- main-menu/scripted-scene state separation fixes
- shader and depth handling for several decal, lighting, culling, and pre-rendered-video problems
- clean-install-oriented Steam installer with executable verification, explicit Epic rejection, backups, manifest validation, uninstall records, release-safe 3Dmigoto validation, and enforcement of the tested Metro graphics combination
- a VR-owned 2560x1440 presentation surface so Metro's monitor-resolution setting no longer changes headset source geometry or splits pre-rendered video at 1080p

## Unresolved or incompletely validated

- stereo rendering remains performance-heavy
- AMD GPU behavior is not verified on real AMD hardware
- some testers report background flicker or menu double vision that has not reproduced on every setup
- the tested Epic Games Store version is incompatible; this release is Steam-only
- OpenXR is experimental
- one localized sewer-water effect can disappear near the lower edge of the view
- the shooting range has a localized aim/projectile discrepancy not seen in normal gameplay testing

## Historical note

The private development repository contains extensive experiments and captures. They were not published because they include large generated binaries, proprietary runtime observations, local paths, and failed candidates that obscure the verified baseline. The public history starts from the last committed integration baseline rather than rewriting that private history.
