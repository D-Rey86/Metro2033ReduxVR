# Metro 2033 Redux VR — Status Log

Seed document for a new thread. All figures are measured unless marked as estimate.

---

## 1. Project overview

**Target:** Metro 2033 Redux (4A Engine, 2014, x64). Closed source, no VR SDK, Steam-DRM encrypted `.bind` section. Deferred renderer, D3D11, single-threaded draw submission.

**Approach:** Fork of bo3b/3Dmigoto — a D3D11 wrapper DLL (`d3d11.dll`) loaded beside the game — plus OpenVR. All VR behavior is implemented by intercepting D3D11 calls and rewriting constant buffers, render targets and shaders at runtime. The engine has no stereo path to enable; everything is built from outside.

**Headset chain:** Samsung Galaxy XR → Virtual Desktop → SteamVR. **72 Hz, 13.9 ms frame budget.**

**Goal:** full VR with 6DOF motion controllers, quality above vorpX, intended for public release.

**Hard constraints stated by the user:**
- Lowering SteamVR/headset render resolution is not an acceptable fix.
- No visual quality regressions that would ship to other users.
- Wants a VR menu with options, including a resolution control and a stereo/AER toggle.
- Commit to git only when explicitly asked.

---

## 2. Environment & setup

### Paths

| | |
|---|---|
| Repo | `<PRIVATE_DEVELOPMENT_ROOT>` |
| Source | `ThirdParty\3Dmigoto\DirectX11\` |
| Solution | `ThirdParty\3Dmigoto\StereovisionHacks.sln` (`Release|x64`) |
| Build output | `ThirdParty\3Dmigoto\builds\x64\Release\d3d11.dll` |
| Game / deploy target | `<METRO_INSTALL>` |
| Runtime log | `<game>\d3d11_log.txt` (recreated per launch; can reach 9M+ lines) |
| Notes | `Notes\03..19*.md` |
| Python tools | `Tools\*.py` |
| RenderDoc | `Tools\RenderDoc\qrenderdoc.exe` (bundles its own Python) |
| Captures | `GameReferences\*.rdc` |

Deploy = copy `d3d11.dll` + `d3d11.pdb` into the game directory.

### RenderDoc — the tool to reach for first

Run headless: `qrenderdoc.exe --python Tools\<script>.py`. It bundles the `renderdoc` Python module internally, so no separate install is needed. RenderDoc and 3Dmigoto conflict at launch (both hook D3D11), so captures are taken with our `d3d11.dll` temporarily moved aside — captures therefore show **vanilla engine behaviour**, which is the right thing for questions about what the engine does.

**Three traps, each of which cost a wrong answer:**

- **`sys.argv` does not exist** in qrenderdoc's embedded Python. Touching it raises `AttributeError` at module scope, killing the script before its first log line and leaving a zero-byte output file — which looks exactly like "the script never ran". Use `getattr(sys, "argv", [])`. `find_skinned_draws.py` still has this fault.
- **RenderDoc's disassembly is NAMED.** A read of `m_VP` appears as `m_VP[0]`, and the only literal `cb1[N]` left is the declaration `dcl_constantbuffer cb1[14]`, where 14 is the buffer **size in rows**. Matching `cb1\[(\d+)\]` tallies declaration sizes as field reads and yields a table that is wrong in every row while looking entirely plausible. The raw disassembly our own DLL prints via `BinaryToAsmText` is *not* named, so there `cb1[14]` really is a row index — the two forms look alike and mean different things.
- **Record the pass.** `cb_main_matrices1` is refilled per pass, so "who reads this field" is meaningless without the viewport it was read in. The main camera is full render resolution; shadow and cubemap passes are small and square.

`Tools\cb1_readers.py` answers "which shaders read which field of `cb_main_matrices1`, in which pass". Reach for it before patching anything in that buffer.

### Toolchain gotchas

- **Historical machine note:** Python was not on `PATH`; the development machine used an explicit Python 3.12 installation. Current contributors should use any supported Python 3 available on `PATH`.
- MSVC 2022 Community. `vcvars64.bat` prints a harmless `vswhere.exe not recognized` warning; the build still succeeds.
- `fxc.exe` under `C:\Program Files (x86)\Windows Kits\10\bin\*\x64\`.

### Key source files

| File | Role |
|---|---|
| `HackerContext.cpp` (~7k lines) | Draw path, CB patching, twin pass, single-pass, weapon transform. The main workhorse. |
| `HackerDevice.cpp` | `CreateTexture2D` widening, view desc narrowing, shader creation hooks, IniParams texture. |
| `HackerDXGI.cpp` | Present, swap chain, per-frame reporting hooks. |
| `VRPose.cpp/.h` | OpenVR poses, per-eye projections, compositor submission, weapon transforms, matrix helpers. |
| `StereoTwin.cpp/.h` | Twin/array-slice render targets, draw classification, bisection harness. |
| `StereoSinglePass.cpp/.h` | Runtime VS patching, PS reflection, GPU timing, reprojection matrix. |
| `StereoBatch.cpp/.h` | Per-pass draw batching. **`gEnabled = false`** — correct but no measured gain. |
| `StereoCensus.cpp/.h` | Frame render-graph census. |

### Game settings that matter

`<game>\user.cfg` is written on exit and is **stale during a session** — do not trust it. Read the in-game menu instead.

- **QUALITY** bundles the internal resolution scale. VERY HIGH renders 1.41× linear (2× area) and resolves down. This was the single largest performance factor found.
- `r_dx11_tess 1` — tessellation. VERY HIGH vs HIGH made no measurable difference at current resolution.
- Menu RESOLUTION setting appears to be ignored; the game fullscreens onto Virtual Desktop's virtual display.

### Controls / diagnostics in-game

- **Left grip held ~2 s** → toggle true stereo ↔ alternate-eye (applied at frame boundary).
- **Right stick click** → step the viewmodel scale through `kWeaponScaleSteps`, logged on every change. This replaced the eye-texture dump *and* the viewmodel-hide cycle, both of which had answered their questions; the hide cycle in particular had to go, since it hid a piece of the weapon on every click while the player was trying to judge its size.
- **Left stick click held ~1 s** → toggle weapon-position adjust. While on: stick up/down raises/lowers, left/right moves laterally, **left grip + up/down** moves forward/back. Movement keys are suppressed so the player does not walk off mid-alignment. Logged continuously and on lock-in, so the chosen value can be read out and baked into `kWeaponOffsetView` / `kWeaponGripView`.

**Both of these exist for the same reason and it is worth stating as a rule:** where the player's eye is the instrument — how big the gun looks, where it sits in the hand — give the player the control and log the number they settle on. Hand-editing a constant, rebuilding and relaunching converges far slower and produced errors of 30 cm and more. Where the question is geometric — is it clipped, is it 1:1, which draw is this — measure it instead and do not ask.

---

## 3. Completed & working

### Established before this thread
Head tracking; alternate-eye rendering (AER); 6DOF controller-driven weapon with GPU-side transform (no readback stalls); viewmodel pass identification; culling-follow pitch (`VRPose::GetCullingCancellationPitch`); per-eye matrix patching of `cb_main_matrices0` (b0, per-object, 208 B: `m_W`/`m_iW`/`m_WV`/`m_WVP`) and `cb_main_matrices1` (b1, camera, 352 B).

### Built and verified in this thread

**Array-slice twin targets** (`StereoTwin::gSliceTwins = true`)
Eye-dependent render targets are created with `ArraySize = 2`; the twin is slice 1 of the same texture. `HackerDevice::CreateTexture2D` widens the desc before creation via `WidenDescForStereo`. Game-created views are forced to slice 0 by `NarrowRTVDesc` / `NarrowDSVDesc` / `NarrowSRVDesc`; twin views are slice 1. The back buffer cannot be sliced (DXGI owns its desc) and keeps a separate twin — both kinds coexist via `TextureTwin::sliced`.

**Single-pass stereo** (`StereoSinglePass::gEnabled = true`)
Every VS is decompiled → patched → recompiled at creation (`OnCreateVertexShader`). **160 patched, 0 decompile failures, 0 compile failures, 14 skipped (no clip position).** Patched draws are issued once with `InstanceCount * 2` into an RTV spanning both slices; the VS derives its eye from the instance id, writes `SV_RenderTargetArrayIndex`, and multiplies the right eye's clip position by a per-frame matrix `M = (P₁V₁)(P₀V₀)⁻¹` delivered through IniParams entries 8–11.
**Measured worth: 3.4 ms** (16.83 ms with, 20.1–20.6 ms without, mess hall, standing still).

`BeginSinglePass(bool instanced)` declines a draw unless all hold:
- not instanced (per-instance vertex fetch breaks — see §4)
- no GS/HS/DS bound (array index is taken from the last pre-raster stage)
- `sBoundVSCB[0] == sTrackedVRSlots[0].buffer` (proves the clip position came from the matrices `M` was derived from)
- no *sampled* SRV has a twin (via PS reflection, not bound state)
- the bound VS has a patched variant

**Combined clears** — one clear through the both-slices view instead of two single-slice clears. **~0.5 ms.**

**GPU timestamp timing** (`StereoSinglePass::FrameTiming`) — Present-to-Present, triple-buffered queries, logged every ~300 frames with mode and fold count.

**Self-bisecting harness** (`StereoTwin::gBisectRunning`, `SkipCategory`) — cycles 9 configurations every ~300 frames, logging real ms for each, so one standing-still run yields a whole breakdown.

**Viewmodel in both eyes** — three stacked bugs fixed (§4).

### Current measurements — mess hall, standing still, QUALITY = HIGH

| Configuration | GPU ms |
|---|---|
| True stereo (current build) | **~16.3** |
| Alternate-eye | 13.89–13.92 (**vsync-locked**, true cost is unknown and lower) |
| 72 Hz budget | 13.9 |
| **Gap to close** | **~2.4 ms** |

Session trajectory: **45.7 ms → 16.3 ms.**

### Bisection breakdown (taken at the OLD 3620×2009 resolution — all values scale down with pixels)

| Removed from the frame | Saved |
|---|---|
| Entire second eye's drawing | 6.5 ms |
| — tessellated draws | 3.7 ms |
| — post draws (sample an eye-dependent texture) | 3.0 ms |
| — plain geometry | 0.9 ms |
| — instanced draws | 0.2 ms |
| Twin clears | 1.0 ms |
| Second compositor submit copy | 0.0 ms |
| CPU building second-eye constant buffers | 0.34 ms |

---

## 4. Troubleshooting & failed attempts

### Methodological failure — the most expensive one

**Framerate was used as the metric for the entire early session.** At 72 Hz the compositor locks to a half or third of refresh, so it can only report ≈72, 36 or 24. Consequences:

- `"the game's entire frame costs 13.9 ms"` was the **72 Hz frame interval** (AER vsync-locked), not a cost.
- `"the second eye costs 33.7 ms"` was the difference between two quantized readings.
- Several *correct* changes measured as "no change" because they moved the frame time without crossing a threshold. Single-pass stereo measured as worthless at 3620×2009 and is worth 3.4 ms at 2560×1421 — same code.

**Rule for the next thread: never evaluate a change by reported fps. Read `GPU:` lines from the log.**

**Second methodological lesson:** guessing a cause and testing that one guess failed five times in a row (CPU constant buffers, VRAM paging, device replacement, stale caches, untwinned targets, eye offset). What worked was building diagnostics that *split the possibility space* — the auto-bisection, per-eye counters, `kBothEyesFrom`. Prefer bisection over hypothesis.

### Wrong premise: what single-pass actually saves

Single-pass stereo via instancing does **not** transform geometry once. One draw with two instances still runs the VS once per vertex **per eye**. It removes draw-call overhead, render-target switches and CB rewrites only. It was built to solve a 31 ms problem and addresses a ~6 ms one. Keep it (measured 3.4 ms) but do not expect vertex-work savings.

### The missing 24 ms — resolved

True stereo was 45.7 ms; removing *every* second-eye draw only reached 39.4 ms while AER was 15.4 ms. ~24 ms was attached to no category of draw. Hypotheses tested and **disproved**: CPU constant-buffer construction (0.34 ms), twin clears (1.0 ms), second compositor submit (0.0 ms), per-draw twin bookkeeping, VRAM paging (never actually measured — the logging was placed in a branch that only ran when bisection was off), device replacement (`NoteDevice` never fired).

**Actual cause: internal render resolution.** QUALITY = VERY HIGH renders 3620×2009 and resolves to a 2560×1440 back buffer. Dropping to HIGH → 2560×1421, and stereo went 45.7 → 16.83 ms. The cost was pixel/bandwidth bound, scaled with resolution, and was therefore invisible to a bisection that could only test draw categories. Nine phases could not find it because it lived in something the wrapper inherits rather than controls.

*Note:* the earlier framing of that supersampling as "throwing pixels away" was wrong — the downsample **is** the AA resolve. Turning it off costs real edge quality. It also reduced the shadow atlas 3072² → 2560².

### D3D11 facts established by a standalone test program (`scratchpad/arraytest.cpp`, see Notes/17)

- `VPAndRTArrayIndexFromAnyShaderFeedingRasterizer = 1` on RTX 5070 Ti.
- Per-instance slice routing via `SV_RenderTargetArrayIndex` from a VS **works**.
- **`SV_RenderTargetArrayIndex` must be declared LAST in the VS output struct.** Declared first, the compiler *silently discards every output after it* — correct slice routing, but the PS receives nothing. This was a live bug that would have corrupted all 160 shaders in a way that looks like a matrix error.
- A plain `Texture2D` SRV over an `ArraySize=2` resource is legal and reads slice 0.
- A `Texture2DArray` SRV of slice 1 alone feeds a shader that declares `Texture2D`.
- **A NULL view desc over an array resource covers ALL slices** — every game-created view needs an explicit `ArraySize = 1` desc.

### The muzzle flash — fixed, and how it was actually found

**It is a PARTICLE draw.** The engine's `inst0..inst7` layout, additive One/One blending, and `SV_Position` built from **`m_WVP` in `cb_main_matrices0`** — not `m_VP` in `cb1`, which is where an entire evening of failed attempts was aimed.

Fixed by folding the weapon's view-space transform `M` into `m_WV`/`m_WVP` inside `PatchMappedVRObjectData`, gated on the draw being a particle near the eye.

**Do it at the patch site, not at draw time.** A draw-time private constant buffer put the flash on the barrel in one eye and a long way off in the other: the twin pass re-issues the draw for the second eye between `BeforeDraw` and `AfterDraw`, and a buffer bound across both hands the second eye the *first* eye's matrices. `PatchMappedVRObjectData` runs once per eye, so per-eye correctness is free there.

**`sWeaponParamsEye` matters.** The stored weapon params are always the *first* eye's — the twin pass adjusts a local copy and never writes it back. Anything undoing their eye-offset term must use the same eye's offset, not the current one. Using the current eye put the flash a full IPD to the right in the right eye while the left was perfect. That will bite again anywhere the stored params get reused.

**How it was located.** Two methods failed and one worked:

- A **firing-diff** (draws that appear only while the trigger is held) found VS `C5A6A1F5B984FBF0` — a real firing-only draw, but *not* the flash. The method has a hole by construction: a flash drawn by a shader also used elsewhere is filtered out. A 2 m probe applied to that draw moved nothing, which is what proved it was wrong.
- **Pixel history on the wrong targets** wasted three rounds: the swapchain (only the post composite lands there), the G-buffer (transparent geometry never writes it), and a "main viewport only" filter.
- **A capture containing the flash**, then reading the shader, gave the answer immediately.

### The muzzle SMOKE — still open

Not the same draw. It lives in the **quarter-resolution offscreen particle buffer** (640×355 against a 2560×1421 scene, target 5340), which is why it appears in none of the obvious places: not in the G-buffer (transparent), not in the lighting output at those pixels (composited from elsewhere), and not in any draw scan that filtered to the main viewport. Its shader (`VS 5457`) uses `POSITION/COLOR/TEXCOORD` — **not** the `inst0..inst7` layout — so the runtime particle gate rejects it regardless of any proximity radius.

Widening the gate to include that pass did not fix it. Unresolved.

### Viewmodel geometry — facts established by measurement (task 27)

Everything here was measured, not inferred. The measurement code lives in `SubstituteWeaponInstanceBuffer` (`VRPose calib:` and `VRPose mesh:` log lines) and in the shader dump in `HackerDevice::CreateVertexShader`.

- **Vertex positions are a packed `short4`, decoded as `short × 12/32768`** (`0.000366210938`) — the int16 range spans ±12 m at 0.366 mm resolution. Identical constant in all six viewmodel vertex shaders. TEXCOORD uses `1/2048`; the world-space effect shader uses `1/32768`.
- **Geometry is pooled.** IA slot 0 is a single ~92 MB buffer for the whole level. An index count locates nothing in it; the draw's own `StartIndexLocation` / `BaseVertexLocation` are required, and are now captured for that purpose.
- **The viewmodel transform chain, confirmed from the shader**: `clip = m_P × (xform × position)`, where `xform` is the per-instance 3×4 local-to-view matrix at slot 1 and `m_P` is the matrix we replace with `P_eye × Correction`. There is no other scaling anywhere in the path.
- **The engine bakes no scale** into the instance matrix — rows measure exactly 1.000.
- **Two of the six viewmodel shaders are skinned** (`79BA9F1ECFAF0CA1`, `03065006306540D5`): they build position from bone matrices and never read the xform rows. Applying the instance matrix to their vertices produces a confident, wrong answer — see the failed placement attempt below.
- Measured sizes: weapon body 0.588 m long; a 3.9 × 1.2 cm object in the cluster; a 30 cm rod. Physically sensible, which corroborates the decode independently of the shader.

### The culling-pitch lead — disproved

The standing hypothesis was that the weapon is carried by a camera pitched away from the head by the culling injection, and that the weapon path needs to subtract `GetCullingCancellationPitch()`.

**It does not.** Measured over 185 samples: while the injection swung from −10° to **+29.6°** of pitch, the rotation inside `vrViewCorrection3x4` stayed at 0.3–3°, and the vertical placement error never moved (−0.001 to −0.018 m, flat across the entire range). The correction already absorbs the injection before the viewmodel is drawn.

Two refinements worth keeping:

- The weapon's deltas are measured in the **head's** frame but written into an instance matrix living in the **engine camera's** frame, so they are conjugated by `Correction`. The exact fix is `D' = C⁻¹DC`, `T' = C⁻¹T`. Bounded at ≤3°, so ~2.6 cm on a 0.5 m lever — real but small, and **not applied**.
- The correction showed transient **roll up to 8.4°** during head movement, more than yaw/pitch composition order explains. On the gun's lever that is ~7 cm of lateral swing, and it was the only thing making the horizontal error noisy while vertical and depth stayed exact. Unexplained.

### The weapon compute shader had no scale term at all

`kWeaponTransformHLSL` only ever rotated and translated. Nothing in it could change how big the weapon is, so every earlier attempt at "the gun is too small" was moving it rather than resizing it. A uniform scale about the rotation pivot has been added.

### `m_VP` was never patched — the muzzle flash finding

`cb_main_matrices1` holds six matrices. `PatchMappedVRCameraData` patched four of them (`m_V`, `m_iV`, `m_P`, `m_iP`) and left **`m_VP` (float 56)** and `m_iVP` (float 72) exactly as the game wrote them.

Any shader reading `m_VP` therefore rendered through the **original flat-screen camera** — no head rotation, no eye offset, no headset frustum. Found via the muzzle flash, whose vertex shader (`C5A6A1F5B984FBF0`) does nothing but `clip = m_VP × worldPosition` over CPU-built world-space particles. That is why it sat still while the world moved.

**Patching it was tried and REVERTED — it broke walls and floors.** RenderDoc then said why, and the reason matters more than the bug.

Readers of `m_VP`, from `Tools/cb1_readers.py` against `metro2033_capture_frame3027.rdc`:

| pass | shaders | sampled draws |
|---|---|---|
| shadow/cascade viewports (918², 690², 632², 400², 378²) | 8 | ~297 |
| main camera (3620×2009) — all skinned, no `xform` | 3 | 34 |
| `m_iVP` — anywhere | 0 | 0 |

**Why it broke is still UNKNOWN.** Three plausible explanations were each tested against the capture and each is false:

- *"m_VP is read by everything."* No — 11 shaders, and only 3 of them in the main viewport.
- *"The `projectionLooksLikeMainCamera` gate lets shadow passes through."* No. Measured per pass: the gate passes **only** for the main camera (3620×2009, `P[0]=0.9471`, `P[5]=1.7062`, aspect 1.8015, near 0.1 far 40). Every shadow pass fails it — their `m_P` slot does not even hold a projection (`row3 = [-0.144, -0.212, -0.967, 26.554]`), so the game evidently packs different content into those offsets per pass. An aspect-based gate was proposed to fix this and is **not needed**.
- *"The game sub-allocates cb1, so a fixed-offset patch lands in another pass's region."* No — every pass binds the same `ResourceId::81` at offset 0, size 65536. One shared buffer, rewritten in place.

So the mechanism is still open. Do not re-apply the patch on a fresh guess. The cheap next test is an in-DLL counter: how many times per frame does `PatchMappedVRCameraData` run, and how often does the gate pass? If it passes more than once per eye per frame, something we believe is the main camera is not.

A capture *with the wrapper active* would settle it outright, but RenderDoc and 3Dmigoto conflict at launch and RenderDoc cannot attach to a running process (Notes/04, Notes/11), so that route is currently closed.

**A previously unexplained result is now explained.** §4 records that at 4× IPD "the doubling props and NPCs did not move at all... those reach the shader by a route that never sees a per-eye matrix." That route is `m_VP` — the three main-pass readers are exactly the skinned character shaders, and they are also the only readers of `m_V`.

`m_iVP` has **no readers at all**, so it never needed patching.

**`Tools\cb1_contents_by_pass.py`** dumps the buffer's actual contents, resource and offset per pass. It is what eliminated the three hypotheses above, and it is the right first move for any future question about this buffer.

### Finding a draw by what it is *not*

The muzzle flash was located without recognising it. Index counts do not identify objects and shaders are shared, but the flash has one property nothing else in the frame has: it exists only while the trigger is held. Recording which draws happen when *not* firing and reporting the ones that appear when firing isolated it in a single run — one shader, one layout, index counts 234/462/738 (all multiples of six: a growing batch of quads).

Generalisable: when an object cannot be recognised, find a condition under which it alone appears.

### IniParams sizing bug

3Dmigoto sizes the IniParams `Texture1D` from what the ini declares — here **one texel**. The reprojection matrix was written to entries 0–3; loads of texels 1–3 returned zero, so the right eye's clip position became `(x, 0, 0, 0)` → `w = 0` → degenerate divide → geometry vanished and blew out to white. `UploadIfDirty` was also memcpy'ing 4 float4s into a 1-texel mapping. Fixed: matrix moved to entries **8–11** (clear of the ini's own `x`/`w`), with `G->iniParamsReserved` raised before the texture is created. **Never resize `G->iniParams` after creation** — the GPU texture will not follow.

### Sticky SRV bindings

The single-pass gate originally asked "is any eye-dependent SRV *bound*?" D3D bindings persist until overwritten, so post-processing's scene texture was still bound during the next frame's world geometry: **288 of 291 draws declined** and single-pass never ran. Fixed by recording, via `D3DReflect` at PS creation, which `t#` slots each PS actually samples (`SampledSlotsOf`) and consulting only those.

