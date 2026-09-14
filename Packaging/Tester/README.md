# Metro 2033 Redux VR test package

This directory is the installer template used in generated test packages. A packaged release also contains a generated `Payload/` directory, `payload-manifest.json`, and license files. Those generated files are deliberately absent from source control.

## Requirements

- Metro 2033 Redux x64 on Windows 10 or 11
- SteamVR/OpenVR configured and working before Metro starts
- a tracked headset and two motion controllers

The installer recognizes the verified Metro executable version 1.0.0.3 with SHA-256 `183EF65212E351C55A2832C3F3C8B04616B153697913D33B1E27683163F14E15`. It can discover Steam and Epic installations, but storefront discovery does not prove that an executable build is compatible.

## Install a generated package

1. Extract the complete ZIP to a normal folder; do not run it inside the ZIP.
2. Close Metro 2033 Redux.
3. Double-click `Install.cmd`.
4. Start SteamVR, then launch Metro normally.

The installer verifies the payload, backs up every replaced file, records the installation, and applies the renderer combination used to validate the VR mod: Medium quality, Very High tessellation, VSync Off, SSAA Off, and 16x texture filtering. It does not replace saves or personal VR settings. An unrecognized `metro.exe` hash produces a warning and requires explicit confirmation.

## Controls

- Hold both stick clicks for about one second: open/close the VR menu
- Left trigger + right stick click: recenter VR
- Left trigger + left stick click: open/close the journal
- Hold left trigger briefly: equipment inventory
- Bring a scoped weapon near the eye: scope/ADS behavior

If controller actions do not match, select Controller Preset 3 in Metro's controller options and restart the game.

## Uninstall

Close Metro and double-click `Uninstall.cmd` from the same extracted package. The uninstaller removes files that still match the installed release and restores each prior Metro graphics setting. If a setting or installed file was changed afterward, the uninstaller leaves that newer value in place and reports it.

## Reporting

Review logs for private path/system information before posting them. Include `d3d11_log.txt`, `vr_compatibility_log.txt`, hardware/runtime details, exact reproduction steps, and a short through-the-lens video for visual problems. Do not post unreviewed crash dumps publicly.

See `TESTING-NOTES.md` for the regression checklist.
