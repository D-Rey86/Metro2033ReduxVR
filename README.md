# Metro 2033 Redux VR

An experimental 6DOF VR mod for the Windows x64 version of **Metro 2033 Redux**. It adds stereoscopic rendering, head tracking, motion-controller aiming and input, VR-aware hands and weapons, and an in-headset configuration menu.

This repository is the clean public source tree. It intentionally does not contain Metro game files, captured frames, shader dumps, crash dumps, private test packages, or the project's large experimental history. You must own Metro 2033 Redux to use or develop the mod.

## Current state

The mod is playable, but it is still an unfinished community project rather than a polished release.

Working areas include:

- OpenVR/SteamVR headset tracking and true stereoscopic rendering
- room-scale head translation and controller-driven weapon aiming
- locomotion, snap/smooth turning, recentering, and configurable world scale
- VR presentation for the main menu, HUD, journal, watch, lighter, charger, weapons, hands, and scopes
- compatibility-oriented render resolution and frame-pacing controls
- automatic Steam and Epic install discovery in the installer template

Important limitations:

- Rendering both eyes is expensive. A true single-pass stereo path is not finished.
- NVIDIA GPUs are the most-tested path; AMD compatibility still needs real hardware testing.
- The verified executable is Metro 2033 Redux 1.0.0.3 with SHA-256 `183EF65212E351C55A2832C3F3C8B04616B153697913D33B1E27683163F14E15`. Other storefront builds may work only if their executable matches or is separately validated.
- OpenVR is the supported runtime. The OpenXR backend is experimental and is not the normal launch path.
- Some hardware combinations have reported distant-scene flicker, menu double vision, or unusually low performance that has not yet been reproduced consistently.
- Known localized issues include gun-range-only aiming behavior and lower-view clipping on one sewer water effect.

See [project status](docs/STATUS.md), [performance work](docs/PERFORMANCE.md), and [initial source provenance](docs/PROVENANCE.md) before starting a fix.

New maintainers and coding agents should begin with the [maintainer/AI handoff](docs/AI_HANDOFF.md). The sanitized [full development log](docs/DEVELOPMENT_LOG.md) preserves the chronological investigation history and failed approaches, while the [historical engineering notes](docs/history/README.md) retain deeper subsystem handoffs cited by the log.

## Building

The project currently targets Windows x64 and Visual Studio 2022 with the Desktop development with C++ workload and a Windows SDK. Open [StereovisionHacks.sln](ThirdParty/3Dmigoto/StereovisionHacks.sln), select `Release | x64`, and build the `DirectX11` project.

The output is written under `ThirdParty/3Dmigoto/builds/x64/Release/`. Detailed instructions and packaging boundaries are in [BUILDING.md](BUILDING.md).

## Testing

Do not install a development build over the only copy of a working game setup. Use a clean Metro installation or keep separate clean/test game directories. The installer template verifies the known executable hash, rejects diagnostic-enabled runtime configurations, backs up replaced files, records hashes, applies the tested Metro graphics combination, and supports safe uninstall.

When reporting a problem, include:

- headset, controllers, GPU, VR runtime, connection method, and refresh rate
- SteamVR per-application render resolution and the mod's resolution scale
- Metro executable SHA-256 and storefront
- `d3d11_log.txt` and `vr_compatibility_log.txt` from the affected run
- level/checkpoint and exact reproduction steps
- a short through-the-lens video for stereo, placement, flicker, or motion issues

Logs can contain system and path information. Review them before posting publicly. Crash dumps should be shared privately unless you have checked their contents.

## Contributing

The most valuable contributions are reproducible compatibility investigations, profiling, AMD testing, and a correct low-regression stereo optimization. Please read [CONTRIBUTING.md](CONTRIBUTING.md), [architecture](docs/ARCHITECTURE.md), and [ROADMAP.md](ROADMAP.md).

## License and attribution

The modified 3Dmigoto-derived code is distributed under GPL-3.0. OpenVR and OpenXR retain their own licenses. See [LICENSE](LICENSE) and [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).

Metro 2033 Redux and the 4A Engine are property of their respective owners. This is an unofficial fan project and is not affiliated with or endorsed by 4A Games, Deep Silver, Valve, or Khronos.