### Instanced draws cannot simply have their instance count doubled

The shader-side `SV_InstanceID` remap does not reach the **input assembler**, which fetches per-instance vertex data using the real instance index. The right eye's copy of object *N* read object *N+1*'s transform — sideways NPCs, props flickering into view. Correct fix (not implemented): a second input layout with every `PER_INSTANCE` element's `InstanceDataStepRate` doubled. Instanced draws are currently declined; they measured only 0.2 ms.

The eye is carried in the **low bit** of the instance id (`vr_eye = id & 1; id = id >> 1`), deliberately, so no per-draw upload of an instance count is needed.

### Tessellated draws cannot fold

`SV_RenderTargetArrayIndex` is taken from the last stage before the rasterizer, so with a GS/HS/DS bound the VS value is discarded and both eyes land in slice 0 — the right eye's copy of a surface appears in the *left* eye carrying the reprojection, reading as "walls and floors missing from one eye and sliding around in the other". They are declined and double-drawn. Folding them needs the eye threaded VS → HS → DS: three coordinated shader patches.

### Video-settings crash — OPEN, unresolved

Changing TESSELLATION crashed the game (at OFF and at HIGH). Log ends mid-word right after the swap chain is recreated and render targets re-twinned.

- Hypothesis 1: device replacement → `StereoTwin::NoteDevice` added. **Never fired** — the device is not replaced.
- Hypothesis 2: swap-chain recreation → `StereoTwin::ResetForNewSwapChain` added, hooked in the `HackerSwapChain` constructor. **Also never fired** — likely the game uses `ResizeBuffers`, which reuses the swap chain object.
- After that build the change did *not* crash, but **the fix cannot be credited** because neither reset ran. Address-reuse bugs hide; treat as open.
- Related symptom: after a settings change, stereo showed double vision until restart — stale state surviving the rebuild.

