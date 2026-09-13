# Gun and shooting: complete current state (2026-08-11)

Written to be self-contained. Everything about how the weapon is attached,
how a shot is produced, why vertical aim is off, why there is no bullet
spread, and every approach already tried and ruled out.

---

# PART 1 - HOW THE GUN IS ATTACHED

## 1.1 Identifying the viewmodel

The engine gives no flag for "this is the player's weapon". Three
discriminators were tried and each failed on its own:

- **index count** - scenery shares counts with the viewmodel,
- **proximity to the eye** - cannot tell "attached to me" from "I am standing
  on it",
- **shader identity** - the game shares viewmodel shaders with world weapon
  models, so walking up to an NPC made THEIR rifle rotate with the controller.

What works is **behavioural**: the viewmodel's view-space transform does not
change as the player moves, while world geometry's always does. Index counts
are probed a few per frame, the verdict is remembered, and rejections EXPIRE
after 600 frames (a count probed while bound to something distant used to be
written off permanently).

The learned set is persisted to `vr_viewmodel_meshes.txt` in the game folder
so later sessions start instantly instead of watching the weapon assemble
itself piece by piece. ~27 meshes were learned initially; the file has since
grown to 63 across weapons.

Key mesh identities found by live bisection (reveal one mesh per press):

| IndexCount | What it is |
|---|---|
| 6576 | the ARMS - permanently hidden, see below |
| 12132 | BOTH HANDS - a single mesh |
| 16251 | SMG body |
| 6273 / 1224 / 6840 | Revolver cluster |

**The arms are permanently hidden** (`kAlwaysHiddenIndexCounts = { 6576 }`)
because they rotate with the weapon as one rigid cluster, and a forearm
swinging from a fixed shoulder looks wrong. Floating hands on the gun reads
better and is where most VR mods land.

**The hands are ONE mesh.** This was confirmed deliberately when investigating
whether the left hand could be detached and driven by the left controller:
reveal step 6 turned on IndexCount 12132 and BOTH hands appeared together. So
there is no per-draw way to separate them - it would require moving bones
inside that mesh's palette. That work was deferred as "nice to have".

## 1.2 Weapon identification (which gun is held)

`IdentifyWeapon(indexCount)` matches body-mesh signatures:

    SMG      : 16251
    Revolver : 6273, 1224, 6840

This drives per-weapon grip calibration. It is a specific, validated
signature - NOT the broad "some 64-byte-stride buffer is bound" check, which
misfired on main-menu desk props and was once wrongly used as a
"gameplay has started" signal.

## 1.3 The transform that positions the gun

Applied on the GPU (`kWeaponGpuTransform = true`) by a compute shader that
maps each vertex:

    v -> s * D * (v - pivot) + pivot + translate

- `s` - uniform scale, stepped through `kWeaponScaleSteps` = {1.0, 1.1, 1.2,
  0.9, 0.8, 1.25, 1.5} during calibration.
- `D` - rotation delta (section 1.4).
- `pivot` - `kWeaponRotationPivot = (0, -0.20, 0.40)`. The model is drawn in
  front of and below the eye, so rotation has to happen near where the grip
  appears; rotating about the origin made the weapon slide out of view
  instead of pivoting.
- `translate` - places the GRIP at the controller.

The grip is placed **absolutely**, not as a rest pose plus a hand-picked
offset:

    translate[i] = ctrl[i] - eyeOffset[i] - kWeaponCalibrations[weapon].grip[i] + adj[i]

The eye offset term is required because the view origin is the EYE, not the
head, so a controller position measured from the head must be expressed in
this eye's frame. The second-eye re-dispatch differs only by that term.

Per-weapon grip offsets:

| Weapon | grip (x, y, z) |
|---|---|
| SMG | 0.142, -0.037, 0.544 |
| Revolver | 0.068, 0.017, 0.790 |

There is also a fixed `kWeaponOffsetView = (-0.016, -0.332, 0.182)` used by
the non-absolute path, and `kWeaponTranslationScale = 1.0` (the weapon tracks
the hand 1:1 in depth; measured 42 cm of hand travel producing 42 cm of
weapon travel).

`kWeaponFollowsController = true` gates the rotation following.

