# Magnifying-scope attachment detection

Date: 2026-08-17

## Goal

Bring Metro's native magnifying-scope presentation up automatically when a
magnifying optic reaches the player's eye, while leaving iron and reflex sights
controller-pinned. Detection must follow the attachment across compatible guns,
not hard-code a weapon.

## Native projection result

Metro narrows its projection during ADS. Because the mod substitutes the OpenVR
eye projection, native ADS initially moved the weapon but showed no zoom. The VR
projection now receives a requested optical scale by multiplying P00 and P11
while preserving each eye's asymmetric center terms. The confirmed 2× scale is
1.744x.

Metro submits alternating world-camera and viewmodel-camera ADS projections.
Using their raw ratios made projection scale alternate too, producing different
AER-eye states, disappearing/moving geometry and unstable rendering. The active
implementation instead gives both eyes and every draw one fixed requested scale
through `VRPose::GetRequestedScopeMagnification()`.

## Failed probe and safety rule

The first automatic classifier pressed right mouse on every unknown weapon and
watched native projection scale. This was rejected even though its measurements
were informative:

- ordinary world camera settled near 1.126x;
- the first 2× world camera settled near 1.744x;
- native ADS affected the two AER eyes differently during classification;
- culling/geometry became corrupted while ordinary guns were held up;
- right mouse invoked shotgun secondary fire, producing 18 projectile-creation
  calls merely from raising the shotgun.

Safety rule: an unknown configuration must never receive native visual ADS as a
probe. Visual ADS is sent only after an attachment is positively identified.
Gameplay-only ADS/recoil suppression remains independent and applies to normal
eye-level aiming.

## Passive attachment isolation

A passive census logs complete sorted weapon mesh sets without sending input.
A same-gun capture before and after adding a scope isolates the attachment.

Confirmed 2× variant A:

```
36 48 384 936 960 5808
```

Confirmed 2× variant B:

```
24 48 120 420 480 8928
```

Each variant requires all six IDs. Detection is the OR of the complete variant
signatures. This avoids host-gun coupling, raw mesh-count heuristics and single
IndexCount collisions. User confirmed both variants zoom correctly and guns
without scopes—including scope-compatible guns—do not zoom.

The passive census remains bounded to the first 32 eye-level raises per session.
It logs both known-variant match counts and is retained for other 2× models and
the untested 4× scope. A future 4× implementation should add its own attachment
signature and magnification, never broaden the 2× check by gun identity.

## Current state

- Both observed 2× attachment variants: confirmed working.
- Unscoped weapons: confirmed unaffected.
- Reflex sights: intended to remain excluded; continue observing during normal
  play.
- 4× scope: identified and confirmed working. The first captured configuration
  is `12 27 42 60 144 273 288 360 2517 4647 10662 11751 15222 21762`.
  It requests a fixed `3.488x` VR projection scale while Metro's native visual
  ADS state is active, using the same stable projection path as the 2× optics.
- User validation confirmed the known 2× optics and the new 4× optic present
  correctly. The optic remains attachment/configuration-driven rather than
  weapon-ID-driven.
- ADS/optic activation uses vertical hand-to-HMD hysteresis: `0.12 m` to
  engage and `0.18 m` to release. These are currently compile-time values;
  exposing the engage distance in the VR menu is a future usability task.
- Minor native words/UI distortion under magnification: deferred.
- Verified deployed DLL SHA-256:
  `376311AF49FF60804C5EA7119672D12E34DFC5C13ECA98672EF4993B61D9E99A`.