### Viewmodel only in the left eye — three stacked bugs (all fixed)

1. **The second-eye draw was never issued.** The viewmodel takes its own early-return path in `DrawIndexedInstanced` (substitute instance buffer → draw → restore → `return`) and never reached `BeginTwinPass`. The right-eye weapon re-dispatch lived *inside* `BeginTwinPass` and had never once run for the object it was written for.
2. **Adding the twin pass broke the left eye.** That path never called `RestoreEye0IfNeeded()`, so the CB was left holding the right eye's matrices and the *next* viewmodel mesh drew its left eye with them — gun, hands and watch came apart from each other.
3. **The real one — D3D11 unbinds a vertex buffer bound as a UAV.** The re-dispatch binds the weapon's private instance buffer as a UAV for the compute shader; D3D11 will not have a resource bound as VB and UAV simultaneously and **silently unbinds it from IA slot 1**. Clearing the UAV does not restore it, so the second draw ran with no per-instance data and produced nothing. Fixed by re-binding to IA slot 1 after the dispatch (`sPendingWeaponStride`, offset 0).

Diagnostics that actually resolved it: per-eye viewmodel counters (11/11/11 proved everything executed) and `kBothEyesFrom = 1` in `VRPose.cpp` (shows the twin image to both eyes — no gun there either, proving the draw produced nothing rather than landing out of view).

