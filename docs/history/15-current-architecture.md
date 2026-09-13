# Current architecture — 2026-08-01

Supersedes the injection/cancellation design in Notes/13, which is obsolete.
Everything here is live and confirmed working in the headset.

## The core insight

For most of this project one camera did two jobs: it decided what got DRAWN
(frustum culling) and where shots WENT. Every attempt to fix aiming broke
culling, and vice versa, because moving the camera moved both.

Reverse engineering separated them (Notes/14). Shot direction lives in the
weapon object at `+0x150`, written one instruction before the engine consumes
it, and has nothing to do with the render camera. Once aim stopped depending
on the camera, the camera became free to do its real job.

## How the pieces fit

| Concern | Driven by | Mechanism |
|---|---|---|
| What the player sees | head | view matrix patched in `PatchMappedVRCameraData` |
| What gets culled | head (slowly) | look input injected by `UpdateCullingFollow` |
| Where bullets go | right controller | `RedirectShot` writes weapon object `+0x150` |
| Movement, turning, actions | VR controllers | `UpdateControllerInput` injects keys/mouse |
| Stereo | — | alternate-eye rendering, pose-tagged submission |

### Why the view stays exact regardless of injection

```
camera = body + injected          (the engine, driven by our look input)
view   = camera - injected + head (what we render)
       = body + head              (always, whatever `injected` is)
```

The cancellation is exact by construction, so the injection can lag
arbitrarily without the picture moving. That is the whole reason this works
now and did not before: previously the cancellation had to be perfect because
aim depended on it, and any error showed up as the world sliding around. Now
injection error only affects culling, where a frame or two of lag at the edges
is invisible.

Injection is deliberately slow (~1.8 deg/frame, half gain). The earlier
shaking came from running it about an order of magnitude faster.

## Rotation composition — two bugs worth remembering

Both were latent since milestone 1 and produced a very specific symptom:
**yaw worked, pitch did not.**

1. **Wrong frame of application.** The head delta was composed as
   `V_new = V_old * delta^T` - a WORLD-space rotation of the camera. It has to
   be a LOCAL one, `V_new = delta^T * V_old`, because the delta is relative to
   a reference captured at startup and the player's heading rotates away from
   that frame every time they turn.

2. **Wrong frame of expression.** `UpdateVRPose` built the delta as
   `R_cur * R_ref^T` (tracking-world frame). A local rotation needs
   `R_ref^T * R_cur`.

Rotations about the world up axis commute with the body's yaw, so yaw survived
both bugs untouched. Pitch is about a horizontal axis that turns with the
heading, so once the player had turned away from the reference direction, head
pitch was applied about the wrong axis - appearing as roll, or as nothing at
all. Symptom two also manifested as the horizon slowly tilting after turning
far enough.

**Rule of thumb: if yaw behaves and pitch does not, suspect a frame mismatch,
not a sign error.** Yaw hides these because it commutes with the body.

## Weapon instance buffer

The viewmodel's transform is a 3x4 affine **local-to-view** matrix in the
game's shared 1MB dynamic instance buffer. Writing into that buffer corrupts
neighbouring props - proven when writing byte-for-byte UNCHANGED data still
froze props across the level (Notes/10 Update 5). The Map/Unmap itself is the
problem, not the values.

So we never touch it. `SubstituteWeaponInstanceBuffer` reads the transform,
puts a copy in a private DEFAULT-usage buffer, and binds that for the draw.
Confirmed live: weapon renders correctly, props intact.

Two things that matter for identifying the weapon:

- **The input layout is not unique to it.** The census counted 103,996 binds
  through the same layout. Substituting at bind time meant a GPU readback per
  bind and dropped the framerate to 15fps. The bind now only RECORDS; the
  decision happens at draw time, gated on index count first so the readback is
  rare.
- **The index count is not unique either.** The real discriminator is that a
  local-to-view transform puts the viewmodel within a couple of units of the
  eye, while scene geometry is metres away.

Per-instance row 3 is NOT part of the transform (observed
`0.0020 0.0020 0.0020 0.4397`) - it is per-instance parameters and must be
preserved.

## The 6DOF weapon — how it works, and what it cost to find