## 1.4 Weapon rotation (`D`)

    ctrlR   = controller rotation (OpenVR, standing universe)
    hmdR    = headset rotation
    rel     = hmdR^T * ctrlR              // controller RELATIVE TO HEAD
    relGame = ConvertOpenVRRotationToGameSpace(rel)
    D       = relGame * sWeaponAnchorRot^T // delta from the rest pose

`sWeaponAnchorRot` is captured once - "this is the weapon's rest pose".

Being head-relative is deliberate and correct: the viewmodel lives in VIEW
space, which is itself head-relative, so the weapon's view-space orientation
should be the controller's orientation relative to the head. Turning the head
therefore leaves the gun where the hand is.

**Note:** `vrWeaponViewCorrection3x4` (a body-frame correction that omits the
head term) is COMPUTED BUT DELIBERATELY UNUSED. Applying it was tried and
made things worse - the gun came out rotated wrongly, hands still detached on
a turn, the watch stayed head-locked, performance dropped. Left in the code
as a record.

---

# PART 2 - HOW A SHOT IS PRODUCED

## 2.1 Catching the shot

**Inline hook** on the fire wrapper at `metro.exe+0x4F3E60`, installed via the
Nektra hook manager already used for the project's cursor hooks
(`InstallFireInlineHook`, VRPose.cpp).

This replaced a hardware-breakpoint probe that had to be armed PER THREAD
from a thread snapshot, re-broadcast every 5 seconds. Shots serviced by a
thread created between sweeps went unredirected. The inline hook patches the
function itself, so every call is caught on every thread immediately - no
warm-up, no misses. The breakpoint remains as a fallback and stops
redirecting while the inline hook is live (`sInlineFireHookActive`), so a
shot is never redirected twice.

Note the executable is Steam-DRM packed (`.bind` section); the on-disk
`.text` is encrypted, so all disassembly was done by dumping decrypted bytes
from the live process and disassembling offline.

## 2.2 What gets written

`RedirectShot` writes three fields in the engine's fire object:

| Offset | Meaning | Written value |
|---|---|---|
| `+0x0E0` | ray origin | `gunOrigin` (the muzzle) |
| `+0x0F0` | ray direction | our aim direction |
| `+0x150` | hit point | `gunOrigin + dir * shotRange` |

Verified live: the engine KEEPS these - 40 of 40 shots returned `KEPT`, with
the written origin ~0.6 m from the camera.

Other known fields in that object: `+0x120` = the player's ground position
(X/Z match the camera, Y ~0), `+0x178` = the engine's hit distance,
`+0x140` = an OUTPUT buffer the callee fills (uninitialised on entry).

## 2.3 CRITICAL: the engine does not trace along our ray

`+0x150` is the **already-computed hit point**. The trace ran along the
engine's own camera ray BEFORE our hook, and we only relocate the result.

Proven by setting the endpoint 60 m out: the impact went to 60 m rather than
landing on a wall 2 m away.

This is why every "put the endpoint on the barrel line" scheme traded one
artifact for another, and why impacts once appeared on an invisible wall - we
were moving the hit point off the surface.

It also means `shotRange` is measured along the CAMERA's ray, which is not
the barrel's. Measured divergence between camera pitch and hand pitch:
23-32 degrees (the camera follows the HEAD).

## 2.4 Where the aim direction comes from

Derived from the transform that DRAWS the weapon, not from the controller
plus a guess:

    D   = GetWeaponRotationDelta()          // section 1.4
    m   = corrected model-space rest axis   // near view +Z
    bv  = D * m                             // barrel in view space
    dir = normalize(viewToWorld(bv))        // rows of vrCachedView3x4

so sights and shot agree by construction. Flag: `kAimFromWeaponModel`.

Model-space trim, measured 2026-08-11:

    kModelBarrelPitch = -3.44 deg
    kModelBarrelYaw   = -1.38 deg

The correction is applied to the rest axis **before** `D`, because the offset
is fixed relative to the MODEL. Applying it after would reintroduce posture
dependence.

ONE calibration covered BOTH weapons - which a shared model-space cause
predicts and a per-weapon cause does not.

## 2.5 Other properties of the aim chain

- **Absolute pitch and yaw**, not offsets from a per-session anchor. The
  anchor was captured from wherever the controller happened to be at
  start-up, making aim arbitrary per session and any calibration worthless on
  the next launch. Flags `kAbsoluteAimPitch`, `kAbsoluteAimYaw`, scoped to the
  shot only (the camera-steering loop still uses its anchor for a clamped
  follow offset).
