# Single-pass stereo, world-placed UI, and the deferred-space fix

Resolved 2026-08-09/10, after the CPU software-occlusion pop-in fix in
Notes/11. Four related problems, one of which turned out to be the root of a
long-running class of artifacts.

---

## 1. Single-pass eye-specific flickering

### Symptoms

Present under single-pass stereo only; double-draw never showed them:

- Flickering around the pig pen.
- Flickering near certain wall lights.
- Localized wall, floor and decal artifacts.
- Some geometry or shading appearing in one eye only.
- Different locations needed different diagnostic selections to stop it.

### Cause

Single-pass issues ONE instanced draw targeting both texture-array slices,
with a patched vertex shader computing a separate clip position per eye.

That is correct only when the shader's sole eye-dependent output is
`SV_Position`. Some Metro shaders also emit other view-dependent values:

- view-space position,
- view-space normals,
- depth-related data,
- data consumed later by deferred lighting or decal passes.

The patch fixed the right eye's clip position but could not safely
reconstruct those additional outputs. Those shaders produced a correct
left-eye payload alongside partially wrong right-eye data.

### Diagnosis

A temporary F7 shader-bucket bisection: each press forced another group of
single-pass shaders back to double-draw. Live testing isolated the
artifact-producing groups (pig pen, a second lighting area, a small
textured/decal artifact, corridor/world surfaces), then narrowed each bucket
down to individual vertex shaders rather than disabling single-pass for a
whole category.

### Fix

Five exact shaders now always use double-draw:

| Hash | Pass |
|---|---|
| `320C272616C11623` | textured world |
| `4E9D4E63C6F1477E` | position/depth world variant |
| `6FC1713CCCC530EE` | small textured/decal geometry |
| `67B434E01A3150AF` | dominant corridor/world-surface variant |
| `367328457692E6B2` | closely related corridor variant |

Everything else stays eligible for single-pass, keeping most of the
performance benefit. The list and its per-hash reasoning live in
`ForceDoubleDrawForShader()` in
ThirdParty/3Dmigoto/DirectX11/StereoSinglePass.cpp.

---

## 2. Main-menu text and "Continue"

### Symptoms

Under double-draw: menu words did not line up between the eyes, sat at the
wrong stereo depth, and did not stay attached to their objects. "Continue"
read correctly in the left eye but appeared to project out of or beside the
television in the right.

### Cause

The main menu is a real 3D scene with text placed on physical objects. Those
labels carry a UI constant buffer holding a complete engine-generated
world-to-clip matrix. The room had been converted to the HMD projection, but
the label matrix still held Metro's original flatscreen projection - so the
scene and its labels were being rendered in two different projection spaces.

Sharing one matrix between the eyes is also wrong: these labels are
world-placed and need genuine per-eye parallax.

### Fix

Capture the exact `cb_misc_0` Metro writes for each UI element before
upload, then per eye:

1. remove the game's original projection from the UI matrix,
2. apply that eye's HMD projection,
3. include the VR view correction,
4. preserve the rest of Metro's per-item transform,
5. rebind the corrected private UI buffer for that eye.

Each label gets proper stereo depth and stays attached to its television,
sign or menu object.

Gameplay HUD is deliberately treated differently: ammo, watch, prompts and
similar flat UI go on a comfortable view-locked VR plane rather than being
treated as world geometry.

Implementation: `BuildWorldUIScreenMatrix()` and `BeginUniversalUICB()` in
ThirdParty/3Dmigoto/DirectX11/HackerContext.cpp.

---

## 3. Moving shadows and lighting - THE IMPORTANT ONE

### Symptoms

In both single-pass and double-draw (single-pass sometimes made them easier
to see):

- Shadows moved when the head rotated.
- The lamp shadow changed shape or vanished from certain viewpoints.
- Lighting around the pig pen and wall lamps moved incorrectly.
- The light above the ammunition seller moved.
- Illumination on the seller moved separately from the visible lamp.
- Some shadows were stable, others were not.
- Both eyes showed the same wrong movement - so not an eye-sync problem.

### What was tested first

Shadow-caster matrices, square and non-square shadow-map passes, depth-only
passes, individual deferred-light pixel shaders, specular vs shadow-map
projection, sections of `cb_light`, and whether particular light matrices
were already in VR space. A temporary F6 mode disabled or preserved parts of
shadow/lighting processing.

Those tests established:

- Disabling a light draw entirely stopped the problem but removed too much.
- Removing only some lighting components improved the artifact without
  preserving the intended look.
- The movement tracked shadow/light PROJECTION, not an animated specular
  highlight.
- Fixing one light matrix made neighbouring shadows move - so it was never
  one bad light or one bad shader.

That inconsistency was the tell: a coordinate-space mismatch shared across
the deferred renderer.

### Root cause: a mixed-space G-buffer

Metro is deferred. Geometry writes view-space position and normals into the
G-buffer; lighting and shadows later interpret that data using Metro's light
matrices.

The geometry path had been patching BOTH:

- `m_WV` - world-to-view, which produces deferred view-space position/normals
- `m_WVP` - world-to-view-projection, which decides where geometry lands on
  screen

So regular geometry wrote VR-corrected view-space data into the G-buffer.
But not everything did:

- much of Metro's instanced geometry keeps the original engine view space
  baked into its instance data,
- deferred lights and shadow matrices still expected Metro's original view
  space.

The G-buffer therefore held a MIXTURE of VR view space and original Metro
view space, while lighting interpreted all of it as the original space. As
the headset rotated, the disagreement changed - so lights and shadows slid,
rotated, vanished or followed the player's head.

### Fix

In `PatchMappedVRObjectData`, separate the two jobs:

- **preserve** Metro's original `m_WV` (floats 24..35),
- **keep writing** the corrected per-eye VR `m_WVP` (floats 36..51).

Result:

- Geometry still appears in the correct eye and tracks the headset, because
  clip-space positioning uses the corrected `m_WVP`.
- Deferred positions and normals stay in Metro's original view space,
  because `m_WV` is preserved.
- Regular geometry, instanced geometry, lights and shadow projections all
  agree on one deferred coordinate system.
- Metro's original `cb_light` matrices can be left alone - which is why
  `PatchMappedDeferredLightCB` returns early when
  `StereoSinglePass::LegacyDeferredSpaceEnabled()` is true.

This single change fixed moving shadows, the lamp shadow, the
ammunition-seller light and its illumination, several remaining head-tracked
or moving assets, and other lighting artifacts that had looked unrelated.

---

## 4. Final single-pass configuration

A live F8 switch let single-pass and double-draw be compared directly for
image quality, shadow definition, blurriness, performance and
location-specific artifacts. Single-pass was confirmed NOT blurrier. With the
unsafe shaders and the deferred-space issue handled, there was no reason to
keep a user-facing mode switch.

Final state:

- Single-pass enabled permanently by default.
- The five unsafe shaders automatically fall back to double-draw.
- No F8 mode toggle, no F7 shader bisection, no F6 diagnostic hotkey.
- No inactive viewmodel-identification controller binding.
- No runtime bucket-selection bookkeeping.

The renderer is really a **safe hybrid**: single-pass wherever it works,
transparently falling back to double-draw for the five shaders proven unsafe.

---

## The lesson worth keeping

The persistent shadow problem was never a bad shadow shader. It was a
deferred-rendering coordinate-space mismatch - VR-corrected clip positioning
had to coexist with Metro's ORIGINAL deferred view space.

The diagnostic pattern that kept misleading: fixing one light made a
neighbouring shadow worse. When a fix MOVES a problem instead of shrinking
it, the cause is shared state, not the thing being fixed.


---

# 5. Weapon aim: why calibration kept drifting (2026-08-11)

## Symptom

Sight zeroing would not stay put. Each pass corrected the shot, and a later
session was off again. Three passes gave TOTALS of:

| Pass | pitch | yaw |
|---|---|---|
| 1 | +11.28 | -29.08 |
| 2 | -13.20 | -17.80 |
| 3 | -3.85 | -26.53 |

Oscillating around roughly -2 and -24 rather than converging. The transplant
arithmetic was verified exact - the live nudge adds to the same constant in
the same space - so the bake was not losing anything.

## Cause

The shot direction was built from the controller through a hard-coded 64.6
degree grip-tilt assumption (`kControllerAimTilt`), with two Euler constants
patching up the difference.

Measuring the drawn gun's barrel against that computed direction, per shot,
across elevations and wrist angles:

    angle  = 14.7 .. 16.3 deg     (essentially CONSTANT, mean ~15.7)
    dYaw   = -16.4 .. +0.91 deg   (swings wildly)
    dPitch = -0.68 .. -15.2 deg   (swings wildly)

**The discrepancy is a fixed rotation of ~15.7 degrees. Its yaw/pitch
DECOMPOSITION depends on orientation.** Two Euler constants cannot represent
one fixed rotation across orientations - so any value measured at one
orientation is wrong at another. That is the whole reason calibration
oscillated instead of converging, and no amount of re-measuring could have
fixed it.

## Fix

Stop deriving the aim from the controller plus a guess. Read it off the
transform that actually DRAWS the weapon:

- the viewmodel lives in view space, rest pose down view +Z,
- so the barrel after the mod's rotation `D` is `D * restAxis`,
- the view matrix rows are the camera basis in world, which converts that
  straight to world space.

Sights and shot then agree BY CONSTRUCTION, with no constant to tune.
`kAimFromWeaponModel` in VRPose.cpp, in `RedirectShot`.

## The residual, and where to correct it

Both weapons then shot slightly high by the SAME amount - the rest axis is
not exactly view +Z. The correction is applied to the model-space rest axis
**before** `D`, not to the finished world direction:

    m   = (sin(yaw)cos(pitch), sin(pitch), cos(yaw)cos(pitch))
    bv  = D * m

Applying it after `D` would reintroduce the posture dependence that caused
the oscillation in the first place, because the offset is fixed relative to
the MODEL, not to the controller.

Measured: `kModelBarrelPitch = -3.44 deg`, `kModelBarrelYaw = -1.38 deg`.

Two things confirm this is now correcting the real thing:

- the magnitude fell from ~26 degrees to ~3.4,
- ONE calibration covered BOTH weapons, which a shared model-space cause
  predicts and a per-weapon cause does not.

Accuracy across distances is good.

## STILL OPEN: elevation dependence

Shots remain slightly off when aiming at the ceiling or floor, by about the
same amount as before the model-space change.

No confident diagnosis. The obvious candidate - a frame mismatch in the
view-space to world conversion - was traced and does NOT explain it:
`ComputeWeaponRotationDelta` builds the rotation as controller-relative-to-
HEAD (`hmdR^T * ctrlR`, converted to game space, times the rest anchor's
inverse), and the weapon is drawn in view space, so converting through the
head's view matrix is consistent.

Next measurement if picked up: log the model barrel direction against the
ACTUAL impact point at several elevations. The `MODELAIM` run only compared
the barrel against our own computed aim, which cannot reveal an error the
two share.

## Lesson

When a calibration oscillates instead of converging, suspect the SHAPE of
the correction, not its value. A fixed rotation cannot be expressed as
constant Euler offsets across orientations, and re-measuring a
mis-shaped constant just moves the error around.
