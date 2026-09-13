# VR Pose Integration Implemented — 2026-07-28

Implemented per the approved private development plan (not published).

## What was built

- `ThirdParty/openvr/` — cloned from `ValveSoftware/openvr` (headers +
  `lib/win64/openvr_api.lib` + `bin/win64/openvr_api.dll`), `.git` stripped
  so it's tracked directly like the 3Dmigoto fork.
- `DirectX11/VRPose.h`/`.cpp` — new module:
  - `VRPose::EnsureInitialised()`: lazy `vr::VR_Init(..., VRApplication_Background)`,
    called every `Present` but only actually attempts init once. Chose
    `VRApplication_Background` deliberately so we never force SteamVR to
    launch or stay running - we only want pose queries, we're not
    submitting stereo frames to the compositor in milestone 1.
  - `VRPose::UpdateVRPose()`: polls `GetDeviceToAbsoluteTrackingPose`,
    converts the HMD's row-major 3x3 rotation into `G->vrHeadRotation3x3`.
    **The OpenVR->game-space axis conversion is currently an identity copy**
    (see the big comment in `VRPose.cpp`) - genuinely unverified until
    tested in-headset, deliberately isolated into one small function so
    it's a quick fix once we see how it actually behaves.
  - `VRPose::ComputeRigidInverse()`: small math helper, `R^T` / `-R^T*T`,
    used to keep `m_iV` consistent with a patched `m_V`.
- `HackerContext::PatchVRCameraConstantBuffer()` (new) +
  `VSSetConstantBuffers` (modified): when a bind touches VS slot 1 and
  `G->vrHeadRotationValid`, copies the real `cb_main_matrices1` buffer into
  a lazily-created shadow buffer (`HackerDevice::GetOrCreateVRCameraCBShadow`),
  overwrites only the first 96 bytes (`m_V`+`m_iV`) with patched rotation +
  the original translation, and binds the shadow buffer instead. Original
  translation is refreshed once per frame via a small 48-byte GPU->CPU
  staging readback (`GetOrCreateVRCameraCBReadbackStaging`) rather than every
  draw, since Notes/04's capture showed `m_V` is constant across all draws
  in a frame (only `m_P` legitimately varies between world/weapon passes) -
  one stall per frame, not per draw.
  - Used `UpdateSubresource1` (not `UpdateSubresource`) for the partial
    96-byte box write - classic `UpdateSubresource` requires a NULL box for
    constant buffers; the D3D11.1 partial-update path (`ConstantBufferPartialUpdate`)
    is what allows a boxed write here, and this codebase already targets
    `ID3D11DeviceContext1` throughout.
  - Everything is gated on `G->vrHeadRotationValid` - false (no headset)
    means `VSSetConstantBuffers` is the exact original bare passthrough.

## Build

Added `VRPose.cpp`/`.h` to `DirectX11.vcxproj` (forgot on the first attempt -
got LNK2001 unresolved externals, fixed by adding the `<ClCompile>`/
`<ClInclude>` entries), plus `openvr/headers` include dir, `openvr_api.lib`
link dependency, and a post-build `xcopy` for `openvr_api.dll` - all on the
`Release|x64` config only (the one we actually build/test). Builds clean, 0
errors.

## Verification so far (no headset attached)

Deployed, ran with `[Logging] calls=1` temporarily: `d3d11_log.txt` shows
`VRPose: vr::VR_Init failed: Not starting vrserver for background app (121)`
- expected, since SteamVR isn't running right now and `VRApplication_Background`
apps intentionally don't auto-launch it. Game ran normally for 20s, no
crash. Confirms the no-headset fallback path: `vrHeadRotationValid` stays
false, `VSSetConstantBuffers` passthrough is unchanged, zero behavior
change for flat-screen play.

## Next (task 5)

Real test needs SteamVR actually running (Virtual Desktop connected to the
Galaxy XR) so `VR_Init` succeeds. Expect the axis-mapping identity-copy
placeholder to need at least one correction - watch for inverted
pitch/swapped roll/etc. and fix in `ConvertOpenVRRotationToGameSpace`
(`VRPose.cpp`). Every 90 frames the log prints the current rotation matrix
(`VRPose: rotation [...]`) for objective verification independent of how it
subjectively feels in the headset.
