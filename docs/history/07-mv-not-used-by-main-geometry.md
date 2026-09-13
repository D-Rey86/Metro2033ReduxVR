# cb_main_matrices1's m_V Is Not Used By Main Geometry — 2026-07-29

## The test

After confirming `PatchMappedVRCameraData` was executing continuously
(500K+ times per session) and the delta-rotation math was producing
correct, changing values, the user still saw **zero visible change** with
real head tracking. To isolate "is this a math/axis-convention bug" from
"does patching this buffer do anything at all," forced a deliberate,
unmissable 180-degree yaw override (ignoring actual head tracking) into
`G->vrHeadRotation3x3`. Result: still no visible change whatsoever.

## Root cause (confirmed via RenderDoc shader disassembly)

Pulled VS disassembly for several draw calls from the existing
`GameReferences/metro2033_capture_frame3027.rdc` capture
(`Tools/inspect_vs_shader.py`):

- **eid=41, eid=125** (plain geometry shaders): `SV_Position` is computed
  directly from `m_WVP` (`cb_main_matrices0`, slot b0) - a **precomputed
  World×View×Projection matrix**, already fully combined and uploaded from
  the CPU. Neither shader reads `cb_main_matrices1` (our patched buffer)
  at all.
- **eid=205** (an instanced-looking shader): does read `cb_main_matrices1`,
  but only `m_P` (projection). The "view" part instead comes from
  **per-instance vertex stream data** (`v5.xyzw`/`v6.xyzw`/`v7.xyzw` - a
  3x4 matrix passed as per-vertex/per-instance input), which is also
  precomputed CPU-side before upload. `m_V` itself is never referenced.

So across every "real geometry" shader sampled, the camera's view
transform is **baked into per-object or per-instance data by the CPU
before it ever reaches a buffer we can intercept at the D3D11 API level**.
`cb_main_matrices1`'s `m_V` is only read directly by the effects shaders
where we originally found it (lens flares, HUD) - exactly why patching it
had no visible effect on the actual 3D scene: those are thin overlay
effects, not the world geometry filling most of the view.

This likely also explains vorpX's known-buggy DirectVR support for this
game (Notes/01) - if the engine bakes view into per-object data broadly,
any generic injector relying on a per-frame camera-matrix hook would hit
the same wall.

## Implication for milestone 1

The current approach (patch `cb_main_matrices1.m_V` at Unmap) cannot make
head rotation visibly affect the main scene. Two paths forward, not yet
decided:

1. **Patch `cb_main_matrices0` (per-object) instead.** It carries `m_W`
   (world alone, uncombined) *and* `m_WVP`/`m_WV` (combined) in the same
   buffer. On every Unmap of a cb0 instance, recompute
   `new_WVP = m_W * newView * P` (and likely `new_WV = m_W * newView`)
   using the buffer's own `m_W`, our patched view rotation, and a cached
   projection matrix (from cb1, still legitimately read elsewhere). This
   needs real 4x4 matrix math (not just the 3x3 rotation-only approach so
   far) and fires far more often than cb1 (per-object, not per-frame) -
   materially more engineering than what's built so far. Doesn't obviously
   cover the eid=205-style instanced path (view baked into a vertex
   stream, not a constant buffer at all).
2. **Different technique entirely** - e.g. post-process reprojection of
   the already-rendered flat frame based on head rotation (skew/warp the
   2D image rather than touching game matrices). Avoids the per-object
   baking problem entirely, but is a cruder approximation (breaks down for
   large rotations or close geometry) - plausibly what vorpX itself falls
   back to for this game, given its known "warping" behavior.

Not implemented yet - needs a decision on which direction before more
code changes.