### Other dead ends

- **`StereoBatch`** — per-pass draw batching, correct, no measured gain. Left behind `gEnabled = false`.
- **Frustum sign** in `VRPose.cpp`: `P[2] = -(r+l)/(r-l)` and `P[6] = -(t+b)/(t-b)` are **correct**. Flipping them was tried and produced a relative yaw between eyes. Do not "fix" this again.
- **`PatchForBothEyes` must not touch `G->vrCurrentEye`** in the single-eye path — forcing it to 0 pinned every frame to the left eye and froze the right eye in AER.

### Recurring self-inflicted errors — check for these first

- **Bash heredocs mangle `\n` into literal newlines inside C string literals** (~10 occurrences). Symptom: `error C2001: newline in constant`. Prefer the `Edit` tool for anything containing escape sequences.
- **Statics declared below first use** (`TicksNow`, `sPendingWeaponStride`) → `C3861: identifier not found`.
- **`printf` `%u` against a `double`** (`frames` is `600.0`) → billions-per-frame garbage in logs.

---

## 5. Current state & immediate next step

### Deployed build

Config A: array slices + single-pass folding + combined clears + viewmodel second-eye fix. Correct stereo in both eyes, viewmodel present in both eyes, **~16.3 ms** mess hall. `gBisectRunning = false`, `gVerifyByRedraw = false`, `kBothEyesFrom = -1`, `kSecondEyeSameTransform = false`.

