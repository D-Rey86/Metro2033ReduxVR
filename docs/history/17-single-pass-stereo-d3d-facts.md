# Single-pass stereo: the D3D11 facts, measured

Four questions decide the shape of single-pass stereo, and all four were
answered by a standalone D3D11 program on the target GPU (RTX 5070 Ti) rather
than by reading documentation or spending in-headset test runs. Source:
`scratchpad/arraytest.cpp`.

`VPAndRTArrayIndexFromAnyShaderFeedingRasterizer` reports 1.

## 1. Per-instance slice routing works

A vertex shader writing `SV_RenderTargetArrayIndex = SV_InstanceID`, drawn once
with two instances into an RTV covering two array slices, fills both slices
with the correct per-instance colour. Drawn with one instance it fills slice 0
and leaves slice 1 untouched, which rules out the two results being confused
for one another.

This is the whole mechanism: one draw, two eyes.

## 2. The array index must be declared LAST

The same shader compiled two ways:

    struct VOut { float4 pos : SV_Position;
                  uint  rt   : SV_RenderTargetArrayIndex;   // first
                  float eye  : TEXCOORD0; };
    -> slices route correctly, but TEXCOORD0 arrives at the pixel shader as
       zero. The disassembly declares `dcl_output o2.x` and never writes it:
       the compiler silently discarded the output that followed the array
       index.

    struct VOut { float4 pos : SV_Position;
                  float eye  : TEXCOORD0;
                  uint  rt   : SV_RenderTargetArrayIndex;   // last
    -> everything correct.

Re-tested with four interpolants of mixed widths (float, float3, float4,
float2): with the array index last, all four survive at their own registers
with correct values.

This was a live bug in `StereoSinglePass::PatchHLSL`, which inserted the new
output as main's FIRST parameter - putting all 160 patched shaders' real
outputs in exactly the position that gets dropped. The failure would have
looked like a matrix bug (geometry in the right place, shading wrong), which
is an expensive thing to chase in a headset. Fixed to append at the end of the
parameter list.

## 3. A plain `Texture2D` SRV over an `ArraySize=2` resource is legal

It creates without complaint and reads slice 0. So the game's own textures can
be widened to two slices underneath it and every existing shader that samples
them keeps working untouched.

## 4. A `Texture2DArray` SRV of slice 1 alone feeds a `Texture2D` shader

Sampling it from a pixel shader that declares `Texture2D` returns slice 1's
contents - the dimension mismatch is not rejected.

Together, 3 and 4 mean the twin render target can simply BE slice 1 of the same
texture rather than a separate texture:

  - the geometry passes bind one RTV covering both slices and draw once with
    doubled instances, filling both eyes in a single pass;
  - the right eye's post-processing binds slice-1 views and runs the game's
    own unmodified shaders;
  - there are no per-frame copies anywhere.

## 5. NULL view descs default to covering EVERY slice

`CreateRenderTargetView(tex, NULL, ...)` on a 2-slice resource yields
`TEXTURE2DARRAY, FirstArraySlice 0, ArraySize 2`. Same for SRVs.

The game creates most of its views with a NULL desc, so left alone this would
make its post-processing write the left eye's image into both eyes. Every
game-created view over a widened texture therefore has to be given an explicit
`ArraySize = 1` desc in our `CreateRenderTargetView` / `CreateShaderResourceView`
/ `CreateDepthStencilView` hooks instead of passing the NULL through.

## Verification of the patch itself

`Tools/check_singlepass_patch.py` applies the identical text transformation to
all 184 decompiled vertex shaders in ShaderCache and compiles each with fxc:
170 compile, 0 fail, 14 skipped for having no clip position. Runtime patching
in-game reports the same shape - 160 patched, 0 decompile failures, 0 recompile
failures - the difference being only which shaders the session happened to
load.
