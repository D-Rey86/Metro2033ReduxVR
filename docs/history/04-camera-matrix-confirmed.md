# Camera Matrix Buffer Confirmed Live — 2026-07-28

## Method

RenderDoc + 3Dmigoto conflict directly at launch: both hook
`D3D11CreateDeviceAndSwapChain` and re-enter each other in a loop
("Hooking Quirk: Unexpected call back...", hundreds of times in
`d3d11_log.txt`), killing the process almost immediately. `renderdoccmd
inject --PID` into an already-running (3Dmigoto-hooked) process also
failed silently — target-control connection succeeded but the capture hook
never attached (no overlay, no captures).

**Working approach**: pulled `d3d11.dll`/`nvapi64.dll` out of the game
folder entirely (stock D3D11, no 3Dmigoto), launched via
`renderdoccmd capture`, captured a frame mid-gameplay with F12. Restored
3Dmigoto's DLLs afterward. Capture saved at
`GameReferences/metro2033_capture_frame3027.rdc` (613 MB, gitignored).

Analyzed headlessly via `qrenderdoc.exe --python Tools/dump_cb1.py`
(qrenderdoc bundles the `renderdoc` Python module internally — no separate
pymodules package needed). Script + notes on the API:
- `state.GetConstantBlocks(rd.ShaderStage.Vertex)` returns blocks in
  shader-reflection order, **not** indexed by HLSL register/bind number.
  Match by name against `state.GetShaderReflection(...).constantBlocks[i].name`
  / `.fixedBindNumber` instead of assuming list position = bind slot.
- Buffer content: `block.descriptor.resource` /`.byteOffset`/`.byteSize`,
  read via `controller.GetBufferData(resource, offset, size)`.
- `cap.OpenFile()`/`cap.OpenCapture()` return `rd.Result` objects — use
  `bool(status)`, not `status == rd.ReplayStatus.Succeeded` (that enum
  doesn't exist in this API version).

## Result: `cb_main_matrices1` (b1) confirmed

Reflection on a real gameplay vertex shader (eid 74) confirms the buffer
name and layout directly: `cb_main_matrices1`, `fixedBindNumber=1`,
`byteSize=352` (bigger than the 4 matrices alone — matches the `...` we saw
after `m_iP` in the shaderfix source, there are more fields after the
matrices we haven't identified yet, not needed for milestone 1).

Read across 6 different draw calls spanning event IDs 74–213 in one frame:

- **`m_V` (floats 0–11, row-major 3x4)**: each row has magnitude ≈1.0
  (verified by hand) — genuine orthonormal rotation + translation, i.e. a
  real view matrix, not garbage/placeholder data.
- **`m_iV` (floats 12–23)**: matches the transpose of `m_V`'s rotation
  block — confirms it's really the inverse view matrix, consistent with
  the named layout from Notes/02.
- **`m_P` (starts float 24)**: identical for eid 74/83, then changes at
  eid 96 onward and stays constant through eid 213. Most likely the game
  switching projection between world geometry and the first-person weapon
  viewmodel (common FPS technique to avoid weapon clipping/distortion at
  the world FOV) — normal engine behavior, not a red flag.
- **Same `m_V` held identical across all 6 draws** (74 through 213) —
  confirms this is a shared per-frame/per-pass camera buffer bound across
  many draw calls, not something per-object we'd have to patch per-shader.

## Conclusion

Milestone 1's hook target is now fully verified from live gameplay data,
not just static shader source. Next: task 4, integrate OpenVR pose polling
into the 3Dmigoto fork and overwrite `m_V`'s rotation on any buffer bound
to b1 matching `cb_main_matrices1`'s size, before each draw.