### Task #27 — RESOLVED. The viewmodel got the eye offset TWICE

**Root cause.** The viewmodel reaches the screen through `m_P`, which `PatchMappedVRCameraData` replaces with `P_eye × Correction`. Expanding `Correction = V_new × V_old⁻¹`:

```
Correction(p) = R_new · R_old⁻¹ · p  −  eyeOffset
```

because `V_new`'s translation already carries `−eyeOffset`. The weapon transform then subtracted it *again*. **Double eye separation is double disparity, which the brain reads as half the distance — and an object at half the apparent distance must be half the size to subtend the same angle.**

That single fault produced every symptom chased in this session:

- the gun looked like a toy at its true scale;
- **exactly** 2.0× looked correct, because that is the precise compensation for a doubled IPD — the suspiciously round number was the clue and went unread for hours;
- the gun still physically reached the eye at 1.0×, so it "behaved as if 1.0 was right while looking wrong";
- the sights were unreachable at 2.0×, being twice as far from the grip as the model intends;
- it appeared during the stereo work, when the correction fold and the weapon's own eye-offset term first coexisted;
- it affected AER too, because both share `m_P`.

**Fixed** by removing the weapon's own eye-offset term (`kWeaponSubtractEyeOffset = false`). Two dependent sites had to change with it, both of which assumed the term existed: the second-eye re-dispatch no longer swaps left-for-right offset (that would inject a full IPD into the second eye), and the muzzle-flash fold no longer adds it back to cancel a term that is gone.