The viewmodel's per-instance data is a 3x4 affine **local-to-view** transform
in the game's shared instance buffer. We never write that buffer (see above);
we copy the weapon's slice into a private buffer, transform it, and bind ours.

### The transform happens on the GPU

`kWeaponTransformHLSL` in HackerDevice.cpp. Per weapon draw:

1. `CopySubresourceRegion` the game's 64 bytes into our private buffer -
   GPU to GPU, no stall.
2. Dispatch a compute shader that rewrites it in place through a raw UAV,
   with the controller's rotation, translation and both pivots supplied in a
   constant buffer.
3. Bind the private buffer as the vertex buffer.

The CPU never reads anything back in the draw path. Four meshes per frame are
sampled synchronously, purely to keep the classification statistics alive.

**Why not read it back:** the original design did `CopySubresourceRegion` then
`Map(READ)` per draw, which stalls the pipeline. Measured at **50-69 stalls
per frame, peak 364**, costing roughly 20-25 FPS - and it scaled with the mesh
count, so it would have worsened with every weapon added.

**Why not do it asynchronously:** reading last frame's copy removes the stalls
and gained ~10 FPS, but is **unsound**. The staging buffers were keyed by index
count, and index counts do not identify an object - world props share them with
viewmodel meshes. A buffer could hold a different object's transform, and no
validation catches it because the data is a perfectly valid matrix, just the
wrong one. It showed up as a flickering artifact at the eye.

### Two pivots, not one

- **Cluster pivot: the origin.** The viewmodel's instance translation sits at
  the origin of view space - `(-0.035, -0.021, -0.009)` in every sample ever
  logged. So this is a constant, not something to seed and track. Seeding it
  "from the first accepted mesh" repeatedly picked the wrong object; the last
  time it landed at `(-0.797, -0.055, -0.031)`, putting the real weapon 0.817
  away against a 0.8 radius, so **all 40 samples were rejected** and the gun
  stopped moving entirely.
- **Rotation pivot: the grip** (`kWeaponRotationPivot`, ~20cm below and 40cm
  in front of the eye). Sharing the cluster pivot meant rotating about the
  player's EYE, which swings the whole weapon around their head rather than
  turning it in their hand. From inside the headset that reads as translation,
  which is why it felt like 3DOF for several builds.

The 45-degree fixed-rotation test is what separated these: the weapon slid out
of view to the right instead of pivoting, proving the rotation was applied
correctly all along and only the pivot was wrong.

### Poses are computed once per frame

`GetWeaponRotationDelta`/`GetWeaponTranslationDelta` were being called per
weapon draw, and each calls `GetDeviceToAbsoluteTrackingPose`, which fills
poses for all 64 tracked devices - roughly **240 full pose queries per frame**.
That cost scaled with weapon draw count rather than scene complexity, which is
exactly why the framerate appeared to fluctuate randomly (mid 40s to high 60s
in the underground town while the gun range stayed steady). Cached per frame,
the result is a **steady 72 FPS**.

## Recurring lesson: index counts do not identify objects

This caught us four separate ways - a world gun prop rotating with the
controller, meshes being classified from the wrong draws, the async readback
returning another object's transform, and identification cycles landing on the
wrong mesh. Any rule keyed on index count alone will eventually be wrong; the
distance-from-cluster test is what actually discriminates.

## Still outstanding

- **Weapon model does not follow the controller yet.** Aim does; the gun does
  not. The stride is 64 bytes - a full 4x4 - so position and rotation are both
  available. This is the last substantial piece.
- Once it does: move `RedirectShot`'s origin from the camera to the muzzle, so
  bullets leave the barrel rather than the player's head.
- Decals, some lighting and shadows track the head - passes we do not correct.
- ~~Culling follow is yaw only.~~ It proved necessary: props vanished above and
  below when the head pitched, because the engine's camera stayed put and culled
  them. Pitch is now injected as well. Two things it needs that yaw does not -
  Metro applies only ~0.54 of a commanded vertical movement, so the scale
  appears on both the command and the booked total; and the target is held
  inside the engine's own pitch clamp (~66 deg), because booking pitch the
  engine refused to apply would leave a view offset that never recovers.
- NPC doubling under alternate-eye rendering (inherent to AER).
- Menus render wrong.