- **Anchoring no longer costs shots.** The settle counter incremented once per
  SHOT rather than per frame, so the first ~30 rounds of a session went
  unredirected. It now anchors on first use.
- **Live zeroing** (`sShotAimBias`) stacks on the model-space trim and is NOT
  cleared on exit, so what is dialled stays for the session. Control:
  modifier (left trigger) + left grip, held ~1 s, goes straight to rotation
  mode (position mode is skipped so the working grip placement cannot be
  disturbed by accident).

---

# PART 3 - WHY THERE IS NO BULLET SPREAD

**This was never a deliberate design choice, and it was not for diagnostics.
It is an unavoidable consequence of the redirect.**

Every shot's ray is recomputed deterministically from the weapon's
orientation and written into the fire object. The engine still applies its own
recoil kick and random spread to ITS aim - we simply overwrite the result
before it reaches the bullet.

> **Update, 2026-08-13:** The recoil portion of this section is superseded.
> The fire hook now invokes the equipped weapon's complete native aim-in
> virtual (`vtable+0x1100`) immediately before Metro's fire wrapper and the
> matching aim-out virtual (`vtable+0x1110`) immediately afterward. This
> synchronous ADS pulse gives eye-level fire Metro's native recoil
> suppression, while no render frame sees the ADS animation, so the gun,
> hands, watch, and attachments remain pinned to the controller. Hip recoil
> returns after the gun is lowered. Live testing confirmed both behaviors.
> The native spread limitation described below remains a separate issue.

Consequences:

- Full auto puts rounds in the same place. No cone, no climb.
- The impact point is a pure function of where the gun is pointing.
- **Metro's native spread is gone.** This was observed during debugging
  (redirected shots landing in one hole while un-redirected ones scattered -
  that difference is how the two paths were told apart during the warm-up
  bug) but was not flagged at the time as a gameplay consequence.

Related facts:

- **Any fire path bypassing the hook gets FULL recoil**, since nothing else
  suppresses it. First thing to check if a weapon ever behaves differently.
- **The engine's internal aim still climbs.** It just stops mattering.
  Anything reading the engine's aim rather than the bullet may still behave
  as though recoil happened.
- **The gun-to-face gesture now controls native recoil suppression.**
  `IsWeaponNearFace()` (engage 0.28 m, release 0.36 m, hysteresis to stop
  chattering) drives the per-shot native ADS pulse. `kUseEngineADS` remains
  false because continuously forwarding ADS still moves the viewmodel.

### Why the engine's ADS was disabled

It suppressed recoil and sway properly, but dragged the weapon toward the
face through the skinned bone palette, fighting the controller the gun is
pinned to. That movement cannot be cancelled without decoding the palette,
and the hands are a single mesh (12132), so there is no cheap way at it.
`kUseEngineADS` is kept as a flag for comparison.

### If spread/recoil is wanted back

Two options:

1. **Author our own** - perturb `dir` by a random cone before writing it.
   Easy. Would be OUR model, not Metro's numbers (its spread values live in
   engine data not yet located). This is also where the gun-to-face gesture
   would finally earn its place: loose at the hip, tight when braced.
2. **Find Metro's real spread values** and reproduce them faithfully - most
   authentic, another reverse-engineering hunt.

A recoil measurement already exists but is **not wired to anything**: the aim
loop is feedforward (tracks what it COMMANDED, never observes the camera), so
rotation the game applies is invisible to it. The unexplained component is

    unexplained = observed - (our command, lagged) * scale

and is logged today.

---

# PART 4 - THE OPEN PROBLEM: VERTICAL ACCURACY

## 4.1 Symptom

Shots are slightly off when aiming at the **ceiling or floor**. Level shots
are accurate. Accuracy holds across **distances**. Both weapons behave
identically and one calibration covers both.

## 4.2 Measured facts

- **The aim chain is internally exact.** Logged per shot,
  `ctrlPitch == aimPitch == dirPitch` to the hundredth of a degree. Nothing
  compresses elevation between the hand and the direction we write.
