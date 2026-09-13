# ARKTIKA.1 native VR reference

Date: 2026-08-17

## Scope

Offline inspection of `<ARKTIKA_REFERENCE_INSTALL>\arktika1.exe`, the native VR 4A Games title supplied as an engine reference for the Metro 2033 Redux VR mod.

No Metro source or deployed DLL was changed during this inspection.

## Findings

ARKTIKA.1 is a 64-bit build of a newer 4A Engine generation. It dynamically loads the Oculus SDK rather than importing Oculus functions statically. The executable contains the expected tracking, input, frame submission, swapchain, and haptics API names, including tracking-state, input-state, render-description, frame submission, and controller-vibration functions.

The engine-side VR layer is visibly broader than the runtime API. Relevant native configuration and system names include:

- `vr_use_xform`
- `vr_dbg_skip_update_view`
- `vr_update_rootp`
- `vr_recenter_delay`
- `vr_rotate_body`
- `vr_attach`
- `vr_weapon`
- `vr_input_flag`
- `vr_tracker_offset_pos`
- `vr_tracker_offset_rot`

At least `vr_dbg_skip_update_view` and `vr_use_xform` are live configuration objects: their strings are referenced by data structures with backing storage and dedicated accessor functions. They are not merely unused text in the binary.

The executable also contains a native VR weapon/attachment type family, including `WEAPON_ITEM_VR`, `WEAPON_ITEM_VR_ATTACH`, `VR_WEAPON`, and related VR weapon classes.

Relevant weapon/recoil names include:

- `weapon_trigger`
- `weapon_aimed`
- `weapon_sight_occluded`
- `recoil_vert`
- `recoil_horiz`
- `recoil_shake`
- `recoil_curve_vert`
- `recoil_curve_horiz`
- `recoil_curve_blend`
- `recoil_curve_return`
- `recoil_increase`
- `recoil_duration_coef`
- `recoil_vert_coef`
- `recoil_horiz_coef`
- `recoil_decrease_speed`
- `recoil_shake_accrue`
- `recoil_shake_falloff`
- `recoil_shake_coef`

## What this can contribute to Metro

The most useful comparison targets are the 4A Engine-side concepts, not the Oculus calls themselves:

1. Separating HMD/root transforms from weapon transforms.
2. Updating a VR root or view transform without treating ordinary head motion as scripted camera ownership.
3. Attaching weapons through a native VR weapon/attachment path rather than relocating the engine's already-computed viewmodel state.
4. Preserving native recoil state while applying a VR aiming/recoil-suppression condition.
5. Using explicit VR control variables and attachment state instead of heuristic camera-motion detection.

## Limitations

This is not a drop-in OpenXR implementation. ARKTIKA.1's runtime boundary is Oculus-specific, and its newer engine layout will not match Metro's addresses or object layouts. The binary can provide patterns, data-model clues, and candidate engine concepts, but Metro still requires independent verification before any hook is changed.

## Next investigation

Use the ARKTIKA findings as a reference while examining Metro's existing weapon and camera hooks. Prioritize locating an engine control/ownership state or weapon VR attachment state. Do not resume the discarded camera-only scripted-scene diagnostics, and do not modify Metro based solely on matching strings.