Scale is now **1.0× — the model's true size** — and confirmed by eye.

**What found it:** the player's own account. They remembered the gun being correctly sized in early AER, that it changed during the stereo work, that it then read wrong in *both* modes, and that they only noticed once the gun rendered in both eyes. Then they quoted a note from the earlier thread flagging that the viewmodel would sit at the wrong depth because a single shared buffer gave it zero disparity. The fix for that went in; nobody checked whether the offset was *also* arriving by the other route. **No measurement taken this session would have found this** — every diagnostic confirmed the transform was correct, because it was. The error was one level up, in a term applied twice.

### Weapon placement, as it now stands

- **Grip pinned to the controller.** `translate = ctrl − grip + adj`, with the rotation and scale pivot AT the grip, so `s·D·(G − pivot)` is identically zero and hand rotation cannot translate the weapon.
- The pivot previously sat 23 cm from the grip; at 2× scale that was a **46 cm lever**, so turning the wrist to aim threw the gun away from the face. Rotating about the grip is also simply what a held object does.
- Placement is **absolute**, not anchored to wherever the controller happened to be at startup — that anchor changed every session and quietly moved the calibration.
- `kWeaponGripView` is derived from the player's own calibration, not measured off a mesh: `grip = anchor − offset`, then converted back through the scale pivot. An earlier attempt to measure it off mesh 16251 put the weapon 70 cm out.

