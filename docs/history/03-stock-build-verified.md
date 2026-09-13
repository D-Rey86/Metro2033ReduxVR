# Stock 3Dmigoto Build Verified — 2026-07-28

- Cloned `bo3b/3Dmigoto` (commit `4ce5f2f`) into `ThirdParty/3Dmigoto`.
- Retargeted `WindowsTargetPlatformVersion` from the pinned `10.0.19041.0` to
  the installed `10.0.26100.0` across the 6 vcxproj files that hardcoded it
  (`D3D_Shaders`, `Injector`, `DirectX11`, `NVAPI`, `BinaryDecompiler`,
  `cmd_Decompiler`) — `PlatformToolset v143` was already fine for VS2022.
- Built `Release|x64` via MSBuild — succeeds, 0 errors. Output:
  `ThirdParty/3Dmigoto/builds/x64/Release/d3d11.dll` (+ nvapi64.dll,
  d3dcompiler_47.dll).
- Note: `CopyToGames.bat` (postbuild step) is gated to bo3b's own dev
  username and no-ops on this machine — confirmed it didn't touch any other
  game installs.
- Deployed stock `d3d11.dll`/`nvapi64.dll` + Helix Mod's game-specific
  `d3dx.ini`/`ShaderFixes/` (from `GameReferences/`) into
  `<METRO_INSTALL>\`.
- Verified load: temporarily set `[Logging] calls=1`, launched `metro.exe`,
  confirmed `d3d11_log.txt` shows `D3D11 DLL starting init - v 1.4.9` with
  the full Helix Mod ini parsed correctly (shader_hash=3dmigoto,
  override_directory=ShaderFixes, stereo_params=125, ini_params=120, etc).
  Reverted `calls` back to `0` after.

**Build pipeline confirmed working end-to-end**: our own compiled 3Dmigoto
hooks this exact game and loads the existing Metro-specific shaderfix. Ready
to start modifying source rather than debugging the toolchain.

## Next up: RenderDoc capture (task 3)

RenderDoc isn't installed on this machine. Needed to confirm `cb1`
(`cb_main_matrices1`, see `02-shaderfix-inspection-findings.md`) is bound
with live camera data during actual first-person gameplay, not just the
lens-flare/HUD shaders where the layout was found in static source. This
requires actually being in a level (not the main menu) when the capture is
taken — that part needs a human at the keyboard, not something drivable
headlessly.
