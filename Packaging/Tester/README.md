# Metro 2033 Redux VR test package

This directory is the installer template used in generated test packages. A packaged release also contains a generated `Payload/` directory, `payload-manifest.json`, and license files. Those generated files are deliberately absent from source control.

## Requirements

- Steam version of Metro 2033 Redux x64 on Windows 10 or 11
- SteamVR/OpenVR configured and working before Metro starts
- a tracked headset and two motion controllers

The installer accepts only the verified Steam Metro executable version 1.0.0.3 with SHA-256 `183EF65212E351C55A2832C3F3C8B04616B153697913D33B1E27683163F14E15`. The Epic Games Store executable has been tested and is not compatible with this build.

## Install a generated package

1. Extract the complete ZIP to a normal folder; do not run it inside the ZIP.
2. Close Metro 2033 Redux.
3. Double-click `Install.cmd`.
4. Start SteamVR, then launch Metro normally.

The installer verifies the payload, backs up every replaced file, records the installation, and applies the renderer combination used to validate the VR mod: Medium quality, Very High tessellation, VSync Off, SSAA Off, and 16x texture filtering. It updates the active profile under `%LOCALAPPDATA%\4A Games\Metro 2033`, not a stale game-folder copy. Steam's most-recent profile can be derived before `user.cfg` exists. The installer does not replace saves or personal VR settings, and it refuses unrecognized or Epic executables.

## VR Controls

- Open the VR menu by holding both L3 and R3.
- Holding the right menu button recenters and recalibrates the left hand and the right hand/gun.
- Holding the left trigger opens the equipment menu; the right analog stick functions as the D-pad while it is open.
- Holding the left trigger also makes L3 open the journal and R3 open the pause menu.
- Holding up on the right thumbstick brings out the lighter.
- Holding down on the right thumbstick uses a med kit.

## VR Gestures

> [!WARNING]
> These gestures may not work reliably because they were not fully polished.

- Turn on the flashlight by pressing the left grip near the left side of your head.
- Bring out the gas mask by moving the left controller to the left side of your waist and holding the left grip, then bring the controller to your face while continuing to hold the grip.
- Replace the filter by moving the left controller to your chest and pressing the left grip, then bring the controller to your mouth while holding the grip and make a twisting motion. This should reset the filter time.
- Bring out the charger by moving the left controller to your waist and pressing the left grip. While holding the grip, pull the controller away from you.
- Reduce recoil by bringing the gun up to your eye.
- Use the 2x and 4x scopes by bringing the gun up to your eye.

If controller actions do not match, select Controller Preset 3 in Metro's controller options and restart the game.

## Uninstall

Close Metro and double-click `Uninstall.cmd` from the same extracted package. The uninstaller removes files that still match the installed release and restores each prior Metro graphics setting. If a setting or installed file was changed afterward, the uninstaller leaves that newer value in place and reports it.

## Reporting

Review logs for private path/system information before posting them. Include `d3d11_log.txt`, `vr_compatibility_log.txt`, hardware/runtime details, exact reproduction steps, and a short through-the-lens video for visual problems. Do not post unreviewed crash dumps publicly.

See `TESTING-NOTES.md` for the regression checklist.