### Superseded: task #27's earlier framing

**Scale — resolved for now.** A uniform scale about the rotation pivot was added to the weapon compute shader, which previously had no scale term at all. Default **2.0×**, chosen in the headset by the player stepping `kWeaponScaleSteps` with the right stick.

Treat that number as provisional, for a reason beyond taste: the scale pivots about `kWeaponRotationPivot` at z = +0.40, which is in front of the weapon, and scaling about a point in front of an object drags it toward that point as it grows. Roughly 38 cm of the apparent increase is the gun being pulled toward the eye rather than made bigger. **Needing 2× at all is a signal that something upstream is off by about a factor of two** — either the mesh measured at 0.588 m is not the weapon body, or the world renders larger than life. Worth chasing rather than papering over.

**Position — still open, and one attempt failed.** Absolute placement (put the grip at the controller, retiring the arbitrary startup anchor) was implemented and **reverted**: it moved the weapon ~70 cm forward, out of the hand and onto the scenery, confirmed by screenshot.

The design is right; the constant fed into it was not. The grip was derived from a "view-space" box computed as `R × local + t` using the instance matrix — valid only for a **rigid** mesh, and it was never checked whether mesh 16251 is one of the two skinned ones. If it is skinned, bone matrices position it and that box is meaningless.

The code is intact behind `kWeaponAbsolutePlacement = false`, and the mesh measurement now logs `VS=` per mesh so the rigid/skinned question can be settled. **That single fact unblocks both the grip and the pivot**, and fixing the pivot should bring the honest scale below 2×.

