# Helix Mod Shaderfix Inspection Findings — 2026-07-28

Source: `GameReferences/d3dx.ini` + `GameReferences/ShaderFixes/*_replace.txt`
(Helifax's Metro 2033 Redux 3D Vision fix, user-downloaded/extracted).

## The stereo mechanism this fix actually uses

This is **not** a per-eye dual-render-pass stereo fix. `[Rendering]` sets
`shader_hash = 3dmigoto` with `override_directory = ShaderFixes` and relies
on 3Dmigoto's automatic vertex-shader stereoization (`stereo_params = 125`,
`ini_params = 120`) — every vertex shader gets an automatic clip-space X
shear applied based on depth (classic NVIDIA 3D Vision technique), driven by
NVAPI convergence/separation (`[Key1]`/`[Key2]`/`[Key3]` bind convergence to
mouse/trigger/F1). The 14 hand-written `*_replace.txt` shaders in
`ShaderFixes/` are narrow bug fixes for specific effects that this automatic
shear broke (flashlight on smoke/dust/webs, lens flares, fog, HUD, crosshair,
loading screens, videos) — confirmed by their header comments.

**Implication for us**: this shear-based approach has no real per-eye camera
— it's a single flat render with a post-hoc geometric skew, not two actual
viewpoints. That's consistent with why vorpX's DirectVR for this game is
buggy (warping, no positional tracking) — it's built on the same kind of
mechanism. **We should not build VR on top of this shear trick.** It's
useful only as a compatibility fallback / proof that 3Dmigoto can hook this
game's shaders at all.

## The actual payoff: camera matrix layout (from shader source, not the shear hack)

Two unrelated replacement shaders — `77840e246bbf6e55-vs_replace.txt` ("LENS
FLARES") and `84870cd85ae567ef-vs_replace.txt` ("HUD Part 3") — both declare
the *same* two constant buffers:

```hlsl
cbuffer cb_main_matrices0 : register(b0)   // per-object
{
  row_major float3x4 m_W   : packoffset(c0);  // World
  row_major float3x4 m_iW  : packoffset(c3);  // Inverse World
  row_major float3x4 m_WV  : packoffset(c6);  // World-View
  row_major float4x4 m_WVP : packoffset(c9);  // World-View-Projection
}

cbuffer cb_main_matrices1 : register(b1)   // per-frame camera
{
  row_major float3x4 m_V   : packoffset(c0);  // View
  row_major float3x4 m_iV  : packoffset(c3);  // Inverse View
  row_major float4x4 m_P   : packoffset(c6);  // Projection
  row_major float4x4 m_iP  : packoffset(c10); // Inverse Projection
  ...
}
```

Two shaders with nothing else in common (a lens-flare effect and a HUD
element) both bind identical b0/b1 layouts — strong evidence `cb1`
(`cb_main_matrices1`) is 4A Engine's **global per-frame camera buffer**,
bound once per frame across most/all vertex shaders, not something
per-object or per-effect. `m_V` is exactly the hook point we want: overwrite
it with an HMD-derived view matrix before each eye's draw calls, per eye.

Also noted: `fix_InvTransform` example in the ini references
`InverseTranslatedViewProjectionMatrix` (line 792) — another naming
convention hint consistent with this being the standard camera buffer.

Other buffers seen (for reference, not camera-related): `cb_misc_0` (b3, fog/
sky/screen-space post fields + `m_screen`), `cb_screen` (b2, render target
dims/depth transform), `cb_light` (b12, ambient/dynamic light).

## Revised milestone 1 plan

Original plan assumed we'd need to *discover* a camera matrix hook via
RenderDoc from scratch. We now have the layout from source. Remaining
verification before writing injection code:

1. Use RenderDoc (or 3Dmigoto hunting mode + `analyse_options`) to confirm
   `cb1` in this exact layout is bound during actual first-person gameplay
   draws (not just these two effects), and that `m_V` visibly changes
   frame-to-frame as the in-game camera rotates — confirms it's live per-frame
   view data, not something baked/static.
2. Injection point: hook constant buffer binding at the API level (3Dmigoto's
   `HackerContext::VSSetConstantBuffers`, or a Map/Unmap override on the
   matching buffer) for any buffer bound to b1 matching this size/layout, and
   overwrite `m_V` with our OpenVR-derived per-eye view matrix immediately
   before the draw call. This must be native C++ in our 3Dmigoto fork — ini
   command-list scripting can read/write bound cbuffers by slot, but can't
   call into the OpenVR API for live pose, so the pose-polling and the
   cbuffer patch happen together in one native hook.
3. Real per-eye stereo (not the shear hack) means rendering the frame twice
   with two different `m_V`/`m_P` values and compositing to the two eye
   textures 3Dmigoto/OpenVR expects — bigger lift than just overwriting one
   buffer once, but this is milestone 2 (positional/stereo) territory.
   Milestone 1 (rotation only) can likely get away with a single render pass
   with `m_V` rotated by head orientation, submitted to both eyes, before we
   build true dual-pass stereo.
