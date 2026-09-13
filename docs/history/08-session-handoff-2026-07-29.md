# Session Handoff — 2026-07-29

Pausing for the day. This captures exactly where things stand so the next
session can pick up without re-deriving everything.

## What's working right now

- 3Dmigoto fork builds clean (`StereovisionHacks.sln`, `Release|x64`) and
  loads against `metro.exe` with zero regression when no headset/SteamVR
  is present (`G->vrHeadRotationValid` stays false, everything is bare
  passthrough).
- With SteamVR + Galaxy XR (via Virtual Desktop) active, OpenVR initialises,
  polls a real delta rotation relative to a captured reference orientation,
  and **the main world/scene geometry now visibly, correctly responds to
  head rotation** - confirmed live by the user (moving the head up made a
  building move out of frame the way real camera rotation should).
  - This works by patching `cb_main_matrices1` (m_V/m_iV, per-frame camera)
    *and* `cb_main_matrices0` (m_WV/m_WVP, per-object - recomputed from the
    buffer's own `m_W` plus the patched view/cached projection) - see
    `Notes/07-mv-not-used-by-main-geometry.md` for why both are needed.
  - Fixed a real math bug while building this: composing a world-space
    delta rotation into the view matrix needs the delta's *transpose*
    (`V_new = V_old * Rdelta^T`), not the delta directly - see the
    derivation in the approved plan (now superseded, but the math is
    recorded there) and the comment in `HackerContext::PatchMappedVRCameraData`.

## What's not working yet (today's stopping point)

The weapon viewmodel (and a few other draw calls in the RenderDoc capture -
eid=96/205/207/213) don't respond to head rotation. Root cause confirmed
via shader disassembly (`Tools/inspect_weapon_shader.py` against the
existing capture): these shaders get their "view" transform from **per-
vertex stream data** (`v5/v6/v7` or `v7/v8/v9`, a baked 3x4 matrix passed
as vertex attributes), not from either constant buffer we patch. This is
the standard "view-space weapon rendering" technique - the engine pre-
fuses the weapon's own bone/local transform with the camera's view matrix
on the CPU and uploads the combined result directly, precisely so the
weapon never has to worry about world-space clipping.

Net effect right now: world geometry rotates correctly with your head:
weapon and (untested, likely) HUD/UI stay glued to the old fixed view,
so it currently looks like the world is swinging around a stationary gun
rather than a coherent head-tracked view. Once the weapon is fixed this
should read as normal head-tracking.

## Next session: fixing the weapon

Plan sketch (not yet built, not yet plan-mode-approved in detail):

1. Identify which vertex buffer feeds input slots v5-v7 (or v7-v9) for the
   actual weapon-viewmodel draws specifically (need to confirm eid=96-style
   really is the weapon and not incidental instanced world props using the
   same shader pattern - `Tools/inspect_weapon_shader.py` only sampled a
   few eids from the existing capture, didn't visually confirm which one
   is the gun).
2. Hook `IASetVertexBuffers` (currently untouched passthrough) to track
   that buffer's identity, same pattern as the constant-buffer tracking
   table in `HackerContext.cpp` (`sTrackedVRSlots`).
3. Patch its Map/Unmap: the per-vertex data already has the *old* view
   baked in combined with the weapon's local/bone transform. Need to
   "un-bake" the old view (multiply by its inverse - we already compute
   `V_old` in `PatchMappedVRCameraData` before overwriting it, would need
   to cache it) to recover the local part, then re-bake with the new view.
4. Verify the eid=96 disassembly pattern (`r0 = local * v0` via v5-v7,
   `dp4 o0 = m_P * r0`) exactly - the "baked" matrix is 3x4, same affine
   math helpers (`Multiply3x4Affine`, already built in `VRPose.cpp`) should
   apply, just on vertex-buffer data instead of constant-buffer data.

## Also worth checking next session

- Whether HUD/UI elements respond or also need patching (not yet tested
  directly - was about to check when we paused).
- The known limitation (instanced geometry without a weapon-specific fix)
  may still apply to *other* instanced props even after the weapon is
  fixed, if their vertex-buffer identity differs from the weapon's.
- Axis-mapping validation (`VRPose.cpp`'s `ConvertOpenVRRotationToGameSpace`,
  currently an identity copy) - now that geometry responds, this can
  finally be tuned against real visual feedback instead of guessing blind.
  Wasn't reached yet since the weapon/reference-point issue made it hard
  to judge "does turning right look right" with confidence.

## Housekeeping done before pausing

- Reverted `d3dx.ini`'s `[Logging] calls`/`unbuffered` back to `0`
  (default, non-verbose) - they were turned on for debugging this session.
- Stopped the game process and any lingering RenderDoc processes.
- `ThirdParty/3Dmigoto`'s build output (`builds/x64/Release/d3d11.dll`) is
  still deployed in the game folder from the last test - matches the
  current source state (cb0+cb1 patching, transpose fix), not stale.