**Method note that paid off, and one that did not.** Measuring beat guessing on every geometric question here — the vertex decode, the pooled buffer, the disproved pitch lead. But perceived size in the player's own hand is the exception: no measurement taken from outside the headset settles it, which is why that one is a stepped control rather than a constant.

### Immediate next step

One short run yields `VS=` for each measured viewmodel mesh. Classify rigid vs skinned against the dumped shaders (`vm_vs_*.txt` in the game directory — rigid ones read `v7/v8/v9`, skinned ones build position from bone matrices), take the view-space box of a **rigid** weapon mesh, and set the grip and pivot from it. Then re-enable `kWeaponAbsolutePlacement` and re-check the scale.

### Open tasks

| # | Task |
|---|---|
| 19 | Submit the full-res target instead of the downscaled back buffer |
| 24 | Couple render resolution to submitted resolution (prerequisite for the VR menu's resolution slider) |
| 25 | Close the last ~2.4 ms |
| 26 | Video-settings crash — cause still unconfirmed |
| 27 | ~~Viewmodel scale/position~~ **DONE** — doubled eye offset; scale now 1.0×, grip pinned to the controller |
| 35 | **Iron-sight accuracy — the next task.** Bullets follow the game's camera, steered by the controller through injected mouse input (`UpdateMotionAiming`); the memory-write override is disabled and its scanner retired. The gun's visual axis and that aim direction differ by a fixed rotation — `sWeaponAnchorRot`, captured from whatever pose the controller was in at startup. Scale-independent, so it survives any later size change. Build a rest-orientation adjustment on the same pattern as the position control, fire at the target wall, lock it in. First task all session with an objective success criterion. Check early whether the injected aim lags the controller — if so the sights will be truthful when still and wrong when moving, which is a separate problem from zeroing |
| 36 | Muzzle smoke — quarter-res offscreen particle buffer, layout not `inst0..inst7`, gate widening did not work |
| 28 | Evaluate OpenXR port for VDXR support |
| 29 | Muzzle flash does not follow the head — it reads `m_VP`, which is unpatched. Blocked on task 34 |
| 30 | **Why did patching `m_VP` break walls and floors?** Three explanations tested against the capture and all false (see §4). Next: in-DLL counter of `PatchMappedVRCameraData` calls per frame and how often the gate passes |
| 34 | Patch `m_VP` for main-camera draws only, once task 30 lands. Fixes the muzzle flash **and** the long-standing "NPCs and props double / ignore the eye offset" issue — those skinned main-pass shaders position via `m_VP` |
| 31 | Why does the viewmodel need 2× to look right? Either the measured weapon mesh is not the weapon, or world scale is off by ~2 |
| 32 | Transient 8.4° roll in `vrViewCorrection3x4` during head movement — unexplained, ~7 cm of lateral weapon swing |
| 33 | Objects ~80 cm off to the side are passing the viewmodel cluster test and being dragged by the controller |

### Known outstanding issues, not yet filed as tasks

Three assets remain head-locked; near-field flicker in both modes; AER flickers in the left eye only (different mechanism); menus render incorrectly; decals and some lighting track the head; aim zeroing not calibrated.

**Re-check the head-locked assets and the head-tracking decals/lighting first.** Both are exactly what an unpatched `m_VP` produces, and `m_VP` was only patched at the end of this thread — some of that list may already be gone.

### Notes worth reading

- `Notes/17` — D3D11 facts for single-pass, measured on the target GPU.
- `Notes/18` — where true stereo's time goes; the quantized-framerate trap.
- `Notes/19` — the viewmodel's missing second eye.
- `Notes/11` — pop-in/flicker ROOT CAUSE: Metro's CPU-side software
  occlusion at `+0x826B50` rejecting objects before D3D submission.
- `Notes/21` — single-pass unsafe-shader list, world-placed menu UI, and
  the mixed-space G-buffer fix that stopped shadows/lights moving with
  the head.

### Path to 72 Hz — honest assessment

The base frame must get cheaper; the second eye is only ~6.5 ms of it and shrinking. Remaining levers, in order of expected value: resolve the render/submit resolution mismatch (task 24/19), fold post-processing passes (needs PS patching to sample per-slice), fold tessellated draws (needs VS→HS→DS eye plumbing). AER already holds 72 Hz with unmeasured headroom. True stereo at 36 Hz is the guaranteed outcome; 72 Hz is plausible but unproven.
