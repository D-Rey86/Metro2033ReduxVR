# Architecture

Metro 2033 Redux has no public VR integration or game modding SDK. The mod therefore runs as a D3D11 proxy based on 3Dmigoto and adds game-specific VR behavior at the renderer and narrowly verified native-state boundaries.

## Major layers

- **D3D11 proxy and state tracking:** `ThirdParty/3Dmigoto/DirectX11/`
- **OpenVR runtime integration:** pose acquisition, eye transforms, texture submission, controller input, resolution, and timing
- **Game-specific native integration:** verified camera, input, visibility, equipment, menu, and scene-state adapters
- **Stereo rendering:** per-eye constants, draw routing, depth behavior, shader replacement, and compositor submission
- **VR presentation:** menu/HUD surfaces, hands and weapon transforms, scopes, journal, watch, lighter, charger, reticle, and comfort controls
- **Configuration:** `ThirdParty/3Dmigoto/Dependencies/d3dx.ini` plus the in-headset menu
- **Installer:** `Packaging/Tester/`, with hash verification, backups, install records, and safe uninstall

## Runtime model

The established path renders Metro's D3D11 work for two eyes and submits the eye textures through OpenVR. This is compatible with the game's existing render organization but has a substantial CPU/GPU cost. The source contains experimental groundwork for alternative stereo execution; it should not be treated as a completed single-pass implementation.

State-sensitive fixes must be scoped to the actual game or render state that owns the issue. A menu-only visibility override, for example, must not suppress hands during scripted gameplay. This project has already encountered several regressions caused by broad classifications, which is why evidence and cross-scene validation are required.

## Resolution

The runtime obtains the headset-recommended eye texture size and applies the user-selected mod scale. Headset aspect ratios differ, so the output should follow the runtime recommendation rather than forcing one universal width and height. Metro's own internal image quality and the submitted eye texture size are related but not interchangeable.

## Source boundaries

The public tree contains authored source and the minimum third-party dependencies needed to build. Reverse-engineering captures, executable dumps, symbols, frame captures, and private test artifacts are deliberately excluded.