- **The game camera is not the hand.** `camPitch` diverges from `ctrlPitch`
  by 23-32 deg (level: +13.8 vs -18.0; ceiling: +43.7 vs +20.7; floor: -26.9
  vs -58.5). The camera follows the HEAD, so `shotRange` describes distance
  along the view, not along the barrel.
- **The rendered view and the engine camera agree** within 0.3-6.5 deg, so
  this is not a view/culling misalignment.
- The model-vs-aim discrepancy measured as a **fixed rotation** (~15.7 deg,
  range 14.7-16.3) whose yaw/pitch DECOMPOSITION swung wildly with
  orientation - which is why Euler constants oscillated.

## 4.3 Everything tried

| Attempt | Result |
|---|---|
| Target at engine hit DEPTH along the view axis | **Caused** elevation compression - the divisor `dir . camFwd` shrinks with tilt, pushing the point past the surface. Reverted. |
| Target range-matched on the barrel line | No fix. |
| Target on the camera ray (`camPos + dir*range`) | No fix. |
| Endpoint far out (60 m) so the engine would trace | Proved the engine does NOT trace; impact went to 60 m. |
| Writing ray origin + direction (`+0x0E0`/`+0x0F0`) | Engine KEEPS them, but impact is set by `+0x150`, so it did not change aim on its own. |
| Bias applied in Euler yaw/pitch | Oscillated across sessions - wrong SHAPE (a fixed rotation is not two constant Euler offsets). |
| Bias rotated about the CONTROLLER's up axis | Wrong axis - stick moved impacts diagonally, and coupling varied with elevation. |
| Bias rotated about the BARREL's own up axis | Correct axis. Still no elevation fix. |
| Correction in model space, before `D` | Fixed magnitude (26 deg -> 3.4 deg) and cross-weapon consistency. **Elevation unchanged.** |
| Weapon-in-body-frame mismatch | **Dead on inspection** - `vrWeaponViewCorrection3x4` is deliberately unused; the weapon is in the same frame as the view. |
| Rest anchor baking in a head pitch | Weak - `D` is a delta FROM that anchor, so it cancels. |

## 4.4 Why cheap measurements cannot settle it

The impact point is derived from `dir`. Comparing impact against `dir` - or
against anything computed from it - is **circular** and looks clean even when
both are wrong together. That is exactly why the `MODELAIM` run showed a
stable offset and no elevation term.

## 4.5 The measurement that would settle it

Compare the barrel direction we COMPUTE from `D` against the barrel direction
**as actually drawn** - reading back a vertex from the weapon's
compute-shader output. That is the only comparison in this chain not derived
from `dir`.

Heavier than anything tried so far, with no strong lead pointing at which
term is wrong. Honest assessment: probably fixable, but a reverse-engineering
job rather than a tuning pass.

---

# PART 5 - QUICK REFERENCE

| Thing | Where |
|---|---|
| Viewmodel mesh learning | `sViewmodelIndexCounts`, `vr_viewmodel_meshes.txt` |
| Hidden arms | `kAlwaysHiddenIndexCounts = { 6576 }` |
| Weapon identity | `IdentifyWeapon`, `kSMGCluster` / `kRevolverCluster` |
| Grip offsets | `kWeaponCalibrations[]` |
| GPU weapon transform | `kWeaponGpuTransform`, compute shader |
| Rotation pivot | `kWeaponRotationPivot = (0, -0.20, 0.40)` |
| Scale steps | `kWeaponScaleSteps` |
| Weapon rotation | `ComputeWeaponRotationDelta` / `GetWeaponRotationDelta` |
| Unused body-frame correction | `vrWeaponViewCorrection3x4` |
| Inline fire hook | `InstallFireInlineHook` |
| Shot redirect | `RedirectShot` |
| Aim from weapon model | `kAimFromWeaponModel` |
| Model-space trim | `kModelBarrelPitch` / `kModelBarrelYaw` |
| Live zeroing | `sShotAimBias`, modifier + left grip |
| Gun-to-face gesture | `IsWeaponNearFace`, 0.28 m / 0.36 m; controls per-shot ADS pulse |
| Visual/input engine ADS (disabled) | `kUseEngineADS` |
| Native recoil suppression | `PulseNativeADS`; vtable `+0x1100` around fire, `+0x1110` afterward |
| Recoil measurement (unwired) | "VRPose recoil:" log |
