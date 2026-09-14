# Tester notes

This build has extensive automated and headset validation on its development
machine, but compatibility across hardware and clean Steam profiles remains the
purpose of this package.

Please exercise at least the following:

1. Cold start, title/loading screens, and a new-game prologue.
2. Physical head movement, physical turning, smooth turning, and recentering.
3. Several weapons, reloads, firing at different angles, scopes, and lasers.
4. Left and right hands before and after clothing/level transitions.
5. Watch clock, gas-mask filter timer, lighter, charger, flashlight, and mask.
6. Journal opening/closing, then opening it again after changing levels.
7. Weapon vendor, scripted camera scenes, Defense, Market, Armory, and Archives.
8. VR-menu changes followed by a complete game restart.
9. Frame pacing with Expanded Visibility enabled and disabled.
10. Confirm Metro reports Medium quality, Very High tessellation, VSync Off,
    SSAA Off, and 16x texture filtering after installation.

The desktop mirror must not show 3Dmigoto's green shader-hunting overlay or a
green `Stereo disabled` line. That text refers to legacy NVIDIA 3D Vision, not
the OpenVR output, and indicates that a development configuration was packaged
by mistake.

If performance becomes persistently lower than an earlier run, close Metro and
restart SteamVR before comparing settings. One controlled development session
showed SteamVR application timing improve materially after a runtime restart;
this is a troubleshooting baseline, not a claimed performance fix.

Known limitations:

- Only the 1.0.0.3 executable hash listed in the package manifest is currently
  verified. Steam and Epic installs are both discoverable, but a different
  storefront executable hash requires separate runtime validation.
- The production runtime is OpenVR/SteamVR; the experimental OpenXR path is not
  the supported configuration.
- The intro video and some non-gameplay screens have historically needed
  additional stereo work.
- A genuinely open journal using an unknown book mesh variant may fall back to
  the native text path; known book geometry is page-attached.
- Expanded Visibility fixes headset-edge disappearance by submitting a wider
  visibility envelope and can have a performance cost.

Do not install ReShade, another D3D11 proxy, or another 3Dmigoto package into
the same game folder for the first test. Their `d3d11.dll` would conflict with
this mod and invalidate the baseline.
