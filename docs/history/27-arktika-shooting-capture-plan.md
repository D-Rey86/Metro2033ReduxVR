# ARKTIKA.1 shooting capture plan

Date: 2026-08-17

## Purpose

Use the native VR title as a reference for the firing pipeline, not as a source of copyable addresses. The useful comparison is the live sequence from VR weapon pose through native spread/recoil, raycast, and hit result.

## Data to capture

- weapon identity and active weapon object;
- VR weapon/controller pose;
- shot origin and endpoint/direction before the native trace;
- native spread/recoil direction before and after the shot;
- raycast range or maximum range;
- final hit point and hit distance;
- ADS/aim state and recoil suppression state;
- the object/function that receives the VR direction before hit detection.

## Additional static targets found in ARKTIKA

The executable contains direct names for the values worth correlating at runtime:

- `_last_fire_dir`, `_last_fire_pos`, and `_last_hit_weapon_clsid`;
- `fire_distance`, `blt_fire_distance`, `blt_fire_distance_min`, and `blt_fire_distance_max`;
- `fire_dispersion`, `bullet_start_disp`, and `num_shot_dispersed`;
- `firepoint_locators` and `_firepoint_xform`;
- `g_vr_mt_bezier_ray_query`;
- `weapon_trigger`, `weapon_aimed`, `fire_begin`, `fire_end`, `aimed_fire`, and `aimed_one_fire`;
- `VR_WEAPON`, `VR_WEAPON_MODULAR`, and `WEAPON_ITEM_VR_ATTACH`.

This means the pistol range is already sufficient for the first capture. ADS-specific values can be added later if the game exposes an aimed pistol state, but the pre-trace origin/direction/range path can be learned without a second weapon.

## Comparison target in Metro

Metro already has bounded hooks at the pre-fire trace, pre-fire preparation, vector-copy, and fire-wrapper stages. The post-trace `+0x150` rewrite is known to be too late for reliable long-range VR shooting. The intended fix is to move the VR direction into the equivalent pre-trace input while leaving Metro's native spread, recoil, collision, and impact code in control.

## Capture policy

The runtime capture should be one short shooting-range session and should log only shot-related calls. It must not alter ARKTIKA gameplay. Static inspection will identify the candidate call sites first; a runtime probe will then confirm the live argument layout before Metro is changed.
