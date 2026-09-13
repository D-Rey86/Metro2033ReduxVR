# Building and packaging

## Requirements

- Windows 10 or 11 x64
- Visual Studio 2022 with **Desktop development with C++**
- a Windows 10 or 11 SDK
- PowerShell 5.1 or newer for the installer tooling
- SteamVR for runtime testing
- a legally owned Windows x64 copy of Metro 2033 Redux

The required OpenVR import library/runtime and OpenXR headers are vendored under `ThirdParty/` with their licenses. No game files are included.

Only the DirectXTK library for the supported `Release | x64` target is vendored; legacy x86 and debug configurations are not self-contained in this repository.

## Compile

1. Open `ThirdParty/3Dmigoto/StereovisionHacks.sln`.
2. Select `Release` and `x64`.
3. Build the `DirectX11` project.
4. Find the output under `ThirdParty/3Dmigoto/builds/x64/Release/`.

The build copies `ThirdParty/3Dmigoto/Dependencies` and `openvr_api.dll` beside `d3d11.dll`.

For a command-line build from a Visual Studio developer shell:

```powershell
msbuild ThirdParty\3Dmigoto\StereovisionHacks.sln /m /t:DirectX11 /p:Configuration=Release /p:Platform=x64
```

Run the self-contained native and static tests with:

```powershell
.\Tools\run_tests.ps1
```

## Shader replacements

`ShaderFixes/` contains the six Metro-specific pixel-shader replacement sources used by the current package. They are kept separate from captured shader dumps. Compile them with 3Dmigoto's normal shader compiler for the correct shader model, then place the resulting hash-named `.bin` files in the package's `ShaderFixes` directory.

Do not commit generated shader bytecode, captured frames, FrameAnalysis folders, RenderDoc captures, game files, logs, or symbols.

## Create a test package

Use `Packaging/Build-Package.ps1` with an explicit runtime directory. That directory must contain the complete, already-tested runtime payload intended for the game directory. The script never reads from a hard-coded Metro installation and refuses to overwrite an existing package directory.

```powershell
.\Packaging\Build-Package.ps1 `
  -RuntimeDirectory .\ThirdParty\3Dmigoto\builds\x64\Release `
  -OutputDirectory .\dist\Metro2033ReduxVR-Test
```

Inspect the generated payload and manifest before zipping or distributing it. The public repository intentionally contains no prebuilt release.

## Runtime safety

- Preserve a known-good build before testing renderer changes.
- Change one measured behavior at a time.
- Validate gameplay, menus, scripted scenes, hands, weapons, effects, decals, and both eyes after renderer changes.
- A successful compile or static test is not proof of visual correctness.
